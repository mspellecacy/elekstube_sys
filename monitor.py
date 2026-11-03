#!/usr/bin/env python3
"""Linux telemetry -> ELEKSTUBE IPS PRO over USB serial or Wi-Fi."""
from __future__ import annotations

import argparse
from collections import defaultdict
import fcntl
import json
import logging
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

DEFAULT_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
DEFAULT_CONFIG = Path(__file__).resolve().with_name("config.json")
LOG = logging.getLogger("elekstube")


def metrics_message(seq: int, panels: list[dict], timestamp: float | None = None) -> dict:
    """Send host-local wall time, including its subsecond phase, with telemetry."""
    now = time.time() if timestamp is None else timestamp
    local = time.localtime(now)
    clock_ms = ((local.tm_hour * 60 + local.tm_min) * 60 + local.tm_sec) * 1000
    clock_ms += int(now * 1000) % 1000
    return {"type": "metrics", "seq": seq, "clock_ms": clock_ms, "panels": panels}


def read_text(path: Path) -> str:
    try:
        return path.read_text().strip()
    except (OSError, UnicodeError):
        return ""


def panel(label: str, value: float | None, unit: str, detail: str = "",
          scale: float = 100, decimals: int = 0) -> dict:
    available = value is not None and math.isfinite(value)
    formatted = f"{value:.{decimals}f}" if available else "--"
    if available and unit == "%" and round(value, decimals) == 100:
        formatted = "100"
    return {"label": label, "value": formatted,
            "unit": unit, "detail": detail[:19],
            "percent": min(100, max(0, value / scale * 100)) if available else -1}


def rate_panel(label: str, rate: float | None) -> dict:
    if rate is None:
        return panel(label, None, "KiB/s", "sampling")
    rate = max(0, rate)
    # Logarithmic graph: full scale at 1 GiB/s, so both quiet and busy links show.
    result = panel(label, rate / 1024, "KiB/s", "all physical links", decimals=1)
    if rate >= 1024 ** 2:
        result.update(value=f"{rate / 1024 ** 2:.1f}", unit="MiB/s")
    if rate >= 1024 ** 3:
        result.update(value=f"{rate / 1024 ** 3:.1f}", unit="GiB/s")
    if float(result["value"]) >= 100:
        result["value"] = f"{float(result['value']):.0f}"
    result["percent"] = min(100, math.log2(1 + rate / 1024) / 20 * 100)
    return result


def traffic_panel(label: str, primary: float | None, secondary: float | None) -> dict:
    result = rate_panel(label, primary)
    other = rate_panel(label, secondary)
    result.update(secondary_value=other["value"], secondary_unit=other["unit"],
                  secondary_percent=other["percent"])
    return result


def drm_fd_paths(proc: Path) -> list[Path]:
    paths = []
    for process in os.scandir(proc):
        if not process.name.isdigit():
            continue
        try:
            with os.scandir(process.path + "/fd") as entries:
                for entry in entries:
                    try:
                        if os.readlink(entry.path).startswith("/dev/dri/"):
                            paths.append(Path(process.path) / "fdinfo" / entry.name)
                    except OSError:
                        pass
        except OSError:
            continue
    return paths


def parse_drm_clients(proc: Path, paths: list[Path] | None = None) -> dict:
    """Deduplicate inherited/opened DRM fds using device + client ID."""
    clients = {}
    for entry in drm_fd_paths(proc) if paths is None else paths:
        fields = {}
        for line in read_text(entry).splitlines():
            key, sep, value = line.partition(":")
            if sep:
                fields[key] = value.strip()
        if "drm-client-id" not in fields:
            continue
        device = fields.get("drm-pdev", fields.get("drm-driver", "unknown"))
        key = (device, fields["drm-client-id"])
        counters = {}
        for field, text in fields.items():
            if not field.startswith(("drm-engine-", "drm-cycles-", "drm-total-cycles-")):
                continue
            try:
                counters[field] = int(text.split()[0])
            except (ValueError, IndexError):
                pass
        # Take the latest snapshot when the same client appears several times.
        clients[key] = counters
    return clients


def drm_usage(previous: dict, current: dict, elapsed: float) -> float | None:
    """Return the busiest engine class on the busiest visible GPU.

    Xe reports busy/total cycle deltas. i915/AMDGPU report engine nanoseconds.
    A newly seen client needs a second sample; never treat lifetime counts as
    interval counts. Capacity normalizes engine classes with multiple engines.
    """
    engines = defaultdict(float)
    for client, fields in current.items():
        old = previous.get(client)
        if old is None:
            continue
        for field, busy in fields.items():
            if field.startswith("drm-engine-capacity-"):
                continue
            if field.startswith("drm-cycles-"):
                engine = field.removeprefix("drm-cycles-")
                total_field = "drm-total-cycles-" + engine
                denominator = fields.get(total_field, 0) - old.get(total_field, 0)
            elif field.startswith("drm-engine-"):
                engine = field.removeprefix("drm-engine-")
                denominator = elapsed * 1e9
            else:
                continue
            if field not in old or busy < old[field] or denominator <= 0:
                continue
            capacity = max(1, fields.get("drm-engine-capacity-" + engine, 1))
            engines[(client[0], engine)] += (busy - old[field]) / denominator / capacity * 100
    return min(100, max(engines.values())) if engines else None


class Sampler:
    def __init__(self, proc: Path = Path("/proc"), sys: Path = Path("/sys")):
        self.proc, self.sys = proc, sys
        self.previous_cpu = None
        self.previous_network = {}
        self.previous_disks = {}
        self.previous_gpu = {}
        self.previous_time = None
        self.gpu_paths = []
        self.gpu_scan_time = -math.inf

    def cpu(self) -> float | None:
        first_line = read_text(self.proc / "stat").splitlines()[0]
        # guest and guest_nice are already included in user/nice; don't count twice.
        times = [int(v) for v in first_line.split()[1:9]]
        total, idle = sum(times), times[3] + times[4]
        previous, self.previous_cpu = self.previous_cpu, (total, idle)
        if previous is None or total <= previous[0]:
            return None
        return min(100, max(0, 100 * (1 - (idle - previous[1]) / (total - previous[0]))))

    def memory(self) -> tuple[float, float]:
        fields = {}
        for line in read_text(self.proc / "meminfo").splitlines():
            name, value = line.split(":", 1)
            fields[name] = int(value.split()[0])
        total, available = fields["MemTotal"], fields["MemAvailable"]
        used = total - available
        return used / 1024**2, total / 1024**2

    def temperature(self) -> float | None:
        for directory in sorted((self.sys / "class/hwmon").glob("hwmon*")):
            driver = read_text(directory / "name")
            if driver not in ("k10temp", "coretemp", "zenpower"):
                continue
            candidates = list(directory.glob("temp*_input"))
            candidates.sort(key=lambda p: 0 if read_text(p.with_name(p.name.replace("_input", "_label")))
                            in ("Tctl", "Package id 0", "Tdie") else 1)
            for sensor in candidates:
                try:
                    return int(sensor.read_text()) / 1000
                except (OSError, ValueError):
                    pass
        return None

    def network(self, elapsed: float) -> tuple[float | None, float | None]:
        counters = {}
        for line in read_text(self.proc / "net/dev").splitlines()[2:]:
            name, values = line.split(":", 1)
            name = name.strip()
            # Exclude loopback, VPNs and virtual bridges to avoid double counting.
            if not (self.sys / "class/net" / name / "device").exists():
                continue
            numbers = values.split()
            counters[name] = (int(numbers[0]), int(numbers[8]))
        old, self.previous_network = self.previous_network, counters
        if self.previous_time is None:
            return None, None
        rx = tx = 0
        for name, (received, sent) in counters.items():
            if name in old:
                rx += max(0, received - old[name][0])
                tx += max(0, sent - old[name][1])
        return rx / elapsed, tx / elapsed

    def gpu(self, elapsed: float) -> tuple[float | None, str]:
        # AMD's device counter includes all clients without requiring root.
        for sensor in (self.sys / "class/drm").glob("card[0-9]*/device/gpu_busy_percent"):
            try:
                return float(sensor.read_text()), "device activity"
            except (OSError, ValueError):
                pass
        if time.monotonic() - self.gpu_scan_time > 5:
            self.gpu_paths = drm_fd_paths(self.proc)
            self.gpu_scan_time = time.monotonic()
        current = parse_drm_clients(self.proc, self.gpu_paths)
        value = drm_usage(self.previous_gpu, current, elapsed)
        self.previous_gpu = current
        if current:
            return value, "visible DRM clients"
        # NVIDIA exposes the whole-device utilization through its shipped tool.
        try:
            result = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                                    capture_output=True, text=True, timeout=1, check=True)
            return max(float(v) for v in result.stdout.splitlines()), "NVIDIA device"
        except (OSError, subprocess.SubprocessError, ValueError):
            return None, "unavailable"

    def disk(self, elapsed: float) -> tuple[float | None, float | None]:
        counters = {}
        # Whole physical devices only: excludes partitions, loop and dm/RAID
        # layers so the same I/O is counted once. Kernel sectors are 512 bytes.
        for device in (self.sys / "block").glob("*"):
            if not (device / "device").exists():
                continue
            try:
                stats = [int(n) for n in (device / "stat").read_text().split()]
                counters[device.name] = (stats[2] * 512, stats[6] * 512)
            except (OSError, ValueError, IndexError):
                continue
        previous, self.previous_disks = self.previous_disks, counters
        if self.previous_time is None or not counters:
            return None, None
        read = written = 0
        for name, (r, w) in counters.items():
            if name in previous:
                read += max(0, r - previous[name][0])
                written += max(0, w - previous[name][1])
        return read / elapsed, written / elapsed

    def sample(self) -> list[dict]:
        now = time.monotonic()
        elapsed = max(0.001, now - self.previous_time) if self.previous_time is not None else 1
        cpu = self.cpu()
        memory_used, memory_total = self.memory()
        temperature = self.temperature()
        rx, tx = self.network(elapsed)
        disk_read, disk_write = self.disk(elapsed)
        gpu, gpu_detail = self.gpu(elapsed)
        self.previous_time = now
        return [panel("CPU USAGE", cpu, "%", f"{os.cpu_count()} THREADS", decimals=1),
                panel("CPU TEMP", temperature, "°C", "PACKAGE / TCTL"),
                panel("MEMORY", memory_used, "GiB", f"OF {memory_total:.0f} GiB", scale=memory_total, decimals=1),
                traffic_panel("NETWORK", rx, tx),
                traffic_panel("DISK I/O", disk_read, disk_write),
                panel("GPU", gpu, "%", "ENGINE ACTIVITY" if gpu_detail != "unavailable" else "UNAVAILABLE", decimals=1)]


class Device:
    def __init__(self, port: str, token: str | None = None):
        self.socket = self.serial = None
        self.token = token
        self.buffer = b""
        if port.startswith("tcp://"):
            endpoint = urlsplit(port)
            if (not endpoint.hostname or endpoint.username or endpoint.password or
                    endpoint.path or endpoint.query or endpoint.fragment):
                raise ValueError("Use tcp://hostname:port for a network endpoint")
            if not isinstance(token, str) or len(token) != 64 or any(c not in "0123456789abcdefABCDEF" for c in token):
                raise ValueError("Network connections need a pairing token; run configure-wifi over USB first")
            self.socket = socket.create_connection((endpoint.hostname, endpoint.port or 32100), timeout=3)
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        else:
            import serial
            self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2,
                                        write_timeout=2, exclusive=True)
            self.serial.dtr = self.serial.rts = False
            self.serial.port = port
            self.serial.open()

    def close(self):
        (self.socket if self.socket is not None else self.serial).close()

    def send(self, message: dict):
        raw = json.dumps(message, separators=(",", ":"), allow_nan=False).encode() + b"\n"
        if len(raw) > 1536:
            raise ValueError("Protocol message exceeds the firmware line buffer")
        if self.socket is not None:
            self.socket.settimeout(2)
            self.socket.sendall(raw)
        else:
            self.serial.write(raw)

    def receive(self, expected: str, timeout: float = 3, seq: int | None = None,
                ignore_errors: bool = False) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                try:
                    message = json.loads(line)
                except (ValueError, UnicodeError):
                    continue  # ESP32 boot diagnostics aren't protocol messages.
                if not isinstance(message, dict):
                    continue
                if message.get("type") == "error":
                    if ignore_errors:
                        continue
                    raise RuntimeError(f"Firmware rejected packet: {message}")
                if message.get("type") == expected and (seq is None or message.get("seq") == seq):
                    return message
            if len(self.buffer) > 8192:
                self.buffer = b""  # Recover from a partial boot log or corrupt line.
            if self.socket is not None:
                self.socket.settimeout(max(0.001, min(0.2, deadline-time.monotonic())))
                try:
                    chunk = self.socket.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    raise ConnectionError("The clock closed the network connection")
            else:
                chunk = self.serial.read(max(1, min(self.serial.in_waiting, 4096)))
            self.buffer += chunk
        raise TimeoutError(f"No {expected} response from the clock")

    def handshake(self) -> dict:
        # Allow for a power-on reset or UART open resetting some USB adapters.
        nonce = (time.monotonic_ns() & 0xFFFFFFFF) or 1
        for _ in range(4):
            request = {"type": "hello", "seq": nonce}
            if self.socket is not None:
                request["auth"] = self.token
            self.send(request)
            try:
                info = self.receive("hello", timeout=1.5, seq=nonce, ignore_errors=self.socket is None)
            except TimeoutError:
                continue
            if info.get("firmware") != "ipspro-monitor-1" or info.get("protocol") != 1 or info.get("screens") != 6:
                raise RuntimeError("The attached device is not compatible monitor firmware")
            return info
        raise TimeoutError("Monitor firmware did not identify itself; check the connection and firmware")


def load_config(path: Path) -> dict:
    if not path.exists():
        return {}
    result = json.loads(path.read_text())
    if not isinstance(result, dict):
        raise ValueError("Monitor config must be a JSON object")
    return result


def save_config(path: Path, config: dict):
    # Atomic and private, including while the file is being created.
    fd, temporary = tempfile.mkstemp(prefix=".elekstube-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as out:
            json.dump(config, out, indent=2)
            out.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def configure_wifi(device: Device, path: Path, config: dict, ssid: str | None):
    import getpass
    import secrets
    token = config.get("token") or secrets.token_hex(32)
    request = {"type": "wifi", "seq": 1, "token": token}
    if ssid is not None:
        request.update(ssid=ssid, password=os.environ.get("ELEKSTUBE_WIFI_PASSWORD"))
        if request["password"] is None:
            request["password"] = getpass.getpass("Wi-Fi password (empty for an open network): ")
    device.send(request)
    device.receive("wifi", seq=1)
    # Preserve the newly paired token even if Wi-Fi association fails afterward.
    endpoint = "tcp://elekstube-monitor.local:32100"
    config.update(endpoint=endpoint, token=token)
    save_config(path, config)
    deadline = time.monotonic()+35
    while time.monotonic() < deadline:
        info = device.handshake()
        wifi = info.get("wifi", {})
        if wifi.get("connected"):
            ip_endpoint = f"tcp://{wifi['ip']}:32100"
            for candidate in dict.fromkeys((endpoint, ip_endpoint)):
                network = None
                try:
                    network = Device(candidate, token)
                    network.handshake()
                except (OSError, TimeoutError, RuntimeError):
                    continue
                else:
                    config["endpoint"] = candidate
                    save_config(path, config)
                    LOG.info("Wi-Fi paired at %s (IP %s); saved private config", candidate, wifi["ip"])
                    return
                finally:
                    if network:
                        network.close()
        time.sleep(1)
    raise TimeoutError("Clock did not join Wi-Fi; USB still works. Retry configure-wifi --ssid YOUR_NETWORK if needed")


def acquire_lock(port: str):
    # An advisory lock shared by manual invocations and the user service.
    import hashlib
    lock_dir = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
    identity = port if port.startswith("tcp://") else os.path.realpath(port)
    digest = hashlib.sha256(identity.encode()).hexdigest()[:16]
    path = lock_dir / f"elekstube-{os.getuid()}-{digest}.lock"
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise RuntimeError("Another monitor process is already using this clock") from None
    return fd


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("sample", "probe", "run", "configure-wifi"))
    parser.add_argument("--port", help="USB device or tcp://hostname:32100; defaults to saved config, then USB")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="Private local connection config")
    parser.add_argument("--ssid", help="configure-wifi only: change network (otherwise reuse the clock's saved Wi-Fi)")
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--samples", type=int, default=0, help="Stop after N acknowledged updates (0 = forever)")
    args = parser.parse_args()
    if not 0.25 <= args.interval <= 3 or args.samples < 0:
        parser.error("interval must be 0.25–3 seconds and samples must be nonnegative")
    if args.ssid is not None and args.command != "configure-wifi":
        parser.error("--ssid is only used with configure-wifi")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    sampler = Sampler()
    if args.command == "sample":
        sampler.sample()
        time.sleep(args.interval)
        print(json.dumps(metrics_message(1, sampler.sample()), indent=2))
        return 0
    import serial
    config = load_config(args.config)
    endpoint = args.port or (DEFAULT_PORT if args.command == "configure-wifi" else config.get("endpoint", DEFAULT_PORT))
    if args.command == "configure-wifi" and endpoint.startswith("tcp://"):
        parser.error("configure-wifi requires a USB port")
    lock_fd = acquire_lock(endpoint)
    stopped = False

    def stop(_signum, _frame):
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    device = None
    seq = 0
    acknowledged = 0
    retries = 0
    try:
        while not stopped:
            try:
                if device is None:
                    device = Device(endpoint, config.get("token"))
                    info = device.handshake()
                    LOG.info("Connected via %s: %s", endpoint, info)
                    if args.command == "configure-wifi":
                        configure_wifi(device, args.config, config, args.ssid)
                        return 0
                    if args.command == "probe":
                        print(json.dumps(info, indent=2))
                        return 0
                    sampler.sample()
                    time.sleep(args.interval)
                started = time.monotonic()
                seq += 1
                device.send(metrics_message(seq, sampler.sample()))
                ack = device.receive("ack", seq=seq)
                acknowledged += 1
                retries = 0
                if seq == 1 or seq % 60 == 0 or args.samples:
                    LOG.info("Update %s acknowledged; device frames=%s", seq, ack.get("frames"))
                if args.samples and acknowledged >= args.samples:
                    return 0
                time.sleep(max(0, args.interval - (time.monotonic() - started)))
            except (OSError, serial.SerialException, TimeoutError, RuntimeError) as error:
                LOG.warning("%s; reconnecting in 3 seconds", error)
                if device:
                    device.close()
                    device = None
                retries += 1
                if args.command in ("probe", "configure-wifi") or (args.samples and retries >= 3):
                    return 1
                time.sleep(3)
    finally:
        if device:
            device.close()
        os.close(lock_fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

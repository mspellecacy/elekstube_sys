import os
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from monitor import Device, Sampler, drm_usage, load_config, save_config, metrics_message, panel, parse_drm_clients, rate_panel, traffic_panel


class TelemetryTests(unittest.TestCase):
    def test_tcp_pairing_framing_sequence_errors_and_disconnect(self):
        token = "ab" * 32
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(3)
        requests = []

        def firmware():
            connection, _ = listener.accept()
            with connection, connection.makefile("rb") as stream:
                request = json.loads(stream.readline())
                requests.append(request)
                hello = json.dumps({"type": "hello", "seq": request["seq"], "protocol": 1,
                                    "firmware": "ipspro-monitor-1", "screens": 6}).encode()+b"\n"
                connection.sendall(hello[:7])
                time.sleep(0.02)
                connection.sendall(hello[7:])
                requests.append(json.loads(stream.readline()))
                connection.sendall(b'{"type":"ack","seq":0}\n{"type":"ack","seq":7}\n')
                requests.append(json.loads(stream.readline()))
                connection.sendall(b'{"type":"error","error":"usb-only"}\n')

        thread = threading.Thread(target=firmware, daemon=True)
        thread.start()
        device = Device(f"tcp://127.0.0.1:{listener.getsockname()[1]}", token)
        try:
            self.assertEqual(device.handshake()["screens"], 6)
            device.send({"type": "metrics", "seq": 7})
            self.assertEqual(device.receive("ack", seq=7)["seq"], 7)
            device.send({"type": "capture", "screen": 0})
            with self.assertRaisesRegex(RuntimeError, "usb-only"):
                device.receive("capture")
            with self.assertRaises(ConnectionError):
                device.receive("ack")
            self.assertEqual(requests[0]["auth"], token)
            self.assertNotIn("auth", requests[1])
        finally:
            device.close()
            listener.close()
            thread.join(timeout=3)

    def test_tcp_requires_token_and_valid_endpoint_before_connecting(self):
        with self.assertRaisesRegex(ValueError, "pairing token"):
            Device("tcp://127.0.0.1:32100")
        for endpoint in ("tcp://", "tcp://user:pass@host:32100", "tcp://host/path"):
            with self.assertRaises(ValueError):
                Device(endpoint, "ab"*32)

    def test_private_config_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/"config.json"
            self.assertEqual(load_config(path), {})
            config = {"endpoint": "tcp://clock.local:32100", "token": "ab"*32}
            save_config(path, config)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(load_config(path), config)
            path.chmod(0o644)
            save_config(path, {**config, "endpoint": "tcp://192.0.2.140:32100"})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(len(list(Path(temp).iterdir())), 1)

    def test_clock_uses_local_time_and_preserves_subseconds_at_midnight(self):
        for hour, minute, second, expected in ((23, 59, 59, 86399875), (0, 0, 0, 875),
                                               (8, 5, 9, 29109875)):
            local = time.struct_time((2026, 11, 2, hour, minute, second, 0, 306, 0))
            with patch("monitor.time.localtime", return_value=local) as localtime:
                packet = metrics_message(7, [], timestamp=123456.875)
            localtime.assert_called_once_with(123456.875)
            self.assertEqual(packet["clock_ms"], expected)
            self.assertEqual(packet["seq"], 7)

    def test_xe_cycles_and_duplicate_clients(self):
        with tempfile.TemporaryDirectory() as temp:
            proc = Path(temp)
            for pid in (10, 20):
                fdinfo = proc / str(pid) / "fdinfo"
                fdinfo.mkdir(parents=True)
                (proc / str(pid) / "fd").mkdir()
                os.symlink("/dev/dri/renderD128", proc / str(pid) / "fd/3")
                (fdinfo / "3").write_text("drm-pdev: 0000:0b:00.0\ndrm-client-id: 42\n"
                                         "drm-cycles-rcs: 150\ndrm-total-cycles-rcs: 1100\n")
            current = parse_drm_clients(proc)
            self.assertEqual(len(current), 1)
            previous = {next(iter(current)): {"drm-cycles-rcs": 100, "drm-total-cycles-rcs": 1000}}
            self.assertEqual(drm_usage(previous, current, 1), 50)
            self.assertIsNone(drm_usage({}, current, 1))

    def test_multiple_gpu_clients_and_engine_capacity(self):
        previous = {("a", "1"): {"drm-engine-render": 0}, ("a", "2"): {"drm-engine-render": 0}}
        current = {("a", "1"): {"drm-engine-render": 500_000_000},
                   ("a", "2"): {"drm-engine-render": 250_000_000}}
        self.assertEqual(drm_usage(previous, current, 1), 75)
        current[("a", "1")]["drm-engine-capacity-render"] = 2
        current[("a", "2")]["drm-engine-capacity-render"] = 2
        self.assertEqual(drm_usage(previous, current, 1), 37.5)

    def test_cpu_guest_not_double_counted(self):
        with tempfile.TemporaryDirectory() as temp:
            proc = Path(temp)
            sampler = Sampler(proc=proc)
            (proc / "stat").write_text("cpu 100 0 0 100 0 0 0 0 50 0\n")
            self.assertIsNone(sampler.cpu())
            (proc / "stat").write_text("cpu 150 0 0 150 0 0 0 0 100 0\n")
            self.assertEqual(sampler.cpu(), 50)

    def test_network_excludes_virtual_and_handles_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "proc/net").mkdir(parents=True)
            (root / "sys/class/net/eth0/device").mkdir(parents=True)
            sampler = Sampler(root / "proc", root / "sys")

            def write(rx, tx):
                (root / "proc/net/dev").write_text("header\nheader\n"
                    f"eth0: {rx} 0 0 0 0 0 0 0 {tx} 0 0 0 0 0 0 0\n"
                    "veth0: 999999 0 0 0 0 0 0 0 999999 0 0 0 0 0 0 0\n")

            write(100, 50)
            self.assertEqual(sampler.network(1), (None, None))
            sampler.previous_time = 1
            write(300, 150)
            self.assertEqual(sampler.network(2), (100, 50))
            write(1, 1)
            self.assertEqual(sampler.network(1), (0, 0))

    def test_missing_sensor_and_units(self):
        self.assertEqual(panel("GPU", None, "%")["value"], "--")
        self.assertEqual(panel("CPU", float("nan"), "%")["percent"], -1)
        self.assertEqual(rate_panel("NET UP", 2 * 1024**2)["unit"], "MiB/s")
        self.assertEqual(rate_panel("NET UP", 0)["percent"], 0)

    def test_disk_rates_exclude_partitions_and_virtual_devices(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            disk = root / "block/nvme0n1"
            (disk / "device").mkdir(parents=True)
            (root / "block/dm-0").mkdir()
            (root / "block/dm-0/stat").write_text("0 0 99999 0 0 0 99999 0 0 0 0")
            (disk / "nvme0n1p1").mkdir()
            (disk / "nvme0n1p1/stat").write_text("0 0 99999 0 0 0 99999 0 0 0 0")
            sampler = Sampler(sys=root)
            (disk / "stat").write_text("0 0 100 0 0 0 200 0 0 0 0")
            self.assertEqual(sampler.disk(1), (None, None))
            sampler.previous_time = 1
            (disk / "stat").write_text("0 0 104 0 0 0 208 0 0 0 0")
            self.assertEqual(sampler.disk(2), (1024, 2048))
            (disk / "stat").write_text("0 0 1 0 0 0 1 0 0 0 0")
            self.assertEqual(sampler.disk(1), (0, 0))

    def test_traffic_panel_preserves_both_directions(self):
        result = traffic_panel("NETWORK", 1024, 2 * 1024**2)
        self.assertEqual((result["value"], result["unit"]), ("1.0", "KiB/s"))
        self.assertEqual((result["secondary_value"], result["secondary_unit"]), ("2.0", "MiB/s"))
        self.assertGreater(result["secondary_percent"], result["percent"])
        self.assertEqual(traffic_panel("DISK I/O", None, None)["secondary_value"], "--")

    def test_serial_handshake_and_sequence_matching(self):
        import json
        import pty
        master, slave = pty.openpty()
        device = Device(os.ttyname(slave))

        def firmware():
            buffer = b""
            for _ in range(2):
                while b"\n" not in buffer:
                    buffer += os.read(master, 4096)
                raw, buffer = buffer.split(b"\n", 1)
                message = json.loads(raw)
                if message["type"] == "hello":
                    os.write(master, b'boot diagnostics\n{"type":"error","error":"invalid-json"}\n'
                             b'{"type":"hello","protocol":1,"seq":0,'
                             b'"firmware":"ipspro-monitor-1","screens":6}\n')
                    reply = {"type": "hello", "protocol": 1, "seq": message["seq"],
                             "firmware": "ipspro-monitor-1", "screens": 6}
                    os.write(master, json.dumps(reply).encode() + b'\n')
                else:
                    os.write(master, b'{"type":"ack","seq":0}\n{"type":"ack","seq":7}\n')

        thread = threading.Thread(target=firmware, daemon=True)
        thread.start()
        try:
            self.assertEqual(device.handshake()["screens"], 6)
            device.send({"type": "metrics", "seq": 7})
            self.assertEqual(device.receive("ack", seq=7)["seq"], 7)
        finally:
            device.close()
            os.close(slave)
            os.close(master)
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()

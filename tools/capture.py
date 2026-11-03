#!/usr/bin/env python3
"""Save the exact six firmware framebuffers as a contact sheet.

Stop elekstube-monitor.service first. Optionally seed with an array of sixty
previously collected panel arrays. The service should be restarted afterward.
"""
import argparse
import json
from pathlib import Path
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from monitor import DEFAULT_PORT, Device, Sampler, acquire_lock, metrics_message


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--history", type=Path)
    parser.add_argument("--mode", choices=("monitor", "clock", "flip", "orbit"), default="monitor")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output is None:
        args.output = Path({"monitor":"docs/monitor-preview.png", "clock":"docs/clock-preview.png",
                            "flip":"docs/flip-clock-capture.png",
                            "orbit":"docs/orbit-clock-capture.png"}[args.mode])
    lock = acquire_lock(args.port)
    device = Device(args.port)
    try:
        print("Firmware:", device.handshake(), flush=True)
        device.send({"type": "mode", "mode": args.mode, "seq": 1})
        device.receive("mode", seq=1)
        sampler = Sampler()
        sampler.sample()
        time.sleep(1)
        samples = json.loads(args.history.read_text()) if args.history else [sampler.sample()]
        seq = 0
        # Re-sync to one instant for every digit: the UART takes ~6s per screen.
        capture_time = int(time.time()) if args.mode != "monitor" else None

        def update(panels):
            nonlocal seq
            seq += 1
            device.send(metrics_message(seq, panels, timestamp=capture_time))
            device.receive("ack", seq=seq)

        for panels in samples[-60:]:
            update(panels)
        image = Image.new("RGB", (940, 280), (4, 6, 9))
        draw = ImageDraw.Draw(image)
        for screen in range(6):
            update(sampler.sample())
            if args.mode == "clock":
                time.sleep(0.3)  # Let the simulated cathode finish igniting.
            elif args.mode == "flip":
                time.sleep(0.6)  # Let the moving card land and settle.
            elif args.mode == "orbit":
                time.sleep(0.6)  # Let the incoming light sweep reveal the digit.
            device.send({"type": "capture", "screen": screen})
            info = device.receive("capture")
            assert info["bytes"] == 64800 and info["format"] == "rgb565be"
            raw, device.buffer = device.buffer, b""
            deadline = time.monotonic() + 15
            while len(raw) < 64800 and time.monotonic() < deadline:
                raw += device.serial.read(min(4096, 64800 - len(raw)))
            if len(raw) != 64800:
                raise RuntimeError(f"Incomplete capture: {len(raw)} bytes")
            pixels = []
            for i in range(0, len(raw), 2):
                value = (raw[i] << 8) | raw[i+1]
                pixels.append(((value >> 11) * 255 // 31, ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31))
            panel = Image.new("RGB", (135, 240))
            panel.putdata(pixels)
            x = 20 + screen * 153
            draw.rounded_rectangle((x-3, 17, x+137, 262), radius=11,
                                   fill=(10, 14, 19), outline=(37, 45, 52), width=1)
            image.paste(panel, (x, 20))
            print("Captured screen", screen + 1, flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        image.resize((1880, 560), Image.Resampling.LANCZOS).save(args.output)
        print("After capture:", device.handshake(), flush=True)
        print("Saved", args.output, flush=True)
    finally:
        device.close()
        import os
        os.close(lock)


if __name__ == "__main__":
    main()

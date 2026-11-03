#!/usr/bin/env python3
"""Measure complete six-tube Orbit FPS while sending real telemetry once a second.

Stop elekstube-monitor.service first; the selected view is restored afterward.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from monitor import DEFAULT_CONFIG, DEFAULT_PORT, Device, Sampler, acquire_lock, load_config, metrics_message


def benchmark(device, seconds):
    original = device.handshake()
    sampler = Sampler()
    sampler.sample()
    device.send({'type': 'mode', 'mode': 'orbit', 'seq': 1})
    device.receive('mode', seq=1)
    latencies, states = [], []
    try:
        before = device.handshake()
        started = time.monotonic()
        for n in range(seconds):
            time.sleep(max(0, started+n-time.monotonic()))
            packet = metrics_message(n+2, sampler.sample())
            sent = time.monotonic()
            device.send(packet)
            device.receive('ack', seq=n+2)
            latencies.append(time.monotonic()-sent)
            states.append(device.handshake())
        # Include the final second so counter deltas span the requested duration.
        time.sleep(max(0, started+seconds-time.monotonic()))
        after = device.handshake()
        elapsed = time.monotonic()-started
        return {
            'seconds': round(elapsed, 3),
            'complete_six_tube_fps': round((after['orbit_frames']-before['orbit_frames'])/elapsed, 2),
            'acknowledged_updates': len(states),
            'maximum_ack_seconds': round(max(latencies), 3),
            'median_render_us_per_band': statistics.median(s['render_us'] for s in states),
            'median_transfer_elapsed_us': statistics.median(s['spi_us'] for s in states),
            'bands_per_tube': after.get('orbit_bands', 1),
            'minimum_free_heap': min(s['free_heap'] for s in states),
            'maximum_free_heap': max(s['free_heap'] for s in states),
            'dma': after.get('orbit_dma', False),
            'glyph_caches': after['orbit_cached'],
        }
    finally:
        device.send({'type': 'mode', 'mode': original['mode'], 'seq': 0xFFFFFFFE})
        device.receive('mode', seq=0xFFFFFFFE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--port', help='Override the saved USB/TCP endpoint')
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--output', type=Path, help='Also save the JSON report')
    args = parser.parse_args()
    if not 2 <= args.seconds <= 300:
        parser.error('seconds must be 2–300')
    config = load_config(args.config)
    endpoint = args.port or config.get('endpoint', DEFAULT_PORT)
    lock = acquire_lock(endpoint)
    device = None
    try:
        device = Device(endpoint, config.get('token'))
        report = benchmark(device, args.seconds)
        raw = json.dumps(report, indent=2)+'\n'
        if args.output:
            args.output.write_text(raw)
        print(raw, end='')
    finally:
        if device:
            device.close()
        os.close(lock)


if __name__ == '__main__':
    main()

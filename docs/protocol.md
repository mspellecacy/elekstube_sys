# USB and TCP protocol

Both transports use newline-delimited JSON, with a maximum of **1535 bytes before
the newline**. USB runs at **115200 baud**. TCP listens on **32100**, advertised as
`elekstube-monitor.local`, and accepts one client. Each transport has an independent
input buffer. The host CLI handles framing, pairing and sequence matching.

## Handshake and authentication

USB handshake:

```json
{"type":"hello","seq":123}
```

The response has `type: "hello"`, `protocol: 1`, `firmware: "ipspro-monitor-1"`,
`screens: 6` and the echoed `seq`. Match your own sequence rather than accepting
the unsolicited boot message.

The first TCP message must authenticate:

```json
{"type":"hello","seq":123,"auth":"<64-character hex pairing token>"}
```

Subsequent messages use that authenticated connection. Authenticated clients
expire after ten seconds without input; unauthenticated clients expire after two.
TCP traffic is not encrypted and is intended for a trusted LAN.

## Pair over USB

Prefer `monitor.py configure-wifi`. Its USB-only request is:

```json
{"type":"wifi","seq":1,"token":"<64-character hex pairing token>"}
```

Optional `ssid` and `password` fields select another network; otherwise the device
rejoins its saved network. Settings use a separate `ipsmonitor` NVS namespace,
preserving factory settings. Replies never echo credentials. The host saves its
endpoint/token atomically in a mode-0600 `config.json`.

## Telemetry

Send `type: "metrics"`, a numeric `seq`, and a `panels` array containing exactly
six panel objects. Each requires string `label`, `value`, `unit` and `detail`
fields and numeric `percent` (0–100, or -1 for unavailable). Network and disk panels
can also have `secondary_value`, `secondary_unit` and `secondary_percent`.

Optional integer `clock_ms` is host-local milliseconds since midnight,
0–86,399,999. Invalid packets are rejected before changing display state or
resetting the offline timer. A successful update replies:

```json
{"type":"ack","seq":1,"frames":1}
```

Time is independent of the selected view. Clients omitting `clock_ms` can still
update stats, but do not initially synchronize the clock.

## Select a view

```json
{"type":"mode","mode":"orbit","seq":1}
```

Modes are `monitor`, `clock` (Nixie), `flip` and `orbit`. The reply echoes `type`,
`mode` and `seq`. Sending telemetry preserves the selected view.

## Status and performance

`hello` also reports:

| Fields | Meaning |
| --- | --- |
| `mode`, `clock_style`, `clock_styles` | Current view and supported faces |
| `clock_synced`, `clock` | Synchronization state and current six-digit time |
| `frames`, `usb_frames`, `tcp_frames` | Accepted telemetry counts by transport and in total |
| `online`, `free_heap` | Whether telemetry is current and available heap bytes |
| `animation_frames`, `orbit_frames` | Completed six-tube Nixie/Orbit frames |
| `flip_updates` | Individual moving-card updates |
| `animation_cached`, `orbit_cached` | Whether the selected face's glyph caches are available |
| `orbit_dma`, `orbit_bands` | Overlapping Orbit transfer state and bands per tube |
| `animation_us` | Active work and waits across a six-tube frame, or one flip-card update |
| `render_us`, `spi_us` | Last render/transfer timings; DMA reports individual bands |
| `button_a_presses` | Debounced A-button presses |
| `wifi` | Connection state, IP, hostname, port and signal strength |

DMA `spi_us` is a transfer's elapsed lifetime and can include polling delays.
Measure completed `orbit_frames` over wall-clock time for actual six-tube FPS;
`tools/benchmark_orbit.py` does this while sending real telemetry.

## Pixel capture over USB

```json
{"type":"capture","screen":0}
```

Screens are zero-based, 0–5. A JSON `capture` header precedes exactly **64,800
bytes** of raw 135 × 240 RGB565 pixels in **big-endian** order. Capture is USB-only
and occupies the connection for several seconds. The firmware pauses its offline
timer during capture to retain graph history. Stop the monitor service first;
the supplied capture tool assembles the six panels.

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

## Button controls and timing tools

USB and authenticated TCP clients can invoke the same actions as physical buttons:

```json
{"type":"control","button":"B","seq":2}
```

The reply echoes `type: "control"`, `button` and `seq`. Only single uppercase
`A`, `B`, `C` or `D` values are accepted; invalid values return `invalid-control`.
Remote controls do not increment physical button press counters.

B cycles selections 0–6: normal clock, stopwatch, then 5/10/25/45/60-minute
countdowns. From the monitor it opens the last selected clock face. C performs
start → stop → reset on the selected timing tool and has no effect on normal time.
A cycles display faces/monitor; D blanks/unblanks. Changing the view with `mode`
preserves the selected tool and all running sessions.

Each stopwatch/timer runs independently of host time, connection status, display
blanking and the other sessions. Completion holds zero until reset; timing state
is transient and resets on device restart.

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
| `button_a_presses`, `button_b_presses`, `button_c_presses` | Debounced physical button presses |
| `screen_enabled` | Whether the displays are unblanked |
| `timing` | Selected tool and its state, described below |
| `wifi` | Connection state, IP, hostname, port and signal strength |

Example timing status:

```json
{"tool":"timer","selection":2,"state":"running","value_ms":298500,"duration_seconds":300,"digits":"000459"}
```

`tool` is `clock`, `stopwatch` or `timer`. `state` is `ready`, `running`, `stopped`
or `finished`; normal clock mode reports `clock`. `value_ms` means elapsed time
for the stopwatch and remaining time for a timer; normal clock mode reports zero.
`duration_seconds` is the timer's preset and otherwise zero. `digits` is the
selected tool's six-digit display, or `------` for an unsynchronized normal clock.
The existing top-level `clock` field always reports wall-clock time.

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


## NTP configuration

USB or an authenticated TCP connection accepts:

```json
{"type":"ntp","seq":1,"timezone":"EST5EDT,M3.2.0,M11.1.0","server":"pool.ntp.org"}
```

Both fields are required. The timezone is a printable POSIX TZ rule (1–95
characters, no spaces); the server is a hostname or IPv4 address (1–127
letters, digits, dots or hyphens). Use valid POSIX rules; the device does not
validate their grammar. Settings are saved together in NVS and applied without
rebooting. Success replies with `{"type":"ntp","seq":1,"saved":true}`;
invalid settings or failed storage return `invalid-or-unsaved-ntp-settings`.

NTP starts automatically on Wi-Fi with defaults `UTC0` and `pool.ntp.org`.
The hello response adds `ntp_synced`: true after the first successful NTP
synchronization during this boot (it does not indicate current connectivity).
NTP takes precedence over `clock_ms` after synchronization. The UTC system
clock keeps running offline and local time conversion applies DST rules.
Before synchronization, telemetry still supplies local time as before.

For diagnostics, send `{"type":"ntp-status","seq":1}`. The response reports
`enabled`, `synced`, UTC `epoch`, configured `server`, resolved `server_ip`,
`gateway`, `dns`, and `uptime_ms`. TCP requires the usual authentication.
USB adapters may reset the device when opening the port; allow several seconds
for Wi-Fi and NTP before interpreting an unsynchronized boot response.

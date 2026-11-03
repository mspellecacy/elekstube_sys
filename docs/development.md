# Architecture and development

## Project map

| Location | Purpose |
| --- | --- |
| `monitor.py` | Linux metrics, USB/TCP transport and pairing CLI |
| `src/main.cpp` | Display composition, protocol, buttons and animation scheduling |
| `src/clock_state.h` | Local clock, mode cycle and debouncing |
| `src/timing_tools.h` | Monotonic stopwatch, independent countdown presets and tool cycle |
| `src/network_link.h` | Wi-Fi, mDNS, pairing and authenticated TCP client |
| `src/*_renderer.h` | Shared native/embedded rendering code |
| `src/*_assets.h`, `src/monitor_fonts.h` | Generated artwork and font data |
| `tools/` | Artwork, previews, capture, benchmark and service installer |
| `tests/` | Python host tests and native C++ checks |
| `systemd/` | User-service template filled by the installer |

The Linux sender collects six panels and sends one compact JSON packet per
second. The firmware validates the complete packet before updating state.
Independent bounded USB/TCP line buffers prevent partial input on one transport
from interfering with the other. Telemetry is never stored in flash.

Clock time is host-local milliseconds since midnight, refreshed with telemetry.
The device's millisecond timer keeps it ticking between updates and while the
host is offline. Clock faces render locally; no animation frames travel over
USB or Wi-Fi.

Stopwatch and countdown sessions accumulate unsigned `millis()` deltas separately
from wall-clock time, handling its rollover and ignoring host time corrections.
Each session survives tool/view switches and display blanking in RAM. C cycles
ready → running → stopped → ready; completion holds zero until reset. Durations
never wrap at midnight: the stopwatch saturates at 99:59:59.999, and countdowns
round upward to whole seconds for display. The native timing tests cover these
transitions, concurrent background sessions, late ticks, and counter rollover.

Timing labels occupy static strips above and below every face's animated region.
They redraw on selection, state changes, and countdown completion; per-frame
rendering and the existing animation schedules are unchanged.

## Run checks and build

```sh
.venv/bin/pip install -r requirements-build.txt
.venv/bin/python -m unittest discover -s tests -v
.venv/bin/pio run
```

Native tests compile with `g++` and exercise the actual firmware headers.
Renderer checks use AddressSanitizer and UndefinedBehaviorSanitizer, covering
all ten digits, transition phases, cache release/re-entry, byte order and output
bounds. Orbit additionally checks 420 reference frames and compares each DMA
band with the corresponding full-frame pixels.

GitHub Actions runs the same test/build commands. Its downloadable firmware
artifact is **application-only** and still requires the hardware/partition checks
in the [installation guide](installation.md).

## Metrics

- **CPU usage:** deltas from `/proc/stat`, excluding double-counted guest time.
- **CPU temperature:** Linux hwmon package/Tctl temperature, with support for
  k10temp, coretemp and zenpower sensors.
- **Memory:** `MemTotal - MemAvailable`, displayed in GiB with a percentage ring.
- **Network:** receive/send deltas for physical interfaces; excludes loopback,
  bridges and VPNs.
- **Disk:** read/write deltas for whole physical block devices; excludes partitions
  and virtual layers. Linux sector counters use fixed 512-byte units. Cached
  filesystem reads do not appear as physical disk I/O.
- **GPU:** AMD busy percentage, Intel Xe busy/total cycle deltas, DRM engine-time
  deltas, or NVIDIA `nvidia-smi`. DRM clients are deduplicated and engine capacity
  is accounted for. The busiest engine class is shown; this is engine activity,
  not shader occupancy. Permissions can limit which clients are visible.

New DRM clients are discovered every five seconds and require two samples for a
delta. See the kernel's [Xe accounting documentation](https://kernel.org/doc/html/v6.16/gpu/xe/xe-drm-usage-stats.html).
Unavailable readings appear as `--`. Traffic uses KiB/s, MiB/s or GiB/s and a
logarithmic graph scale up to 1 GiB/s. Each graph retains sixty samples, normally
sixty seconds at the default interval.

## Animation and performance

All faces share one 135 × 240 RGB565 sprite. Nixie and Orbit allocate glyph caches
only while selected and release them when leaving the face, preserving memory
for Wi-Fi. They have flash fallbacks if allocation fails. Renderers allocate no
memory per frame.

Nixie composes its 135 × 202 glow region from packed distance fields and a live
lighting table. Flip updates only changing 123 × 190 cards, with a final settled
frame. Orbit composes a 135 × 192 animation region as **two 135 × 96 bands**:
one half of the existing sprite is transferred by DMA while the CPU draws into
the other. Full redraws, mode changes, blanking and captures drain DMA first.
Small RAM tables cache orbital geometry, glow emission and background palettes.

| View | Scheduling |
| --- | --- |
| Monitor | Redraw on telemetry arrival, normally once per second |
| Nixie | Minimum 12 ms between tube updates; rendering usually takes longer |
| Flip | Minimum 16 ms between moving-card updates; skip settled cards |
| Orbit | No frame-rate cap; one scheduler yield per complete six-tube frame |

At **240 MHz CPU / 40 MHz SPI**, Orbit measured approximately **13.8–14 FPS**
across all six tubes with one-second Wi-Fi telemetry, up from 7 FPS before
DMA and rendering optimizations. The pixel payload alone has a 40 MHz upper
bound of `40,000,000 / (135 × 192 × 16 × 6) = 16.08 FPS`, before commands and
processing. These are completed six-tube frames, not individual card updates
or the LCD's internal refresh rate.

An experimental 80 MHz SPI build reached 14.22 FPS but failed a Nixie cache check
during view switching; its cache failure's cause was not established. The 40 MHz
build was retained and rechecked across all views at 13.95 FPS. Physical screen
integrity at 80 MHz and chip temperature were not measured.

To benchmark, stop the service first. The tool sends real metrics and restores
the previous view afterward:

```sh
systemctl --user stop elekstube-monitor.service
.venv/bin/python tools/benchmark_orbit.py --seconds 30
systemctl --user start elekstube-monitor.service
```

## Artwork and previews

Generated headers are included in Git. Regeneration needs Pillow and DejaVu
fonts at the default Linux font paths (Debian/Ubuntu package `fonts-dejavu-core`):

```sh
.venv/bin/pip install -r requirements-artwork.txt
.venv/bin/python tools/generate_fonts.py
.venv/bin/python tools/generate_nixie.py
.venv/bin/python tools/generate_flip.py
.venv/bin/python tools/generate_orbit.py
.venv/bin/python tools/preview_nixie.py
.venv/bin/python tools/preview_flip.py
.venv/bin/python tools/preview_orbit.py
```

Preview tools compile the actual C++ renderers and produce the still images and
GIFs in `docs/`. Nixie and Orbit use original numeral geometry; flip numerals and
monitor fonts derive from DejaVu Sans. Retain `FONT-LICENSE.txt` when redistributing
those assets. Rebuild/reinstall firmware after regenerating assets.

Capture the device's rendered RGB565 pixels over USB:

```sh
systemctl --user stop elekstube-monitor.service
.venv/bin/python tools/capture.py --mode monitor
.venv/bin/python tools/capture.py --mode clock
.venv/bin/python tools/capture.py --mode flip
.venv/bin/python tools/capture.py --mode orbit
systemctl --user start elekstube-monitor.service
```

Each six-screen capture takes tens of seconds at 115200 baud. The tool resets
each clock digit to the same instant so a slow transfer does not mix times in the
contact sheet. Capture is an application framebuffer readback; it cannot verify
the electrical integrity of pixels reaching the physical display.

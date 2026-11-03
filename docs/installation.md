# Backup and application-only installation

These instructions target the tested **ELEKSTUBE IPS PRO / Gen3 PCB 4.3.3**
with ESP32-PICO-D4 and 4 MiB flash. Confirm your own board and partition table
before writing anything. A different generation may use different wiring or
flash offsets.

## Prepare the tools

```sh
git clone https://github.com/mspellecacy/elekstube_sys.git
cd elekstube_sys
python3 -m venv .venv
.venv/bin/pip install -r requirements-build.txt
.venv/bin/pio run
```

Select your serial device. The tested CH340 adapter uses the following path;
change it if your adapter has a different name:

```sh
export ELEKSTUBE_PORT=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

Your Linux user needs permission to open that device. On distributions using
the `dialout` group, add your user to that group and log in again if necessary.
Close serial terminals and stop the monitor service if installed:

```sh
systemctl --user stop elekstube-monitor.service
```

## Back up your own device

A full flash backup contains Wi-Fi credentials, configuration and device identity.
Keep it private and keep a second copy somewhere safe. This repository excludes
the entire `backups/` directory and does not provide someone else's recovery image.

```sh
umask 077
mkdir -p backups
.venv/bin/python -m esptool --chip esp32 \
  --port "$ELEKSTUBE_PORT" --baud 460800 \
  read_flash 0x0 0x400000 backups/original.bin
sha256sum backups/original.bin > backups/SHA256SUMS
.venv/bin/python -m esptool --chip esp32 \
  --port "$ELEKSTUBE_PORT" --baud 460800 \
  verify_flash 0x0 backups/original.bin
```

The checksum detects later file changes; `verify_flash` checks the backup against
the device. Do not proceed if reading or verification fails.

## Verify the partition layout

Read the existing partition table and decode it using the tool installed by the
firmware build:

```sh
.venv/bin/python -m esptool --chip esp32 \
  --port "$ELEKSTUBE_PORT" --baud 460800 \
  read_flash 0x8000 0x1000 backups/device-partitions.bin
.venv/bin/python .platformio/packages/framework-arduinoespressif32/tools/gen_esp32part.py \
  backups/device-partitions.bin
```

The tested device has this layout, mirrored in the project's `partitions.csv`:

| Partition | Offset | Size |
| --- | --- | --- |
| nvs | `0x9000` | `0x5000` |
| otadata | `0xe000` | `0x2000` |
| app0 | `0x10000` | `0x2b0000` |
| spiffs | `0x2c0000` | `0x130000` |

If your layout differs, stop and adapt the build and installation procedure to
your hardware. Keep the manufacturer bootloader, partition table, settings and
image filesystem.

## Install the application

**Flash only `firmware.bin` at `0x10000`. Do not use `pio run -t upload`**:
PlatformIO's normal upload also replaces its configured bootloader and partition
table. Do not flash its generated bootloader/partition binaries or run erase-flash.

```sh
sha256sum -c backups/SHA256SUMS
.venv/bin/python -m esptool --chip esp32 \
  --port "$ELEKSTUBE_PORT" --baud 460800 \
  write_flash 0x10000 .pio/build/ipspro_monitor/firmware.bin
.venv/bin/python monitor.py probe --port "$ELEKSTUBE_PORT"
.venv/bin/python monitor.py run --port "$ELEKSTUBE_PORT"
```

The build checks the application size against the vendor partition's limit.
Generated artwork and fonts are embedded, so there is no filesystem image to
upload. Stop the foreground sender with Ctrl+C before pairing Wi-Fi or
installing the service; see the [README](../README.md).

## Restore the stock clock

Use the full backup from **this exact device**, with the same serial port:

```sh
systemctl --user stop elekstube-monitor.service
sha256sum -c backups/SHA256SUMS
.venv/bin/python -m esptool --chip esp32 \
  --port "$ELEKSTUBE_PORT" --baud 460800 \
  write_flash 0x0 backups/original.bin
```

This restores the original application, settings and image filesystem together.

## Wiring reference

| Signal | GPIO |
| --- | --- |
| Display MOSI / SCLK | 23 / 18 |
| Display DC / reset / backlight | 25 / 26 / 27 |
| 74HC595 data / clock / latch | 14 / 9 / 10 |
| RGB LEDs | 12 |
| Button A / D | 33 / 34 |

The six display-select bits run in reverse physical order. Display chip selects
are controlled through the shift register, including during DMA transfers.

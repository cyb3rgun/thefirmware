# CYB3RGUN thefirmware

Everything that runs on an ESP32 in the CYB3RGUN system, in one ESP-IDF
project with several build targets.

Start with `docs/concept.md`. The wire format to the target computer is
`docs/usb-protocol.md`. Why the tree looks the way it does is
`docs/decisions.md`. What is being built right now is `docs/seasons.md`.

## Layout

```
components/     shared code, one component per subject
  cgproto/      ESP-NOW packets, concept.md section 3
  cgusb/        COBS framing, CRC-16, messages of usb-protocol.md
  cgbeacon/     the four beacon clusters and the multiplex clock
  cgcam/        the PAJ7025R2 object camera
  cgoled/       the SSD1306 on the Heltec board
targets/        one directory per firmware, each holding a main component
  module/       beacons, hears pistols, acknowledges, forwards over USB
  pistolstub/   stands in for the pistol until the S3 board exists
  cambench/     reads the camera and prints what it sees
tools/          host side helpers, Python
test/host/      unit tests for the pure C parts, Unity under MinGW
docs/           concept, decisions, seasons, measurements, briefings, handovers
```

## Building

The target is chosen with `CGTARGET`. Each target keeps its own build
directory and its own generated `sdkconfig`.

```
idf.py -D CGTARGET=module -B build/module ^
       -D SDKCONFIG=sdkconfig.module ^
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.module" ^
       -p COM6 build flash monitor
```

`tools/cgflash.ps1` is the same command with the flags filled in:

```
tools\cgflash.ps1 module COM6 build flash monitor
tools\cgflash.ps1 pistolstub COM7 build flash monitor
tools\cgflash.ps1 cambench COM8 build flash monitor
```

Erasing wipes NVS, which means pairings, the stored channel and the zeroing
offset. It is needed when NVS or the partition layout changes, and it is
always announced before it runs, never after.

```
tools\cgflash.ps1 module COM6 erase-flash
```

## Checks before a commit

```
python tools/check_dashes.py
test\host\run.sh
```

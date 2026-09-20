# thefirmware decisions

Status: living record. One entry per decision, newest section at the bottom.
An entry is never rewritten; a later entry supersedes it and says so.

## D-001 ESP-IDF, no Arduino core

Decided in S01-B01. thefirmware is built with ESP-IDF. The Arduino core is
not used anywhere in the tree.

Installed on the build machine and read from `idf.py --version` on
20 September 2026:

```
ESP-IDF v5.5.2-dirty
```

The framework lives at `C:\Espressif\frameworks\esp-idf-v5.5.2`, the tools at
`C:\Users\sash710\.espressif`. The `-dirty` suffix is the state of the
founder's own checkout, not of thefirmware: the tree carries a modified
`components/mbedtls/mbedtls` submodule pointer and five stray files named
`#`, `New-Item`, `Rename-Item`, `cd` and `dir` in the framework root. They do
not affect a build. Installing or repairing ESP-IDF is the founder's hand
move, so the tree was left untouched.

## D-002 One project, several targets

Decided in S01-B01. One ESP-IDF project builds every firmware in the system.

- `components/` holds shared code: `cgproto` for the ESP-NOW packets of
  `concept.md` section 3, `cgusb` for the COBS and CRC link of
  `usb-protocol.md`, `cgbeacon` for the cluster driver, `cgcam` for the
  PAJ7025R2, `cgoled` for the on board display.
- `targets/<name>/main/` holds the main component of one firmware.
- `sdkconfig.defaults` holds what every target shares,
  `sdkconfig.defaults.<name>` what one target adds.

The target is chosen with the CMake variable `CGTARGET`, which the top level
`CMakeLists.txt` turns into an entry in `EXTRA_COMPONENT_DIRS`. Each target
builds into its own directory and keeps its own generated `sdkconfig`, so
switching targets never forces a full rebuild of the other one.

Targets in S01: `module`, `pistolstub`, `cambench`.

## D-003 Language and repository rules, as in theserver

Decided in S01-B01.

- Code and documentation are English.
- No em dashes, and no other dash but the plain hyphen. `tools/check_dashes.py`
  enforces it over every tracked text file and is part of the pass.
- Commit messages follow Conventional Commits, scoped by component, for
  example `feat(cgusb): COBS framing and CRC-16`.
- The documentation trio is `docs/concept.md`, `docs/decisions.md` and
  `docs/seasons.md`. Briefings live in `docs/briefings/`, handovers in
  `docs/handovers/`, bench numbers in `docs/measurements.md`.

## D-004 The flash method is stated after every change

Decided in S01-B01. Every change that reaches a board is followed by the
command that puts it there, with the port named. The plain form of the
briefing is extended by the target selection flags of D-002.

Build, flash and watch one target, here `module` on `COM6`:

```
idf.py -D CGTARGET=module -B build/module ^
       -D SDKCONFIG=sdkconfig.module ^
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.module" ^
       -p COM6 build flash monitor
```

`tools/cgflash.ps1` wraps the same command as
`tools\cgflash.ps1 module COM6 build flash monitor`.

When NVS or the partition layout changes, the board is erased first:

```
idf.py -D CGTARGET=module -B build/module -p COM6 erase-flash
```

An erase wipes NVS, which means pairings, the stored channel and the zeroing
offset are gone. A warning is given before every erase, never after.

## D-005 CRC-16/CCITT parameters, open question for the architect

Decided in S01-B01, provisional. `usb-protocol.md` section 1 names
"CRC-16/CCITT" without pinning the parameters, and the name is used for at
least three different algorithms in the field. thefirmware implements the
variant usually meant by that name in an embedded protocol specification:

| Parameter | Value |
| --- | --- |
| Width | 16 |
| Polynomial | 0x1021 |
| Initial value | 0xFFFF |
| Input reflected | no |
| Output reflected | no |
| Final xor | 0x0000 |
| Check value for "123456789" | 0x29B1 |

This is the variant catalogued as CRC-16/IBM-3740, also known as
CRC-16/CCITT-FALSE. It is computed over the type byte and the payload, and
appended little endian, as the document says.

The choice is one constant in `components/cgusb/cgusb.c` and one table in
`tools/cgusb_host.py`, which also carries CRC-16/KERMIT and CRC-16/XMODEM so
that a different ruling costs one line on each side. theclient B04 must agree
before either side ships. Open until the architect rules.

## D-006 The console shares UART0 with the protocol link

Decided in S01-B01. The Heltec V2 has one USB to UART bridge, on UART0.
`usb-protocol.md` section 1 puts the protocol on that bridge, and ESP-IDF
puts its log there as well. Both share it, at 921600 baud.

This is safe because the protocol is COBS framed and CRC checked: log text
carries no zero byte, so it can never be mistaken for a frame boundary, and
anything that is not a valid frame is dropped on the CRC. The document asks
for exactly this property, "so a receiver can resynchronise on any error".

`tools/cgusb_host.py` prints decoded frames and passes anything between them
through as text, which makes boot messages readable at the bench instead of
being a nuisance. If the log volume ever costs measurable bandwidth, the
answer is to lower the log level, not to change the framing.

## D-007 Host unit tests run under Unity through CMake, not the linux target

Decided in S01-B01. The briefing asks for unit tests with the IDF test
framework on the host build. ESP-IDF's own host build is the `linux` target,
which Espressif does not support on Windows, and this machine is Windows.

`cgusb` is therefore written as plain C with no IDF dependency in its core,
and `test/host/` builds that core together with Unity, taken from the
installed ESP-IDF at `components/unity/unity/src`, using the MinGW toolchain
already present on the machine. The test framework is the same Unity that
`idf.py test` would use; only the driver around it differs.

The same tests are additionally checked against `tools/cgusb_host.py`, so the
firmware encoder and the Python reference encoder are proven to agree byte
for byte rather than merely being written from the same document.

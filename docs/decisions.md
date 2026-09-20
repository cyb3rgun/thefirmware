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

## D-008 A zero delimiter at each end of a frame

Decided in S01-B01, found by a test rather than by reading.
`usb-protocol.md` section 1 says frames are "COBS encoded and terminated with
a zero byte". Termination alone is not enough on a port that also carries the
ESP-IDF log (D-006). Log text contains no zero byte, so it does not terminate
anything: it sits in the receiver's buffer and the next frame's bytes are
appended to it. The delimiter that ends the frame then closes one block made
of log text and frame, which fails the CRC. The log line does not merely look
untidy, it eats the frame behind it.

thefirmware therefore writes a zero byte before the frame as well as after
it. A receiver that already follows the document sees an empty block between
the two delimiters and skips it, because an empty block carries no type and
no CRC and was never a frame. The change costs one byte per frame and is
invisible to a correct implementation of version 1.

The document is not changed here, since `usb-protocol.md` says neither side
changes it alone. The architect is asked to fold the leading delimiter into
section 1 so theclient B04 is written against it rather than discovering it.

## D-009 The multiplex slot count is a build time value, open question

Decided in S01-B01, provisional. `usb-protocol.md` section 2 gives the
beacons message a `slot` and a `period_ms`, and `concept.md` section 4 says
the clusters multiplex "with a period and a slot from the core". Neither
says how many slots one period holds, and without that number the module
cannot work out how wide its own window is.

thefirmware divides the period by `CONFIG_CGBEACON_SLOTS`, a menuconfig
value that defaults to 4, and lights its clusters during window `slot - 1`.
Slots are counted from 1, because a shot carrying slot 0 already means "no
beacons identified" in `concept.md` section 3. A module that is in multiplex
mode and still holds slot 0 stays dark rather than colliding with slot 1.

Two readings would remove the build time value, and the architect is asked
to pick one: either the room plan's slot count joins the beacons message, or
`period_ms` is redefined as the width of one slot rather than of the whole
period. Until then a room with more than four targets needs a rebuild, which
is fine on the bench and not fine in a venue.

## D-010 status counts shots heard on the radio and shots the core acknowledged

Decided in S01-B01. `usb-protocol.md` section 3 gives status a `shots heard`
and a `shots acked` counter without saying who did the acknowledging, and
section 4 says a shot the core never acknowledges is dropped "and counts it
in status" without naming a field for it.

thefirmware reads the pair as the two ends of the module's job: `shots
heard` counts distinct shots that arrived over ESP-NOW, and `shots acked`
counts the ones the core acknowledged over USB. The difference is then the
shots that are still in flight or were dropped, which is the only reading
that lets section 4 count a drop in status at all.

A pistol resends until it is acknowledged, so the same shot arrives more
than once. Every copy is acknowledged on the radio, and only the first is
counted and forwarded. Without that, `shots heard` would count radio traffic
rather than shots, and the acceptance test of S01-B01, forwarded frames
equal heard shots, could never hold.

## D-011 The status temperature field is sent as zero on the Heltec bench

Decided in S01-B01. status carries a `temp i8`. The ESP32-D0WDQ6 on the
Heltec V2 has no temperature sensor that ESP-IDF exposes; the one on later
chips is absent on this silicon. The field is sent as 0 rather than as an
invented number, and it starts carrying a real reading on the ESP32-S3 and
ESP32-P4 boards of later seasons, which do have the sensor.

If the core needs to tell "no sensor" from "zero degrees" before then, the
architect is asked for a sentinel value; -128 is free.

## D-012 The PAJ7025R2 register map is missing, so the bench probes for it

Decided in S01-B01. This one is a question, not a decision, and it is the
one thing in this pass that hardware alone cannot settle.

S01-B01 task 5 says the init sequence follows "the datasheet and the trzy
reference". The copy of the datasheet on this machine,
`THEHARDWARE/pixart-paj7025r2_-_pb_v1.3_5134941_11.pdf`, is the real
PAJ7025R2 Product Datasheet version 1.3 of 21 November 2019, but the file
holds only pages 1 to 20 of it. Its own table of contents lists the sections
that matter here well past that end:

| Section | Subject | Page |
| --- | --- | --- |
| 6.1.2 | SPI Data Format | 26 |
| 6.1.3 | SPI Bus Timing Specifications | 29 |
| 6.1.4 | SPI Single Read Example | 32 |
| 7.1.1 | Initialization Flow | 33 |
| 7.1.2 | Initial Settings | 33 |
| 7.1.3 | Product ID Register | 34 |
| 7.3.2 | Registers Bank Switching | 40 |
| 7.4 | Output Access | 42 |

No trzy reference is in the tree either. Without those pages nobody can say
how a register read is framed, where the product ID lives, how a bank is
selected, or how the object data is laid out. Writing a register map from
memory would produce a bench that looks like it works and reports numbers
nobody can trust, which is worse than a bench that says it does not know.

What pages 1 to 20 do give is real and is used: the pin assignment of table
1, the supply and logic levels of tables 2 to 4, and the reference circuit
of figure 13. Those are in the header of `components/cgcam` and in the
checklist the bench prints. Two of them are worth repeating here, because
they destroy hardware rather than merely failing:

- VDDMA takes 2.0 to 3.6 V and dies above 3.96 V, and every signal pin is
  limited to VDDMA + 0.3 V. The 5 V supply that feeds the beacon clusters
  must never reach this module. At 3.3 V no level shifter is needed in
  either direction.
- Pin 14 VSSD and pin 20 VSSD_LED are both required grounds. A module wired
  to only one of them looks powered and answers nothing.

So `cgcam` asks the silicon instead of guessing. `cgcam_probe` sweeps four
plausible transaction encodings across the banks and all 256 registers and
reports every place that answers 0x7025, which pins down the SPI data
format of section 6.1.2 and the product ID register of section 7.1.3 in one
run. The object count and the coordinates still need the output access map
of section 7.4 and are not invented in the meantime.

Asked of the architect and the founder: pages 21 to 57 of this datasheet, or
the trzy reference the briefing names. With either in hand, the rest of task
5 is a table of register constants and an afternoon, and none of the code
around it changes.

## D-013 A value in sdkconfig.defaults is ignored when the symbol has no prompt

Decided in S01-B01, found on the bench rather than by reading. Worth an
entry because it fails silently and costs an hour.

`sdkconfig.defaults` carried `CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600` from
the first commit, and every generated `sdkconfig` came out holding 115200
anyway. No warning, no error, and the build was perfectly happy.

The reason is in `components/esp_system/Kconfig`:

```
config ESP_CONSOLE_UART_BAUDRATE
    int
    prompt "UART console baud rate" if ESP_CONSOLE_UART_CUSTOM
    depends on ESP_CONSOLE_UART
    default 115200
```

The symbol only carries a prompt under `ESP_CONSOLE_UART_CUSTOM`. A symbol
without a prompt is not user settable, so Kconfig discards what
`sdkconfig.defaults` says about it and keeps its own default. The fix is to
choose CUSTOM rather than DEFAULT and to leave the two GPIO values at -1,
which means the console keeps the pins it would have used anyway:

```
CONFIG_ESP_CONSOLE_UART_CUSTOM=y
CONFIG_ESP_CONSOLE_UART_TX_GPIO=-1
CONFIG_ESP_CONSOLE_UART_RX_GPIO=-1
CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600
```

The module hid the mistake, because `cgusb_link_start` configures UART0 to
921600 itself and the protocol worked from the first try. Only the pistol
stub, which has no such link and speaks over the plain console, showed it,
as a port that transmitted steadily and decoded to nothing.

Two things follow for the rest of the tree. A generated `sdkconfig` is the
truth and `sdkconfig.defaults` is only a wish, so a value that matters is
worth grepping for in the generated file once. And the generated files take
precedence on later builds, so a corrected default needs the stale
`sdkconfig.<target>` deleted before it takes.

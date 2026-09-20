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

**Agreed with the architect.** Agreed with the architect, 20 September 2026. `usb-protocol.md` section 1 now names CRC-16/CCITT-FALSE and states all five parameters and the check value, so the variant below is the protocol rather than thefirmware's reading of it. Nothing in the code changed.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026. The console keeps sharing UART0 with the protocol link, and `usb-protocol.md` section 1 now says the leading delimiter exists so that stray console text cannot damage a frame.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026. Unchanged: the host tests stay under Unity through CMake while the build machine is Windows.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026. The leading delimiter is now in `usb-protocol.md` section 1, so theclient B04 is written against it rather than discovering it. Nothing in the code changed.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026, and settled the first way: the beacons message now carries a `slots` byte, so the module is told how many slots share a period instead of being built with the number. `CONFIG_CGBEACON_SLOTS` survives only as what the module uses before the first beacons message arrives.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026, and settled more fully than asked: status now carries four counters rather than two, so the two subtractions are both explicit. Heard minus forwarded are shots for another module's slot; forwarded minus acked by the core are shots the core dropped. The reading below was right and is now unambiguous.

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

**Agreed with the architect.** Agreed with the architect, 20 September 2026. -128 is the sentinel, as suggested, and `usb-protocol.md` section 3 says so. The module sends it instead of 0.

Decided in S01-B01. status carries a `temp i8`. The ESP32-D0WDQ6 on the
Heltec V2 has no temperature sensor that ESP-IDF exposes; the one on later
chips is absent on this silicon. The field is sent as 0 rather than as an
invented number, and it starts carrying a real reading on the ESP32-S3 and
ESP32-P4 boards of later seasons, which do have the sensor.

If the core needs to tell "no sensor" from "zero degrees" before then, the
architect is asked for a sentinel value; -128 is free.

## D-012 The PAJ7025R2 register map is missing, so the bench probes for it

**Agreed with the architect.** Partly closed by D-015 and partly still open: the datasheet text of pages 21 to 57 is still wanted, and the camera board is still not wired.

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

Superseded in part by D-015 on 20 September 2026: the founder allowed the
trzy reference to be read, and the register map is now derived and
implemented. The four transaction encodings this entry describes were all
wrong, which is worth keeping visible: the real one is a command byte before
the register, and the bus is LSB first. What remains open here is the
datasheet text itself, which the reference does not replace.

## D-013 A value in sdkconfig.defaults is ignored when the symbol has no prompt

**Agreed with the architect.** Not a question for the architect, a trap recorded so it costs nobody else an hour.

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

## D-014 A bench run is started and read over the radio, not over two cables

**Agreed with the architect.** Agreed with the architect, 20 September 2026, and given a proper home: `usb-protocol.md` section 6 is now a bench annexe reserving types 0xF0 to 0xFF in both directions, and `concept.md` section 3 carries the radio counterparts. The types moved from 0x7E and 0xFE to 0xF0 and 0xF1, the core now owns the run id, and start is acknowledged by the stub and resent by the module until it is. See the note at the end of this entry on the one thing the amendment left open.

Decided in S01-B01, at the founder's direction, after the first attempt at
the measurements failed on something simple: to put the pistol at 5 m you
unplug it, and the moment you unplug it you lose the serial port that was
going to tell you what it measured.

So the pistol no longer needs a cable. A run is started over the radio and
its result comes back the same way, and everything is read on the module's
USB port:

1. The core sends `bench_start` with a shot count and a rate.
2. The module broadcasts a `start` packet carrying a run id, the count, the
   rate and its own mac, three times, because there is no acknowledgement
   for it and a lost one stalls the bench.
3. The pistol fires exactly that many shots, keeping the round trip
   statistics it already kept, then unicasts one `report` back to the
   module, also three times.
4. The module prints the report as a line on the port and forwards it as a
   `bench_report` frame.

The run id is echoed from `start` into `report`. Repeats of either are
dropped by it, and a report that arrives late, from the run before, is
recognised as late rather than written into the current row.

Two protocols grow by two messages each, and both additions are bench only:

| Protocol | Type | Name | Direction |
| --- | --- | --- | --- |
| cgproto | 5 | `start` | module to pistols, broadcast |
| cgproto | 6 | `report` | pistol to module, unicast |
| cgusb | 0x7E | `bench_start` | core to module |
| cgusb | 0xFE | `bench_report` | module to core |

The two USB types sit well outside the space version 1 uses, 0x01 to 0x07
and 0x81 to 0x85. A conformant implementation of `usb-protocol.md` never
sends or expects them, so theclient B04 is unaffected by their existence,
and the module answers anything it does not know with `error unknown_type`
either way. As with D-008, `usb-protocol.md` and `concept.md` are not edited
here: the architect is asked whether these belong in the documents as a
bench annexe or should stay a firmware side arrangement.

`CONFIG_CGPISTOL_AUTOFIRE` now defaults to off. A stub firing on its own
timer would add shots to a radio started run and spoil the count. The timer
is the fallback, the radio is the normal way to drive a bench, and the PRG
button still fires a single shot whatever either is set to.

## D-015 The PAJ7025R2 bus, derived from the trzy reference, to be confirmed on silicon

Decided in S01-B01, after the founder allowed the reference to be read.
This closes the part of D-012 that blocked the code. It does not close the
part that needs hardware.

`github.com/trzy/PixArt` is the reference the briefing names: "PAJ7025R2 for
6dof tracking: code (Arduino, Windows) and PCB (KiCad)". It was cloned to a
scratch directory outside this tree, read, and not copied. It carries no
licence file, which is one more reason to take facts from it and nothing
else. What was taken is how the silicon behaves, which is not anyone's to
own; what was left is every line of their code.

Three things about this bus have to be right together or the part is simply
mute, and not one of them is a default:

| Thing | Value | Why it would not have been guessed |
| --- | --- | --- |
| Bit order | LSB first | Every other device on this bench is MSB first, and ESP-IDF defaults to MSB first. |
| Transaction shape | command byte, then register, then data | The command leads: 0x00 write, 0x80 read one byte, 0x81 burst read. The register address carries no direction bit, which is the usual convention and is wrong here. |
| Chip select | held across a bank switch and the operation after it | So it cannot be the peripheral's automatic chip select, which drops between transactions. cgcam drives it as an ordinary GPIO. |

The rest of the map:

| Subject | Where |
| --- | --- |
| Bank select | register 0xEF, in every bank |
| Product ID 0x7025 | bank 0, register 0x02 low and 0x03 high |
| Frame period | bank 0x0C, registers 0x07 to 0x09, 24 bit, units of 100 ns |
| Report | write the format code to 0xEF, then burst read from register 0 |
| Format codes | 5 gives 256 bytes, 9 gives 96, 10 gives 144, 11 gives 208 |
| Object stride | 16 bytes in format 1, sixteen objects |

Object fields in format 1, and the bit widths are the sensor's own: area is
14 bits across bytes 0 and 1, the centre is 12 bits per axis across bytes 2
to 5, which is the 4095 by 4095 grid `concept.md` section 5 names. Average
and maximum brightness are bytes 6 and 7. Range and radius share byte 8, 4
bits each. The four boundaries are bytes 9 to 12 at 7 bits, because the
array is 98 pixels across. Aspect ratio, vx and vy are bytes 13 to 15.

The initial settings are a flat list of register writes in
`components/cgcam/cgcam.c`. They are datasheet section 7.1.2 content,
hardware configuration values rather than a program, and the list is flat on
purpose so that nobody reorders it.

**This is a claim until the part agrees with it.** Everything above was read
out of somebody else's driver for a different board and a different
toolchain, and the camera board on this bench is not wired yet.
`cgcam_probe` sweeps the banks for the product ID and is kept for exactly
that: the first run on real silicon either confirms the map or says which
part of it is wrong. Until that run happens the camera rows of
`docs/measurements.md` stay empty.

Still wanted, and still worth having: pages 21 to 57 of the datasheet. The
reference gives the values but not the reasoning, and a bench that has to
change the frame rate, the gain or the exposure will want the text.


## D-016 What the architect's amendments changed in the firmware

Recorded 20 September 2026, after `usb-protocol.md` and `concept.md` came
back amended. Every open question of the S01-B01 handover is now ruled on,
and this entry is the list of what moved in the code so the next reader does
not have to diff two documents to find out.

| Change | Where |
| --- | --- |
| beacons grew a `slots` byte, 5 payload bytes to 6 | `cgusb_beacons_t`, and `cgbeacon_set` takes it at runtime |
| status grew from two counters to four, 20 payload bytes to 28 | `cgusb_status_t`, and the module counts all four |
| temp is now -128 rather than 0 when there is no sensor | `CGUSB_TEMP_NO_SENSOR` |
| bench types moved from 0x7E and 0xFE to 0xF0 and 0xF1 | the bench annexe, `usb-protocol.md` section 6 |
| the core owns the run id, and it is 32 bit | `cgusb_bench_start_t`, and the module's random seed is gone |
| bench_result is the module to core name, and carries seven fields rather than thirteen | `cgusb_bench_result_t` |
| the radio bench packets moved to 0xF0 and 0xF1 | `cgproto` |
| start is resent until acknowledged | `bench_task` on the module, `send_start_ack` on the pistol |
| the acknowledgement timeout is 8 ms | `CONFIG_CGPISTOL_ACK_TIMEOUT_MS` |

One thing the amendment leaves open, and thefirmware has made a choice that
is easy to change. `concept.md` section 3 says start is "acknowledged by the
stub, resent by the module until acknowledged" without saying what the
acknowledgement looks like. thefirmware sends a `start_ack` at type 0xF2,
inside the bench range and on the radio only, carrying the pistol's mac and
the run id. theclient never sees it. If the architect would rather the
acknowledgement were the first shot of the run, or a different type, it is
one struct and two call sites.

The module's mac was dropped from the start packet. ESP-NOW hands the
receiver the source address, which is the same six bytes, and a field that
can disagree with the packet it arrived in is a field worth not having.

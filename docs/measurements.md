# thefirmware measurements

Status: living record. Every bench ends with numbers here, with the board and
the distance, as `concept.md` principle 5 asks.

A row is filled in only when it was measured. An empty cell means the bench
has not run yet, never that the number was close enough to guess.

## S01-B01, 20 September 2026

Boards: two Heltec WiFi LoRa 32 V2 as module and pistol stub, one Heltec V2
with a PAJ7025R2 on flying wires. Firmware `0.1.0`, ESP-IDF v5.5.2.

### Toolchain and build sizes

Measured on the build machine, 20 September 2026.

| Item | Value |
| --- | --- |
| `idf.py --version` | ESP-IDF v5.5.2 |
| module image | 716 KB, 53 percent of the app partition free |
| pistolstub image | 682 KB, 55 percent free |
| cambench image | 201 KB, 87 percent free |
| host unit tests | 27 tests, 0 failures |

### 1. Shot to acknowledgement round trip

Not yet measured. No board was attached to the build machine during this
pass, so nothing was flashed. The firmware and the method are ready.

What is measured: the time from the pistol stub's first transmission of a
shot to the arrival of the module's unicast acknowledgement, in
microseconds, over 100 shots at each distance. Resends are counted
separately; a shot that is still unacknowledged after the first send and
three resends counts as lost.

The numbers come off the board. The pistol stub keeps them itself and prints
a summary when the run completes, so there is no stopwatch and no host clock
in the path.

| Distance | Shots | Median us | p95 us | Loss | Resends | RSSI dBm |
| --- | --- | --- | --- | --- | --- | --- |
| 1 m | 100 | | | | | |
| 3 m | 100 | | | | | |
| 5 m | 100 | | | | | |

Acceptance from S01-B01: 100 shots at 3 m with zero loss after resends.

### 2. Forwarded frames against heard shots

Not yet measured. Same reason.

What is measured: that the module forwards exactly one `shot` frame per
distinct shot it heard, with no duplicates and none missing. A pistol
resends until it is acknowledged, so the module hears some shots more than
once; every copy is acknowledged on the radio and only the first is
forwarded (D-010).

`shots heard` and `shots acked` are read from the module's `status` message.
`frames` is what `tools/cgusb_host.py monitor` counted on the port.

| Distance | Shots fired | Shots heard | Frames forwarded | Frames acked by the host | Duplicates dropped |
| --- | --- | --- | --- | --- | --- |
| 1 m | 100 | | | | |
| 3 m | 100 | | | | |
| 5 m | 100 | | | | |

### 3. Camera object count

Not yet measured, and blocked on more than hardware. See D-012: the copy of
the PAJ7025R2 datasheet in THEHARDWARE holds only pages 1 to 20, and the SPI
data format, the product ID register and the output access map are all past
that end. The object count cannot be read until those pages or the trzy
reference arrive.

What the bench can do today is prove the module is alive and pin down the
transaction format on real silicon, which is the first row below. The object
rows wait.

| Subject | Result |
| --- | --- |
| Product ID 0x7025 answers | |
| SPI transaction encoding that worked | |
| Bank and register holding the ID | |
| SPI mode that worked | |
| SPI clock | 1 MHz on flying wires |

| Source | Distance | Objects seen | Notes |
| --- | --- | --- | --- |
| TV remote | 1 m | | |
| TV remote | 2 m | | |
| TV remote | 3 m | | |
| Candle | 1 m | | |
| Candle | 2 m | | |
| Candle | 3 m | | |
| One beacon cluster | 1 m | | once soldered |
| One beacon cluster | 2 m | | once soldered |
| One beacon cluster | 3 m | | once soldered |

## How to run the bench

Three boards, three ports. Confirm which port is which before flashing;
`erase-flash` is only needed when NVS or the partition layout changes, and
it wipes the stored channel and the pairings.

### Round trip and forwarding, sections 1 and 2

1. Flash the module and the pistol stub. The channel must match on both.
   The module keeps its channel in NVS and takes it from the core; the stub
   takes it from menuconfig.

   ```
   tools\cgflash.ps1 module COM6 build flash monitor
   tools\cgflash.ps1 pistolstub COM7 build flash monitor
   ```

2. Open the module's port with the host tool, which stands in for theclient
   until B04 exists. It says `hello`, acknowledges every `shot`, and prints
   what comes back. Non frame bytes are printed as text, so the boot log is
   readable rather than a nuisance (D-006).

   ```
   python tools/cgusb_host.py monitor COM6 --beacons 0x0F --mode 0 --poll 5
   ```

3. Put the two boards at 1 m. Hold PRG on the stub for a second to reset the
   statistics and start a run. The stub fires five shots a second and prints
   the summary after 100 shots.

4. Read the summary off the stub's serial output into the table. Read
   `shots heard` and `shots acked` off the module's `status` line, which the
   `--poll 5` above asks for every five seconds.

5. Repeat at 3 m and 5 m. Note the RSSI the stub reports; it is the
   module's acknowledgement as heard by the stub.

For multiplex mode, pass `--mode 1 --period 40 --slot 1` instead, and make
sure `CONFIG_CGPISTOL_SLOT` on the stub matches the slot the module is on.
See D-009 for the slot count, which is still a build time value.

### Camera, section 3

1. Check the wiring before power. The pin numbers are the module's own and
   the list is in `components/cgcam/include/cgcam.h`. VDDMA is 3.3 V and the
   part dies above 3.96 V, so the 5 V beacon supply must not reach it. Both
   pin 14 and pin 20 are grounds and both are required.

   ```
   tools\cgflash.ps1 cambench COM8 build flash monitor
   ```

2. The firmware sweeps the transaction encodings and the register space on
   boot and prints every place that answers 0x7025. Copy those lines into
   the table above; they are the answer to D-012 as far as the bench can
   give one.

3. If nothing answers, the firmware stops and prints the wiring and logic
   level checklist in the order that finds faults fastest. Work down it.

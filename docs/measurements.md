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
| module image | 718096 bytes, 53 percent of the app partition free |
| pistolstub image | 699008 bytes, 54 percent free |
| cambench image | 206224 bytes, 87 percent free |
| host unit tests | 27 tests, 0 failures |

### 1. Shot to acknowledgement round trip

Measured 20 September 2026. Module on COM7, pistol stub on a power bank at
the stated distance, run started and read over the radio (D-014).

What is measured: the time from the pistol stub's first transmission of a
shot to the arrival of the module's unicast acknowledgement, in
microseconds, over 100 shots at each distance. Resends are counted
separately; a shot that is still unacknowledged after the first send and
three resends counts as lost.

The numbers come off the board. The pistol stub keeps them itself and prints
a summary when the run completes, so there is no stopwatch and no host clock
in the path.

RSSI is given as the module's acknowledgement heard at the pistol, then the
pistol's shots heard at the module.

| Distance | Shots | Median us | p95 us | Loss | Resends | RSSI dBm | Mean us | Min us | Max us |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 m | 100 | 2673 | 5753 | 0 percent | 0 | -61 / -61 | 3653 | 2356 | 17375 |
| 3 m | 100 | 2591 | 4699 | 0 percent | 0 | -65 / -64 | 2789 | 2351 | 5621 |
| 5 m, wall in the path | 100 | 2619 | 22407 | 0 percent | 12 | -89 / -88 | 5138 | 2377 | 42438 |
| 5 m, same spot, 8 ms timeout | 100 | 2683 | 10495 | 0 percent | 16 | -89 | | | |

Acceptance from S01-B01, 100 shots at 3 m with zero loss after resends: met,
and with no resend needed at all.

Three things in this table are worth more than the pass mark.

The median does not move with distance. 2673, 2591, 2619 microseconds across
1, 3 and 5 m, while the signal falls 28 dB. A shot that gets through gets
through at the same speed, and `concept.md` principle 1, single digit
milliseconds on the radio, holds at every distance measured.

The tail is set by the resend timeout, not by the radio. At 5 m twelve shots
of a hundred needed a resend, and p95 jumped from 4699 to 22407
microseconds. That is the 20000 microsecond
`CONFIG_CGPISTOL_ACK_TIMEOUT_MS` plus one median, almost to the
microsecond. The link did not slow down; the pistol waited out a timeout
that was set nearly eight times the median round trip. See the note below.

The 5 m row has a wall in the path, not clear line of sight, which is where
the 24 dB step between 3 m and 5 m comes from. Free space over that distance
would cost about 4 dB. The row is a wall measurement and should not be read
as a distance measurement.

#### The acknowledgement timeout, shortened and measured

`CONFIG_CGPISTOL_ACK_TIMEOUT_MS` was 20 ms, chosen before any number
existed on the reasoning that the round trip would be single digit
milliseconds and 20 ms was therefore generous. The first three rows said
otherwise: the median is 2.6 ms and the worst clean round trip at 3 m was
5.6 ms, so 20 ms was not generous, it was idle waiting. A shot that is
going to fail has already failed by about 6 ms.

The architect set it to 8 ms and the 5 m run was repeated from the same
spot, against the same wall, with the same firmware otherwise. RSSI came
back at -89 dBm both times, which is the evidence that the position really
was the same.

| | Median us | p95 us | Loss | Resends |
| --- | --- | --- | --- | --- |
| 20 ms | 2619 | 22407 | 0 percent | 12 |
| 8 ms | 2683 | 10495 | 0 percent | 16 |

The prediction and the measurement agree closely enough to be worth
stating: 8000 plus one median of 2683 is 10683 microseconds, against a
measured p95 of 10495. The tail is one timeout plus one median, and
nothing else.

The resend count rose from 12 to 16, and that is the change working rather
than a regression. A first attempt that would have been answered at 10 ms
now becomes a resend instead of a slow success. Loss stayed at zero because
every resend still succeeded on the attempt after, which is the property
that matters: the cost of the shorter timeout is paid in radio traffic, not
in lost shots.

#### At a weak signal the loss is all on the uplink

The four counters of the amended `status` show something the old two could
not. In the 8 ms run the module acknowledged the pistol exactly 100 times
for 100 shots heard, while the pistol counted 16 resends.

If a resend had been caused by a lost acknowledgement, the module would
have heard that shot twice and acknowledged it twice, and the count would
have been 116. It was 100. So all sixteen retransmissions were shots that
never reached the module at all, and not one acknowledgement was lost on
the way back.

That is `concept.md` principle 3 showing its shape in the numbers. Shots go
out as ESP-NOW broadcasts, which get no acknowledgement or retry from the
WiFi MAC layer underneath. Acknowledgements go back as unicast, which does
get both. The asymmetry is deliberate and the cost of it lands entirely on
the pistol to module direction, which is exactly where the pistol's own
resend logic is there to cover it.

For a close range comparison, the 20 shot check at RSSI -43 counted 22
acknowledgements to the pistol against 20 shots heard, with 2 resends: at
that signal both copies of the two resent shots arrived, so the module
acknowledged each of them twice. The same two counters read the opposite
way round, and both readings are correct for their distance.

### 2. Forwarded frames against heard shots

Measured 20 September 2026, from the same runs as section 1.

What is measured: that the module forwards exactly one `shot` frame per
distinct shot it heard, with no duplicates and none missing. A pistol
resends until it is acknowledged, so the module hears some shots more than
once; every copy is acknowledged on the radio and only the first is
forwarded (D-010).

`shots heard` and `shots acked` are read from the module's `status` message.
`frames` is what `tools/cgusb_host.py monitor` counted on the port.

| Distance | Shots fired | Shots heard | Frames forwarded | Frames acked by the host | Heard but not forwarded |
| --- | --- | --- | --- | --- | --- |
| 1 m | 100 | 100 | 100 | 100 | 0 |
| 3 m | 100 | 100 | 100 | 100 | 0 |
| 5 m, wall in the path | 100 | 100 | 100 | 100 | 0 |
| 5 m, same spot, 8 ms timeout | 100 | 100 | 100 | 100 | 0 |

Exact at every distance, including the 5 m run where twelve shots were
transmitted more than once. The pistol resent those twelve, the module
acknowledged every copy on the radio and forwarded each shot once, and the
hundred distinct pistol sequence numbers arrived as a hundred frames. That
is D-010 doing its job, and it is the acceptance line "forwarded frames
equal heard shots".

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

Ports on the founder's machine, 20 September 2026: the module is COM7 and
the pistol stub is COM6 when it is plugged in at all. Confirm before
flashing rather than trusting this line; the briefing's example said the
opposite way round.

`erase-flash` is only needed when NVS or the partition layout changes, and
it wipes the stored channel and the pairings. Nothing in this pass needed
it.

### Round trip and forwarding, sections 1 and 2

The pistol needs no cable. Everything is started and read on the module's
port (D-014), which is what makes a 5 m run possible at all: the moment you
unplug the pistol to carry it away, its serial port is gone.

1. Flash both boards once. The channel must match; both default to 1.

   ```
   tools\cgflash.ps1 module COM7 build flash
   tools\cgflash.ps1 pistolstub COM6 build flash
   ```

2. Unplug the pistol, put it on a power bank, and place it at the distance.
   It does nothing until it is told to, because `CONFIG_CGPISTOL_AUTOFIRE`
   is off by default.

3. Run the measurement. The script is the core: it says hello, sets the
   beacons, starts the run over the radio, acknowledges every forwarded
   shot, and prints both table rows when the pistol's report comes back.

   ```
   python tools/bench_s01.py --module COM7 --distance "3 m"
   ```

   Add `--verbose` to see the module's log between the frames, `--shots`
   and `--rate` to change the run, and `--mode 1 --period 40 --slot 1` for
   multiplex instead of steady beacons.

4. Move the pistol and run it again. Nothing needs reflashing or resetting
   between distances.

If the pistol never reports, the three things worth checking, in order: it
is powered and its display is lit; both boards are on the same channel, which
the module's own `status` line gives as `ch=`; and the run id is not
repeating, which it cannot since the fix of 20 September but which is what
the failure looked like the first time.

### Camera, section 3

1. Check the wiring before power. The pin numbers are the module's own and
   the list is in `components/cgcam/include/cgcam.h` and in `concept.md`
   section 5. VDDMA is 3.3 V and the part dies above 3.96 V, so the 5 V
   beacon supply must not reach it. Both pin 14 and pin 20 are grounds and
   both are required.

   ```
   tools\cgflash.ps1 cambench COM6 build flash monitor
   ```

2. The firmware sweeps the transaction encodings and the register space on
   boot and prints every place that answers 0x7025. Copy those lines into
   the table above; they are the answer to D-012 as far as the bench can
   give one.

3. If nothing answers, the firmware stops and prints the wiring and logic
   level checklist in the order that finds faults fastest. Work down it.

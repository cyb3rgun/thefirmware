# thefirmware seasons

Status: living plan. A season is a block of work with one goal; a briefing is
one pass inside it; a handover closes a pass.

## S01: the module talks, the camera sees

Goal from `concept.md` section 6. A Heltec V2 as module drives four beacon
clusters, hears a second Heltec as pistol stub, acknowledges over ESP-NOW,
forwards the shot over USB per `usb-protocol.md`, and theclient on a PC
receives it. A third Heltec with the PAJ7025R2 reports the objects it sees.

Aim computation, the S3 pistol, the IMU and recoil are later seasons.

### Passes

| Pass | Subject | State |
| --- | --- | --- |
| S01-B01 | repository foundation, target module bench, camera bench | done, camera open on D-012 |

### S01-B01 tasks

| Task | Scope | State |
| --- | --- | --- |
| 1 | `chore(repo)` skeleton, decisions, seasons | done |
| 2 | `feat(cgusb)` COBS, CRC-16, message structs, host reference, tests | done |
| 3 | `feat(module)` beacons, ESP-NOW receive, acknowledge, forward, OLED | done, measured |
| 4 | `feat(pistolstub)` shot sender, resend, counters, OLED | done, measured |
| 5 | `feat(cgcam)` PAJ7025R2 on VSPI, object read, OLED | blocked, D-012 |
| 6 | `docs` measurements, handover, push | radio done, camera open |

### What S01-B01 left for the next pass

- The camera, blocked on pages 21 to 57 of the PAJ7025R2 datasheet or the
  trzy reference (D-012). The board was not wired during the pass either.
- The open questions of `docs/handovers/S01-B01.md`, which are the
  architect's to rule on: D-005, D-008, D-009, D-010, D-011, D-014, and
  whether to shorten the acknowledgement timeout.
- A pulldown on GPIO 12 before the beacon clusters are soldered, because it
  is the MTDI strapping pin and a floating gate stops the board booting.

## Later seasons, as sketched in concept.md

- The aim computation: homography from the four beacon points, zeroing offset
  from NVS, on the pistol.
- The `pistol` target on an ESP32-S3 with the real trigger, the IMU and
  recoil.
- The `display` target on the ESP32-P4, class A, module plus a reduced client
  core in C.
- Signing the radio packets with a per pistol key, named in `concept.md`
  section 3 as a later pass.

# CYB3RGUN thefirmware: concept

Status: living reference, version 1, 20 September 2026. This document only grows.

## 1. What thefirmware is

Everything that runs on an ESP32 in the CYB3RGUN system, in one ESP-IDF project with build targets:

| Target | Chip (bench) | Chip (series) | Does |
| --- | --- | --- | --- |
| `pistol` | Heltec WiFi LoRa 32 V2 (ESP32) | ESP32-S3 | reads the PAJ7025R2, computes the aim point from the beacons, IMU, trigger, sends the shot over ESP-NOW, waits for the acknowledgement, resends |
| `module` | Heltec WiFi LoRa 32 V2 (ESP32) | ESP32-S3 | drives the four beacon clusters with the multiplex clock, hears pistols over ESP-NOW, acknowledges, forwards shots to the target computer over USB serial (`usb-protocol.md`), relays `time_mark` |
| `display` | ESP32-P4 display board | ESP32-P4 | class A: module plus a reduced client core in C, speaking theserver's device link directly |

## 2. Principles

1. Real time never crosses a network. Trigger, aim, shot, acknowledgement: all on the radio, single digit milliseconds.
2. The pistol computes, the module relays. A shot leaves the pistol as finished coordinates; nobody downstream sees raw camera data.
3. Broadcast shots, unicast acknowledgements. No ESP-NOW peer registration for pistols; the module that heard the shot acknowledges by unicast and the pistol resends until acknowledged (from the ESP-NOW reliability findings of 16 September).
4. One fixed channel per venue, set by the module from the core (`channel`), modem sleep off wherever WiFi station and ESP-NOW share a chip.
5. Everything measured: every bench ends with numbers in `docs/measurements.md`, with the board and the distance.

## 3. Radio packet, version 1

ESP-NOW payload, little endian, CRC-16 at the end, signed with a per pistol key in a later pass:

`shot`: type u8 (1), pistol mac 6, pistol seq u16, target slot u8 (the multiplex slot of the beacons the pistol identified), target mac 6 (zero when the slot is enough, filled once the pistol has heard the module's `ack` once), ts_pistol ms u32, x u16, y u16, quat 4 x i16, buttons u8, flags u8. About 37 bytes.

Only the module whose slot (or mac) matches acknowledges and forwards; every other module ignores the shot. A shot with slot 0 (no beacons identified, steady mode) is acknowledged by every module that hears it and forwarded with flag `unaimed`, which the core scores as a miss. Slots are assigned by theserver's room plan and reach the module through the core (`beacons` in usb-protocol.md).
`ack`: type u8 (2), pistol seq u16, module mac 6, slot u8.
`time_mark`: type u8 (3), unix ms u64, from the module, broadcast.
`hello`: type u8 (4), pistol mac 6, fw version 3 bytes; sent when the trigger is held on power up, for pairing by shooting.

## 4. Beacons

Four clusters of three TSHG6400, 850 nm, 100 mA each, one MOSFET per cluster on GPIO 12, 13, 14 and 25 of the Heltec V2 (OLED on 4, 15, 16 and LoRa on 5, 18, 19, 26, 27 stay untouched). Modes: all steady; multiplex with a period and a slot from the core, so several targets in a room can be told apart by the pistol.

## 5. Camera

PAJ7025R2 over SPI (VSPI on the Heltec bench: 5, 18, 19, 23 with LoRa disabled; own host on the S3 in series), 1 to 2 MHz on flying wires, up to 14 MHz on the board, 0.1 uF and 10 uF at the module pins, up to 16 objects at up to 200 frames per second, 4095 x 4095 interpolated. The bench reads objects; the aim computation (homography from four points, then zeroing offset from NVS) comes when the beacons exist.

## 6. Seasons

S01 goal: a Heltec V2 as module drives four clusters, hears a second Heltec as pistol stub, acknowledges, forwards over USB per `usb-protocol.md`, and theclient on a PC receives the shots; a Heltec with the PAJ7025R2 reports objects it sees. Aim computation, the S3 pistol, IMU and recoil are later seasons.

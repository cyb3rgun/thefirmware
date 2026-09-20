# Target module USB protocol, version 1

Status: draft for theclient S01 and thefirmware S01, written before either exists. Both implement this document; neither changes it without the other.

## 1. Transport

The target module (an ESP32 with the beacon driver and the ESP-NOW radio) is a USB serial device: CDC on chips with native USB, a USB to UART bridge (CP2102 or similar) on boards like the Heltec V2. 921600 baud, 8N1, no flow control. Frames are COBS encoded and terminated with a zero byte, so a receiver can resynchronise on any error. Inside a frame: one byte type, payload, two bytes CRC-16/CCITT over type and payload, little endian.

The module never speaks first; it waits for `hello` from the core after the port opens. Every message the module sends carries a 16 bit sequence number; the core acknowledges shots.

## 2. Messages, core to module

| Type | Payload | Meaning |
| --- | --- | --- |
| `0x01 hello` | proto version u8 | start of a session on the port; the module answers `hello_ack` |
| `0x02 beacons` | mask u8 (bit per cluster), mode u8 (0 all steady, 1 multiplex), period_ms u16, slot u8 | which clusters light and how they multiplex; slot is this target's slot in the room plan |
| `0x03 time_mark` | unix ms u64 | the shared clock from theserver, relayed by the module over ESP-NOW to the pistols |
| `0x04 ack` | seq u16 | acknowledges a `shot` |
| `0x05 channel` | wifi channel u8 | the room's fixed 2.4 GHz channel; the module restarts its radio on change |
| `0x06 status_req` | none | asks for `status` |
| `0x07 reboot` | none | |

## 3. Messages, module to core

| Type | Payload | Meaning |
| --- | --- | --- |
| `0x81 hello_ack` | proto u8, fw version u8 u8 u8, chip u8, mac 6 bytes | |
| `0x82 shot` | seq u16, pistol id 6 bytes (mac), pistol seq u16, slot u8, flags u8 (bit 0 unaimed), ts_pistol ms u32, x u16, y u16 (0..65535 normalised to the beacon frame), quat 4 x i16, buttons u8, rssi i8 | a shot aimed at this module's slot, heard over ESP-NOW and already acknowledged to the pistol; shots for other slots are never forwarded |
| `0x83 status` | uptime s u32, channel u8, beacons mask u8, mode u8, temp i8, free heap u32, shots heard u32, shots acked u32 | on request and every 30 s |
| `0x84 pistol_seen` | pistol id 6 bytes, rssi i8 | a pistol announced itself; used for pairing by shooting |
| `0x85 error` | code u8, detail 0..32 bytes | |

## 4. Rules

- The module acknowledges the pistol over ESP-NOW itself, immediately, before forwarding; the core's `ack` only frees the module's forwarding buffer. If the core does not acknowledge within 500 ms the module resends the `shot` up to three times, then drops it and counts it in `status`.
- `x` and `y` are the pistol's aim point in the beacon frame of the module's own clusters: `0,0` is the top left cluster, `65535,65535` the bottom right. The core maps this to the scenario canvas using the target's calibration (a setting: the beacon rectangle in canvas coordinates).
- The module holds the multiplex clock. The core sends `beacons` once per plan change, not per slot.
- `time_mark` arrives from theserver over the device link and goes out over the module at most once per second.

## 5. Bench without hardware

theclient can replace the module with a simulator that generates `shot` frames on a schedule or from keyboard input, so the serial layer, the acknowledgement rules and the mapping are tested before the Heltec exists.

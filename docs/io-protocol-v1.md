# HALTZ I/O protocol v1 (provisional)

Status: draft, rev 1, 2026-11-22. Goes with architecture-v1.md (I/O board section) and wiring-v1.md (stages 4 and 5).
Covers how the S3 talks to the ESP32 I/O board over the UART link: transport rules, boot handshake, the input, sound and LED models, and every message.
The single source of truth for IDs and sizes is `shared/haltz_proto/include/haltz/msg_ids.def`; constants and bit layouts are in `haltz/io.h`. This document explains them.
Points that differ from architecture-v1: commands get no ACK (same choice as the GPU link), the input messages carry whole states instead of single events, and the I/O firmware uses its own small Bluepad32 platform in C instead of the template's Arduino layer (section 8).

## Design goals
1. Same packet as every link: `SYNC 0xA5 | TYPE | LEN | PAYLOAD | CRC8`. Unchanged.
2. Input can't get stuck. Every input message carries the whole state of its device (all keys held, both stick axes and the buttons, the knob) and is repeated four times a second. A lost frame is corrected by the next one, so a key can't stay "down" because its release was lost.
3. Fire and forget. Commands are not acknowledged; errors come back as NAK and the counters in IO_STATUS show whether anything was lost.
4. The I/O board does its own timing. A tune is sent once and the board plays it note by note; LED patterns (blink, breathe, fade) run on the board. The S3 only sends a message when something changes.
5. Resetting the I/O board is expensive: it drops the Bluetooth keyboard, which then takes a few seconds to reconnect. So the S3 resets it only at boot and when the link is really dead.

## 1. Transport (the UART link)
| Setting | Value | Why |
|---|---|---|
| UART | S3 UART1 (TX GPIO17, RX GPIO18) <-> ESP32 UART2 (RX2 GPIO16, TX2 GPIO17) | wiring-v1 stage 4 |
| Format | 8N1, no flow control, idle high | RX pins have the internal pull-up on, so an unplugged wire reads idle |
| Baud | 115200 (`HALTZ_IO_BAUD`) | Proven by the wiring test. The traffic needs about 1.5 KB/s of the 11.5 KB/s (section 7) |

**Two byte streams.** Each direction is one continuous stream of frames, back to back, with no filler. Each side writes a frame in one piece (one `uart_write_bytes` call), so there are no gaps inside a frame.
**Idle flush.** A UART has no chip select to mark the end of a burst. A receiver that has a frame in progress and gets no byte for 10 ms (`HALTZ_IO_IDLE_FLUSH_MS`) calls `haltz_decoder_flush()`. A false SYNC in line noise, or the tail of a frame cut off by a reset, then can't hold back the frames behind it. The longest frame takes 22.5 ms to send, but its bytes come at 87 us intervals, far below 10 ms.

**EN** (S3 GPIO8 -> ESP32 EN, open drain): hard reset, held low for at least 20 ms (`HALTZ_IO_RESET_MS`; the DevKit has an RC delay on EN). Used at boot, during standby (held), and when the I/O board stops answering.

## 2. Boot handshake and health
1. The S3 pulls EN low for 20 ms, then releases it.
2. The I/O board boots. Before it starts Bluetooth it opens the UART, measures the stick centre, starts the LED breathing blue ("booting") and queues its HELLO.
3. The S3 sends its HELLO every 100 ms and watches RX. The I/O board answers every HELLO it receives with its own HELLO. No answer within 3 s: reset again. After 3 tries: I/O board missing (console message), a new attempt every 10 s.
4. The S3 compares `proto_version`. If it differs, the S3 shows a warning and carries on (repo-layout rule 5).
5. Link up. The S3 sends what it keeps for the board (section 9): the LED pattern, the volumes, and the Bluetooth scan setting. Then the kernel can play its start-up chime.

Health:
- The I/O board sends IO_STATUS every 250 ms by itself (`HALTZ_IO_STATUS_EVERY_MS`); no request needed, the UART is full duplex. With it, it repeats IO_KEYS, IO_STICK and IO_KNOB (the refresh, goal 2).
- The S3 resets the I/O board and repeats the handshake when nothing at all arrives for 1.5 s (`HALTZ_IO_DEAD_MS`).
- An unrequested HELLO from the I/O board (more than 300 ms after link up), or `uptime_ms` going backwards in IO_STATUS, means the board rebooted by itself. No reset: the S3 sends its kept state again.
- When the link goes down the S3 releases every key it thinks is held (KEY up events), and the stick goes back to the centre, so nothing stays pressed while the board is gone.

## 3. The input model
**Keyboard.** One IO_KEYS message is the whole keyboard state, like a USB HID boot report: the modifier bits plus the list of other keys held (up to 10, `HALTZ_IO_KEYS_MAX`). Values are HID usage codes (keyboard page 0x07): 0x04 = A ... 0x1D = Z, 0x1E-0x27 = 1-9 and 0, 0x28 Enter, 0x29 Escape, 0x2A Backspace, 0x2B Tab, 0x2C Space, 0x3A-0x45 F1-F12, 0x4F-0x52 arrows. The usages come from the keyboard as they are, so the layout is the keyboard's (US); turning a usage into a character is the S3's job.
- Modifier bits (HID standard): 0 left Ctrl, 1 left Shift, 2 left Alt, 3 left GUI, 4 right Ctrl, 5 right Shift, 6 right Alt, 7 right GUI. Modifiers never appear in the key list.
- The I/O board sends IO_KEYS whenever a report from the keyboard changes the state, and in the refresh. If more than one keyboard is connected their states are merged (modifiers ORed, keys joined, at most 10).
- Reports with the HID "rollover error" codes (0x01-0x03: too many keys) are skipped; the last good state stays.
- The S3 compares each IO_KEYS with the previous one and turns the differences into KEY down / up events for the kernel (section 9). Key repeat is the S3's job.

**Thumbstick and buttons.** IO_STICK carries both axes and every button.
- x, y from -1000 to +1000 (`HALTZ_IO_AXIS_MAX`), already centred: right = +x, up = +y (as mounted; `STICK_*` flags in `firmware/io-esp32/main/board_pins.h`). The screen's y goes down, so the S3 flips y for the pointer.
- Inside the dead zone the axis is exactly 0. Outside it the value grows to 1000 at the end of the travel, scaled separately on each side of the centre (the centre is never mid-scale).
- buttons: bit 0 stick click (GPIO32), bit 1 the spare push button (GPIO33, not wired yet, reads 0). Debounced on the board (20 ms).
- Sampled at 100 Hz with light smoothing. Sent when a value changes (at most 100 times a second), and in the refresh. While the stick rests in the centre nothing is sent except the refresh.
- The centre is measured at boot (don't touch the stick while the board boots) and again on IO_STICK_CAL. A centre far from mid-scale (an open wire) is reported as `STICK_OK = 0` in IO_STATUS; the axes then read 0.

**Knob.** IO_KNOB carries the position, 0-255 (full left to full right). The very ends of the ADC range are flat, so the board maps a little less than the full travel to 0-255 so both ends can be reached. A change smaller than 2 steps is ignored unless it reaches 0 or 255 (no jitter). Sent on change and in the refresh. What it does is still open (architecture-v1); for the protocol it's a plain input.

**Devices.** IO_DEVICE reports a Bluetooth device connecting (ready to use) or disconnecting, with its kind, battery level and USB-style IDs. Only keyboards feed IO_KEYS in v1; a mouse or gamepad is reported but its data is not forwarded yet. IO_STATUS also carries the set of connected slots and a "keyboard connected" flag, so the S3 can't miss a change for long.

## 4. The sound model
Two voices, A (GPIO25) and B (GPIO26), each a square wave from its own LEDC timer. A silent voice has its pin released (input, no pull; wiring-v1).
- **Tune buffer.** Each voice has a buffer of 255 notes (`HALTZ_IO_TUNE_NOTES`). IO_NOTES appends to it (or replaces it), and the voice plays it in order. Without LOOP, played notes leave the buffer, so a long tune can be streamed in parts: IO_STATUS reports the free space per voice. With LOOP the voice starts again at the first note when it reaches the end.
- **Notes.** MIDI note numbers, 12-127 (C0 16 Hz ... G9 12.5 kHz; 69 = A4 = 440 Hz), 0 = rest. A note's length is `ticks x tick_ms` (both from the message; up to 65 s). Unless LEGATO is set, the last 1/8 of each note (at most 20 ms) is silent, so two equal notes in a row are heard as two.
- **IO_TONE** plays one frequency in Hz for a time (or until stopped). It replaces the voice's tune.
- **IO_SFX** plays a built-in sound effect (section 6) on a voice. The tune on that voice pauses while the effect plays and then continues where it was; a new effect replaces one that is still playing. This is how a game gets music on A and B and still beeps.
- **IO_STOP** silences voices and empties their tune buffers.
- **IO_VOLUME** sets the volume of voices, 0 (mute) to 255 (full, the default). Passive buzzers can only change the pulse width, so this is rough: full volume is a 50% square wave, lower levels are narrower pulses (the board uses a curve so the steps sound more even). Volume stays until changed and applies to everything played after.
- Timing runs on the board in 1 ms steps.

## 5. The LED model
One RGB LED, three LEDC channels on one timer (~5 kHz). Colours are 0-255 per channel; the board applies a gamma curve so 128 looks about half as bright as 255.
IO_LED sets a pattern:
| Mode | Value | What it does | period_ms | count |
|---|---|---|---|---|
| SOLID | 0 | the colour, steady | ignored | 0 |
| BLINK | 1 | colour for half the period, off for the other half | >= 20 | 0 |
| BREATHE | 2 | smoothly from off to the colour and back, one cycle per period | >= 20 | 0 |
| FLASH | 3 | the colour for period ms once, then back to the pattern that was running (which kept its timing). Any other pattern ends a FLASH that is still showing | >= 1 | 0 |
| FADE | 4 | from what is showing now to the colour over period ms, then steady (0 = at once) | any | 0 |
| CODE | 5 | `count` blinks (each one period: half on, half off), then off for 3 periods, repeated | >= 20 | 1-15 |

So architecture-v1's status lights are: booting = BREATHE blue, waiting for the keyboard = BLINK blue, running = SOLID green, cartridge read = FLASH white, halted = SOLID red, error = CODE red with the error number, shutting down = FADE to black.
At power-up the I/O board breathes blue by itself, before the S3 says anything.

## 6. Messages
ID ranges: 0x01-0x0F every link, 0x10-0x3F GPU link, 0x40-0x4F I/O board -> S3, 0x50-0x6F S3 -> I/O board.
All multi-byte fields are little-endian; i16 is two's complement. "n" is worked out from LEN.
Common messages on this link: HELLO 0x01 (both directions), NAK 0x03 (I/O -> S3). ACK 0x02 is not used.

### Summary
| ID | Name | Dir | LEN | Payload |
|---|---|---|---|---|
| 0x40 | IO_KEYS | I/O -> S3 | 1-11 | modifiers u8, n x usage u8 (n = 0-10) |
| 0x41 | IO_STICK | I/O -> S3 | 5 | x i16, y i16, buttons u8 |
| 0x42 | IO_KNOB | I/O -> S3 | 1 | value u8 |
| 0x43 | IO_DEVICE | I/O -> S3 | 8 | slot u8, state u8, kind u8, battery u8, vendor_id u16, product_id u16 |
| 0x4F | IO_STATUS | I/O -> S3 | 16 | see below |
| 0x50 | IO_LED | S3 -> I/O | 7 | mode u8, r u8, g u8, b u8, period_ms u16, count u8 |
| 0x51 | IO_TONE | S3 -> I/O | 5 | voice u8, hz u16, ms u16 |
| 0x52 | IO_NOTES | S3 -> I/O | 5-255, odd | voice u8, flags u8, tick_ms u8, n x (note u8, ticks u8) |
| 0x53 | IO_SFX | S3 -> I/O | 2 | id u8, voice u8 |
| 0x54 | IO_STOP | S3 -> I/O | 1 | voices u8 |
| 0x55 | IO_VOLUME | S3 -> I/O | 2 | voices u8, level u8 |
| 0x56 | IO_STICK_CAL | S3 -> I/O | 0 | - |
| 0x57 | IO_BT | S3 -> I/O | 1 | action u8 |

### I/O board -> S3
- **IO_KEYS**: `modifiers` (section 3), then n usage codes, each 0x04-0xDF, no duplicates. n = 0 means no key except maybe modifiers.
- **IO_STICK**: x, y in -1000..1000. buttons: bit 0 stick click, bit 1 spare button, bits 2-7 must be 0.
- **IO_KNOB**: 0-255.
- **IO_DEVICE**: `slot` 0-3 (Bluepad32's device index), `state` 0 disconnected / 1 connected, `kind` 0 other / 1 keyboard / 2 mouse / 3 gamepad, `battery` 0 unknown, 1 empty ... 255 full, then the device's vendor and product IDs (0 if unknown). Sent when a device is ready, when it disconnects, and when its battery level changes.

### IO_STATUS (I/O -> S3, 16 bytes, every 250 ms)
| Field | Type | Meaning |
|---|---|---|
| uptime_ms | u32 | ms since the I/O board booted. Going backwards = it rebooted |
| rx_frames | u16 | good frames received from the S3 (wraps). Lets the S3 check none got lost |
| crc_errors | u16 | frames from the S3 dropped for a bad CRC (wraps) |
| cmd_errors | u16 | commands refused with a NAK, sent or not (wraps) |
| rx_overruns | u16 | times the UART receive buffer overflowed, data lost (wraps). Must stay 0 |
| flags | u8 | bit 0 a keyboard is connected, bit 1 Bluetooth scanning (new devices can connect), bit 2 voice A sounding, bit 3 voice B sounding, bit 4 stick centre OK, bits 5-7 0 |
| devices | u8 | bit n = slot n connected |
| tune_free_a | u8 | free notes in voice A's tune buffer |
| tune_free_b | u8 | free notes in voice B's tune buffer |

### S3 -> I/O board
- **IO_LED**: mode and rules in section 5. Other modes: NAK BAD_VALUE.
- **IO_TONE**: `voice` 0 = A, 1 = B. `hz` 0 (silence) or 20-20000. `ms` 0 = until stopped. Replaces the voice's tune.
- **IO_NOTES**: `voice` 0/1. `flags`: bit 0 REPLACE (empty the tune buffer first), bit 1 LOOP (the tune repeats; stays on until the next REPLACE, TONE or STOP), bit 2 LEGATO (no gap after these notes), bits 3-7 0. `tick_ms` >= 1. Then n = (LEN - 3) / 2 notes (1-126): `note` 0 or 12-127, `ticks` >= 1. If the notes don't all fit in the buffer, none are added and the board answers NAK NO_SPACE.
- **IO_SFX**: `id` below, `voice` 0/1.
- **IO_STOP**: `voices` bit 0 = A, bit 1 = B; at least one bit, bits 2-7 must be 0.
- **IO_VOLUME**: `voices` as for STOP, `level` 0-255.
- **IO_STICK_CAL**: measure the stick centre again now. The stick must be released.
- **IO_BT**: `action` 0 SCAN_OFF (no new devices; see open question 3), 1 SCAN_ON (the default), 2 FORGET (delete every stored pairing and disconnect; the keyboard has to be paired again).

Sound effects (`haltz_io_sfx_t`; the sounds themselves are in the I/O firmware, `firmware/io-esp32/main/tunes.c`):
| ID | Name | ID | Name |
|---|---|---|---|
| 0 | BEEP | 7 | POWER_UP |
| 1 | CLICK | 8 | POWER_DOWN |
| 2 | KEY (very short tick) | 9 | BOOT (start-up chime) |
| 3 | OK | 10 | CART (cartridge read) |
| 4 | ERROR | 11 | HALT |
| 5 | COIN | 12 | HIT |
| 6 | JUMP | | |

**Errors.** Wrong length: NAK BAD_LEN. Unknown TYPE: NAK UNKNOWN_TYPE. Out-of-range values or reserved bits: NAK BAD_VALUE, and the whole command is ignored. Bad CRC: dropped without a NAK, counted in IO_STATUS. The S3 drops bad input messages the same way (counted, logged). NAKs are best effort.

## 7. Sizes and speed
At 115200 baud the link carries 11.5 KB/s each way. Each frame adds 4 bytes.
| What | Bytes on the wire | Time |
|---|---|---|
| A key press (IO_KEYS with one key) | 6 | 0.5 ms |
| Stick moving: IO_STICK at 100 Hz | 9 per message, 900 B/s | |
| Refresh + IO_STATUS, 4 times a second | about 50 per round, 200 B/s | |
| Set the LED | 11 | 1 ms |
| A 126-note tune part (IO_NOTES) | 259 | 22.5 ms |
| A whole 255-note tune buffer | 3 messages, about 520 | 45 ms |

Busy input (stick moving, typing) uses about 1.5 KB/s, an eighth of the link. Key latency is dominated by Bluetooth (a BLE keyboard reports every 7.5-15 ms), not by the UART.

## 8. I/O board side
Implemented in `firmware/io-esp32/main`, 2026-11-22.
- Core 0: BTstack and Bluepad32 (the run loop in `app_main`). HALTZ's own Bluepad32 platform (`haltz_platform.c`, replacing the template's Arduino platform) gets every keyboard report from Bluepad32's `on_controller_data` callback and puts it in a queue, together with connect and disconnect events. The Arduino layer kept only the newest report per device and the template's `loop()` read it every 150 ms, so a quick key tap between two reads would have been lost. `sketch.cpp` is no longer built.
- Core 1, task `io_link` (`io_link.c`): UART2 driver, decoder with idle flush, the command handlers, HELLO, IO_STATUS and the refresh every 250 ms. It also samples the stick, knob and buttons every 10 ms (`io_input.c`) and turns the Bluetooth queue into IO_KEYS / IO_DEVICE. Only this task writes to the UART.
- Core 1, task `io_fx` (`io_fx.c`): every 1 ms it advances the sound sequencer and sets the two buzzer timers; every 10 ms it updates the LED. The sequencer (`tunes.c`) and the LED patterns (`ledfx.c`) are portable C with PC tests (`firmware/io-esp32/test`).
- Console (Bluepad32's console on the USB port, 115200): HALTZ adds `io` (link and input state), `io_watch` (print every input message while it is sent), `io_test` (LED colours, every sound effect, a two-voice tune; for bring-up without the S3), `bt_scan on|off` and `bt_forget`.
- Boot: banner and self-test, then `io_start()` (UART, stick centre, LED, tasks, HELLO), then Bluetooth. So the S3 gets its HELLO before Bluetooth starts.

## 9. S3 side
Implemented in `firmware/cpu-s3/src/io.c` (API in `io.h`), 2026-11-22.
- An `io` driver task on core 0 (priority 7) owns UART1 RX: decoder, idle flush, handshake, health, EN reset. Commands (`io_led()`, `io_tone()`, `io_notes()`, `io_sfx()`, ...) may be called from any task: they check the values with the shared pack rules (`HALTZ_ERR_BAD_VALUE`: nothing is sent), then write the frame under a mutex, so frames never interleave.
- Kept state, sent again after every link up: the last LED pattern that isn't a FLASH, both volumes, the Bluetooth scan setting. While the link is down these calls only change the kept state; sound calls return `HALTZ_ERR_BUSY` (a sound that can't play now is just dropped).
- `io_notes()` takes a whole tune and splits it into IO_NOTES messages (REPLACE only on the first).
- Input: each IO_KEYS is compared with the previous one (`io_keys.c`, portable, PC test `firmware/cpu-s3/test/test_io_keys.c`), and every change becomes an event: KEY down / up (usage, modifiers, and the character for a US layout, with Shift, Caps Lock and Ctrl), STICK, KNOB, DEVICE, LINK. Events go into a queue (64 entries) that the kernel reads with `io_get_event()`; the latest state can also be read at any time with `io_get_input()` (for VM syscalls that poll). A refresh that changes nothing makes no event.
- Bring-up program (`io_bringup.c`, used by the bring-up screen in `gpu_bringup.c`): two status rows (link state, version, keyboard, stick, buttons, knob) and a pointer sprite that the stick moves. Typed keys appear in the console window. LED: blinks blue while no keyboard is connected, green when one is; BOOT chime at link up; the stick click plays CLICK. Keys on the S3's USB console: `i` link statistics, `p` two-voice test tune, `b` next sound effect, `l` next LED pattern, `c` re-measure the stick centre, `e` reset the I/O board, `k` link test (2000 LED commands as fast as the UART allows; PASS if `rx_frames` grew by exactly that many and no CRC error, NAK or overrun happened on either side).

## 10. Shared code
- `msg_ids.def`: the IO_* lines (0x40-0x57).
- `haltz/io.h`: link settings and timings, value limits, modifier and button bits, LED modes, sound effect IDs, note flags, Bluetooth actions, device kinds, status flags.
- `haltz/io_msgs.h`: one struct per message, `haltz_io_<name>_pack()` / `_unpack()` for each, and `haltz_io_cmd_unpack()` / `haltz_io_cmd_pack()` for any S3 -> I/O command as one union (what the I/O board's command loop calls). Pack and unpack apply the same value rules.
- `HALTZ_PROTO_VERSION` goes from 2 to 3 (the I/O link gets its message set). All three boards need re-flashing; the GPU firmware itself doesn't change.

## 11. Bring-up order
1. I/O board alone (architecture-v1 step 5): flash, then on its console `io_test` (LED colours, every effect, the two-voice tune) and `io_watch` (stick, knob and button messages while you move them). Pair the keyboard, type: IO_KEYS lines appear.
2. Link (architecture-v1 step 6): flash the S3 too. The bring-up screen shows the I/O row as UP with the board's version, the LED blinks blue, then turns green when the keyboard connects. The BOOT chime plays at link up.
3. Input on the TFT: type (the text shows in the console window), move the stick (the pointer moves, the values change), turn the knob.
4. Sound and LED from the S3: `p`, `b`, `l`.
5. Link test `k`: PASS. Then `e` (reset): the link comes back by itself, the keyboard reconnects.

## Open questions
1. Is it OK that commands get no ACK on the I/O link either (architecture-v1 says command links answer ACK/NAK)? The `rx_frames` counter shows lost commands without ACK traffic.
2. Baud rate: 115200 is plenty for v1. Raise it (921600 works on short wires) if tunes or a future feature need more?
3. Bluetooth SCAN_OFF: a BLE keyboard that is already paired reconnects only while the board scans, so SCAN_OFF also stops the keyboard from coming back after it sleeps. Keep scanning always on (the default), or find a way to reconnect bonded keyboards only?
4. Key repeat on the S3: what delay and rate (suggested 500 ms, then 30 per second)?
5. Mouse and gamepad: forward their data later (a Bluetooth mouse could drive the pointer too)?
6. Should the knob have a system meaning (architecture-v1 open question 1)? The protocol doesn't care.

# HALTZ architecture v1 (provisional)

Status: draft, rev 7, 2026-11-20. Replaces v0.
Rev 2: the micro:bits are out. The ESP32 DevKit V1 becomes the I/O board: input hub + sound (two passive buzzers = two voices) + one RGB status LED. The hardware HALT button moves to the S3's own BOOT button.
Rev 3: parts confirmed: the joystick is a wired thumbstick (KY-023 style) on the I/O board's ADC, so Bluetooth only carries the keyboard. Buzzers are 3-pin modules with magnetic (black sealed) buzzers. The RGB LED is a module with resistors. Resistors, transistors and diodes are on hand.
Rev 4: keyboard confirmed: OnMicro HS6621CG, BLE 5.1. A Classic Bluetooth keyboard may be added later; the original ESP32 already supports Classic.
Rev 5: added a 10 k potentiometer (the knob; purpose not decided, generic input for now) and a soft power button. Buzzer pins idle released (high-impedance) because the module transistor is unmarked.
Rev 6: power button confirmed as a momentary push button.
Rev 7: S3 memory plan after checking ESP-IDF's PSRAM pitfalls: the running program's 64 KB VM memory moves to internal RAM; PSRAM holds everything big; XIP from PSRAM is on so flash writes don't stall the VM. Details and the pitfall list: repo-layout-v1.md.
Main change from v0: no HDMI. The ST7789V TFT is the main screen, and the Pico GPU drives it.
Nothing has been bought; everything here uses parts the user already has. Open questions at the end.

## The idea
HALTZ is laid out like a small 1980s computer split into chips, except each "chip" is a dev board: a CPU board, a video board, an I/O board (input, sound, status light) and a cartridge slot. The boards talk over simple wired serial links that all use the same packet format. The S3 sits in the middle (star topology) and can reset every other board.

## Boards and parts
| Part | Role | Why this part |
|---|---|---|
| ESP32-S3-DevKitC-1 N16R8 | CPU ("the computer"): kernel, HALTZ VM, storage, cartridge reader, power control | Most RAM (8 MB PSRAM) and flash (16 MB), dual core |
| Raspberry Pi Pico 2 W #1 | GPU: owns the TFT, turns draw commands into pixels | 520 KB SRAM fits two full RGB565 frames; PIO + DMA |
| ESP32-DevKit V1 (original ESP32) | I/O board: Bluetooth keyboard host, thumbstick, knob, sound chip, status LED | Original ESP32 does BLE and Classic BT (the S3 is BLE only); Bluepad32 uses one core, the other is free for sound/LED/ADC |
| Raspberry Pi Pico 2 W #2 | Debug probe for Pico #1 during development; later reserve | Runs Raspberry Pi debugprobe firmware |
| ST7789V 240x320 TFT | Main screen, used landscape 320x240 | |
| RC522 + MIFARE Classic 1K | Cartridge slot | |
| 2 x passive buzzer module (GND/VCC/SIG, magnetic) | Sound, two voices | On the I/O board |
| RGB LED module (resistors on board) | Status light | On the I/O board |
| Wired thumbstick (KY-023 style) | Pointer (mouse replacement) | On the I/O board, ADC1 |
| 10 k potentiometer | The knob: a front-panel control, purpose TBD | On the I/O board, ADC1 |
| Momentary push button | Soft power on/off | On the S3 (wakes it from deep sleep) |
| Bluetooth keyboard (HS6621CG, BLE 5.1) | Text input, shortcuts, right click | Paired by the I/O board |

## Links
- S3 SPI host A (master) -> Pico #1 (slave): GPU command link. Extra lines: BUSY (Pico->S3, flow control), FRAME (Pico->S3, frame done), RESET (S3 GPIO -> Pico RUN).
- Pico #1 SPI (master) -> TFT: SCK, MOSI, CS, DC, RST, backlight.
- S3 SPI host B -> RC522: SCK, MOSI, MISO, CS (labelled SDA on the module), RST. 3.3 V only.
- S3 UART1 <-> ESP32 I/O board: TX/RX, two-way: input events up, sound and LED commands down. S3 GPIO -> ESP32 EN (reset).
- Pico #2 SWD -> Pico #1 SWD pads (development only).
- S3 USB -> PC: console, firmware flashing, uploading programs to flash.
- GND shared by every board. Mandatory.

S3 pin rules: never use GPIO35-37 (taken by the octal PSRAM on N16R8). Avoid strapping pins (GPIO0, 3, 45, 46) for wiring, native USB pins (GPIO19/20) and the UART0 console pins (GPIO43/44). GPIO0 is used only through the on-board BOOT button. Exact pins: see wiring-v1.md.

## Packet format (every link)
`SYNC 0xA5 | TYPE | LEN | PAYLOAD (0-255 bytes) | CRC8`
The same parser runs on every board; each link has its own TYPE table. Command links (GPU, sound/LED) answer with ACK/NAK. Streaming links (input) just drop bad packets.

## CPU: ESP32-S3
- Core 0: system services as FreeRTOS tasks: GPU link, I/O link, cartridge service, storage (LittleFS on flash), power control, kernel event queue.
- Core 1: the HALTZ VM only.
- Kernel: single event queue, pointer logic, halt logic, program loader, syscall table.
- HALTZ VM: a virtual CPU with its own instruction set. 16-bit address space (64 KB per program), dense bytecode so programs fit on a card, syscalls for video, sound, LED, input and cartridge save. The kernel checks a halt flag every N instructions, so a running program can always be stopped.
- Ways to halt: the S3's BOOT button (GPIO0, read as a normal input after boot; holding it while pressing RESET still enters flashing mode, as usual), a keyboard combo (TBD), pulling the cartridge, or the watchdog.
- Halt opens the monitor: shows PC/registers/stack, allows single-step, resume, or back to the launcher (MENU = a keyboard key, TBD).

Memory plan (S3):
| Where | What |
|---|---|
| Internal RAM (~512 KB, fast) | Running program's 64 KB VM memory and VM registers, all task stacks, all DMA buffers (SPI, UART), kernel tables |
| PSRAM (8 MB, slower, only via heap_caps_malloc) | App code + read-only data (XIP from PSRAM), loaded program images, file cache, system ROM assets (fonts, sprites, tunes), monitor trace/history buffers |
| Flash (16 MB) | App (~4 MB partition), filesystem for KEY-cartridge programs and settings (rest) |
XIP from PSRAM means saving a file to flash doesn't switch off the caches, so the VM keeps running during a save.

## Power (momentary push button on S3 GPIO16)
The button is "soft": every board stays plugged into USB, and "off" means standby.
- OFF (standby): the S3 is in deep sleep, holding the Pico (RUN) and the ESP32 (EN) in reset with its pin-hold feature. TFT dark, LED off, no Bluetooth. Only a little standby current is drawn.
- Press while OFF: the press wakes the S3 (ext0 wakeup on GPIO16 going LOW) -> normal boot. The S3 waits for the button to be released before it treats further presses, so the wake-up press can't also count as "off".
- Short press while ON: graceful shutdown: tell the running program, finish any cartridge save, show a goodbye screen, play a short "power down" sound, fade the LED, put the Pico and ESP32 into reset, then deep sleep.
- Hold ~4 s while ON: forced off (skip saving), like a PC.
- Button handling: internal pull-up, pressed = LOW, ~30 ms debounce in software.
- A real power cut would need all boards on one 5 V supply with a switch in it; that's a possible later step, not part of v1.

## GPU: Pico 2 W #1
- Screen 320x240 landscape.
- Modes: TEXT (8x8 font, 40x30 characters), TILES + SPRITES (16x16 tiles, 20x15 map, sprites, a cursor sprite for the pointer), BITMAP (8-bit palette).
- Memory: 320x240 at 8 bpp = 76.8 KB; at RGB565 = 153.6 KB; double-buffered RGB565 fits in 520 KB.
- Core 0 receives commands by DMA into a ring buffer, decodes them, draws into the back buffer. Core 1 pushes the finished frame to the TFT with SPI DMA (only dirty rectangles when possible), swaps buffers, pulses FRAME.
- The S3 sends commands, never raw frames: the Pico's SPI is much slower as a slave than as a master.
- Fallback: if the GPU board causes trouble, the TFT can move onto S3 SPI host B (shared with the RC522, separate CS) and the same command set runs locally on the S3. Programs don't notice.

## I/O board: ESP32-DevKit V1
Core 0 runs Bluepad32 (it uses only one core). Core 1 runs the stick/knob sampling, sound sequencer and LED.

Keyboard:
- Bluepad32 pairs the Bluetooth keyboard. Current keyboard: BLE (HS6621CG). A Classic keyboard later works on the same board, since the original ESP32 supports both.
- Sends KEY events (HID code, down/up, modifiers) and DEVICE events (connected/disconnected, battery).
- Backup: Pico 2 W also runs Bluepad32, so Pico #2 could take over the keyboard if the ESP32 has trouble with it (Bluepad32 keyboard support is beta).

Thumbstick (pointer):
- X/Y on ADC1 (GPIO34/35, full 0-3.3 V range), click on GPIO32 with pull-up. ADC1 is used because ADC2 clashes with the radio.
- Sampled at ~100 Hz with light smoothing. Centre is measured at startup (it's never exactly mid-scale); a dead zone hides wobble.
- Sends AXIS (x, y, already centred) and BUTTON events, same format as any other input.
- The S3 kernel turns AXIS into a pointer: speed grows with deflection, max speed cap. Stick click = left click. Right click = a keyboard key or modifier + click (TBD; or a spare push button on GPIO33 if one is on hand). The GPU draws the cursor sprite.

Knob (10 k potentiometer):
- Wiper on ADC1 (GPIO36), ends on 3V3 and GND. Averaged, with a small change threshold so it doesn't jitter.
- Sends KNOB (0-255) only when the value changes.
- Purpose not decided. Because it's a plain input event, the meaning can be chosen later without rewiring. Candidates:
  - VM speed: turn the HALTZ VM clock from full speed down to a few instructions per second, watching each step in the monitor. Fits the "halt" theme and the learning goal.
  - Volume (rough, the buzzers only allow duty-cycle volume).
  - Screen brightness (TFT backlight PWM on the Pico).
  - Paddle controller for games (Pong/Breakout style), and programs can read it by syscall.
  - Possible mix: system meaning by default (e.g. speed or brightness), programs can take it over while they run.

Sound (two voices):
- Each buzzer gets its own LEDC timer, so each voice can play a different note at the same time (melody + bass, or music + sound effect). ESP32 LEDC has 8 timers and 16 channels; HALTZ uses 3 timers, 5 channels.
- Commands from the S3: TONE (voice, freq, ms), NOTES (voice, list of note/length), SFX (id: beep, click, error, coin, power-up, power-down...), STOP (voice), VOLUME (rough, by changing PWM duty).
- The I/O board keeps a note queue per voice and does the timing itself, so the S3 only sends a tune once.
- Silent = pin released (input, no pull), not HIGH or LOW. The module's transistor is unmarked (PNP or NPN unknown); a released pin keeps either type off, and also a bare buzzer. A 1 k resistor sits in each SIG line.
- Passive buzzers play square waves only; volume control by duty is crude. That's the "HALTZ sound chip" sound for now.

Status LED (RGB module, 3 PWM channels on one LEDC timer, ~5 kHz):
- Blue breathing: booting. Blue blinking: waiting for Bluetooth keyboard.
- Green: running. Short white flash: cartridge read.
- Red: halted. Red blink pattern: error code.
- Fade out: shutting down.
- Programs can also set the LED by syscall.

## Cartridges: MIFARE Classic 1K (752 usable bytes)
Layout:
- Sector 0, blocks 1-2 (32 B): header: magic "HZ", format version, cart type, title (up to 16 chars), program length, CRC16.
- Sectors 1-13 (624 B): bytecode or data.
- Sectors 14-15 (96 B): save area (writable).
Cart types:
- CODE: the program is on the card.
- KEY: the card holds an ID; the real program lives in S3 flash (uploaded over USB or Wi-Fi). For anything bigger than 624 bytes.
- DATA: saves, settings, a user profile.
The card's read-only UID is its serial number.
Behaviour: S3 polls the RC522 a few times a second; card appears -> read all -> check CRC -> title on screen, white LED flash, beep -> load into VM memory -> run. Card removed -> program halts (like pulling a cartridge). Burner mode writes cards from flash. Non-default sector keys prevent accidental overwrite (not real security). About 100,000 write cycles per card: save on purpose, never every frame.
System ROM in S3 flash: fonts, sprite sheets, tunes, sound effects and shared routines that cards can call, so cards stay tiny.

## Boot
1. Power button pressed (or USB first plugged in): S3 starts, holds the other boards in reset, then releases them.
2. LED breathes blue. GPU reports READY; boot log appears on the TFT. Start-up chime. Thumbstick centre is measured (don't touch the stick during boot). Knob position read.
3. I/O board reports devices; LED blinks blue until the keyboard connects.
4. Launcher: "INSERT CARTRIDGE" plus the list of KEY programs in flash.
5. Card in -> load -> run (green). HALT -> monitor (red) -> MENU -> launcher. Power button -> shutdown -> standby.

## Firmware per board (suggested)
- S3: ESP-IDF (C), FreeRTOS.
- Pico #1: Pico SDK (C).
- ESP32 I/O board: ESP-IDF + Bluepad32, LEDC driver for sound and LED, ADC driver for the stick and knob.
- Pico #2: debugprobe firmware.
Project setup and settings: repo-layout-v1.md.

## Bring-up order
1. S3 alone: check module marking, USB console, flash/PSRAM sizes, BOOT button and power button read, deep sleep + wake on the power button.
2. RC522 on S3: print card UID, read/write a test block.
3. Pico #1 + TFT alone: test pattern, then text.
4. S3 -> Pico link: S3 prints a text console on the TFT.
5. ESP32 I/O board alone: LED colours, buzzer beep test, two-voice test tune, stick and knob values on the USB console, then pair the keyboard.
6. S3 <-> ESP32 link: keys, stick and knob show on the TFT, S3 plays sounds and sets the LED.
7. VM, cartridge loader, launcher, monitor, full power on/off sequence.
8. Pointer, sprites, tunes, knob meaning, extras.

## Electrical
All boards use 3.3 V logic, so no level shifters. RC522, thumbstick, knob and buzzer modules on 3.3 V. Common ground everywhere. During development each board runs from its own USB cable; final power plan TBD.

## Open questions
1. Knob: what should it do (see candidates)?
2. Buzzer modules: beep test result (transistor on board or not)?
3. Right click: keyboard key, or is there a spare push button?
4. Wiring: breadboards and jumper wires (male-male, male-female)? Roughly how many?
5. Power: enough USB cables and ports to run 4 boards at once (or a powered hub / 5 V supply)?
6. TFT module: SD card slot or touch? Pin labels?
7. Headers soldered on the RC522, both Picos, and Pico #1's DEBUG pads?
8. S3 module marking: WROOM-1 N16R8 (expected) or WROOM-2 / ...R8V?

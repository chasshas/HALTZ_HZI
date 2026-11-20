# HALTZ wiring v1 (provisional)

Status: draft, rev 5, 2026-11-19. Goes with architecture-v1.md. Pin choices are first proposals; change them freely at bring-up.
Rev 2: micro:bit stage removed; buzzers + RGB LED added on the ESP32.
Rev 3: parts confirmed: buzzers are 3-pin modules with black sealed (magnetic) buzzers, the RGB LED is a module with resistors, the joystick is a wired thumbstick (KY-023 style). The user has resistors, transistors and diodes.
Rev 4: buzzer modules are labelled GND / VCC / SIG. Keyboard confirmed BLE (HS6621CG).
Rev 5: buzzer transistor has no readable marking, so the buzzer pins idle released (high-impedance) when silent, which is safe for every module type. Added a 10 k potentiometer (ESP32 GPIO36) and a power button (S3 GPIO16).
Pico pins are given as GPx (physical pin number).

## Ground rules
1. Power every board from its own USB cable (ideally one powered USB hub, so they all switch on together). Never join 3V3 or 5V/VBUS between boards.
2. Join every GND. Make one ground rail on the breadboard and run a GND wire from every board and module to it.
3. Only signals and GND cross between boards. UART is crossed: TX goes to RX.
4. Don't leave one board unpowered while another drives a signal into it (it can back-power through the pin). Power everything at once, or unplug the signal wires.
5. RC522 runs on 3.3 V only. Everything here is 3.3 V logic, so no level shifters.
6. Reset lines (S3 -> Pico RUN, S3 -> ESP32 EN) are driven open-drain from the S3: pull low to reset, release to run.
7. Keep SPI wires short (under ~15 cm, TFT wires shortest). Start SPI slow (TFT ~10-20 MHz, GPU link ~1-4 MHz) and raise once it works.
8. Wire one stage at a time, in bring-up order, and test before adding the next.

## Pins to avoid
- ESP32-S3: GPIO35-37 (octal PSRAM), 0/3/45/46 (strapping; GPIO0 is only used via the BOOT button), 19/20 (native USB), 43/44 (USB-UART console), 38 or 48 (RGB LED, depends on board revision).
- Pico 2 W: GP23, 24, 25, 29 are used inside the board by the wireless chip and aren't on the header anyway.
- ESP32 DevKit V1: GPIO6-11 (flash), 0/2/5/12/15 (strapping), 1/3 (USB console), GPIO34-39 are input-only with no internal pull-ups.

## Stage 1: RC522 + power button -> ESP32-S3
### RC522 (SPI host B)
| RC522 | S3 |
|---|---|
| SDA (this is chip select) | GPIO7 |
| SCK | GPIO4 |
| MOSI | GPIO5 |
| MISO | GPIO6 |
| IRQ | not connected |
| GND | GND |
| RST | GPIO15 |
| 3.3V | 3V3 |

### Power button (soft power)
| Button | S3 |
|---|---|
| One leg | GPIO16 (internal pull-up on) |
| Other leg | GND |
GPIO16 is an RTC GPIO (S3 RTC GPIOs are 0-21), so it can wake the S3 from deep sleep (ext0 wakeup). The reset lines GPIO8 (ESP32 EN) and GPIO21 (Pico RUN) are also RTC GPIOs, so the S3 can hold them low while it sleeps.
A 4-pin tactile button has two pairs of legs that are always connected; use two legs on the same side, diagonally opposite to be safe.

## Stage 2: TFT -> Pico 2 W #1 (Pico SPI1, master)
Module labels vary; common ones shown. SCL/SDA here are SPI clock/data, not I2C.
| TFT | Pico #1 |
|---|---|
| VCC | 3V3(OUT), pin 36 |
| GND | GND, pin 38 |
| SCL / SCK / CLK | GP10, pin 14 |
| SDA / MOSI / DIN | GP11, pin 15 |
| CS | GP13, pin 17 |
| DC / RS | GP14, pin 19 |
| RES / RST | GP15, pin 20 |
| BL / BLK / LED | GP22, pin 29 |
| SDO / MISO (if present) | not connected |

## Stage 3: ESP32-S3 <-> Pico 2 W #1, GPU link (S3 SPI host A master, Pico SPI0 slave)
| Signal | S3 | Pico #1 | Direction |
|---|---|---|---|
| SCK | GPIO12 | GP18, pin 24 | S3 -> Pico |
| MOSI | GPIO11 | GP16, pin 21 | S3 -> Pico |
| MISO | GPIO13 | GP19, pin 25 | Pico -> S3 |
| CS | GPIO10 | GP17, pin 22 | S3 -> Pico |
| BUSY | GPIO9 | GP20, pin 26 | Pico -> S3 |
| FRAME | GPIO14 | GP21, pin 27 | Pico -> S3 |
| RESET | GPIO21 | RUN, pin 30 | S3 -> Pico |
| GND | GND | GND, pin 23 | |

## Stage 4: ESP32-S3 <-> ESP32 DevKit V1 I/O board (S3 UART1, ESP32 UART2)
| S3 | ESP32 |
|---|---|
| GPIO17 (TX) | GPIO16 (RX2) |
| GPIO18 (RX) | GPIO17 (TX2) |
| GPIO8 (reset) | EN |
| GND | GND |

## Stage 5: thumbstick, knob, RGB LED, buzzers -> ESP32 DevKit V1
### Thumbstick (KY-023 style)
| Thumbstick | ESP32 | Note |
|---|---|---|
| GND | GND | |
| +5V (label) | 3V3 | Use 3.3 V: the ESP32 ADC must never see 5 V |
| VRx | GPIO34 | ADC1, input-only |
| VRy | GPIO35 | ADC1, input-only |
| SW | GPIO32 | Internal pull-up on; reads LOW when pressed |
Firmware notes: ADC1 at full-range attenuation (0-3.3 V, 12-bit). Read the rest position at startup as the centre (it's never exactly mid-scale), then apply a dead zone.

### 10 k potentiometer (the knob)
| Pot pin | ESP32 |
|---|---|
| Outer pin 1 | 3V3 |
| Middle pin (wiper) | GPIO36 (labelled VP), ADC1 |
| Outer pin 2 | GND |
Swap the two outer pins if the knob turns "the wrong way". Firmware notes: same ADC settings as the stick; average several readings and only report a change bigger than a small threshold, so the value doesn't jitter. The ESP32 ADC is flat at the very ends of the range, so the last few degrees of rotation read the same.

### RGB LED module (resistors on board, common cathode)
| LED module | ESP32 |
|---|---|
| R | GPIO18 |
| G | GPIO19 |
| B | GPIO21 |
| - / GND (common) | GND |

### Buzzer modules (GND / VCC / SIG, black sealed buzzer, unmarked transistor)
| Buzzer module | Buzzer A (voice 1) | Buzzer B (voice 2) |
|---|---|---|
| VCC | 3V3 (never 5 V) | 3V3 (never 5 V) |
| GND | GND | GND |
| SIG | 1 k resistor -> GPIO25 | 1 k resistor -> GPIO26 |

Silent = pin released. When a voice is silent, the firmware switches its pin to input (high-impedance, no pull-up or pull-down) instead of holding it HIGH or LOW. With no current into SIG, a PNP or NPN transistor stays off and a bare buzzer gets no current, so the idle state is safe whatever is on the module. At reset the ESP32 pins start as inputs too, so it's also silent while booting.
Playing a note: the pin outputs a square wave (50% duty), which works the same for PNP, NPN or bare buzzer.

Beep test (first power-up): play ~2 kHz.
- Clear beep: a transistor is on board. Keep this wiring; the 1 k stays.
- Barely audible: no transistor. Build one NPN driver per buzzer: GPIO -> 1 k -> base; emitter -> GND; collector -> buzzer "-"; buzzer "+" -> 3V3 (or VIN for louder); diode across the buzzer, stripe on the "+" side. Check the NPN pinout on its datasheet.

GPIO25/26 are also the ESP32's DAC pins, which could allow better audio later.

## Stage 6: Pico 2 W #2 (debug probe) -> Pico 2 W #1 (development only)
Pico #1's DEBUG pads are the 3 small holes labelled DEBUG in the middle of the board (a small 3-pin JST socket on the "WH" version). Check the labels on the underside: SWCLK, GND, SWDIO.
| Pico #2 (debugprobe firmware) | Pico #1 |
|---|---|
| GP2, pin 4 | DEBUG SWCLK |
| GP3, pin 5 | DEBUG SWDIO |
| GND, pin 3 | DEBUG GND |
| GP4, pin 6 (probe UART TX) | GP1, pin 2 (UART0 RX) |
| GP5, pin 7 (probe UART RX) | GP0, pin 1 (UART0 TX) |
The probe does not power Pico #1; Pico #1 keeps its own USB.

## Spare pins after all stages
- S3: GPIO1, 2, 47, 39-42 (39-42 are JTAG pins, free while using USB for debugging). UART2 is free.
- Pico #1: GP2-9, GP12, GP26-28.
- ESP32: GPIO4, 13, 14, 22, 23, 27, 33, 39 (39 is input-only).

## Wire count (approx.)
RC522 8, power button 2, TFT 8, GPU link 8, I/O link 4, thumbstick 5, knob 3, LED 4, buzzers 6 (+2 resistors), probe 5: about 53 wires, plus jumpers to the ground rail.

## Open questions
- Power button: momentary push button or a latching switch?
- Buzzer modules: beep test result?
- Right click: keyboard key (default), or a spare push button on ESP32 GPIO33.
- TFT pin labels (exact names on the module)?
- Are headers soldered on both Picos, and on Pico #1's DEBUG pads?
- Breadboards and jumper types?

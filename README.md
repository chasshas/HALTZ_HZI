# HALTZ

HALTZ is a hobby homebrew computer built from several dev boards that talk to each other over
wires. An ESP32-S3 runs the kernel and the HALTZ virtual machine. A Raspberry Pi Pico 2 W drives
the display, and a classic ESP32 handles input, sound and the status LED. All boards speak one
small framed protocol (`shared/haltz_proto`).

## Boards

| Board | Role | Project folder |
|---|---|---|
| ESP32-S3-DevKitC-1, ESP32-S3-WROOM-1 N16R8 (16 MB quad flash, 8 MB octal PSRAM) | CPU: kernel + HALTZ virtual machine | `firmware/cpu-s3` |
| ESP32 DevKit V1 (ESP32-WROOM-32, 30-pin) | I/O: Bluetooth keyboard (Bluepad32), thumbstick, knob, 2 buzzers, RGB LED | `firmware/io-esp32` |
| Raspberry Pi Pico 2 W #1 (RP2350) | GPU: drives a 240x320 ST7789V TFT | `firmware/gpu-pico` |
| Raspberry Pi Pico 2 W #2 | Debug probe (official debugprobe UF2) | none |

Links: S3 SPI host A (master) to Pico #1 SPI0 (slave) for GPU commands. S3 UART1 to ESP32
UART2 for input events, sound and LED. S3 SPI host B to an RC522 card reader. Pins are in each
project's `board_pins.h`.

## Repo layout

```
haltz.code-workspace   VS Code multi-root workspace
shared/haltz_proto/    C11 protocol library (frames, CRC-8, messages) + PC unit tests
firmware/cpu-s3/       PlatformIO, ESP-IDF, C
firmware/io-esp32/     PlatformIO, ESP-IDF + Arduino + Bluepad32, C++ (from the Bluepad32 template)
firmware/gpu-pico/     Pico SDK 2.x, C
tools/                 host-side Python tools (none yet)
docs/                  pointer to the design docs
```

## Opening the workspace

Open `haltz.code-workspace` in VS Code (File > Open Workspace from File...). Each firmware
project is its own root, so the PlatformIO and Raspberry Pi Pico extensions each find their
projects. Install the recommended extensions when VS Code offers them: PlatformIO IDE,
Raspberry Pi Pico, C/C++, CMake Tools, Python.

The workspace has the tasks **build cpu**, **build io**, **build gpu** and **test shared**
(Terminal > Run Task...). They expect `pio`, `cmake` and `ninja` on `PATH`; **build io** calls
io-esp32's own `pio` (see below). The PlatformIO extension provides `pio` in its own
terminals. The Pico extension keeps CMake and Ninja under `~/.pico-sdk/`.

## Building

Run all commands from the repo root.

Each ESP project has its own PlatformIO folder (`core_dir` in its `platformio.ini`), because
the two use different versions of the same packages and would otherwise keep replacing each
other's copies (and those of your other PlatformIO projects) in `~/.platformio`:

| Project | Platform | ESP-IDF | PlatformIO folder | Build with |
|---|---|---|---|---|
| cpu-s3 | `espressif32 @ 6.13.0` | 5.5.3 | `~/.platformio-haltz-cpu` | normal `pio` (Core 6.2.0) |
| io-esp32 | pioarduino `55.03.31-2` (template pin) | 5.5.1 | `~/.platformio-haltz-io` | `~/.platformio-haltz-io/penv/bin/pio` |

**cpu-s3:**

```bash
pio run -d firmware/cpu-s3
```

**io-esp32:**

```bash
~/.platformio-haltz-io/penv/bin/pio run -d firmware/io-esp32
```

io-esp32 needs pioarduino's fork of PlatformIO Core (6.1.18, it pins SCons 4.8.1). The fork
lives only in `~/.platformio-haltz-io/penv`, so your normal `pio` stays the official one. The
PlatformIO IDE buttons use the normal `pio`, so build io-esp32 with the **build io** task or
the command above. To recreate that environment on a new machine:

```bash
~/.platformio/python3/bin/python3 -m venv ~/.platformio-haltz-io/penv
```

```bash
~/.platformio-haltz-io/penv/bin/python -m pip install https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.18.zip
```

The first build of each project downloads its ESP-IDF and toolchains (several GB; keep
~12 GB free for both).

After changing a `sdkconfig.defaults`, delete the generated config
(`firmware/cpu-s3/sdkconfig.cpu` or `firmware/io-esp32/sdkconfig.esp32dev`) before building.
ESP-IDF only takes defaults for options that aren't in the generated file yet, so changed
values would otherwise be ignored.

Serial ports: on macOS they look like `/dev/cu.usbserial-…` (USB-UART chip, e.g. the
DevKits' UART port) or `/dev/cu.usbmodem…` (native USB, e.g. the S3's USB port), on Windows
`COMx`. List them with `pio device list`, then set `upload_port` / `monitor_port` in each
`platformio.ini`.

If the cpu-s3 build stops at `bootloader.bin` with `No module named 'intelhex'`, the esptool
shipped with espressif32 6.13.0 is missing a dependency. Install it into PlatformIO's own
Python once:

```bash
~/.platformio/penv/bin/python -m pip install intelhex
```

Both ESP projects add `shared/haltz_proto` with `list(APPEND EXTRA_COMPONENT_DIRS ...)`.
Keep it an append: PlatformIO passes its own source folder in that variable, and `set()`
would drop it ("Couldn't find the main target of the project!").

`firmware/io-esp32` is a plain copy (no submodules) of
[esp-idf-arduino-bluepad32-template](https://github.com/ricardoquesada/esp-idf-arduino-bluepad32-template)
at commit `2c881858` (2026-10-13), with `components/arduino` and `external/btstack` from its
submodules. Its platform pin (pioarduino `55.03.31-2`, ESP-IDF 5.5) is kept as the template
ships it. HALTZ changes: only the `esp32dev` environment, the `haltz_proto` component,
`main/board_pins.h` and `main/haltz_selftest.cpp`. The optional btstack patch in
`external/patches` (DualShock 3 support) is not applied.

**gpu-pico** (Pico SDK 2.3.1, board `pico2_w`, platform `rp2350-arm-s`): open the
`gpu-pico` folder with the Raspberry Pi Pico extension and press *Compile*, or from a shell:

```bash
cmake -S firmware/gpu-pico -B firmware/gpu-pico/build -G Ninja
```

```bash
cmake --build firmware/gpu-pico/build
```

Without the extension, set `PICO_SDK_PATH` to your SDK checkout first. The output is
`firmware/gpu-pico/build/haltz_gpu.uf2` (plus `.elf` for the debug probe).

Each firmware embeds the first 8 hex characters of the current git commit in its HELLO
message, or `00000000` if git isn't available.

## Wiring test firmware

Separate builds that check the wires of `docs/wiring-v1.md`, stage by stage. They are not the
real firmware: flash the normal builds back when the wiring is done.

| Board | Build | Checks |
|---|---|---|
| Pico #1 | `firmware/gpu-pico/build/haltz_gpu_wiring.uf2` (built with the normal gpu build) | Stage 2: TFT test pattern. The Pico end of stage 3. Stage 6: its console comes over the probe UART |
| ESP32 | `~/.platformio-haltz-io/penv/bin/pio run -d firmware/io-esp32 -e esp32dev_wiring -t upload -t monitor` | Stage 5: beep test, LED colours, live thumbstick / knob / stick button. The ESP32 end of stage 4 |
| S3 | `pio run -d firmware/cpu-s3 -e cpu_wiring -t upload -t monitor` | Stage 1: RC522 and power button. Stage 3: GPU link. Stage 4: I/O link |

Flash the Pico and ESP32 test builds first, then the S3: its tests run at boot. It prints
`[ OK ]` or `[FAIL]` for each wire, with the wire to check, and a summary. Press **BOOT** on
the S3 to run the tests again. A stage that isn't wired yet just shows FAIL or SKIP.
After the tests, the S3 prints power button presses and the UID of any card you tap.

How the link tests work: the Pico copies CS to FRAME and SCK to BUSY, then echoes SPI blocks
inverted (mode 3, 1 MHz); the ESP32 sends a boot banner and echoes UART bytes inverted
(115200). The S3 also pulses RESET and EN and checks that the other board stops and restarts.
If the GPU link fails, the Pico console shows what it received on each line.

The TFT pattern is full red, green, blue, white, black, then colour bars with a white frame
and corner squares (red top-left, green top-right, blue bottom-left), then a backlight fade.
Wrong colours or orientation are settings, not wiring: change `TFT_INVERT`, `TFT_BGR` or
`TFT_MADCTL` at the top of `firmware/gpu-pico/src/wiring_test.c`. A white screen with no
pattern means SCK, MOSI, CS, DC or RES; a dark screen means BL or power.

Stage 6 (SWD) is checked by flashing Pico #1 through the probe.

## Shared library tests (PC)

```bash
cmake -S shared/haltz_proto -B build-host -DHALTZ_BUILD_TESTS=ON
```

```bash
cmake --build build-host
```

```bash
./build-host/haltz_proto_tests
```

## Flashing the debug probe (Pico 2 W #2)

1. Download the Pico 2 build of debugprobe (`debugprobe_on_pico2.uf2`) from the
   [Raspberry Pi debugprobe releases](https://github.com/raspberrypi/debugprobe/releases).
2. Hold the **BOOTSEL** button on Pico #2 and plug it into USB. It mounts as a drive
   (`RP2350`).
3. Drag the `.uf2` file onto that drive. The board reboots as a CMSIS-DAP probe with a
   USB serial port.
4. Wire it to Pico #1: probe SWD to the Pico #1 SWD pads, and probe UART to Pico #1 GP0 (TX)
   / GP1 (RX), which carries the GPU's stdio.

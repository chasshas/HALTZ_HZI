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
(Terminal > Run Task...). They expect `pio`, `cmake` and `ninja` on `PATH`. The PlatformIO
extension provides `pio` in its own terminals. The Pico extension keeps CMake and Ninja under
`~/.pico-sdk/`.

## Building

Run all commands from the repo root.

**cpu-s3** (PlatformIO pins `espressif32 @ 6.13.0`):

```bash
pio run -d firmware/cpu-s3
```

**io-esp32** (PlatformIO, the template's pioarduino platform pin, env `esp32dev`):

```bash
pio run -d firmware/io-esp32
```

The first build of each downloads its ESP-IDF and toolchain into `~/.platformio` (several GB,
keep ~12 GB free for both).

If the cpu-s3 build stops at `bootloader.bin` with `No module named 'intelhex'`, the esptool
shipped with espressif32 6.13.0 is missing a dependency. Install it into PlatformIO's own
Python once:

```bash
~/.platformio/penv/bin/python -m pip install intelhex
```

Known issue: cpu-s3 (official espressif32 6.13.0, ESP-IDF 5.5.3) and io-esp32 (pioarduino
55.03.31-2, ESP-IDF 5.5.1) use different versions of the same PlatformIO packages and replace
each other's copies. The first build after switching projects re-downloads them, and it can
take a long time. If io-esp32 then fails with "Multiple ways to build the same target",
delete `firmware/io-esp32/.pio/build` and build again. The pioarduino platform also replaces
PlatformIO Core in `~/.platformio/penv` with its own fork (6.1.18).

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

// SPDX-License-Identifier: Apache-2.0
// Copyright 2019 Ricardo Quesada
// http://retro.moe/unijoysticle2
//
// HALTZ: from the Bluepad32 template's main.c. Bluepad32 runs with HALTZ's own platform
// (haltz_platform.c) instead of the Arduino one, and the I/O link (io_link.c) starts before
// Bluetooth. docs/io-protocol-v1.md section 8.

#include "sdkconfig.h"

#include <stddef.h>
#include <stdio.h>

// BTstack related
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>

// Bluepad32 related
#include <uni.h>

#include "haltz_platform.h"
#include "io_link.h"

// HALTZ boot banner + HELLO self-test (haltz_selftest.cpp)
void haltz_io_selftest(void);
#ifdef HALTZ_WIRING_TEST
// HALTZ wiring test (haltz_wiring_test.c, env esp32dev_wiring). Never returns.
void haltz_io_wiring_test(void);
#endif

#if CONFIG_AUTOSTART_ARDUINO
#error "HALTZ uses its own Bluepad32 platform: keep CONFIG_AUTOSTART_ARDUINO=n"
#endif

int app_main(void) {
    haltz_io_selftest();
#ifdef HALTZ_WIRING_TEST
    haltz_io_wiring_test();  // Bluetooth is not started in the wiring test
#endif

    // Buzzers, LED, stick, knob and the UART link to the S3: before Bluetooth, so the S3
    // gets its HELLO early (protocol section 2).
    if (io_start() != ESP_OK) {
        printf("io: the link did not start; Bluetooth starts anyway\n");
    }

    // hci_dump_open(NULL, HCI_DUMP_STDOUT);

// Don't use BTstack buffered UART. It conflicts with the console.
#ifndef CONFIG_ESP_CONSOLE_UART_NONE
#ifndef CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
    btstack_stdio_init();
#endif  // CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
#endif  // CONFIG_ESP_CONSOLE_UART_NONE

    // Configure BTstack for ESP32 VHCI Controller
    btstack_init();

    // hci_dump_init(hci_dump_embedded_stdout_get_instance());

    // Must be called before uni_init()
    uni_platform_set_custom(haltz_platform_get());

    // Init Bluepad32.
    uni_init(0 /* argc */, NULL /* argv */);

    // Does not return.
    btstack_run_loop_execute();
    return 0;
}

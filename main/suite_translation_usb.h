#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Translation-only USB Serial/JTAG provisioning window.
 *
 * Never uses UART0 (GPIO21 is the backlight). A user-selected window lasts at
 * most 60 seconds, installs the native USB Serial/JTAG driver, and runs exactly
 * one reader while normal console logging continues through the same VFS.
 * Outside a window the driver and reader are absent.
 */

bool suite_translation_usb_open_window(void);
void suite_translation_usb_cancel(void);
bool suite_translation_usb_window_open(void);

/* Optional translation-runtime busy predicate for later UI wiring. The window
   already refuses to open while a shared audio scratch lease is held. */
void suite_translation_usb_set_busy_check(bool (*busy)(void));

#pragma once
#include <stdint.h>

// Call once at startup, before any other kbd_ function.
void kbd_input_init(void);

// Poll once; returns the key code (see keyboard_define.h / AGI key constants)
// or -1 if no key was pressed. Blocks ~16 ms for the I2C round-trip.
// Shift+F1-F5 is transparently mapped to F6-F10.
int kbd_read(void);

#include "kbd_input.h"
#include "i2ckbd.h"
#include <stdbool.h>

// Key code constants (matches PicoCalc keyboard_define.h)
#define KEY_BACKSPACE   0x08
#define KEY_TAB         0x09
#define KEY_ENTER       0x0A
#define KEY_MOD_ALT     0xA1
#define KEY_MOD_SHL     0xA2  // left shift
#define KEY_MOD_SHR     0xA3  // right shift
#define KEY_MOD_SYM     0xA4
#define KEY_MOD_CTRL    0xA5
#define KEY_ESC         0xB1
#define KEY_LEFT        0xB4
#define KEY_UP          0xB5
#define KEY_DOWN        0xB6
#define KEY_RIGHT       0xB7
#define KEY_CAPS_LOCK   0xC1
#define KEY_BREAK       0xD0
#define KEY_INSERT      0xD1
#define KEY_HOME        0xD2
#define KEY_DEL         0xD4
#define KEY_F1          0x81
#define KEY_F5          0x85

static bool shift_held = false;

void kbd_input_init(void) {
    init_i2c_kbd();
    shift_held = false;
}

int kbd_read(void) {
    int c = read_i2c_kbd();
    if (c < 0) return -1;

    // Track shift modifier; consume the event without returning a key.
    if (c == KEY_MOD_SHL || c == KEY_MOD_SHR) {
        shift_held = true;
        return -1;
    }

    // The keyboard firmware pre-combines Shift+F1-F5 into F6-F10 codes.
    // F6-F9 arrive as 0x86-0x89; F10 arrives as 0x90 (firmware quirk).
    // Those codes fall through the range check below and are returned as-is.

    // Any non-modifier key clears shift state.
    shift_held = false;
    return c;
}

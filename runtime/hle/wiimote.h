// Wii Remote + Nunchuk (runtime/hle/wiimote.c): what the remote reports, for input.c, motion.c and the menu.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Buttons (the remote's own bits, its two core bytes)
enum {
    WM_LEFT = 0x0001, WM_RIGHT = 0x0002, WM_DOWN = 0x0004, WM_UP = 0x0008, WM_PLUS = 0x0010,
    WM_TWO = 0x0100, WM_ONE = 0x0200, WM_B = 0x0400, WM_A = 0x0800, WM_MINUS = 0x1000, WM_HOME = 0x8000,
};

typedef struct { int x, y; bool valid; } WiimoteDot;  // an IR camera dot: 0..1023 x 0..767

typedef struct {
    bool connected, plus, nunchuk, ext_plugged;
    char name[64];
    uint16_t buttons;           // WM_*
    float accel[3];             // G, the remote's own axes
    WiimoteDot dots[4];         // the IR camera's
    uint64_t ir_ns;             // when the last IR report came
    float stick[2];             // the Nunchuk's, -1..1 (y up)
    bool nunchuk_c, nunchuk_z;
    float nunchuk_accel[3];     // G (roughly)
    float battery;              // %
    bool pointer_valid;         // the sensor bar seen just now
    float pointer[2];           // where the remote points on the screen: -1..1 at its edges (x right, y up), calibrated
} WiimoteState;

void wiimote_start(void);                          // the reader thread (after SDL_Init)
void wiimote_state(WiimoteState *out);             // a copy, with the pointer worked out
bool wiimote_calibrate_middle(void);               // the pointer now = the screen's middle (false: no sensor bar seen)
void wiimote_rumble(bool on);
bool wiimote_test_pointer(uint64_t frame, float *x, float *y);  // LARECOMP_WIIMOTE_TEST

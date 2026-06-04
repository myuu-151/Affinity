// Affinity PSP runtime — input sampling. Maps PSP buttons to the engine's
// KEY_* bitmasks and derives edge (pressed/released) state + analog direction.
#include "input.h"
#include <pspctrl.h>

unsigned int afn_keys_held = 0;
unsigned int afn_keys_pressed = 0;
unsigned int afn_keys_released = 0;
int afn_input_fwd = 0;
int afn_input_right = 0;

void input_update(void) {
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    unsigned b = pad.Buttons;
    unsigned int k = 0;
    if (b & PSP_CTRL_CROSS)    k |= KEY_A;
    if (b & PSP_CTRL_CIRCLE)   k |= KEY_B;
    if (b & PSP_CTRL_SQUARE)   k |= KEY_X;
    if (b & PSP_CTRL_TRIANGLE) k |= KEY_Y;
    if (b & PSP_CTRL_LTRIGGER) k |= KEY_L;
    if (b & PSP_CTRL_RTRIGGER) k |= KEY_R;
    if (b & PSP_CTRL_START)    k |= KEY_START;
    if (b & PSP_CTRL_SELECT)   k |= KEY_SELECT;
    if (b & PSP_CTRL_UP)       k |= KEY_UP;
    if (b & PSP_CTRL_DOWN)     k |= KEY_DOWN;
    if (b & PSP_CTRL_LEFT)     k |= KEY_LEFT;
    if (b & PSP_CTRL_RIGHT)    k |= KEY_RIGHT;

    // Analog stick also drives the d-pad keys (and the raw fwd/right for nodes).
    int ax = (int)pad.Lx - 128, ay = (int)pad.Ly - 128;
    if (ay < -48) k |= KEY_UP;
    if (ay >  48) k |= KEY_DOWN;
    if (ax < -48) k |= KEY_LEFT;
    if (ax >  48) k |= KEY_RIGHT;
    afn_input_right = ax;
    afn_input_fwd   = -ay;   // forward positive when stick pushed up

    afn_keys_pressed  = k & ~afn_keys_held;
    afn_keys_released = ~k & afn_keys_held;
    afn_keys_held     = k;
}

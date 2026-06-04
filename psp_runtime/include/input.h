// Affinity PSP runtime — input. Engine key bitmasks (GBA/DS layout, the values
// the generated script code uses via key_is_down/key_hit/key_released) mapped
// to PSP buttons. ABXY -> Cross/Circle/Square/Triangle.
#pragma once

#define KEY_A       0x0001   // Cross
#define KEY_B       0x0002   // Circle
#define KEY_SELECT  0x0004
#define KEY_START   0x0008
#define KEY_RIGHT   0x0010
#define KEY_LEFT    0x0020
#define KEY_UP      0x0040
#define KEY_DOWN    0x0080
#define KEY_R       0x0100   // R trigger
#define KEY_L       0x0200   // L trigger
#define KEY_X       0x0400   // Square
#define KEY_Y       0x0800   // Triangle

extern unsigned int afn_keys_held;
extern unsigned int afn_keys_pressed;   // edge: pressed this frame
extern unsigned int afn_keys_released;   // edge: released this frame
extern int afn_input_fwd;                // analog stick: forward (+up), -127..127
extern int afn_input_right;              // analog stick: right (+right), -127..127

void input_update(void);   // sample sceCtrl, build the bitmasks (call once/frame)

static inline int key_is_down(int m)  { return (afn_keys_held     & m) != 0; }
static inline int key_hit(int m)      { return (afn_keys_pressed  & m) != 0; }
static inline int key_released(int m) { return (afn_keys_released  & m) != 0; }

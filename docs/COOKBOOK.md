# Affinity Custom Code Cookbook

Practical recipes for Custom Code nodes — grouped by goal, not by symbol. Paste each snippet inside the `{ ... }` of the relevant event node's **Mode 4 Runtime** pane.

Symbol-level reference: [REFERENCE.md](REFERENCE.md) and [AFN_API.md](AFN_API.md).

---

## Movement & physics

### Jump (one-shot impulse)

Wire to **OnKeyPressed (A)**.

```c
if (player_on_ground) player_vy = 512;  // 2.0 in 16.8 fixed
```

### Double jump

OnKeyPressed:
```c
static int jumps = 0;
if (player_on_ground) jumps = 0;
if (jumps < 2) { player_vy = 480; jumps++; }
```

### Bounce on landing

OnUpdate:
```c
if (player_on_ground && player_vy < -200) {
    player_vy = -(player_vy * 192) >> 8;  // ~75% reflection
}
```

### Wall jump (after collision)

Wire to **OnCollision**:
```c
if (key_is_down(KEY_A) && player_vy < 0) {
    player_vy = 480;
    afn_force_x += (lu_cos(orbit_angle) * -200) >> 8;
    afn_force_z -= (lu_sin(orbit_angle) * -200) >> 8;
}
```

### Dash with cooldown

3 nodes — OnStart (declare), OnKeyPressed B (trigger), OnUpdate (apply):

```c
// OnStart
int dash_cd = 0;
int dash_frames = 0;
FIXED dash_vx = 0, dash_vz = 0;
```

```c
// OnKeyPressed B
if (dash_cd == 0) {
    dash_vx =  (lu_cos(orbit_angle) * 64) >> 8;
    dash_vz = -(lu_sin(orbit_angle) * 64) >> 8;
    dash_frames = 10;
    dash_cd = 60;
}
```

```c
// OnUpdate
if (dash_cd > 0) dash_cd--;
if (dash_frames > 0) {
    player_x += dash_vx;
    player_z += dash_vz;
    dash_frames--;
}
```

### Zero gravity zone

OnUpdate, with a state global controlling the zone:
```c
if (zero_g) {
    afn_gravity = 0;
    if (key_is_down(KEY_UP))   player_vy += 32;
    if (key_is_down(KEY_DOWN)) player_vy -= 32;
} else {
    afn_gravity = 48;  // restore default
}
```

### Push player around

OnUpdate or OnCollision:
```c
afn_force_x += 64;   // east
afn_force_z -= 32;   // north
// decays via afn_friction each frame
```

### Snap to a position (e.g. teleport)

```c
player_x = 32768;  // (128 << 8)
player_y = 0;
player_z = 32768;
player_vy = 0;
```

---

## Camera

### Lock camera angle

OnCollision (e.g. cutscene trigger):
```c
static int saved_angle = 0;
saved_angle = orbit_angle;
afn_cam_locked = 1;
```

OnUpdate (re-clamp each frame):
```c
if (afn_cam_locked) orbit_angle = saved_angle;
```

### Spin camera (cinematic)

OnUpdate, with a flag:
```c
if (spinning) {
    orbit_angle += 256;
    if ((afn_frame_count & 0x7F) == 0) spinning = 0;  // stop after ~2s
}
```

### Set field of view dynamically

```c
cam_h = 4096;  // raise camera 16 px
```

### Screen shake

```c
afn_shake_intensity = 4;
afn_shake_frames = 30;
```

### Fade out

```c
afn_fade_target = 16;
afn_fade_frames = 30;
afn_fade_counter = 0;
```

---

## Input

### Test a button

```c
if (key_is_down(KEY_A))      { /* held */ }
if (key_hit(KEY_START))      { /* this frame only */ }
```

### Two-button combo

```c
if (key_is_down(KEY_L) && key_is_down(KEY_R)) {
    // special action
}
```

### Track last key for menus

```c
afn_last_key = 0;
if (key_hit(KEY_UP))    afn_last_key = KEY_UP;
if (key_hit(KEY_DOWN))  afn_last_key = KEY_DOWN;
```

---

## Flags & variables

### Toggle a flag

```c
afn_flags ^= (1u << 3);  // toggle flag #3
```

### Check if multiple flags are set

```c
if ((afn_flags & ((1u << 0) | (1u << 5))) == ((1u << 0) | (1u << 5))) {
    // both flag 0 and 5 are set
}
```

### Per-slot counter (e.g. coins)

```c
afn_vars[2]++;             // increment coin counter
if (afn_vars[2] >= 100) {  // every 100 coins...
    afn_vars[2] = 0;
    afn_score += 1000;     // bonus!
}
```

### Manual RNG roll

```c
afn_rng = afn_rng * 1103515245 + 12345;
int roll = (afn_rng >> 16) & 0x7F;  // 0..127
if (roll < 32) { /* 25% chance */ }
```

---

## HP / damage

### Damage on collision

OnCollision:
```c
afn_hp[0] -= 10;
if (afn_hp[0] < 0) afn_hp[0] = 0;
afn_flash_obj[0] = 8;  // visual feedback: flash white
```

### Heal over time

OnUpdate:
```c
static int heal_tick = 0;
if (++heal_tick >= 60) {  // every 1 sec
    heal_tick = 0;
    if (afn_hp[0] < afn_max_hp[0]) afn_hp[0]++;
}
```

### One-hit kill on death

OnUpdate:
```c
if (afn_hp[0] <= 0) {
    afn_pending_scene = afn_current_scene;  // reload
    afn_pending_scene_mode = afn_current_mode;
}
```

---

## Score / pickups

### Add score on collision

OnCollision:
```c
afn_score += 100;
afn_sprite_visible[2] = 0;  // hide the picked-up sprite
afn_collision_enabled[2] = 0;
```

### Persist progress to SRAM

OnKeyPressed (e.g. menu Save button):
```c
afn_sram_save();  // persists afn_flags, afn_score, afn_vars
```

OnStart of "Continue" menu:
```c
afn_sram_load();
```

---

## Sprites

### Hide / show

```c
afn_sprite_visible[3] = 0;  // hide sprite 3
afn_sprite_visible[3] = 1;  // show sprite 3
```

### Tint red for 30 frames

```c
afn_sprite_tint[0] = 0x001F;   // RGB15 red
afn_flash_obj[0] = 30;
```

### Rotate sprite

```c
afn_sprite_rot[0] += 1024;  // ~5.6° per frame
```

### Pulse scale (use g_sprites directly)

```c
int pulse = lu_sin(afn_frame_count * 1024) >> 12;  // -255..+255
g_sprites[0].scale = 256 + (pulse >> 2);  // ~256 ± 64
```

### Move sprite toward target

```c
FIXED dx = g_sprites[1].wx - g_sprites[0].wx;
FIXED dz = g_sprites[1].wz - g_sprites[0].wz;
if (dx > 64) g_sprites[0].wx += 32;
else if (dx < -64) g_sprites[0].wx -= 32;
if (dz > 64) g_sprites[0].wz += 32;
else if (dz < -64) g_sprites[0].wz -= 32;
```

### Spawn projectile via OAM (advanced)

```c
static FIXED proj_x = 0, proj_z = 0;
static int proj_active = 0;
if (key_hit(KEY_R) && !proj_active) {
    proj_x = player_x;
    proj_z = player_z;
    proj_active = 1;
}
if (proj_active) {
    proj_x += (lu_cos(orbit_angle) * 32) >> 8;
    proj_z -= (lu_sin(orbit_angle) * 32) >> 8;
    // ...project to screen, write OAM at slot 30
}
```

---

## Animation

### Force player anim

```c
afn_play_anim = 2;   // anim index 2
```

### Stop player anim (freeze on current frame)

```c
afn_play_anim = -1;     // un-override
tm_anim_timer = 0;       // stop ticking
```

### Slow-mo

```c
afn_anim_speed = 0;      // halt anim cycling
```

---

## State machine

### Transition to "attack" state

```c
afn_prev_state[0] = afn_state[0];
afn_state[0] = 3;            // 3 = attack
afn_state_timer[0] = 0;
```

### Auto-exit state after 60 frames

OnUpdate:
```c
if (afn_state[0] == 3 && afn_state_timer[0] > 60) {
    afn_prev_state[0] = afn_state[0];
    afn_state[0] = 0;        // back to idle
    afn_state_timer[0] = 0;
}
```

---

## Dialogue

### Open dialog

```c
afn_dlg_text = 5;
afn_dlg_line = 0;
afn_dlg_speaker = 1;
afn_dlg_open = 1;
```

### Advance line on A press

OnKeyPressed A:
```c
if (afn_dlg_open) afn_dlg_line++;
```

### Close dialog

```c
afn_dlg_open = 0;
afn_player_frozen = 0;  // restore input if dialog froze player
```

---

## HUD

### Set a HUD slot value

```c
afn_hud_value[0] = afn_score;  // mirror score to HUD slot 0
```

### Show / hide a HUD element

```c
afn_hud_visible[1] = 1;  // show element 1
```

### Backdrop color flash

```c
pal_bg_mem[0] = 0x001F;   // red
// next frame, set back to 0
```

---

## Scene control

### Reload current scene

```c
afn_pending_scene = afn_current_scene;
afn_pending_scene_mode = afn_current_mode;
```

### Switch to scene 3 in Mode 4

```c
afn_pending_scene = 3;
afn_pending_scene_mode = 0;
```

### Set checkpoint

```c
afn_checkpoint_x = player_x;
afn_checkpoint_z = player_z;
afn_checkpoint_set = 1;
```

### Load checkpoint

```c
if (afn_checkpoint_set) {
    player_x = afn_checkpoint_x;
    player_z = afn_checkpoint_z;
    player_vy = 0;
}
```

---

## Timers & cooldowns

### Repeat every N frames

OnUpdate:
```c
static int tick = 0;
if (++tick >= 30) {
    tick = 0;
    // fires twice per second
}
```

### Countdown timer with effect

OnStart:
```c
int level_timer = 60 * 60;  // 60 seconds at 60 fps
```

OnUpdate:
```c
if (level_timer > 0) {
    level_timer--;
    afn_hud_value[0] = level_timer / 60;  // seconds remaining
} else {
    afn_pending_scene = 99;  // game over scene
    afn_pending_scene_mode = 0;
}
```

---

## Audio

### Play a sound

```c
afn_play_sound(0);   // play sound instance 0
```

### Play SFX (raw sample to FIFO A)

```c
afn_play_sfx(2, 256, 0);  // sample 2, full gain, FIFO A
```

### Stop everything

```c
afn_stop_sound();
```

---

## Hardware tricks

### Direct palette write

```c
pal_bg_mem[0] = 0x7C00;        // red backdrop
pal_obj_mem[1 * 16 + 3] = 0x03E0;  // green at OBJ pal bank 1, slot 3
```

### Force a specific sprite OAM slot to coords

```c
((volatile u16*)0x07000000)[0 * 4] = (80 & 0xFF);                  // attr0 y
((volatile u16*)0x07000000)[0 * 4 + 1] = (50 & 0x1FF);             // attr1 x
```

### Enable alpha blend

```c
REG_BLDCNT = 0x0040 | (1 << 4);   // BG2 source, OBJ target
REG_BLDALPHA = (8 << 0) | (8 << 8);
```

### Mosaic on all sprites

```c
REG_MOSAIC = (3 << 8) | (3 << 12);  // 4-px mosaic on OBJ
REG_DISPCNT |= 0x1000;              // enable OBJ mosaic
```

---

## Math helpers

### Sine wave oscillation

```c
int wave = (lu_sin(afn_frame_count * 1024) * 16) >> 12;   // ±16 px range
g_sprites[0].wy = (200 + wave) << 8;
```

### Angle between objects

```c
FIXED dx = g_sprites[1].wx - g_sprites[0].wx;
FIXED dz = g_sprites[1].wz - g_sprites[0].wz;
int angle = ArcTan2(dz, dx);   // brads
afn_sprite_rot[0] = angle;
```

### Distance check (Manhattan-approx)

```c
FIXED dx = g_sprites[1].wx - player_x;
FIXED dz = g_sprites[1].wz - player_z;
if (dx < 0) dx = -dx;
if (dz < 0) dz = -dz;
int dist = ((dx > dz) ? dx + (dz >> 1) : dz + (dx >> 1)) >> 8;
if (dist < 32) { /* within ~32 px */ }
```

### Lerp between two values

```c
int a = 100, b = 500, t = 128;  // t in 0..256
int result = a + (((b - a) * t) >> 8);  // = 300
```

---

## Shared state across nodes

Declare without `static` in one node (file-scope global in `mapdata.h`); every other custom node in the same project can see it.

```c
// Node A (OnStart) — declares
int my_combo = 0;
int my_timer = 0;
FIXED my_anchor_x = 0, my_anchor_z = 0;
```

```c
// Node B (OnUpdate) — uses
my_timer++;
if (my_timer > 60) { my_combo = 0; my_timer = 0; }
```

```c
// Node C (OnCollision) — modifies
my_combo++;
my_timer = 0;
```

For state visible to only one node, use `static` locals.

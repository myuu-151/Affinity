# Affinity Custom Code Reference

Reference for writing Custom Code nodes — what's in scope, what isn't, and how to expose more from `main.c` when needed.

## The `afn_` prefix

`afn_` is the project namespace — short for **Affinity**. Used for both globals and functions:

- **Globals** — runtime state: `afn_flags`, `afn_vars[]`, `afn_hp[]`, `afn_score`, `afn_play_anim`, `afn_player_frozen`, `afn_pending_scene`, ...
- **Functions** — runtime helpers: `afn_play_sound()`, `afn_stop_sound()`, `afn_spawn_sprite()`, ...
- **Generated functions** (from nodes) — `afn_script_jump_5()`, `afn_bp2_custom_14()`, ...

Convention, not a type marker — same prefix for everything that belongs to the Affinity runtime.

## How custom code reaches the ROM

Custom Code nodes get emitted into `gba_runtime/include/mapdata.h` as `static inline void` functions. The body is whatever you put in the Mode 4 Runtime pane of the node (inside `{ ... }`).

`mapdata.h` is `#include`d at line 102 of `main.c`. Only symbols declared **before** that line are visible inside your custom node body.

## Visible by default

These are usable from any custom node without modifying `main.c`.

### Player state

| Symbol | Type | Notes |
| --- | --- | --- |
| `player_x` | `FIXED` (16.8) | X position, world-space |
| `player_y` | `FIXED` | Height |
| `player_z` | `FIXED` | Z position |
| `player_vy` | `FIXED` | Vertical velocity, gravity decreases each frame |
| `afn_player_height` | `FIXED` | Collision height |

### Camera / input

| Symbol | Type | Notes |
| --- | --- | --- |
| `orbit_angle` | `u16` | brads (0..65535), camera rotation around player |
| `afn_input_fwd` | `FIXED` | Forward input accumulator, zeroed each frame |
| `afn_input_right` | `FIXED` | Strafe input accumulator |
| `afn_auto_orbit_speed` | `int` | Auto-orbit rate |

### Runtime flags & counters

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_flags` | `u32` | 32-bit flag bitfield (CheckFlag / SetFlag) |
| `afn_vars[16]` | `int[]` | General-purpose user variables |
| `afn_hp[]` | `int[]` | Per-sprite HP |
| `afn_max_hp[]` | `int[]` | Per-sprite max HP |
| `afn_score` | `int` | Global score counter |
| `afn_inventory[16]` | `int[]` | Item counts per slot |
| `afn_state[16]` | `int[]` | Per-sprite state machine value |
| `afn_prev_state[16]` | `int[]` | Previous state (for OnStateEnter/Exit) |
| `afn_state_timer[16]` | `int[]` | Frames since state changed |
| `afn_frame_count` | `int` | Frames since boot |
| `afn_rng` | `u32` | RNG seed (mutate to roll) |

### Scene control

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_pending_scene` | `int` | Set to scene index to trigger scene switch (main.c polls each frame) |
| `afn_pending_scene_mode` | `int` | 0=Mode4, 1=Mode0, 2=Mode1 |
| `afn_current_scene` | `int` | Read-only, current scene index |
| `afn_current_mode` | `int` | Read-only, current mode |

### Sprite control

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_sprite_visible[]` | `u8[]` | 0=hidden, 1=visible |
| `afn_sprite_layer[]` | `int[]` | OAM priority (0=front, 3=back) |
| `afn_sprite_alpha[]` | `int[]` | Blend alpha 0-16 |
| `afn_sprite_rot[]` | `u16[]` | Rotation in brads |
| `afn_sprite_tint[]` | `u16[]` | RGB15 tint color |
| `afn_sprite_shake[]` | `int[]` | Shake countdown frames |
| `afn_sprite_flip[]` | `int[]` | Horizontal flip flag |
| `afn_collision_enabled[]` | `int[]` | Per-sprite collision toggle |
| `afn_collision_size[]` | `int[]` | Per-sprite collision radius |
| `afn_lifetime[]` | `int[]` | Destroy-after countdown |
| `afn_flash_obj[]` | `int[]` | Flash white countdown |
| `afn_ai_mode[]` | `u8[]` | AI behavior selector |

### Player / movement state

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_player_frozen` | `int` | Disables input, anim, movement |
| `afn_play_anim` | `int` | Anim override (-1 = no override) |
| `afn_move_speed` | `int` | Walk/sprint speed |
| `afn_gravity` | `int` | Per-frame downward accel |
| `afn_terminal_vel` | `int` | Max fall speed |
| `afn_anim_speed` | `int` | Anim timer increment per frame |
| `afn_force_x` | `FIXED` | Persistent force, decays via afn_friction |
| `afn_force_z` | `FIXED` | |
| `afn_friction` | `int` | Force decay (8.8 fixed, 256 = no decay) |

### Camera

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_cam_locked` | `int` | Disable orbit input |
| `afn_cam_speed` | `int` | Smoothing factor for cam-follow |
| `afn_shake_intensity` | `int` | Screen shake amplitude |
| `afn_shake_frames` | `int` | Shake duration countdown |
| `afn_fade_target` | `int` | 0-16 (0=clear, 16=black) |
| `afn_fade_frames` | `int` | Fade duration |
| `afn_fade_counter` | `int` | Fade progress |
| `afn_draw_distance` | `int` | Per-sprite/mesh cull distance |

### HUD

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_hud_value[]` | `int[]` | HUD text slot values |
| `afn_hud_visible[]` | `u8[]` | HUD element visibility |
| `afn_active_element` | `int` | Active HUD element index |
| `afn_cursor_stop` | `int` | Current cursor position |
| `afn_stop_count` | `int` | Total cursor stops |
| `afn_stop_links[]` | `int[]` | Per-stop navigation targets |
| `afn_text_color` | `u16` | RGB15 text color |
| `afn_timer_visible` | `int` | Show/hide HUD timer |
| `afn_bar_color[4]` | `u16[]` | Bar fill colors |
| `afn_bar_max[4]` | `int[]` | Bar max values |

### Dialogue / cursor

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_dlg_open` | `int` | Dialogue box open flag |
| `afn_dlg_text` | `int` | Current text ID |
| `afn_dlg_line` | `int` | Current line index |
| `afn_dlg_speaker` | `int` | Speaker portrait ID |
| `afn_dlg_choice_a` | `int` | Choice A value |
| `afn_dlg_choice_b` | `int` | Choice B value |
| `afn_dlg_choosing` | `int` | Awaiting player choice |

### Sound

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_scripts_stopped` | `int` | Master script kill switch |

### Save data

| Symbol | Type | Notes |
| --- | --- | --- |
| `afn_checkpoint_x` | `FIXED` | Saved player X |
| `afn_checkpoint_z` | `FIXED` | Saved player Z |
| `afn_checkpoint_set` | `int` | 1 = checkpoint exists |
| `afn_last_key` | `int` | Last key pressed |

## Visible functions

### Affinity runtime helpers

| Function | Notes |
| --- | --- |
| `afn_play_sound(int instanceId)` | Trigger a sound instance |
| `afn_stop_sound()` | Stop all channels |
| `afn_spawn_sprite(asset, x, z)` | Spawn new sprite (stub) |
| `afn_clone_sprite(src)` | Duplicate sprite (stub) |
| `afn_spawn_projectile(obj, asset, speed)` | Stub |
| `afn_emit_particle(type, x, z)` | Stub |
| `afn_draw_number(val, x, y)` | Stub |
| `afn_draw_text(id, x, y)` | Stub |
| `afn_draw_bar(x, y, w, fill)` | Stub |
| `afn_draw_sprite_icon(asset, x, y)` | Stub |
| `afn_clear_text()` | Stub |
| `afn_sram_save()` | Persist flags/score/vars to SRAM |
| `afn_sram_load()` | Restore from SRAM |
| `afn_apply_stop_modifiers(stop)` | Cursor stop handler |
| `sprites_colliding(a, b)` | AABB overlap test |

### libgba symbols

| Symbol | Notes |
| --- | --- |
| `key_is_down(KEY_*)` | Test held key (0 or 1) |
| `key_hit(KEY_*)` | Test press-this-frame |
| `lu_sin(brads)` | Sine LUT (returns 8.20 fixed) |
| `lu_cos(brads)` | Cosine LUT |
| `ArcTan2(y, x)` | Returns brads |
| `KEY_A`, `KEY_B`, `KEY_L`, `KEY_R`, `KEY_UP`, etc. | Bitmasks |
| `REG_*` | Hardware registers (BG/OBJ/blend/window/DMA/sound) |
| `pal_bg_mem[]` | BG palette RAM (`(volatile u16*)0x05000000`) |
| `pal_obj_mem[]` | OBJ palette RAM (`+0x200`) |
| `OAM` | Sprite attribute table (`(volatile u16*)0x07000000`) |
| `obj_aff_mem[]` | OAM affine matrices |

## NOT visible by default

Declared in `main.c` AFTER the `mapdata.h` include — out of scope for custom code.

### Globals

- `tm_anim_idx`, `tm_anim_frame`, `tm_anim_timer` — player anim cycling
- `auto_orbit_smooth` — smoothed orbit value
- `cam_x`, `cam_y`, `cam_h` — derived camera position
- `g_sprites[]` — full sprite array with per-instance state (wx, wz, asset, anim, scale, facing, etc.)
- `tm_player_tx`, `tm_player_ty` — Mode 0 tile coordinates
- `tm_obj_tx[]`, `tm_obj_ty[]`, `tm_obj_facing[]` — Mode 0 object tile coordinates
- `tm_fol_*` — Mode 0 follower trail state
- `player_on_ground`, `player_ground_y` — collision query results
- `g_m4_dir_facing[]` — sprite direction tables
- `g_scene_transition`, debug counters

### `static` helper functions

- `scene_load()`, `start_scene_transition()`, `scene_teardown()`
- `render_meshes_sw()`, `rasterize_convex()`, `clip_render_poly_tex()` — 3D rasterizer
- `update_sprites()`, `init_obj_sprites()` — sprite pipeline
- `m7_hbl()`, `m7_build_table()` — Mode 7 affine
- `render_floor_sw()`, `render_sky_m4()` — floor / sky renderers
- `afn_sound_mix()`, `afn_sound_tick()`, `afn_trigger_sample()` — audio mixer
- `collide_walls()`, `collide_floor()` — physics queries
- `switch_dir_anim_set()`, `mode0_dma_dir_facing()` — DMA tile loaders

## Exposing locked-out symbols

Two ways to make a `main.c` symbol visible to custom code.

### Option 1 — Move the declaration before the include

Cut the existing `static int tm_anim_idx;` (line ~877 in `main.c`) and paste it above the `#include "mapdata.h"` at line 102.

```c
// Near the top of main.c, before #include "mapdata.h"
static int tm_anim_idx;
static int tm_anim_frame;
static int tm_anim_timer;
static int auto_orbit_smooth;

#include "mapdata.h"
```

Anything declared above the include is in scope from inside custom node bodies.

### Option 2 — Forward declare above the include

Add a tentative declaration above the include without moving the real one:

```c
// Above #include "mapdata.h"
static int tm_anim_idx;   // tentative — real def is below
```

A second `static int tm_anim_idx;` later in the same TU collapses to one. Slightly less invasive than moving.

### Functions

For `static` functions, the same applies: add a prototype above the include, or remove `static` from the definition and declare `extern` above.

```c
// Above #include "mapdata.h"
static void scene_load(int sceneMode, int sceneIdx);
```

Then custom code can call `scene_load(0, 3);` directly.

## Custom code patterns

### State persistence

**`static` locals** persist between calls to the same node — invisible to other nodes:

```c
static int cooldown = 0;
if (cooldown > 0) cooldown--;
else if (key_is_down(KEY_B)) { cooldown = 60; /* fire */ }
```

**File-scope globals** (no `static`) at the top of any node body become shared across all custom nodes in the same `mapdata.h`:

```c
// Custom node A
int dash_cd = 0;
FIXED dash_vx = 0;
FIXED dash_vz = 0;
```

```c
// Custom node B (different event) — can read/write the above
if (dash_cd == 0) { dash_cd = 60; dash_vx = 64; }
```

### Multi-node systems

Wire one custom node per event:

| Event | Use for |
| --- | --- |
| **OnStart** | Initialize shared globals once at scene load |
| **OnUpdate** | Per-frame logic, timers, state machines |
| **OnKeyPressed / Held / Released** | Discrete input reactions |
| **OnCollision / OnCollision2D** | Contact reactions |
| **OnDeath / OnHit** | HP-driven events |
| **OnTimer** | Periodic firing |
| **OnAnyKey / OnRise** | Trigger detection |

Shared state flows through file-scope globals, not exec wires.

### Data input pins

`$0` through `$7` in custom code get substituted with the integer value of each Data Input pin at codegen time:

```c
// With Integer node wired to pin 0
player_y = $0 << 8;
```

Substitution is purely lexical — `$0` is replaced anywhere it appears, including inside comments.

### Direct hardware access

Custom code can write to any GBA register or memory region the runtime doesn't own. Examples:

```c
// Flash backdrop palette red
pal_bg_mem[0] = 0x001F;

// Move OAM sprite 0 to (50, 80)
((volatile u16*)0x07000000)[0] = 80;
((volatile u16*)0x07000000)[1] = (((volatile u16*)0x07000000)[1] & 0xFE00) | 50;

// Enable alpha blending
REG_BLDCNT = 0x0040 | (1 << 4);
REG_BLDALPHA = (8 << 0) | (8 << 8);
```

## Fixed-point conventions

| Suffix / type | Meaning |
| --- | --- |
| `FIXED` | 16.8 fixed-point (1.0 = 256). Used for player position, sprite positions, forces |
| `8.8 fixed` | Same as FIXED — 1.0 = 256 |
| `brads` | 0..65535, full circle = 65536. Used for angles (orbit_angle, sprite_rot) |
| `lu_sin/cos` output | 1.20 fixed (range ~ -1.048M to +1.048M). Multiply then `>> 12` for typical use |
| `afn_input_*` | FIXED (16.8). +/- 256 = full press |

## Project layout

| File | Purpose |
| --- | --- |
| `gba_runtime/source/main.c` | Hand-written runtime — game loop, render, physics, sound |
| `gba_runtime/include/mapdata.h` | Generated by the editor's exporter — your scene + scripts |
| `src/editor/frame_loop.cpp` | Editor UI + visual scripting graph + setActionFunc previews |
| `src/platform/gba/gba_package.cpp` | GBA exporter — turns nodes into mapdata.h |
| `src/platform/gba/gba_package.h` | Export struct definitions |

# Affinity Runtime API

Every symbol with the `afn_` prefix, grouped by purpose. All are declared in `gba_runtime/source/main.c` or emitted into `gba_runtime/include/mapdata.h`. Custom Code nodes can read/write any of these unless noted otherwise.

For symbols without the prefix (`player_x`, `orbit_angle`, etc.) see [REFERENCE.md](REFERENCE.md).

---

## Player state

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_player_height` | `FIXED` | 3072 (12 px) | Player collision height. Used by `collide_walls` and `collide_floor` for vertical overlap test |
| `afn_player_frozen` | `int` | 0 | When 1: blocks input processing in `MovePlayer`, skips anim cycling, ignores `afn_move_speed`. Cleared by `UnfreezePlayer` |
| `afn_play_anim` | `int` | -1 | Forced animation index. -1 = use movement-based default. Set by `PlayAnim`, cleared by `StopAnim` |
| `afn_move_speed` | `FIXED` | — | Walk/sprint rate. Drives `tm_move_frames = 48 / speed` (Mode 0) and `player_x += viewSin * fwd * speed >> 16` (Mode 4) |
| `afn_anim_speed` | `int` | 1 | Per-frame anim timer increment. Higher = faster cycling |
| `afn_start_x` | `FIXED` | — | Player spawn X (used by `Respawn` node) |
| `afn_start_y` | `FIXED` | — | Player spawn Y |
| `afn_start_z` | `FIXED` | — | Player spawn Z |

## Physics

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_gravity` | `FIXED` | from camera config | Per-frame `player_vy -= afn_gravity` while airborne |
| `afn_terminal_vel` | `FIXED` | from camera config | Clamp on `player_vy` to prevent infinite fall acceleration |
| `afn_force_x` | `FIXED` | 0 | Persistent force on X axis. Applied each frame, decayed by `afn_friction` |
| `afn_force_z` | `FIXED` | 0 | Same for Z |
| `afn_friction` | `int` | 256 | Per-frame multiplier on `afn_force_*` (8.8 fixed; 256 = no decay, 230 ≈ 0.9 decay) |

## Input

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_input_fwd` | `FIXED` | 0 | Forward/back input accumulator. Set by key handler each frame, consumed by movement code |
| `afn_input_right` | `FIXED` | 0 | Strafe input accumulator |
| `afn_last_key` | `int` | 0 | Most recent key pressed (raw GBA keycode) |

## Camera

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_cam_locked` | `int` | 0 | When 1: orbit input handler skips `orbit_angle` updates |
| `afn_cam_speed` | `int` | 256 | Smoothing factor for camera follow. 8.8 fixed; lower = laggier |
| `afn_auto_orbit_speed` | `int` | 0 | Auto-orbit rate when strafing. 0 = disabled. brads per frame target |
| `afn_shake_intensity` | `int` | 0 | Screen shake amplitude (pixels). Set by `ScreenShake` |
| `afn_shake_frames` | `int` | 0 | Shake duration countdown. Each frame: decrement, apply jitter to `REG_BG_OFS[2]` |
| `afn_fade_target` | `int` | 0 | Target brightness for `FadeOut`/`FadeIn` (0=clear, 16=black) |
| `afn_fade_frames` | `int` | 0 | Total fade duration in frames |
| `afn_fade_counter` | `int` | 0 | Fade progress counter |
| `afn_fade_level` | `int` | — | Computed current brightness, written to `REG_BLDY` |

## Scene control

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_pending_scene` | `int` | -1 | Set ≥ 0 to trigger scene switch. Main loop polls each frame and calls `start_scene_transition()` |
| `afn_pending_scene_mode` | `int` | -1 | Mode for the pending switch: 0=Mode4 (3D), 1=Mode0 (tilemap), 2=Mode1 (Mode 7) |
| `afn_current_scene` | `int` | 0 | Read-only. Index of currently-loaded scene |
| `afn_current_mode` | `int` | from mapdata.h | Read-only. Current render mode (0/1/2) |
| `afn_checkpoint_x` | `FIXED` | 0 | Saved player X for `SetCheckpoint`/`LoadCheckpoint` |
| `afn_checkpoint_z` | `FIXED` | 0 | Saved player Z |
| `afn_checkpoint_set` | `int` | 0 | 1 = checkpoint snapshot exists |
| `afn_scripts_stopped` | `int` | 0 | When 1: blueprint/script dispatch loop short-circuits. Set by `StopAll` |

## Flags & variables

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_flags` | `u32` | 0 | 32-bit flag bitfield. `SetFlag`/`ToggleFlag` write, `CheckFlag`/`IsFlagSet`/`GetFlag` read |
| `afn_vars[16]` | `int[]` | 0 | General-purpose user integer variables. `SetVariable`/`AddVariable`/`Increment`/`Decrement` write, `GetVariable`/`CompareVar`/`ArrayGet`/`ArraySet` access |
| `afn_rng` | `u32` | 12345 | Linear congruential RNG seed. Mutated by `RandomInt`/`GetRandom` |
| `afn_frame_count` | `int` | 0 | Frames since boot. Reset on scene load |

## Score & HP

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_score` | `int` | 0 | Global score counter. `AddScore` writes, `GetScore` reads |
| `afn_hp[16]` | `int[]` | 100 | Per-sprite hit points (slot indexed by object) |
| `afn_max_hp[16]` | `int[]` | 100 | Per-sprite max HP clamp for `HealHP`/`SetHP2` |

## Inventory

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_inventory[16]` | `int[]` | 0 | Per-slot item counts. `AddItem`/`RemoveItem`/`UseItem` mutate, `HasItem`/`GetItemCount` query |

## Sprite state (per-instance, indexed by object)

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_sprite_visible[]` | `u8[]` | 1 | 0 = hidden in OAM, 1 = rendered |
| `afn_sprite_flip[16]` | `u8[]` | 0 | Horizontal flip flag. Direction DMA mirrors tile data when set |
| `afn_sprite_layer[16]` | `u8[]` | 0 | OAM priority bits (0 = front, 3 = back) |
| `afn_sprite_alpha[16]` | `u8[]` | 16 | Blend alpha (0 = transparent, 16 = opaque). Drives `REG_BLDALPHA` |
| `afn_sprite_rot[16]` | `u16[]` | 0 | Rotation angle in brads. Drives OAM affine matrix |
| `afn_sprite_tint[16]` | `u16[]` | 0 | RGB15 tint color. Blended via `REG_BLDCNT` |
| `afn_sprite_shake[16]` | `u8[]` | 0 | Shake countdown. While > 0, OAM x/y get rand jitter |
| `afn_flash_obj[16]` | `u8[]` | 0 | Flash white countdown. While > 0, sprite palette overridden |
| `afn_ai_mode[16]` | `u8[]` | 0 | AI behavior selector (interpreted by user code) |
| `afn_sprite_anim_spr` | `int` | -1 | Target sprite index for next `SetSpriteAnim`. -1 = none |
| `afn_sprite_anim_val` | `int` | -1 | Target anim value to apply to `afn_sprite_anim_spr` |

## Collision

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_collision_enabled[16]` | `u8[]` | 1 | Per-sprite collision toggle. 0 = no overlap test vs player |
| `afn_collision_size[16]` | `int[]` | — | Per-sprite collision radius (overrides default 16) |
| `afn_collision_ignore[16]` | `int[]` | — | Per-sprite ignore-pair mask |
| `afn_collided_sprite` | `int` | -1 | Sprite index touched this frame (Mode 4). -1 = nothing |
| `afn_collided_tm_obj` | `int` | -1 | Tilemap object index touched this frame (Mode 0). -1 = nothing |

## Lifetime / spawn

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_lifetime[16]` | `int[]` | — | Per-sprite remaining frames. Decrements each frame; sprite hidden at 0 |
| `afn_patrol_home_x[16]` | `FIXED[]` | — | Patrol anchor X per sprite (used by `Patrol` node) |
| `afn_patrol_home_z[16]` | `FIXED[]` | — | Patrol anchor Z |

## State machine

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_state[16]` | `int[]` | 0 | Per-sprite state machine value. `SetState`/`TransitionState` write |
| `afn_prev_state[16]` | `int[]` | 0 | Previous state. Compared each frame to detect `OnStateEnter`/`OnStateExit` |
| `afn_state_timer[16]` | `int[]` | 0 | Frames since state changed. Read by `StateTimer` |

## HUD

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_hud_value[4]` | `int[]` | — | HUD text slot values. Text elements with `sourceSlot >= 0` read these |
| `afn_hud_visible[4]` | `u8[]` | 0 | HUD element visibility per slot |
| `afn_hud_prev_visible[4]` | `u8[]` | — | Previous-frame visibility (for transition detection) |
| `afn_hud_anim_frame[4]` | `int[]` | — | Per-HUD-layer animation frame counter |
| `afn_bar_color[4]` | `u16[]` | — | Bar fill colors for `DrawBar` |
| `afn_bar_max[4]` | `int[]` | — | Bar max values |
| `afn_bg_color` | `u16` | 0 | Backdrop palette color (`pal_bg_mem[0]`) |
| `afn_text_color` | `u16` | 0x7FFF | Text rendering color (used by `draw_text`/`draw_number`) |
| `afn_timer_visible` | `int` | 0 | Show/hide HUD timer overlay |

## Dialogue

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_dlg_open` | `int` | 0 | 1 = dialogue box open. Gated by `IsDialogueOpen` |
| `afn_dlg_text` | `int` | 0 | Current text ID being shown |
| `afn_dlg_line` | `int` | 0 | Current line index within the text |
| `afn_dlg_speaker` | `int` | 0 | Speaker portrait/name index |
| `afn_dlg_choice_a` | `int` | 0 | First branch value for `DialogueChoice` |
| `afn_dlg_choice_b` | `int` | 0 | Second branch value |
| `afn_dlg_choosing` | `int` | 0 | 1 = awaiting player choice input |

## Draw distance

| Symbol | Type | Default | What it does |
| --- | --- | --- | --- |
| `afn_draw_distance` | `int` | 0 | Global far cull. 0 = unlimited. Sprites/meshes beyond skip rendering |

## Sound — read by mixer (advanced)

These drive the audio mixer. Editor's Sound tab manages them — touching from custom code is risky.

| Symbol | Type | What it does |
| --- | --- | --- |
| `afn_snd_voices[]` | from mapdata.h | Per-instance max simultaneous voices |
| `afn_snd_softfade[]` | from mapdata.h | Legacy combined fade flag |
| `afn_snd_softfade_a[]` | from mapdata.h | FIFO A fade enable |
| `afn_snd_softfade_b[]` | from mapdata.h | FIFO B fade enable |
| `afn_snd_attenuate_a[]` | from mapdata.h | FIFO A polyphony attenuation enable |
| `afn_snd_interp[]` | from mapdata.h | Interpolation mode per instance |
| `afn_pcm_vol_scale[]` | from mapdata.h | Per-instance PCM volume scaling |

## Mapdata-emitted (per-project)

These are emitted by the exporter into `mapdata.h` based on project assets:

| Symbol | What it does |
| --- | --- |
| `afn_asset_desc[][5]` | Per-asset metadata (size, anim count, etc.) |
| `afn_asset_streamable[]` | 1 = streamable on-demand, 0 = static-loaded |
| `afn_pal[][16]` | Per-asset RGB15 palette |
| `afn_anim_desc[][N][4]` | Per-asset anim definitions (frame range, fps, loop, gameState) |
| `afn_dir_anim_tiles[]` | Tile data for directional sprites |

## Functions

### Sound

| Function | Notes |
| --- | --- |
| `afn_play_sound(int instanceId)` | Start sound instance from sound bank. Activates the sequence player |
| `afn_play_sfx(int smpIdx, int gain, int fifo)` | One-shot sample to FIFO A (0) or B (1) |
| `afn_trigger_sample(int smpIdx, int note, int vel, int durTicks, int ch)` | Internal — used by sequence playback |
| `afn_stop_sound()` | Stop all voices, deactivate sequence player |
| `afn_sound_swap()` | Internal — double-buffer mix swap |
| `afn_sound_init()` | Internal — boot-time sound system init |
| `afn_sound_tick()` | Internal — per-frame sequence advance |
| `afn_sound_mix()` | Internal — fill DMA audio buffer (IWRAM) |

### Stub helpers (not implemented yet)

| Function | Notes |
| --- | --- |
| `afn_spawn_sprite(asset, x, z)` | Stub — finds free `g_sprites` slot |
| `afn_clone_sprite(src)` | Stub — copies one sprite to a free slot |
| `afn_spawn_projectile(obj, asset, speed)` | Stub |
| `afn_emit_particle(type, x, z)` | Stub — no particle system in current runtime |
| `afn_draw_number(val, x, y)` | Stub |
| `afn_draw_text(id, x, y)` | Stub |
| `afn_draw_bar(x, y, w, fill)` | Stub |
| `afn_draw_sprite_icon(asset, x, y)` | Stub |
| `afn_clear_text()` | Stub |

### Save / persistence

| Function | Notes |
| --- | --- |
| `afn_sram_save()` | Writes `afn_flags`, `afn_score`, `afn_vars` to SRAM at 0x0E000000 |
| `afn_sram_load()` | Reads them back |

### HUD / cursor

| Function | Notes |
| --- | --- |
| `afn_apply_stop_modifiers(stop)` | Run cursor-stop logic when cursor moves |

### Internal (called from main loop only)

| Function | Notes |
| --- | --- |
| `afn_vblank_isr()` | VBlank handler — bumps `afn_vblank_counter` |
| `afn_sound_set_rate()` | Adjust mixer sample rate |
| `afn_sound_hw_start()` | Start DMA audio output |
| `afn_sound_hw_stop()` | Stop DMA audio output |

## Generated function names

The visual script exporter generates inline functions with these naming patterns. Don't call them directly from custom code — they're invoked by the dispatch system.

| Pattern | What it is |
| --- | --- |
| `afn_script_<suffix>_<nodeId>()` | Scene-level action node body (e.g. `afn_script_jump_5()`) |
| `afn_bp<bpIdx>_<suffix>_<nodeId>()` | Blueprint action node body (e.g. `afn_bp2_walk_8()`) |
| `afn_bp<bpIdx>_<event>()` | Blueprint event chain entry point (e.g. `afn_bp2_start()`, `afn_bp2_update()`) |
| `afn_bp<bpIdx>_custom_<nodeId>()` | Custom Code node body |
| `afn_bp_dispatch_<event>()` | Per-event dispatch over all blueprint instances |
| `afn_script_timer_<id>()` | Generated OnTimer wrapper |

## Internal helpers (forward-declared so user code can use)

These live in `main.c` after the mapdata include but are forward-declared above it:

| Symbol | Notes |
| --- | --- |
| `cam_x`, `cam_h`, `cam_y_smooth` | Camera position state |
| `tm_anim_idx`, `tm_anim_frame`, `tm_anim_timer` | Player anim cycling |
| `tm_cam_x`, `tm_cam_y` | Mode 0 camera scroll |
| `auto_orbit_smooth` | Smoothed orbit value |
| `player_on_ground`, `player_ground_y` | Collision query result |

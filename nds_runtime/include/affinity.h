// Affinity NDS runtime — shared globals, fixed-point helpers, forward decls.
// Every .c file in nds_runtime/source/ includes this BEFORE mapdata.h.
#ifndef AFFINITY_H
#define AFFINITY_H

#include <nds.h>

// ---------------------------------------------------------------------------
// Fixed-point conventions
//
// Project data lands in mapdata.h as 16.8 fixed-point ("fx8"). The DS GPU
// wants 20.12 ("f32") for matrices and 4.12 ("v16") for vertex positions.
// ---------------------------------------------------------------------------
#define FX_SHIFT  8
#define FX_ONE    (1 << FX_SHIFT)
#define FX_MUL(a, b) (((a) * (b)) >> FX_SHIFT)

// Project coords are 8.8 fixed where 256 = 1 editor pixel. Max scene extent
// ~32767 fx8 = 128 px — shifting left 4 into f32/v16 wraps int16. Treat
// 1 fx8 = 1 v16/f32 unit directly: 4096 fx8 = 1.0 DS world unit ≈ 16 px.
// Whole 128-px scene → 8 DS world units → fits v16's ±8 range.
static inline int32_t fx8_to_f32(int fx8) { return fx8; }
static inline int16_t fx8_to_v16(int fx8) {
    if (fx8 >  32767) return  32767;
    if (fx8 < -32768) return -32768;
    return (int16_t)fx8;
}

// libnds sin/cos return .12, we operate in .8 — wrap once.
static inline int brad_sin(uint16_t a) { return sinLerp(a) >> 4; }
static inline int brad_cos(uint16_t a) { return cosLerp(a) >> 4; }

// ---------------------------------------------------------------------------
// Camera state (defined in fps3d.c)
// ---------------------------------------------------------------------------
extern int cam_x, cam_z, cam_h;     // 16.8 fixed
extern uint16_t cam_angle;          // brad (0..65535)
extern int g_cosf, g_sinf;          // precomputed cam_angle sin/cos in .8

// ---------------------------------------------------------------------------
// Player state (defined in fps3d.c for now; will migrate to mode0.c for tilemap)
// ---------------------------------------------------------------------------
extern int player_x, player_z, player_y;
extern uint16_t orbit_angle;
extern int orbit_dist;
extern int player_sprite_idx;
extern int player_moving;
extern uint16_t player_move_angle;

// ---------------------------------------------------------------------------
// Mode 7 floor (defined in fps3d.c — Phase 6 may split into floor.c)
// ---------------------------------------------------------------------------
extern int m7_horizon;
extern int m7_bg;
void m7_hbl(void);

// ---------------------------------------------------------------------------
// Scene-transition state (defined in fps3d.c — Phase 3 will move to scene.c)
//
// setActionFunc bodies for ChangeScene set afn_pending_scene + start a fade.
// Brightness ramps from 0 → -16 (full black) over afn_fade_frames, swaps the
// scene, then ramps back. Matches GBA semantics; uses NDS hardware brightness
// (REG_MASTER_BRIGHT) instead of REG_BLDCNT.
// ---------------------------------------------------------------------------
extern int afn_pending_scene;       // next scene index, -1 = no pending
extern int afn_pending_scene_mode;  // 0 = FPS, 1 = tilemap, 2 = legacy Mode 7
extern int afn_current_scene;
extern int afn_current_mode;
extern int afn_fade_target;         // -16..0 desired brightness (0 = full bright)
extern int afn_fade_counter;        // frames remaining in current fade
extern int afn_fade_frames;         // duration of fade in frames

void afn_scene_start_transition(int sceneIdx, int sceneMode, int fadeFrames);
void afn_scene_tick(void);          // called per-frame from main loop

// ---------------------------------------------------------------------------
// Per-module init / per-frame entry points
//   - main.c calls these in a fixed order each frame
//   - modules that aren't ported yet supply no-op stubs (see their .c file)
// ---------------------------------------------------------------------------
void afn_audio_init(void);     // audio.c   — Phase 1
void afn_audio_tick(void);     // audio.c   — Phase 1 (per VBlank: advance MIDI seq)

void afn_fps3d_init(void);     // fps3d.c   — boot scene (current behavior)
void afn_fps3d_update(void);   // fps3d.c   — per frame: input + camera + render

void afn_sprite_init(void);    // sprites.c — upload OBJ tiles + palettes at boot
void afn_sprite_update(void);  // sprites.c — per frame: project + emit OAM

void afn_mode0_init(void);     // mode0.c   — Phase 4
void afn_mode0_update(void);   // mode0.c   — Phase 4

void afn_hud_init(void);       // hud.c     — Phase 5
void afn_hud_draw(void);       // hud.c     — Phase 5

void afn_script_init(void);    // script_glue.c — Phase 3
void afn_script_tick(void);    // script_glue.c — Phase 3 (OnUpdate, key hooks)

#endif // AFFINITY_H

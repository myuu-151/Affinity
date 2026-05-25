// Affinity NDS — visual scripting runtime glue (Phase 3).
//
// Bridges the per-frame dispatchers main.c calls (afn_script_init / tick)
// to the actual emitted script bodies in mapdata.h (afn_emitted_script_*).
// Also owns the storage for every script-side global declared as `extern`
// by mapdata.h. mapdata.h is included by multiple .c files on NDS (main.c,
// fps3d.c, sprites.c, hud.c, ...), so each global gets a single definition
// here instead of static in mapdata.h (the GBA pattern doesn't translate).

#include "affinity.h"
#include "mapdata.h"

// ---------------------------------------------------------------------------
// Script-side globals
//
// Defined only when scripts actually exist in the build. AFN_HAS_SCRIPT is
// gated on the editor having at least one script node or blueprint, so most
// of these stay out of the link for script-less projects.
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_SCRIPT
int  afn_input_fwd;
int  afn_input_right;
int  afn_move_speed;
int  afn_auto_orbit_speed;
int  afn_play_anim;
int  afn_sprite_anim_spr = -1;
int  afn_sprite_anim_val = -1;
int  afn_anim_prio;
int  afn_collided_sprite = -1;
int  afn_collided_tm_obj = -1;
int  afn_bp_cur_tm_obj   = -1;
int  afn_bp_cur_spr_idx  = -1;
int  afn_gravity;
int  afn_terminal_vel;
int  afn_player_frozen;
int  afn_anim_speed = 1;
unsigned int afn_rng = 12345;
int  afn_shake_intensity;
int  afn_shake_frames;
int  afn_fade_level;
int  afn_score;
int  afn_frame_count;
int  afn_draw_distance;

// Phase 3b additions — script-side state expected by emitted node bodies.
int  afn_bg_color;
int  afn_cam_locked;
int  afn_cam_speed;
int  afn_checkpoint_set;
int  afn_checkpoint_x;
int  afn_checkpoint_y;
int  afn_checkpoint_z;
int  afn_dt_tick = 1;             // ticks per frame (1 = no DT scaling)
unsigned int afn_flags;
int  afn_force_x;
int  afn_force_z;
int  afn_friction;
int  afn_last_key;
int  afn_player_height = 3072;    // 12 editor px in 16.8 (matches GBA default)
int  afn_scripts_stopped;
int  afn_start_x;
int  afn_start_y;
int  afn_start_z;
int  afn_text_color = 0x7FFF;     // RGB15 white
int  afn_timer_visible;
// afn_wall_collided_sprite lives in collision.c when collision data exists.
#ifndef AFN_COL_FACE_COUNT
int  afn_wall_collided_sprite = -1;
#endif
int  afn_fade_target;
int  afn_fade_frames;
int  afn_fade_counter;
unsigned char afn_sprite_visible[NUM_SPRITES];
unsigned char afn_sprite_flip[NUM_SPRITES];
unsigned char afn_collision_enabled[NUM_SPRITES];
int  afn_hp[NUM_SPRITES];
int  afn_state_timer[NUM_SPRITES];

// Player physics shared with fps3d.c — defined here when scripts are on so
// emitted code can read/write them. fps3d.c uses its file-static fallbacks
// in script-less builds.
int  player_vy;
int  player_ground_y;

// Mode 0 tilemap state — referenced by emitted scripts even on 3D scenes.
int  tm_player_facing = 4;        // 4 = south, matches GBA default
int  tm_move_timer;
int  player_on_ground = 1;

void afn_script_state_init(void)
{
    int i;
    for (i = 0; i < NUM_SPRITES; i++) {
        afn_sprite_visible[i]    = 1;
        afn_sprite_flip[i]       = 0;
        afn_collision_enabled[i] = 1;
        afn_hp[i]                = 0;
        afn_state_timer[i]       = 0;
    }
}
#endif

void afn_script_init(void)
{
#ifdef AFN_HAS_SCRIPT
    afn_script_state_init();
#endif
    afn_emitted_script_init();
}

// Collision detection: mirror GBA's radius-based check in main.c. Walks all
// non-player sprites once per frame and flags the first one inside the
// player's interaction radius into afn_collided_sprite, then fires the
// OnCollision dispatcher. The 10-frame post-load grace window matches GBA
// so the player doesn't ping-pong on a sprite they started inside of.
#ifdef AFN_HAS_SCRIPT
static void afn_script_check_collisions(void)
{
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    afn_collided_sprite = -1;
    for (int i = 0; i < AFN_SPRITE_COUNT; i++) {
        if (i == AFN_PLAYER_IDX) continue;
        if (!afn_sprite_visible[i]) continue;
        if (!afn_collision_enabled[i]) continue;
        int dx = (player_x - afn_sprite_data[i][0]) >> 4;
        int dz = (player_z - afn_sprite_data[i][2]) >> 4;
        // Threshold mirrors gba_runtime/main.c:7592 — (24px radius)^2 in 12.4.
        if (dx * dx + dz * dz < 147456) {
            afn_collided_sprite = i;
            break;
        }
    }
    if (afn_collided_sprite >= 0 && afn_frame_count > 10)
        afn_emitted_script_collision();
#endif
}
#endif

void afn_script_tick(void)
{
#ifdef AFN_HAS_SCRIPT
    afn_input_fwd   = 0;
    afn_input_right = 0;
    afn_play_anim   = -1;
    afn_anim_prio   = 0;
#endif
    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
#ifdef AFN_HAS_SCRIPT
    // Blueprint instance dispatch — handlers per blueprint with the
    // current sprite slot in afn_bp_cur_spr_idx.
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
    afn_bp_dispatch_key_released();
    afn_script_check_collisions();
    afn_frame_count++;
#endif
}

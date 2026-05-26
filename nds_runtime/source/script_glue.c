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
int  afn_hud_value[4];            // counters; SetHudValue accumulates
unsigned char afn_hud_visible[4]; // ShowHUD/HideHUD toggles
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
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
// Per-mesh local-space AABB, lazily computed on first collision check.
// Verts are 16.8 fixed pixels (s16). Bounds get scaled by the sprite's
// scale field (8.8 fixed, 256 = 1.0) and translated by sprite pos at
// query time so each instance gets its own world AABB without per-instance
// recompute cost.
static int s_mesh_min[AFN_MESH_COUNT][3];
static int s_mesh_max[AFN_MESH_COUNT][3];
static int s_mesh_bounds_ready = 0;
static void compute_mesh_bounds(void)
{
    for (int m = 0; m < AFN_MESH_COUNT; m++) {
        int vc = afn_mesh_desc[m][0];
        const short* v = afn_mesh_vert_ptrs[m];
        if (vc <= 0 || !v) {
            s_mesh_min[m][0] = s_mesh_min[m][1] = s_mesh_min[m][2] = 0;
            s_mesh_max[m][0] = s_mesh_max[m][1] = s_mesh_max[m][2] = 0;
            continue;
        }
        int minX = v[0], maxX = v[0];
        int minY = v[1], maxY = v[1];
        int minZ = v[2], maxZ = v[2];
        for (int i = 1; i < vc; i++) {
            int x = v[i*3+0], y = v[i*3+1], z = v[i*3+2];
            if (x < minX) minX = x; if (x > maxX) maxX = x;
            if (y < minY) minY = y; if (y > maxY) maxY = y;
            if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
        }
        s_mesh_min[m][0] = minX; s_mesh_max[m][0] = maxX;
        s_mesh_min[m][1] = minY; s_mesh_max[m][1] = maxY;
        s_mesh_min[m][2] = minZ; s_mesh_max[m][2] = maxZ;
    }
    s_mesh_bounds_ready = 1;
}
#endif

static void afn_script_check_collisions(void)
{
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    if (afn_frame_count <= 10) { afn_collided_sprite = -1; return; }
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    if (!s_mesh_bounds_ready) compute_mesh_bounds();
#endif
    int firstHit = -1;
    for (int i = 0; i < AFN_SPRITE_COUNT; i++) {
        if (i == AFN_PLAYER_IDX) continue;
        if (!afn_sprite_visible[i]) continue;
        if (!afn_collision_enabled[i]) continue;

        int hit = 0;
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
        int meshIdx = afn_sprite_data[i][9];
        if (meshIdx >= 0 && meshIdx < AFN_MESH_COUNT) {
            // Mesh sprite — AABB test on the scaled local bounds in world space.
            // Skipping rotation; reset/zone meshes are axis-aligned in practice.
            int sx = afn_sprite_data[i][0];
            int sy = afn_sprite_data[i][1];
            int sz = afn_sprite_data[i][2];
            int s  = afn_sprite_data[i][5]; if (s <= 0) s = 256;
            int mnX = sx + ((s_mesh_min[meshIdx][0] * s) >> 8);
            int mxX = sx + ((s_mesh_max[meshIdx][0] * s) >> 8);
            int mnY = sy + ((s_mesh_min[meshIdx][1] * s) >> 8);
            int mxY = sy + ((s_mesh_max[meshIdx][1] * s) >> 8);
            int mnZ = sz + ((s_mesh_min[meshIdx][2] * s) >> 8);
            int mxZ = sz + ((s_mesh_max[meshIdx][2] * s) >> 8);
            if (player_x >= mnX && player_x <= mxX &&
                player_y >= mnY && player_y <= mxY &&
                player_z >= mnZ && player_z <= mxZ) hit = 1;
        } else
#endif
        {
            // Plain sprite — XZ radius test (24px circle, matches GBA).
            int dx = (player_x - afn_sprite_data[i][0]) >> 4;
            int dz = (player_z - afn_sprite_data[i][2]) >> 4;
            if (dx * dx + dz * dz < 147456) hit = 1;
        }
        if (hit) {
            afn_collided_sprite = i;
            if (firstHit < 0) firstHit = i;
            afn_bp_dispatch_collision();
        }
    }
    if (firstHit >= 0) {
        afn_collided_sprite = firstHit;
        afn_emitted_script_collision();
    } else {
        afn_collided_sprite = -1;
    }
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

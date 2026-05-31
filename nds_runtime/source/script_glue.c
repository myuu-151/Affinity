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
int  afn_speed_prio;   // set by Sprint so Walk can't overwrite speed this frame
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

// HUD cursor navigation — CursorUp/Down/FollowLink nodes index into the
// active element's stop list. afn_stop_count / afn_stop_links are wired up
// by ShowHUD when the menu element is shown (TODO Mode 0 cursor work).
int  afn_cursor_stop;
int  afn_stop_count;
int  afn_stop_links[8];
int  afn_elem_idx;
int  afn_active_element;
int  afn_scripts_stopped;
int  afn_start_x;
int  afn_start_y;
int  afn_start_z;
int  afn_text_color = 0x7FFF;     // RGB15 white
int  afn_timer_visible;
// afn_wall_collided_sprite / afn_floor_sprite live in collision.c when collision
// data exists; define fallbacks here otherwise.
#ifndef AFN_COL_FACE_COUNT
int  afn_wall_collided_sprite = -1;
int  afn_floor_sprite = -1;
#endif
extern int afn_floor_sprite;
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
// World-axis boost/knockback velocity (Mode 4). SetVelocityX/Z write these;
// fps3d.c adds them to player_x/z every frame; VelocityFalloff linearly
// decays them to 0 over N frames.
int  afn_player_vx_world;
int  afn_player_vz_world;
int  afn_velocity_falloff;
int  afn_pending_boost_fwd;
// Rail grinding (StartGrind/StopGrind nodes). afn_grinding is the script-set
// flag; the rest is runtime state owned by fps3d.c (locked rail direction +
// momentum), seeded from the player's entry heading when grinding starts.
int  afn_grinding;
int  afn_grind_dx;
int  afn_grind_dz;
int  afn_grind_vel;
int  afn_grind_rail = -1;   // sprite index of the rail being ground (its mesh axis = grind dir)
// GrindPower / GrindBoost nodes. afn_grind_power = base downhill gain (0 => default
// 24). afn_grind_boost = extra downhill gain THIS frame (cleared every frame in
// fps3d.c, so a held-button gate re-sets it each frame for hold-to-boost).
int  afn_grind_power = 0;
int  afn_grind_boost = 0;
// Physics-validated grind flag the script's Is Grinding / Is Not Grinding gates
// read (mirrored from afn_grinding at the END of the fps3d physics tick, after
// onRail is resolved). afn_grinding alone is the StartGrind INTENT and can be 1
// for a frame while airborne over the rail, which broke grind-SFX retrigger.
int  afn_grinding_active = 0;
// GrindBleed node: right-shift used to decay the boosted speed-cap bonus each
// grind frame (default 6 ~= 64-frame bleed). Higher = momentum carries farther;
// 0 = never bleeds. Persistent (NOT cleared per frame), so set it under On Start.
int  afn_grind_bleed = 6;

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
    // Fire OnStart chains once at boot — scene-level and per-bp-instance.
    // Drives blueprints that auto-trigger setup like background music
    // (PlaySound on the song instance) or initial HUD config.
    afn_emitted_script_start();
#ifdef AFN_HAS_SCRIPT
    afn_bp_dispatch_start();
#endif
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
        // Verts may be downscaled by 2^vshift to fit s16 (long meshes); shift
        // the bounds back up so the collision AABB matches true world extent.
        int vs = afn_mesh_vshift[m];
        s_mesh_min[m][0] = minX << vs; s_mesh_max[m][0] = maxX << vs;
        s_mesh_min[m][1] = minY << vs; s_mesh_max[m][1] = maxY << vs;
        s_mesh_min[m][2] = minZ << vs; s_mesh_max[m][2] = maxZ << vs;
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
    // Vertical travel this frame, for a swept Y test against thin mesh
    // triggers (deathplanes etc.). Without this, a fast fall steps player_y
    // clean past a thin collider's Y slab in one frame and the trigger is
    // missed (tunneling). We test the whole [yLo, yHi] segment the player
    // moved through, so crossing the slab at any speed registers.
    static int s_prevColPlayerY = 0;
    static int s_prevColInit = 0;
    if (!s_prevColInit) { s_prevColPlayerY = player_y; s_prevColInit = 1; }
    int yLo = player_y, yHi = s_prevColPlayerY;
    if (yLo > yHi) { int t = yLo; yLo = yHi; yHi = t; }
    s_prevColPlayerY = player_y;

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
            // Swept Y: the player's vertical travel segment [yLo,yHi] this
            // frame must overlap the slab [mnY,mxY] — so a fast fall through a
            // thin deathplane still registers instead of tunneling past it.
            if (player_x >= mnX && player_x <= mxX &&
                yHi >= mnY && yLo <= mxY &&
                player_z >= mnZ && player_z <= mxZ) hit = 1;
        } else
#endif
        {
            // Plain sprite — XZ radius test (24px circle, matches GBA) PLUS a
            // vertical band so you don't collect/trigger a sprite you're flying
            // over. The band is the player's collision height (afn_player_height,
            // set by Set Player Height) — so jumping clearly above a ring misses
            // it, but walking into / passing through it at body height collects.
            int dx = (player_x - afn_sprite_data[i][0]) >> 4;
            int dz = (player_z - afn_sprite_data[i][2]) >> 4;
            int dy = player_y - afn_sprite_data[i][1];
            if (dy < 0) dy = -dy;
            if (dx * dx + dz * dz < 147456 && dy <= afn_player_height) hit = 1;
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
    // Wall-collision path: collision.c flags afn_wall_collided_sprite when
    // the player's swept-AABB hits a mesh face tagged with a sprite index.
    // GBA fires the BP collision dispatcher off that too (main.c:7781-7783)
    // — otherwise walking into a wall sprite never triggers its BP
    // (e.g. a Mode-4 ChangeScene attached to a wall mesh).
    if (afn_wall_collided_sprite >= 0 && afn_frame_count > 10) {
        afn_collided_sprite = afn_wall_collided_sprite;
        afn_bp_dispatch_collision();
        afn_emitted_script_collision();
    }
    // Floor-collision path: afn_floor_sprite is the sprite whose floor face the
    // player is STANDING on (set by collide_floor, which is rotation-correct
    // unlike the AABB test above). Fire that sprite's BP so a mesh you stand on
    // — e.g. a rotated/diagonal grind rail — triggers its On Collision even when
    // its un-rotated bounding box wouldn't catch you. Uses last frame's value
    // (collide_floor runs in fps3d after this), which is fine for a sustained
    // contact like standing/grinding.
    if (afn_floor_sprite >= 0 && afn_frame_count > 10) {
        afn_collided_sprite = afn_floor_sprite;
        afn_bp_dispatch_collision();
        afn_emitted_script_collision();
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
    afn_speed_prio  = 0;
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

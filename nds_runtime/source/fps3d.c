// Affinity NDS — FPS / 3D mesh scene (Mode 4 GBA equivalent on NDS hardware 3D).
// Owns: camera state, Mode 7 affine floor HBlank handler, mesh upload + render.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/videoGL.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Camera state — definitions (extern in affinity.h)
// ---------------------------------------------------------------------------
int cam_x, cam_z, cam_h;
uint16_t cam_angle;
int g_cosf, g_sinf;

int player_x, player_z, player_y;
uint16_t orbit_angle;
int orbit_dist;
int player_sprite_idx = -1;
int player_moving;
// Init so the picker formula (sprAngle = pma - 2*orbit_angle) resolves to
// sprAngle = 0x4000 → dir 0 (N, back) regardless of what AFN_CAM_ANGLE
// the project ships with. Previously hard-coded to 0x4000, which only
// gave the back-facing pose for the default AFN_CAM_ANGLE — set the
// editor's Angle slider to anything else and sonic would start facing
// the camera (S sprite) until the first DPAD-UP snap.
uint16_t player_move_angle = (uint16_t)(0x4000 + (AFN_CAM_ANGLE << 1));
uint16_t orbit_angle = AFN_CAM_ANGLE;
// Last frame's world-space movement direction (un-normalized). The sprite
// dir picker reads these to face the player in the direction of motion.
int s_lastMoveDX, s_lastMoveDZ;
static int s_wasMoving = 0;

int m7_horizon = 60;
int m7_bg;

#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
static int gl_sky_tex_id = 0;
#endif

// ---------------------------------------------------------------------------
// Scene-transition state (defs match affinity.h externs)
// ---------------------------------------------------------------------------
int afn_pending_scene      = -1;
int afn_pending_scene_mode = 0;
int afn_current_scene      = 0;
int afn_current_mode       = 0;
// Fade state lives in script_glue.c when scripts are on (so SceneChange /
// FadeTo nodes can write it directly); fallback here otherwise.
#ifndef AFN_HAS_SCRIPT
int afn_fade_target        = 0;
int afn_fade_counter       = 0;
int afn_fade_frames        = 0;
#endif
static int s_fade_phase    = 0;   // 0 = idle, 1 = fading out, 2 = fading in

// ---------------------------------------------------------------------------
// Mode 7 floor — HBlank driven affine BG
// ---------------------------------------------------------------------------
void m7_hbl(void)
{
    int vc = REG_VCOUNT;
    int row = vc - m7_horizon;

    if (row <= 0)
    {
        REG_BG2PA = 0;
        REG_BG2PB = 0;
        REG_BG2PC = 0;
        REG_BG2PD = 0;
        REG_BG2X = 0;
        REG_BG2Y = 0;
        return;
    }

    int D = 160;
    int lam = (D << 8) / row;

    int lcf = FX_MUL(lam, g_cosf);
    int lsf = FX_MUL(lam, g_sinf);

    REG_BG2PA = (int16_t)(lcf >> 0);
    REG_BG2PC = (int16_t)(lsf >> 0);
    REG_BG2PB = (int16_t)(-lsf >> 0);
    REG_BG2PD = (int16_t)(lcf >> 0);

    int cx = cam_x >> FX_SHIFT;
    int cz = cam_z >> FX_SHIFT;

    int dx = 128 - FX_MUL(lcf, 128) + FX_MUL(lsf, 128);
    int dy = 128 - FX_MUL(lsf, 128) - FX_MUL(lcf, 128);

    REG_BG2X = (cx << 8) + (dx << 0);
    REG_BG2Y = (cz << 8) + (dy << 0);
}

// ---------------------------------------------------------------------------
// Floor tilemap (only present when the project exports one)
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
static void load_floor(void)
{
    dmaCopy(afn_floor_tiles, BG_TILE_RAM(0), AFN_FLOOR_TILES_LEN);
    dmaCopy(afn_floor_map, BG_MAP_RAM(4), AFN_FLOOR_MAP_LEN);
    dmaCopy(afn_floor_pal, BG_PALETTE, AFN_FLOOR_PAL_LEN);
}
#endif

// AFN_PLAYER_BASE_Y: spawn-Y of the player sprite (used as the ground
// reference for camera height + flat-ground fallback). Falls back to 0
// when a project has no sprites or no Player-typed sprite — keeps
// fps3d.c linkable for empty scenes that boot straight into Mode 0.
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
#define AFN_PLAYER_BASE_Y  afn_sprite_data[AFN_PLAYER_IDX][1]
#else
#define AFN_PLAYER_BASE_Y  0
#endif

// ---------------------------------------------------------------------------
// Mesh textures → VRAM
// ---------------------------------------------------------------------------
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
static int gl_tex_ids[AFN_MESH_COUNT];

static void load_mesh_textures(void)
{
    for (int i = 0; i < AFN_MESH_COUNT; i++)
    {
        gl_tex_ids[i] = 0;
        if (!afn_mesh_desc[i][8]) continue;

        int texW = afn_mesh_desc[i][9];
        // Slot [6] carries texH for non-square textures; fall back to
        // texW when 0 (older mapdata.h that pre-dates the split).
        int texH = afn_mesh_desc[i][6] > 0 ? afn_mesh_desc[i][6] : texW;
        if (texW == 0) continue;

        // glTexImage2D wants size as the TEXTURE_SIZE_* enum
        // (TEXTURE_SIZE_8=0, _16=1, _32=2, _64=3, _128=4, _256=5, _512=6, _1024=7),
        // not the raw pixel count. log2(N) - 3 gets us there for power-of-2 sizes.
        int sizeW = 0, tw = texW; while (tw > 8) { tw >>= 1; sizeW++; }
        int sizeH = 0, th = texH; while (th > 8) { th >>= 1; sizeH++; }

        glGenTextures(1, &gl_tex_ids[i]);
        glBindTexture(0, gl_tex_ids[i]);
        // Texture parameter flags:
        //   TEXGEN_TEXCOORD         — use UVs from glTexCoord2t16
        //   GL_TEXTURE_WRAP_S/_T    — tile when UVs go past 0..texSize
        //                             (default is CLAMP → bricks stop at edges)
        //   GL_TEXTURE_COLOR0_TRANSPARENT — palette index 0 = transparent
        int flags = TEXGEN_TEXCOORD | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
        if (afn_mesh_desc[i][11]) flags |= GL_TEXTURE_COLOR0_TRANSPARENT;
        glTexImage2D(0, 0, GL_RGB16, sizeW, sizeH, 0, flags,
                     afn_mesh_tex_ptrs[i]);
        glColorTableEXT(0, 0, 16, 0, 0, afn_mesh_tex_pal_ptrs[i]);
    }
}

static void render_meshes(void)
{
#if defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    for (int si = 0; si < AFN_SPRITE_COUNT; si++)
    {
        int meshIdx = afn_sprite_data[si][9];
        if (meshIdx < 0 || meshIdx >= AFN_MESH_COUNT) continue;
        // visible == 0 → collision-only geometry; skip rendering (matches
        // the editor's "hidden" toggle that's already respected on GBA).
        if (!afn_mesh_desc[meshIdx][15]) continue;

        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];
        int spriteScale = afn_sprite_data[si][5];  // 256 = 1.0
        int rot = afn_sprite_data[si][7];

        const int16_t* verts = afn_mesh_vert_ptrs[meshIdx];
        const uint16_t* idx  = afn_mesh_idx_ptrs[meshIdx];
        const int16_t* uvs   = afn_mesh_uv_ptrs[meshIdx];

        int indexCount    = afn_mesh_desc[meshIdx][1];
        int quadIdxCount  = afn_mesh_desc[meshIdx][2];
        uint16_t color    = afn_mesh_desc[meshIdx][3];
        int cullMode      = afn_mesh_desc[meshIdx][4];
        int lit           = afn_mesh_desc[meshIdx][5];
        int textured      = afn_mesh_desc[meshIdx][8];
        int grayscale     = afn_mesh_desc[meshIdx][13];

        glPushMatrix();
        // Absolute world coords — gluLookAtf32 already applied the camera
        // transform.
        glTranslatef32(fx8_to_f32(wx),
                       fx8_to_f32(wy),
                       fx8_to_f32(wz));
        if (rot != 0)
            glRotateYi(rot << 16 >> 6);
        // Per-sprite scale (8.8 fixed, 256 = 1.0). gluPerspective + identity
        // fx8→v16 alone makes meshes microscopic — multiply by the sprite's
        // own scale field which the editor uses for the same purpose on GBA.
        int s32 = spriteScale << 4;  // 8.8 → 20.12 f32
        glScalef32(s32, s32, s32);

        uint32_t polyFmt = POLY_ALPHA(31);
        if (cullMode == 0) polyFmt |= POLY_CULL_BACK;
        else if (cullMode == 1) polyFmt |= POLY_CULL_FRONT;
        else polyFmt |= POLY_CULL_NONE;
        if (lit) polyFmt |= POLY_FORMAT_LIGHT0;
        glPolyFmt(polyFmt);

        int r, g, b;
        if (textured && gl_tex_ids[meshIdx]) {
            // Texture color is modulated by glColor; force white so the
            // texture comes through unmodified. Per-mesh color is ignored
            // for textured meshes (matches GBA behavior).
            r = g = b = 255;
            glBindTexture(0, gl_tex_ids[meshIdx]);
        } else {
            // Unbind any leftover texture from a previous mesh, otherwise the
            // texture lookup still happens with stale UVs/binding and the
            // mesh inherits a tint from whatever was last drawn.
            glBindTexture(0, 0);
            // Untextured meshes render WHITE by default — per-mesh RGB15 tints
            // were producing dark blues/greens because the editor stores its
            // "default" color as a dark hue and lighting on NDS makes it
            // worse. White lets the geometry's silhouette read clearly.
            r = g = b = 255;
            (void)color; (void)grayscale;
        }
        glColor3b(r, g, b);

        // Helper macro: emit (UV, vertex) for index i. UVs come from the
        // exporter in t16 format (.4 fixed texel units) so we pass directly.
        #define EMIT(i) do { \
            if (textured) glTexCoord2t16(uvs[(i)*2+0], uvs[(i)*2+1]); \
            glVertex3v16(fx8_to_v16(verts[(i)*3+0]), \
                         fx8_to_v16(verts[(i)*3+1]), \
                         fx8_to_v16(verts[(i)*3+2])); \
        } while (0)

        if (indexCount > 0)
        {
            glBegin(GL_TRIANGLES);
            for (int t = 0; t + 3 <= indexCount; t += 3)
            {
                EMIT(idx[t + 0]); EMIT(idx[t + 1]); EMIT(idx[t + 2]);
            }
            glEnd();
        }

        if (quadIdxCount > 0)
        {
            const uint16_t* qidx = afn_mesh_qidx_ptrs[meshIdx];
            glBegin(GL_QUADS);
            for (int q = 0; q + 4 <= quadIdxCount; q += 4)
            {
                EMIT(qidx[q + 0]); EMIT(qidx[q + 1]);
                EMIT(qidx[q + 2]); EMIT(qidx[q + 3]);
            }
            glEnd();
        }
        #undef EMIT

        glPopMatrix(1);
    }
#endif
}
#endif

// ---------------------------------------------------------------------------
// Input + camera tick (WASD = D-pad: W/S = walk fwd/back, A/D = turn)
// Integer square root (for normalizing the rail-grind direction vector).
static int afn_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, r = 0;
    int b = 1 << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Movement/jump state. Walk speed ramps with AFN_WALK_EASE_IN/OUT toward
// the target (walk or sprint). player_y is a vertical offset added to the
// base cam_h — jump impulse pushes it up, gravity pulls it back, cam_h
// follows with a separate smoothing delay (land vs air).
static int s_moveSpeed;             // current smoothed speed
// player_vy lives in script_glue.c when scripts are on (so scripts can
// read/write it); local fallback otherwise.
#ifndef AFN_HAS_SCRIPT
int player_vy;
#endif
static int s_playerY;               // 16.8 vertical offset from ground
static int s_camYSmooth;            // smoothed cam_h follow of s_playerY
static int s_groundSnapTol;         // per-frame ground-snap distance (downhill slopes)
// player_on_ground is the script-side global (defined in script_glue.c when
// AFN_HAS_SCRIPT, else here as a fallback so fps3d.c always has it).
#ifndef AFN_HAS_SCRIPT
int player_on_ground = 1;
#endif

#ifndef AFN_WALK_SPEED
#define AFN_WALK_SPEED 37
#endif
#ifndef AFN_SPRINT_SPEED
#define AFN_SPRINT_SPEED 56
#endif
#ifndef AFN_WALK_EASE_IN
#define AFN_WALK_EASE_IN 25
#endif
#ifndef AFN_WALK_EASE_OUT
#define AFN_WALK_EASE_OUT 25
#endif
#ifndef AFN_SPRINT_EASE_IN
#define AFN_SPRINT_EASE_IN 15
#endif
#ifndef AFN_SPRINT_EASE_OUT
#define AFN_SPRINT_EASE_OUT 10
#endif
#ifndef AFN_JUMP_VEL
#define AFN_JUMP_VEL 512
#endif
#ifndef AFN_GRAVITY
#define AFN_GRAVITY 23
#endif
#ifndef AFN_TERMINAL_VEL
#define AFN_TERMINAL_VEL 1536
#endif
#ifndef AFN_JUMP_CAM_LAND
#define AFN_JUMP_CAM_LAND 94
#endif
#ifndef AFN_JUMP_CAM_AIR
#define AFN_JUMP_CAM_AIR 30
#endif

static void update_camera(void)
{
    scanKeys();
    uint32_t held = keysHeld();
    uint32_t down = keysDown();
    {
        static int s_dbgFrame = 0;
        s_dbgFrame++;
        if ((s_dbgFrame & 15) == 0) {
            iprintf("\x1b[10;0Hkeys=%04x cx=%d cz=%d ca=%d   ",
                    (unsigned)held, cam_x >> FX_SHIFT, cam_z >> FX_SHIFT, (int)cam_angle);
        }
    }

#ifndef AFN_HAS_SCRIPT
    // Debug free-cam controls — only when no scripts. Once nodes exist,
    // OrbitCamera / Jump / etc. are the only thing that touches camera state.
    if (held & KEY_L) cam_h += AFN_WALK_SPEED;
    if (held & KEY_R) cam_h -= AFN_WALK_SPEED;
    if (held & KEY_LEFT)  orbit_angle += 512;
    if (held & KEY_RIGHT) orbit_angle -= 512;
#endif

#ifdef AFN_ORBIT_MAX_DELTA
    // Clamp this frame's orbit_angle change so the camera lerp can keep
    // up and the player sprite stays centered. OrbitCamera scripts have
    // already applied their requested delta; we re-anchor to the prev
    // value and let through at most ±AFN_ORBIT_MAX_DELTA brads.
    {
        static uint16_t s_prev_orbit_angle = 0xFFFF;
        if (s_prev_orbit_angle == 0xFFFF) s_prev_orbit_angle = orbit_angle;
        int delta = (int16_t)(orbit_angle - s_prev_orbit_angle);
        if (delta >  AFN_ORBIT_MAX_DELTA) delta =  AFN_ORBIT_MAX_DELTA;
        if (delta < -AFN_ORBIT_MAX_DELTA) delta = -AFN_ORBIT_MAX_DELTA;
        orbit_angle = (uint16_t)(s_prev_orbit_angle + delta);
        s_prev_orbit_angle = orbit_angle;
    }
#endif
    // GBA writes cam_angle = orbit_angle once per frame (main.c:7946) — that's
    // how OrbitCamera scripts (which modify orbit_angle) actually propagate
    // to camera-direction-dependent code paths. Without this, manual orbit
    // changes orbit_angle but cam_angle / g_sinf / g_cosf stay stale.
    cam_angle = orbit_angle;
    g_cosf = brad_cos(cam_angle);
    g_sinf = brad_sin(cam_angle);

#ifdef AFN_HAS_SCRIPT
    // Script-driven path: MovePlayer nodes set afn_input_fwd/right, Walk/
    // Sprint nodes set afn_move_speed. Map view-space input → world XZ via
    // the camera basis (forward = (sin,cos), right = (cos,-sin) for our
    // gluLookAt convention). script_tick ran before fps3d_update so the
    // values are fresh.
    // Rail grinding: when afn_grinding, the player is locked to the rail
    // direction and slides on momentum instead of taking input movement.
    extern int afn_grinding, afn_grind_dx, afn_grind_dz, afn_grind_vel;
    extern int afn_player_vx_world, afn_player_vz_world;
    int fwd = afn_input_fwd, right = afn_input_right;
    if (fwd && right) { fwd = (fwd * 181) >> 8; right = (right * 181) >> 8; }
    int spd = afn_move_speed;
    int dx = ((g_sinf * fwd + g_cosf * right) >> 8);
    int dz = ((g_cosf * fwd - g_sinf * right) >> 8);
    extern int afn_grind_rail;
    int mvX, mvZ;
    if (afn_grind_vel != 0) {
        // Engaged grind: locked to the rail axis, sliding on momentum. Player
        // input is FROZEN here (dx/dz ignored) — only the jump (handled below)
        // gets you off. Engage + direction seeding happen in the floor block,
        // once we confirm the player is actually standing on the rail's floor.
        mvX = FX_MUL(afn_grind_dx, afn_grind_vel);
        mvZ = FX_MUL(afn_grind_dz, afn_grind_vel);
    } else {
        mvX = FX_MUL(dx, spd);
        mvZ = FX_MUL(dz, spd);
    }
    player_x += mvX;
    player_z += mvZ;
    // Horizontal distance moved this frame (Manhattan) — used as the
    // ground-snap tolerance so walking DOWN a slope keeps the player glued
    // to the floor instead of float-then-landing each frame (which flickered
    // Is Jumping / Is Falling and stuttered the anim). A 45° slope drops ~one
    // horizontal step per frame, so this tolerance covers it; a real ledge
    // drop is far larger than one frame's move, so we won't snap off cliffs.
    s_groundSnapTol = (mvX < 0 ? -mvX : mvX) + (mvZ < 0 ? -mvZ : mvZ);
    s_groundSnapTol += s_groundSnapTol >> 1;  // ~1.5x margin
#ifdef AFN_HAS_SCRIPT
    // Forward decls in case the loaded mapdata.h was generated before
    // these externs were emitted. Real defs live in script_glue.c.
    extern int afn_player_vx_world;
    extern int afn_player_vz_world;
    extern int afn_velocity_falloff;
    extern int afn_pending_boost_fwd;
    // BoostForward(speed) wrote a pending magnitude — decompose it here
    // using the *current* view angle, then clear. Keeps emitted script
    // code view-agnostic.
    if (afn_pending_boost_fwd) {
        afn_player_vx_world = FX_MUL(g_sinf, afn_pending_boost_fwd);
        afn_player_vz_world = FX_MUL(g_cosf, afn_pending_boost_fwd);
        afn_pending_boost_fwd = 0;
    }
    // Node-driven world-axis push velocity (boost pads / knockback).
    // SetVelocityX/Z write the globals; VelocityFalloff(N) linearly ramps
    // them to 0 over N frames (vx -= vx/N as N decrements gives true linear).
    player_x += afn_player_vx_world;
    player_z += afn_player_vz_world;
    if (afn_velocity_falloff > 0) {
        afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
        afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
        if (--afn_velocity_falloff == 0) {
            afn_player_vx_world = 0;
            afn_player_vz_world = 0;
        }
    }
#endif
    player_moving = (fwd != 0 || right != 0);
    if (player_moving) { s_lastMoveDX = dx; s_lastMoveDZ = dz; }
    // GBA-style player facing tracking. player_move_angle is INPUT-space
    // while moving (atan2 of dpad input), then converted to world-space
    // at the moment of stopping. orbit_angle stays fixed at AFN_CAM_ANGLE
    // (matches GBA — manual L/R orbit modifies cam_angle, not orbit_angle).
    if (player_moving) {
        // Brad ArcTan2(inputRight=x, inputFwd=y). GBA convention: angle
        // measured from +X axis, so atan2(0,+y) = 0x4000 (DPAD-up = N
        // brad). Picker formula then maps brad 0x4000 → dir 0 (N image).
        // L/R input brad swapped: DPAD-LEFT → E image, DPAD-RIGHT → W image
        uint16_t ang = 0x4000;
        if (afn_input_fwd > 0 && afn_input_right == 0)      ang = 0x4000; // UP
        else if (afn_input_fwd > 0 && afn_input_right > 0)  ang = 0x6000; // UP+RIGHT
        else if (afn_input_fwd == 0 && afn_input_right > 0) ang = 0x8000; // RIGHT
        else if (afn_input_fwd < 0 && afn_input_right > 0)  ang = 0xA000; // DOWN+RIGHT
        else if (afn_input_fwd < 0 && afn_input_right == 0) ang = 0xC000; // DOWN
        else if (afn_input_fwd < 0 && afn_input_right < 0)  ang = 0xE000; // DOWN+LEFT
        else if (afn_input_fwd == 0 && afn_input_right < 0) ang = 0x0000; // LEFT
        else if (afn_input_fwd > 0 && afn_input_right < 0)  ang = 0x2000; // UP+LEFT
        player_move_angle = ang;
    } else if (s_wasMoving) {
        // On stop: bake current orbit into player_move_angle so the idle
        // formula `player_move_angle - 2*orbit_angle` gives the same dir
        // the moving picker just gave. Prevents a snap on key release.
        player_move_angle = player_move_angle + (orbit_angle << 1);
    }
    s_wasMoving = player_moving;
    (void)s_moveSpeed;
#else
    // No scripts — built-in WASD-style movement with ease ramp + sprint.
    int wantMove   = (held & (KEY_UP | KEY_DOWN)) != 0;
    int wantSprint = (held & KEY_B) && wantMove;
    int targetSpeed = wantMove ? (wantSprint ? AFN_SPRINT_SPEED : AFN_WALK_SPEED) : 0;
    int easeNum    = wantMove
        ? (wantSprint ? AFN_SPRINT_EASE_IN : AFN_WALK_EASE_IN)
        : (s_moveSpeed > AFN_WALK_SPEED ? AFN_SPRINT_EASE_OUT : AFN_WALK_EASE_OUT);
    s_moveSpeed += ((targetSpeed - s_moveSpeed) * easeNum) >> 8;
    if (s_moveSpeed < 0) s_moveSpeed = 0;

    int dx = 0, dz = 0;
    if (held & KEY_UP)    { dx += g_sinf; dz += g_cosf; }
    if (held & KEY_DOWN)  { dx -= g_sinf; dz -= g_cosf; }
    player_x += FX_MUL(dx, s_moveSpeed);
    player_z += FX_MUL(dz, s_moveSpeed);
    player_moving = (dx != 0 || dz != 0);
#endif

#ifndef AFN_HAS_SCRIPT
    // Built-in jump on KEY_A. With scripts, a Jump node sets player_vy.
    if ((down & KEY_A) && player_on_ground) {
        player_vy = AFN_JUMP_VEL;
        player_on_ground = 0;
    }
#else
    (void)down;
#endif
    // Gravity: scripts can override via SetGravity (afn_gravity); fallback
    // to the editor-exported AFN_GRAVITY constant otherwise.
#ifdef AFN_HAS_SCRIPT
    player_vy -= afn_gravity ? afn_gravity : AFN_GRAVITY;
    int term = afn_terminal_vel ? afn_terminal_vel : AFN_TERMINAL_VEL;
    if (player_vy < -term) player_vy = -term;
#else
    player_vy -= AFN_GRAVITY;
    if (player_vy < -AFN_TERMINAL_VEL) player_vy = -AFN_TERMINAL_VEL;
#endif
    player_y += player_vy;

    // Floor + wall collision against mesh data when the project exports it.
#ifdef AFN_COL_FACE_COUNT
    {
        extern int afn_floor_sprite;
        int wasGround = player_on_ground;
        int floorY;
        int onFloor = afn_collide_floor(player_x, player_z, player_y, &floorY);
        // "On the rail" = standing on a floor face that belongs to the rail
        // sprite (afn_grind_rail), captured by StartGrind. This is what makes
        // the grind follow JUST the pipe and end the instant you leave it.
        int onRail = (onFloor && afn_grind_rail >= 0 && afn_floor_sprite == afn_grind_rail);
        // Slope fix: collide_floor only accepts a floor within afn_player_height
        // (~12px) of the player's feet, so a tilted rail's surface — which moves
        // up or down by a whole horizontal grind step each frame — falls outside
        // that window and the grind detaches (only a flat beam stayed in range).
        // While grinding, re-query with the player Y biased UP by the horizontal
        // step (+margin) so an uphill OR downhill rail surface stays in range.
        // Gated on the rail sprite, so a non-rail floor that sneaks into the
        // wider window won't be mistaken for the rail.
        if (!onRail && (afn_grind_vel != 0 || afn_grinding) && afn_grind_rail >= 0) {
            int stepMag = (mvX < 0 ? -mvX : mvX) + (mvZ < 0 ? -mvZ : mvZ);
            int probeY = player_y + stepMag + stepMag / 2 + afn_player_height;
            int fY2;
            if (afn_collide_floor(player_x, player_z, probeY, &fY2) &&
                afn_floor_sprite == afn_grind_rail) {
                onFloor = 1; floorY = fY2; onRail = 1;
            }
        }
        static int s_grindPrevFloorY = 0;

        if (afn_grind_vel != 0) {
            // --- Currently grinding ---
            if (player_vy > 0) {
                // Jump node fired → hop off, carry momentum into world velocity.
                afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                afn_grind_vel = 0; afn_grinding = 0;
            } else if (!onRail) {
                // Slid off the edge / end of the rail — release momentum and let
                // it decay so you fly off keeping speed then ramp down (Sonic rail
                // launch), instead of sliding forever. A VelocityFalloff node can
                // override the 30-frame default.
                afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                afn_grind_vel = 0; afn_grinding = 0;
            } else {
                // Stick to the rail; slope drives momentum. slope > 0 means the
                // floor dropped this frame (going DOWNHILL) → accelerate harder;
                // slope < 0 is uphill → bleed speed. Asymmetric so a downhill
                // run visibly ramps up like a Sonic rail.
                int slope = s_grindPrevFloorY - floorY;
                s_grindPrevFloorY = floorY;
                if (slope > 0) afn_grind_vel += slope;       // downhill: full slope gain
                else           afn_grind_vel += slope >> 1;  // uphill: lose half the climb
                afn_grind_vel -= afn_grind_vel >> 9;         // tiny friction (very slippery)
                if (afn_grind_vel < (AFN_WALK_SPEED >> 3)) afn_grind_vel = (AFN_WALK_SPEED >> 3);
                // Cap so a long steep rail doesn't fling you uncontrollably.
                if (afn_grind_vel > AFN_SPRINT_SPEED * 3) afn_grind_vel = AFN_SPRINT_SPEED * 3;
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            }
        } else if (afn_grinding && onRail && player_vy <= 0) {
            // --- Engage: StartGrind fired AND we're standing on the rail ---
            // Lock the slide to the rail's mesh axis (so you follow the pipe even
            // if you landed at an angle), seed speed from current move speed.
            int rdx = s_lastMoveDX, rdz = s_lastMoveDZ;
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
            if (afn_grind_rail < AFN_SPRITE_COUNT) {
                int mi = afn_sprite_data[afn_grind_rail][9];
                if (mi >= 0 && mi < AFN_MESH_COUNT) {
                    const short* v = afn_mesh_vert_ptrs[mi];
                    int vc = afn_mesh_desc[mi][0];
                    if (v && vc > 0) {
                        int mnx=v[0],mxx=v[0],mnz=v[2],mxz=v[2];
                        for (int k = 1; k < vc; k++) {
                            int x=v[k*3], z=v[k*3+2];
                            if(x<mnx)mnx=x; if(x>mxx)mxx=x;
                            if(z<mnz)mnz=z; if(z>mxz)mxz=z;
                        }
                        int ex = mxx-mnx, ez = mxz-mnz;
                        long d1 = (long)ex*s_lastMoveDX + (long)ez*s_lastMoveDZ;
                        long d2 = (long)ex*s_lastMoveDX - (long)ez*s_lastMoveDZ;
                        int ddx = ex, ddz = (d1 >= d2) ? ez : -ez;
                        long dot = (long)ddx*s_lastMoveDX + (long)ddz*s_lastMoveDZ;
                        if (dot < 0) { ddx = -ddx; ddz = -ddz; }
                        int len = afn_isqrt(ddx*ddx + ddz*ddz); if (len < 1) len = 1;
                        rdx = (ddx * 256) / len;
                        rdz = (ddz * 256) / len;
                    }
                }
            }
#endif
            afn_grind_dx = rdx; afn_grind_dz = rdz;
            // Seed the grind speed from the player's ACTUAL momentum entering the
            // rail (input movement + any boost-pad world velocity), projected
            // onto the rail axis — so sprinting / boosting onto a rail carries
            // that speed instead of snapping to a flat default. rdx/rdz are
            // 256-normalized, so the >>8 yields displacement units (same as
            // afn_grind_vel). Fold the world velocity in and clear it so it
            // doesn't keep adding drift off the rail while grinding.
            {
                int tvx = mvX + afn_player_vx_world;
                int tvz = mvZ + afn_player_vz_world;
                int mom = (tvx * rdx + tvz * rdz) >> 8;
                if (mom < 0) mom = -mom;
                afn_grind_vel = mom;
                if (afn_grind_vel < (AFN_WALK_SPEED >> 3))
                    afn_grind_vel = (AFN_WALK_SPEED >> 3);
                afn_player_vx_world = 0; afn_player_vz_world = 0;
                afn_velocity_falloff = 0;
            }
            s_grindPrevFloorY = floorY;
            player_y = floorY; player_vy = 0; player_on_ground = 1;
        } else {
            // --- Not grinding: normal floor resolution ---
            if (onFloor && player_y <= floorY) {
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            } else if (onFloor && wasGround && player_vy <= 0
                       && (player_y - floorY) <= s_groundSnapTol) {
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            } else {
                player_on_ground = 0;
            }
            // Don't let a StartGrind intent linger when we're not on the rail
            // (e.g. brushed the rail's bounding box from the side without
            // standing on it) — otherwise IsGrinding would stay true.
            if (!onRail) afn_grinding = 0;
        }
    }
    afn_collide_walls(&player_x, &player_z, player_y);
#else
    {
        // No mesh collision data — fall back to flat ground at player init Y.
        int groundY = AFN_PLAYER_BASE_Y;
        if (player_y <= groundY) {
            player_y = groundY;
            player_vy = 0;
            player_on_ground = 1;
        } else {
            player_on_ground = 0;
        }
    }
#endif
    s_playerY = player_y - AFN_PLAYER_BASE_Y;

    // 3rd-person camera: target = player - orbit_dist * view-forward. Lerp
    // cam_x/z toward target with the same ease rate as movement so the cam
    // glides into position rather than rubber-banding.
    {
        int targetX = player_x - ((g_sinf * orbit_dist) >> 8);
        int targetZ = player_z - ((g_cosf * orbit_dist) >> 8);
        int ddx = targetX - cam_x;
        int ddz = targetZ - cam_z;
        if (ddx > -16 && ddx < 16 && ddz > -16 && ddz < 16) {
            cam_x = targetX;
            cam_z = targetZ;
        } else {
            // Ease rate based on whether the player is actually moving.
            // Without scripts we have a richer state (wantSprint), with
            // scripts we just use afn_move_speed as a heuristic.
            int moving, sprintLike;
#ifdef AFN_HAS_SCRIPT
            // GBA reads KEY_B directly for sprint here (afn_move_speed is
            // sticky — Walk/Sprint nodes set it but nothing resets it).
            moving = (afn_input_fwd != 0 || afn_input_right != 0);
            sprintLike = (held & KEY_B) != 0;
#else
            int wantMove2   = (held & (KEY_UP | KEY_DOWN)) != 0;
            int wantSprint2 = (held & KEY_B) && wantMove2;
            moving = wantMove2;
            sprintLike = wantSprint2;
#endif
            int ease = sprintLike
                ? (moving ? AFN_SPRINT_EASE_IN : AFN_SPRINT_EASE_OUT)
                : (moving ? AFN_WALK_EASE_IN   : AFN_WALK_EASE_OUT);
            // Orbit-camera ease: in-rate while L/R held (ramping into
            // orbit), out-rate after release (settling). Picks the max
            // vs the walk/sprint ease so the camera never lags BEHIND
            // those defaults — only catches up faster when orbiting.
#ifdef AFN_ORBIT_EASE_IN
            if (held & (KEY_L | KEY_R)) {
                if (AFN_ORBIT_EASE_IN > ease) ease = AFN_ORBIT_EASE_IN;
            } else {
                if (AFN_ORBIT_EASE_OUT > ease) ease = AFN_ORBIT_EASE_OUT;
            }
#endif
            cam_x += (ddx * ease) >> 8;
            cam_z += (ddz * ease) >> 8;
            {
                static int s_dbgF = 0;
                s_dbgF++;
                if ((s_dbgF & 15) == 0)
                    iprintf("\x1b[14;0Hca=%d e=%d dx=%d dz=%d cx=%d cz=%d  ",
                            (int)cam_angle, ease, ddx >> 8, ddz >> 8,
                            cam_x >> 8, cam_z >> 8);
            }
        }
    }

    // Smooth cam_h Y follow — quick on the way down (landing), lazy in the
    // air so the camera lags a beat behind a jump's apex.
    {
        int dy = s_playerY - s_camYSmooth;
        int rate = player_on_ground ? AFN_JUMP_CAM_LAND : AFN_JUMP_CAM_AIR;
        s_camYSmooth += (dy * rate) >> 8;
        if (dy > -4 && dy < 4) s_camYSmooth = s_playerY;
    }
    // cam_h is the camera's world Y. AFN_CAM_H is the camera's offset above
    // the player. s_camYSmooth is the SMOOTHED player-Y offset (driven by
    // AFN_JUMP_CAM_LAND / AFN_JUMP_CAM_AIR rates) so the camera lags through
    // jumps instead of snapping. baseline = the player's spawn Y; adding the
    // smoothed delta gives the camera's tracked world Y.
    cam_h = AFN_PLAYER_BASE_Y + s_camYSmooth + AFN_CAM_H;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
static void load_sky_texture(void)
{
    glGenTextures(1, &gl_sky_tex_id);
    glBindTexture(0, gl_sky_tex_id);
    // 256x256 8bpp paletted (GL_RGB256). TEXTURE_SIZE_256 = 5.
    // GL_TEXTURE_WRAP_S = horizontal wrap so the panorama can scroll past
    // its 256-px edge as cam_angle rotates past 360° without clamping or
    // garbage. Vertical wrap intentionally off (top of sky is top).
    glTexImage2D(0, 0, GL_RGB256, 5, 5, 0,
                 TEXGEN_TEXCOORD | GL_TEXTURE_WRAP_S, afn_sky_tex);
    glColorTableEXT(0, 0, 256, 0, 0, afn_sky_pal);
}

// Draw the sky panorama as a view-space quad behind the 3D scene.
// UV.u scrolls with cam_angle so the panorama appears to wrap as you turn.
// UV.v covers the top half of the 256-tall texture, mapped to the top
// portion of the screen (above the horizon).
static void render_sky(void)
{
    if (!gl_sky_tex_id) return;
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    // Push the quad to ~900 DS units away (near the far plane = 1024) so
    // meshes in the normal scene scale always pass depth test against it.
    // Then scale by 200x so the small v16 vertex coords cover the screen
    // at that depth: visible half-width at depth 900 with 70°fov+4:3 ≈ 840
    // DS units, so verts of ±5 × 200 = ±1000 over-covers slightly.
    glTranslatef32(0, 0, -inttof32(900));
    glScalef32(inttof32(200), inttof32(200), inttof32(200));
    glBindTexture(0, gl_sky_tex_id);
    glColor3b(255, 255, 255);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

    // cam_angle 0..65535 = full 360° panorama wrap.
    // Map to t16 (.4 fixed texel units): one full wrap = 256 px = 4096 t16.
    // Screen shows the full 256-px panorama at 1:1 (matches GBA's Mode 7
    // sky which just scrolls a 256-px tilemap; a perspective-correct
    // ~86°/360° slice looked too stretched compared to the reference).
    // Match GBA's update_sky_scroll exactly: pixScroll = (cam_ang * 256) >> 16
    // and 1:1 panorama-to-screen mapping (one texture pixel = one screen
    // pixel, full 256-px panorama spans full 256-px screen).
    // -1280 t16 = -80 px shift in source = panorama appears 80 px RIGHT.
    int uOffset = ((int)cam_angle * 4096) >> 16;
    int uLeft  = uOffset - 1280;
    int uRight = uOffset - 1280 + 4096;  // 1:1 panorama mapping (matches GBA)
    int vTop   = 0;
    int vBot   = 4096;             // full 256-row panorama — anything smaller stretches each texel taller

    // Quad pushed to the far depth so all meshes draw on top.
    // v16 range is ±8 (4.12 fixed); z = -7.9 is as far as a vertex can go.
    // Then the quad's X/Y extent must be large enough to fill the screen at
    // that depth: with 70° FOV, halfTan ≈ 0.7 → half-extent = 7.9 * 0.7 ≈ 5.5.
    // Use 7.9 to be safe.
    // Local v16 coords scaled by 200x. Quad sized to EXACTLY match the
    // visible screen at depth 900 (half-width = 900*tan(43°) ≈ 840 DS,
    // half-height = 900*tan(35°) ≈ 630). With the quad exactly visible,
    // uRight-uLeft = 4096 maps the full 256-px panorama 1:1 to screen
    // width (was ~215 px visible before because the over-sized quad
    // wasted texture coords past the screen edges).
    int16_t qZ  = floattov16( 0.0f);
    int16_t qXl = floattov16(-4.2f), qXr = floattov16( 4.2f);
    int16_t qYt = floattov16( 3.15f), qYb = floattov16(-3.15f);

    glBegin(GL_QUADS);
        glTexCoord2t16(uLeft,  vTop); glVertex3v16(qXl, qYt, qZ);
        glTexCoord2t16(uRight, vTop); glVertex3v16(qXr, qYt, qZ);
        glTexCoord2t16(uRight, vBot); glVertex3v16(qXr, qYb, qZ);
        glTexCoord2t16(uLeft,  vBot); glVertex3v16(qXl, qYb, qZ);
    glEnd();

    glPopMatrix(1);
    glBindTexture(0, 0);
}
#endif

void afn_fps3d_init(void)
{
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
    vramSetBankB(VRAM_B_MAIN_BG);
    m7_bg = bgInit(2, BgType_Rotation, BgSize_R_256x256, 4, 0);
    bgSetPriority(m7_bg, 3);
    load_floor();
#endif
    // Load the sky panorama FIRST. It's a single 256x256 texture (64KB) and
    // needs one CONTIGUOUS 64KB block in texture VRAM bank A (128KB total).
    // The mesh textures are small 4bpp tiles (~46KB total); if they load first
    // they scatter across bank A and leave no contiguous 64KB hole, so the sky
    // upload fails silently (glGenTextures succeeds but glTexImage2D can't
    // place it) and the sky vanishes once a scene has enough meshes. Grabbing
    // the big block first guarantees it; the small mesh textures then fill the
    // remaining ~64KB. (Bank B is the Mode-7 floor, banks C/D fail as 3D
    // texture banks, so we can't just add VRAM — order is what matters here.)
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
    load_sky_texture();
#endif
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    load_mesh_textures();
#endif

    cam_h     = AFN_CAM_H;
    cam_angle = AFN_CAM_ANGLE;
    g_cosf    = brad_cos(cam_angle);
    g_sinf    = brad_sin(cam_angle);

    // Initialize player position from the player sprite (3rd-person camera
    // follows). Fall back to AFN_CAM_X/Z (free-cam start) if no player sprite.
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    player_x = afn_sprite_data[AFN_PLAYER_IDX][0];
    player_y = afn_sprite_data[AFN_PLAYER_IDX][1];
    player_z = afn_sprite_data[AFN_PLAYER_IDX][2];
#else
    player_x = AFN_CAM_X;
    player_z = AFN_CAM_Z;
    player_y = 0;
#endif
#ifdef AFN_HAS_SCRIPT
    // Seed Respawn defaults at player spawn so the Respawn node works
    // before any UpdateRespawnPos / checkpoint hit.
    afn_start_x = player_x;
    afn_start_y = player_y;
    afn_start_z = player_z;
#endif
    orbit_dist = AFN_ORBIT_DIST;
    // Camera = player - orbit_dist along view forward. NDS convention:
    // forward = (sin, cos), so camera sits at -(sin, cos) * dist behind player.
    cam_x = player_x - ((g_sinf * orbit_dist) >> 8);
    cam_z = player_z - ((g_cosf * orbit_dist) >> 8);

    glLight(0,
            RGB15(31, 31, 31),
            floattov10(-0.5f),
            floattov10(-0.7f),
            floattov10(-0.5f));
    glMaterialf(GL_AMBIENT,  RGB15(8, 8, 8));
    glMaterialf(GL_DIFFUSE,  RGB15(28, 28, 28));
    glMaterialf(GL_SPECULAR, RGB15(0, 0, 0));
    glMaterialf(GL_EMISSION, RGB15(0, 0, 0));
}

// ---------------------------------------------------------------------------
// Scene transitions — brightness-ramp fade out → swap → fade back in.
// ---------------------------------------------------------------------------
void afn_scene_start_transition(int sceneIdx, int sceneMode, int fadeFrames)
{
    // Idempotent: if a transition is already running toward the same
    // target, don't restart the fade counter. ChangeScene fires from
    // OnCollision2D every frame the player stays in the radius — without
    // this guard the counter never reached 0 and the swap only happened
    // after the player backed out of the trigger.
    if (s_fade_phase != 0 &&
        afn_pending_scene == sceneIdx && afn_pending_scene_mode == sceneMode)
        return;
    if (fadeFrames < 1) fadeFrames = 15;
    afn_pending_scene      = sceneIdx;
    afn_pending_scene_mode = sceneMode;
    afn_fade_frames        = fadeFrames;
    afn_fade_counter       = fadeFrames;
    afn_fade_target        = -16;
    s_fade_phase           = 1;
}

void afn_scene_tick(void)
{
    if (s_fade_phase == 0) {
        REG_MASTER_BRIGHT = 0;
        return;
    }

    int cur;
    if (afn_fade_counter > 0) afn_fade_counter--;

    if (s_fade_phase == 1) {
        int t = afn_fade_frames - afn_fade_counter;
        cur = (afn_fade_target * t) / (afn_fade_frames ? afn_fade_frames : 1);
        if (afn_fade_counter == 0) {
            afn_current_scene = afn_pending_scene;
            afn_current_mode  = afn_pending_scene_mode;
            afn_pending_scene = -1;
            afn_fade_counter  = afn_fade_frames;
            s_fade_phase      = 2;
#ifdef AFN_HAS_SCRIPT
            // Scene-load reset: respawn player, clear momentum + transient
            // anim state, restore sprite visibility / collision so the new
            // scene starts fresh. Mirrors GBA's scene_load() but keeps
            // afn_flags / afn_score / afn_vars persistent across scenes.
            player_x = afn_start_x;
            player_y = afn_start_y;
            player_z = afn_start_z;
            player_vy = 0;
            player_moving = 0;
            afn_play_anim = -1;
            afn_anim_prio = 0;
            afn_collided_sprite = -1;
            afn_frame_count = 0;
            afn_player_frozen = 0;
            afn_shake_frames = 0;
            for (int i = 0; i < NUM_SPRITES; i++) {
                afn_sprite_visible[i] = 1;
                afn_collision_enabled[i] = 1;
                afn_hp[i] = 100;
                afn_state_timer[i] = 0;
                afn_sprite_flip[i] = 0;
            }
#endif
#ifdef AFN_HAS_MODE0
            // If swapping into Mode 0, load the destination scene's tilemap
            // (tile gfx + palette + map) into BG VRAM. Without this the BG
            // still shows the previous scene (or boot scene).
            if (afn_current_mode == 1) {
                extern void afn_mode0_init_scene(int sceneIdx);
                afn_mode0_init_scene(afn_current_scene);
            }
#endif
            // If swapping back into Mode 4 from Mode 0, restore the 3D
            // video mode + texture VRAM and re-run fps3d_init so all
            // textures / floor / sky reload. mode0_init clobbered VRAM_A
            // (it was MAIN_BG holding tilemap tiles). Re-init OAM too —
            // videoSetMode clears the sprite mapping size bits to 1D_32
            // default, which made our 1D_128-addressed sprite tile
            // pointers land on garbage data (sprites rendered "snapped
            // in half"). oamInit reasserts SpriteMapping_1D_128 and the
            // OBJ-on-top priority dance.
            if (afn_current_mode == 0) {
                // Replay the full boot 3D init sequence — videoSetMode +
                // glInit leaves the geometry engine in a state that needs
                // every step. Earlier attempts that only restored viewport
                // / VRAM / OAM left the right ~64px of the screen black
                // (3D output drew into only the first 192px wide region).
                videoSetMode(MODE_0_3D | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);
                vramSetBankA(VRAM_A_TEXTURE);
                vramSetBankE(VRAM_E_TEX_PALETTE);
                oamInit(&oamMain, SpriteMapping_1D_128, false);
                REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
                // Mode 0 was scrolling BG0 to track the player camera; for
                // Mode 4 the same BG0 is the 3D layer, and a non-zero scroll
                // shifts the entire 3D output sideways — that's what made
                // the right ~64px render black.
                REG_BG0HOFS = 0;
                REG_BG0VOFS = 0;
                // Don't re-call glInit() — second invocation leaves the
                // GE half-initialised. But the texture allocator's
                // internal bookkeeping still points at VRAM_A from before
                // it was reassigned to MAIN_BG, so glGenTextures returns
                // handles backed by stale/unmapped memory and the next
                // load_mesh_textures uploads silently fail (the whole
                // backdrop renders white on the 2nd Mode 0 -> Mode 4 swap
                // even though meshes are still drawn). glResetTextures
                // wipes that bookkeeping so the re-upload lands cleanly.
                glResetTextures();
                glResetMatrixStack();
                glEnable(GL_TEXTURE_2D);
#if defined(AFN_NDS_AA) && AFN_NDS_AA
                glEnable(GL_ANTIALIAS);
#else
                glDisable(GL_ANTIALIAS);
#endif
                glClearColor(10, 18, 31, 31);
                glClearPolyID(63);
                glClearDepth(0x7FFF);
                glViewport(0, 0, 255, 191);
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                gluPerspective(70, 256.0 / 192.0, 0.1, 1024);
                glFlush(0);
                afn_fps3d_init();
                // Sprite VRAM still holds whatever Mode 0 last DMA'd, but
                // sprite_update / mode0 share g_active_frame[] as their
                // "what's loaded" cache. Reset so each asset re-DMAs its
                // proper Mode-4 frame on the next render.
#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
                extern int g_active_frame[AFN_ASSET_COUNT];
                for (int ai = 0; ai < AFN_ASSET_COUNT; ai++) g_active_frame[ai] = -1;
#endif
            }
#ifdef AFN_HAS_SCRIPT
            // Re-fire OnStart for BPs that live in the new scene. Without
            // this, scene-1 BPs only ran once at boot and never re-armed on
            // ChangeScene (e.g. the song never restarted in the new scene).
            extern void afn_bp_dispatch_start(void);
            afn_bp_dispatch_start();
#endif
        }
    } else {
        int t = afn_fade_counter;
        cur = (afn_fade_target * t) / (afn_fade_frames ? afn_fade_frames : 1);
        if (afn_fade_counter == 0) s_fade_phase = 0;
    }

    int level = cur < 0 ? -cur : cur;
    if (level > 16) level = 16;
    REG_MASTER_BRIGHT = (cur < 0)
        ? (level | (1 << 14))
        : (cur > 0 ? (level | (2 << 14)) : 0);
}

// ---------------------------------------------------------------------------
// Per-frame render: FPS camera + project meshes.
// ---------------------------------------------------------------------------
void afn_fps3d_update(void)
{
    update_camera();

    // Re-set projection every frame (init-time set doesn't survive to first
    // user frame — cheap to redo each frame and always correct).
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0 / 192.0, 0.1, 1024);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    int lookX = cam_x + (g_sinf << 2);
    int lookZ = cam_z + (g_cosf << 2);

    // Pitch the camera down so the horizon lands at m7_horizon (in NDS
    // screen pixels). sprites.c reads the same m7_horizon to align its
    // sprite projection — single source of truth keeps meshes + OAM
    // sprites lined up regardless of the pitch tuning constant.
    //   pitch_rad = screenOffPx * (70° / 192) * (π/180) ≈ screenOffPx * 0.00637
    //   lookY_offset_fx8 = look_dist_fx8 * pitch_rad
    //                    = 1024 * 0.00637 * screenOffPx ≈ 6.5 * screenOffPx
    // Round to 7 for cheap integer math (~6% high but visually fine).
    int lookY = cam_h;
    {
        m7_horizon = (AFN_CAM_HORIZON * 6) / 5;     // GBA px → NDS px
        int screenOffPx = 96 - m7_horizon;          // +ve = look down
        lookY -= screenOffPx * 7;
    }

    gluLookAtf32(
        fx8_to_f32(cam_x), fx8_to_f32(cam_h), fx8_to_f32(cam_z),
        fx8_to_f32(lookX), fx8_to_f32(lookY), fx8_to_f32(lookZ),
        0, inttof32(1), 0
    );

#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
    render_sky();
#endif
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    render_meshes();
#endif

    glFlush(0);
}

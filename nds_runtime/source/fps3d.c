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
uint16_t player_move_angle;

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
        int texH = afn_mesh_desc[i][9];
        if (texW == 0) continue;

        // glTexImage2D wants size as the TEXTURE_SIZE_* enum
        // (TEXTURE_SIZE_8=0, _16=1, _32=2, _64=3, _128=4, _256=5, _512=6, _1024=7),
        // not the raw pixel count. log2(texW) - 3 gets us there for power-of-2 sizes.
        int sizeEnum = 0, tw = texW;
        while (tw > 8) { tw >>= 1; sizeEnum++; }

        glGenTextures(1, &gl_tex_ids[i]);
        glBindTexture(0, gl_tex_ids[i]);
        // Texture parameter flags:
        //   TEXGEN_TEXCOORD         — use UVs from glTexCoord2t16
        //   GL_TEXTURE_WRAP_S/_T    — tile when UVs go past 0..texSize
        //                             (default is CLAMP → bricks stop at edges)
        //   GL_TEXTURE_COLOR0_TRANSPARENT — palette index 0 = transparent
        glTexImage2D(0, 0, GL_RGB16, sizeEnum, sizeEnum, 0,
                     TEXGEN_TEXCOORD, afn_mesh_tex_ptrs[i]);
        glColorTableEXT(0, 0, 16, 0, 0, afn_mesh_tex_pal_ptrs[i]);
    }
}

static void render_meshes(void)
{
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
}
#endif

// ---------------------------------------------------------------------------
// Input + camera tick (WASD = D-pad: W/S = walk fwd/back, A/D = turn)
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
    if (held & KEY_LEFT)  cam_angle += 512;
    if (held & KEY_RIGHT) cam_angle -= 512;
#endif

    g_cosf = brad_cos(cam_angle);
    g_sinf = brad_sin(cam_angle);

#ifdef AFN_HAS_SCRIPT
    // Script-driven path: MovePlayer nodes set afn_input_fwd/right, Walk/
    // Sprint nodes set afn_move_speed. Map view-space input → world XZ via
    // the camera basis (forward = (sin,cos), right = (cos,-sin) for our
    // gluLookAt convention). script_tick ran before fps3d_update so the
    // values are fresh.
    int fwd = afn_input_fwd, right = afn_input_right;
    if (fwd && right) { fwd = (fwd * 181) >> 8; right = (right * 181) >> 8; }
    int spd = afn_move_speed;
    int dx = ((g_sinf * fwd + g_cosf * right) >> 8);
    int dz = ((g_cosf * fwd - g_sinf * right) >> 8);
    player_x += FX_MUL(dx, spd);
    player_z += FX_MUL(dz, spd);
    player_moving = (fwd != 0 || right != 0);
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
        int floorY;
        int onFloor = afn_collide_floor(player_x, player_z, player_y, &floorY);
        if (onFloor && player_y <= floorY) {
            player_y = floorY;
            player_vy = 0;
            player_on_ground = 1;
        } else {
            player_on_ground = 0;
        }
    }
    afn_collide_walls(&player_x, &player_z, player_y);
#else
    {
        // No mesh collision data — fall back to flat ground at player init Y.
        int groundY = afn_sprite_data[AFN_PLAYER_IDX][1];
        if (player_y <= groundY) {
            player_y = groundY;
            player_vy = 0;
            player_on_ground = 1;
        } else {
            player_on_ground = 0;
        }
    }
#endif
    s_playerY = player_y - afn_sprite_data[AFN_PLAYER_IDX][1];

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
    // cam_h is the camera's world Y. AFN_CAM_H is reinterpreted as the
    // camera's offset ABOVE the player (the GBA Mode 7 version treated it
    // as an absolute height above Y=0, but our 3D scenes have meshes
    // floating in the air, so the camera must follow the player's world Y).
    cam_h = player_y + AFN_CAM_H;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
static void load_sky_texture(void)
{
    glGenTextures(1, &gl_sky_tex_id);
    glBindTexture(0, gl_sky_tex_id);
    // Texture size + dimensions come from the exporter — using the
    // panorama's native PNG size avoids the 2x4 nearest-neighbour
    // upsample that previously gave the sky visible speckle artefacts.
    // GL_TEXTURE_WRAP_S = horizontal wrap so the panorama can scroll
    // past its texel-width edge as cam_angle rotates past 360°.
    glTexImage2D(0, 0, GL_RGB256, AFN_SKY_TEX_SIZE_W, AFN_SKY_TEX_SIZE_H, 0,
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
    // Map to t16 (.4 fixed texel units): one full wrap = AFN_SKY_W texels
    // = AFN_SKY_W*16 t16 units. -80 px shift in source ≈ -SKY_W*5 t16.
    int fullWrap = AFN_SKY_W * 16;
    int shiftRight = (AFN_SKY_W * 5);    // ~80px on a 256-w pano scales down
    int uOffset = ((int)cam_angle * fullWrap) >> 16;
    int uLeft  = uOffset - shiftRight;
    int uRight = uOffset - shiftRight + fullWrap;
    int vTop   = 0;
    int vBot   = AFN_SKY_H * 16;   // full pano height in t16 units

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
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    load_mesh_textures();
#endif
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
    load_sky_texture();
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

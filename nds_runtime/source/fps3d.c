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

// ---------------------------------------------------------------------------
// Scene-transition state (defs match affinity.h externs)
// ---------------------------------------------------------------------------
int afn_pending_scene      = -1;
int afn_pending_scene_mode = 0;
int afn_current_scene      = 0;
int afn_current_mode       = 0;
int afn_fade_target        = 0;
int afn_fade_counter       = 0;
int afn_fade_frames        = 0;
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
            int cr = (color & 0x1F);
            int cg = ((color >> 5) & 0x1F);
            int cb = ((color >> 10) & 0x1F);
            r = (cr << 3) | (cr >> 2);
            g = (cg << 3) | (cg >> 2);
            b = (cb << 3) | (cb >> 2);
            if (grayscale) {
                int y = (r * 76 + g * 150 + b * 29) >> 8;
                r = g = b = y;
            }
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
static void update_camera(void)
{
    scanKeys();
    uint32_t held = keysHeld();
    {
        static int s_dbgFrame = 0;
        s_dbgFrame++;
        if ((s_dbgFrame & 15) == 0) {
            iprintf("\x1b[10;0Hkeys=%04x cx=%d cz=%d ca=%d   ",
                    (unsigned)held, cam_x >> FX_SHIFT, cam_z >> FX_SHIFT, (int)cam_angle);
        }
    }

    int speed = AFN_WALK_SPEED;

    // Q (KEY_L) ascend, E (KEY_R) descend — useful for poking at meshes from
    // different heights while we tune the NDS scene scale.
    if (held & KEY_L) cam_h += AFN_WALK_SPEED;
    if (held & KEY_R) cam_h -= AFN_WALK_SPEED;

    if (held & KEY_LEFT)  cam_angle -= 512;
    if (held & KEY_RIGHT) cam_angle += 512;

    g_cosf = brad_cos(cam_angle);
    g_sinf = brad_sin(cam_angle);

    int dx = 0, dz = 0;
    if (held & KEY_UP)    { dx += g_sinf; dz += g_cosf; }
    if (held & KEY_DOWN)  { dx -= g_sinf; dz -= g_cosf; }

    cam_x += FX_MUL(dx, speed);
    cam_z += FX_MUL(dz, speed);

    if (held & KEY_A) m7_horizon++;
    if (held & KEY_B) m7_horizon--;
    if (m7_horizon < 10) m7_horizon = 10;
    if (m7_horizon > 180) m7_horizon = 180;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
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

    cam_x     = AFN_CAM_X;
    cam_z     = AFN_CAM_Z;
    cam_h     = AFN_CAM_H;
    cam_angle = AFN_CAM_ANGLE;
    g_cosf    = brad_cos(cam_angle);
    g_sinf    = brad_sin(cam_angle);

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

    gluLookAtf32(
        fx8_to_f32(cam_x), fx8_to_f32(cam_h), fx8_to_f32(cam_z),
        fx8_to_f32(lookX), fx8_to_f32(cam_h), fx8_to_f32(lookZ),
        0, inttof32(1), 0
    );

#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    render_meshes();
#endif

    glFlush(0);
}

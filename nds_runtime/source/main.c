// Affinity NDS Runtime — Hardware 3D + Mode 7 affine floor
// Built with devkitARM + libnds

#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <nds/arm9/videoGL.h>

#include "mapdata.h"

// ---------------------------------------------------------------------------
// Fixed-point helpers (DS uses 1.19.12 for f32, 1.3.12 for v16)
// ---------------------------------------------------------------------------
#define FX_SHIFT  8
#define FX_ONE    (1 << FX_SHIFT)
#define FX_MUL(a, b) (((a) * (b)) >> FX_SHIFT)

// Convert our 16.8 fixed to DS f32 (20.12)
static inline int32_t fx8_to_f32(int fx8)
{
    return fx8 << 4; // 8.8 -> 20.12: shift left 4
}

// Convert our 16.8 fixed to DS v16 (1.3.12) — for vertex positions
// v16 range is roughly -8.0 to +7.999
static inline int16_t fx8_to_v16(int fx8)
{
    return (int16_t)(fx8 << 4); // 8.8 -> 4.12
}

// ---------------------------------------------------------------------------
// Camera state
// ---------------------------------------------------------------------------
static int cam_x;       // 16.8 fixed
static int cam_z;       // 16.8 fixed
static int cam_h;       // 16.8 fixed (height)
static uint16_t cam_angle; // brad (0-65535)

// Precomputed sin/cos (16.8 scale)
static int g_cosf, g_sinf;

// Orbit camera
static int player_x, player_z;
static int player_y;
static uint16_t orbit_angle;
static int orbit_dist;
static int player_sprite_idx = -1;
static int player_moving;
static uint16_t player_move_angle;

// Horizon for Mode 7 floor
static int m7_horizon = 60;

// LUT for sin/cos (libnds provides sinLerp/cosLerp)
static inline int brad_sin(uint16_t angle)
{
    // sinLerp returns .12 fixed, we want .8
    return sinLerp(angle) >> 4;
}

static inline int brad_cos(uint16_t angle)
{
    return cosLerp(angle) >> 4;
}

// ---------------------------------------------------------------------------
// Mode 7 floor — HBlank driven affine BG
// ---------------------------------------------------------------------------
static int m7_bg;

static void m7_hbl(void)
{
    int vc = REG_VCOUNT;
    int row = vc - m7_horizon;

    if (row <= 0)
    {
        // Above horizon — hide floor by scaling to zero
        REG_BG2PA = 0;
        REG_BG2PB = 0;
        REG_BG2PC = 0;
        REG_BG2PD = 0;
        REG_BG2X = 0;
        REG_BG2Y = 0;
        return;
    }

    // Perspective projection: scale = D / row
    // D is focal length (~160 for good look)
    int D = 160;
    int lam = (D << 8) / row; // .8 fixed

    int lcf = FX_MUL(lam, g_cosf);
    int lsf = FX_MUL(lam, g_sinf);

    REG_BG2PA = (int16_t)(lcf >> 0);
    REG_BG2PC = (int16_t)(lsf >> 0);
    REG_BG2PB = (int16_t)(-lsf >> 0);
    REG_BG2PD = (int16_t)(lcf >> 0);

    // Scroll: camera position in tilemap space (each tile = 8 pixels)
    int cx = cam_x >> FX_SHIFT;
    int cz = cam_z >> FX_SHIFT;

    int dx = 128 - FX_MUL(lcf, 128) + FX_MUL(lsf, 128);
    int dy = 128 - FX_MUL(lsf, 128) - FX_MUL(lcf, 128);

    REG_BG2X = (cx << 8) + (dx << 0);
    REG_BG2Y = (cz << 8) + (dy << 0);
}

// ---------------------------------------------------------------------------
// Init video, GL, backgrounds
// ---------------------------------------------------------------------------
static void init_video(void)
{
    // Power on everything
    powerOn(POWER_ALL_2D);

    // Top screen: Mode 0 with 3D on BG0
    videoSetMode(MODE_0_3D);

    // Bottom screen: console for debug
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleDemoInit();

    // VRAM banks
    vramSetBankA(VRAM_A_TEXTURE);     // 128KB for 3D textures
    vramSetBankB(VRAM_B_MAIN_BG);     // 128KB for affine BG tiles/map
    vramSetBankE(VRAM_E_TEX_PALETTE); // Texture palettes

    // OAM for sprites
    oamInit(&oamMain, SpriteMapping_1D_32, false);
    vramSetBankF(VRAM_F_MAIN_SPRITE); // 16KB for sprite tiles

    // Init 3D engine
    glInit();
    glEnable(GL_ANTIALIAS | GL_TEXTURE_2D);
    glClearColor(8, 12, 20, 31); // dark sky
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);

    // Projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0 / 192.0, 0.1, 100);

    // Setup BG2 as affine rotation background for Mode 7 floor
    // BG2 behind 3D layer (priority 3 = lowest = drawn behind)
    m7_bg = bgInit(2, BgType_Rotation, BgSize_R_256x256, 4, 0);
    bgSetPriority(m7_bg, 3);

    // BG0 (3D) at priority 0 (highest = drawn in front)
    // This is set automatically by MODE_0_3D
}

// ---------------------------------------------------------------------------
// Load tilemap data into BG2
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
static void load_floor(void)
{
    // Copy tile pixel data to BG tile VRAM
    dmaCopy(afn_floor_tiles, BG_TILE_RAM(0), AFN_FLOOR_TILES_LEN);

    // Copy tilemap to BG map VRAM
    dmaCopy(afn_floor_map, BG_MAP_RAM(4), AFN_FLOOR_MAP_LEN);

    // Copy palette
    dmaCopy(afn_floor_pal, BG_PALETTE, AFN_FLOOR_PAL_LEN);
}
#endif

// ---------------------------------------------------------------------------
// Load mesh textures into VRAM
// ---------------------------------------------------------------------------
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
static int gl_tex_ids[AFN_MESH_COUNT];

static void load_mesh_textures(void)
{
    int i;
    for (i = 0; i < AFN_MESH_COUNT; i++)
    {
        gl_tex_ids[i] = 0;
        if (!afn_mesh_desc[i][8]) continue; // not textured

        int texW = afn_mesh_desc[i][9];
        int texH = afn_mesh_desc[i][9]; // square textures
        if (texW == 0) continue;

        glGenTextures(1, &gl_tex_ids[i]);
        glBindTexture(0, gl_tex_ids[i]);

        // Upload as 16-color paletted (4bpp)
        glTexImage2D(0, 0, GL_RGB16, texW, texH, 0,
                     TEXGEN_TEXCOORD, afn_mesh_tex_ptrs[i]);

        // Upload palette
        glColorTableEXT(0, 0, 16, 0, 0, afn_mesh_tex_pal_ptrs[i]);
    }
}
#endif

// ---------------------------------------------------------------------------
// Render 3D meshes
// ---------------------------------------------------------------------------
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
static void render_meshes(void)
{
    int si;
    for (si = 0; si < AFN_SPRITE_COUNT; si++)
    {
        int meshIdx = afn_sprite_data[si][9]; // meshIdx field
        if (meshIdx < 0 || meshIdx >= AFN_MESH_COUNT) continue;

        // Get world position (16.8 fixed)
        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];

        // Get rotation (brad)
        int rot = afn_sprite_data[si][7];

        const int16_t* verts = afn_mesh_vert_ptrs[meshIdx];
        const int8_t*  norms = afn_mesh_norm_ptrs[meshIdx];
        const uint16_t* idx  = afn_mesh_idx_ptrs[meshIdx];

        int vertCount  = afn_mesh_desc[meshIdx][0];
        int indexCount  = afn_mesh_desc[meshIdx][1];
        int quadIdxCount = afn_mesh_desc[meshIdx][2];
        uint16_t color  = afn_mesh_desc[meshIdx][3];
        int cullMode    = afn_mesh_desc[meshIdx][4];
        int lit         = afn_mesh_desc[meshIdx][5];
        int textured    = afn_mesh_desc[meshIdx][8];

        glPushMatrix();

        // Transform to world position
        // Convert 16.8 fixed to DS f32 (20.12)
        glTranslatef32(fx8_to_f32(wx - cam_x),
                       fx8_to_f32(wy),
                       fx8_to_f32(wz - cam_z));

        // Rotation
        if (rot != 0)
            glRotateYi(rot << 16 >> 6); // brad to DS angle

        // Set polygon format
        uint32_t polyFmt = POLY_ALPHA(31);
        if (cullMode == 0) polyFmt |= POLY_CULL_BACK;
        else if (cullMode == 1) polyFmt |= POLY_CULL_FRONT;
        else polyFmt |= POLY_CULL_NONE;
        glPolyFmt(polyFmt);

        // Set color from mesh color (RGB15)
        int cr = (color & 0x1F);
        int cg = ((color >> 5) & 0x1F);
        int cb = ((color >> 10) & 0x1F);
        glColor3b((cr << 3) | (cr >> 2),
                  (cg << 3) | (cg >> 2),
                  (cb << 3) | (cb >> 2));

        // Bind texture if textured
        if (textured && gl_tex_ids[meshIdx])
            glBindTexture(0, gl_tex_ids[meshIdx]);

        // Draw triangles
        if (indexCount > 0)
        {
            glBegin(GL_TRIANGLES);
            int t;
            for (t = 0; t + 3 <= indexCount; t += 3)
            {
                int i0 = idx[t + 0];
                int i1 = idx[t + 1];
                int i2 = idx[t + 2];

                // Emit vertices (positions are 8.8 fixed, convert to v16 = 4.12)
                glVertex3v16(fx8_to_v16(verts[i0 * 3 + 0]),
                             fx8_to_v16(verts[i0 * 3 + 1]),
                             fx8_to_v16(verts[i0 * 3 + 2]));
                glVertex3v16(fx8_to_v16(verts[i1 * 3 + 0]),
                             fx8_to_v16(verts[i1 * 3 + 1]),
                             fx8_to_v16(verts[i1 * 3 + 2]));
                glVertex3v16(fx8_to_v16(verts[i2 * 3 + 0]),
                             fx8_to_v16(verts[i2 * 3 + 1]),
                             fx8_to_v16(verts[i2 * 3 + 2]));
            }
            glEnd();
        }

        // Draw quads
        if (quadIdxCount > 0)
        {
            const uint16_t* qidx = afn_mesh_qidx_ptrs[meshIdx];
            glBegin(GL_QUADS);
            int q;
            for (q = 0; q + 4 <= quadIdxCount; q += 4)
            {
                int i0 = qidx[q + 0];
                int i1 = qidx[q + 1];
                int i2 = qidx[q + 2];
                int i3 = qidx[q + 3];

                glVertex3v16(fx8_to_v16(verts[i0 * 3 + 0]),
                             fx8_to_v16(verts[i0 * 3 + 1]),
                             fx8_to_v16(verts[i0 * 3 + 2]));
                glVertex3v16(fx8_to_v16(verts[i1 * 3 + 0]),
                             fx8_to_v16(verts[i1 * 3 + 1]),
                             fx8_to_v16(verts[i1 * 3 + 2]));
                glVertex3v16(fx8_to_v16(verts[i2 * 3 + 0]),
                             fx8_to_v16(verts[i2 * 3 + 1]),
                             fx8_to_v16(verts[i2 * 3 + 2]));
                glVertex3v16(fx8_to_v16(verts[i3 * 3 + 0]),
                             fx8_to_v16(verts[i3 * 3 + 1]),
                             fx8_to_v16(verts[i3 * 3 + 2]));
            }
            glEnd();
        }

        glPopMatrix(1);
    }
}
#endif

// ---------------------------------------------------------------------------
// Input + camera update
// ---------------------------------------------------------------------------
static void update_camera(void)
{
    scanKeys();
    uint32_t held = keysHeld();

    int speed = AFN_WALK_SPEED;
    if (held & KEY_R) speed = AFN_SPRINT_SPEED;

    // Rotation
    if (held & KEY_LEFT)  cam_angle -= 512;
    if (held & KEY_RIGHT) cam_angle += 512;

    // Recompute sin/cos
    g_cosf = brad_cos(cam_angle);
    g_sinf = brad_sin(cam_angle);

    // Movement
    int dx = 0, dz = 0;
    if (held & KEY_UP)    { dx += g_sinf; dz += g_cosf; }
    if (held & KEY_DOWN)  { dx -= g_sinf; dz -= g_cosf; }

    cam_x += FX_MUL(dx, speed);
    cam_z += FX_MUL(dz, speed);

    // Horizon (pitch)
    if (held & KEY_A) m7_horizon++;
    if (held & KEY_B) m7_horizon--;
    if (m7_horizon < 10) m7_horizon = 10;
    if (m7_horizon > 180) m7_horizon = 180;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void)
{
    // Init
    init_video();

    // Load floor tilemap
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
    load_floor();
#endif

    // Load mesh textures
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    load_mesh_textures();
#endif

    // Setup HBlank for Mode 7 floor
    irqSet(IRQ_HBLANK, m7_hbl);
    irqEnable(IRQ_HBLANK);

    // Camera init
    cam_x     = AFN_CAM_X;
    cam_z     = AFN_CAM_Z;
    cam_h     = AFN_CAM_H;
    cam_angle = AFN_CAM_ANGLE;
    g_cosf    = brad_cos(cam_angle);
    g_sinf    = brad_sin(cam_angle);

    iprintf("Affinity NDS\n");

    while (pmMainLoop())
    {
        update_camera();

        // --- 3D rendering ---
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Camera look direction
        int lookX = cam_x + (g_sinf << 2);
        int lookZ = cam_z + (g_cosf << 2);

        gluLookAtf32(
            fx8_to_f32(cam_x), fx8_to_f32(cam_h), fx8_to_f32(cam_z),
            fx8_to_f32(lookX), 0,                  fx8_to_f32(lookZ),
            0,                 inttof32(1),         0
        );

#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
        render_meshes();
#endif

        glFlush(0);
        swiWaitForVBlank();
    }

    return 0;
}

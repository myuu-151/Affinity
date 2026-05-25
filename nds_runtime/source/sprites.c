// Affinity NDS — OAM sprite projection + render.
// Mirrors GBA's update_sprites: per-frame world→screen projection with
// affine-scale OBJs for distance shrink. Sprite assets are uploaded once
// at boot from afn_all_tiles + afn_pal (emitted by nds_package).

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/sprite.h>
#include <nds/arm9/cache.h>
#include <stdio.h>

#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
#define HAS_SPRITES 1
#endif

void afn_sprite_init(void)
{
#if defined(HAS_SPRITES)
    // Bank B (128KB) instead of bank F (16KB). Per-asset 8-direction
    // sprites overflow F immediately — 6 assets × ~16KB = ~96KB at the
    // 64x64-with-8-dirs upper bound.
    // Disable F first so it doesn't shadow B's first 16KB (NDS allows
    // multiple banks at the same MST address; reads from the overlap
    // are undefined and we saw exactly that symptom: only the first
    // asset rendered correctly, the rest were missing or scrambled).
    vramSetBankF(VRAM_F_LCD);
    vramSetBankB(VRAM_B_MAIN_SPRITE);
    // 1D_128: OAM tile slot is 10 bits → max addressable byte = 1024 ×
    // boundary. 1D_32 caps at 32KB, 1D_64 at 64KB; once per-asset
    // animation frames join the directional tiles, even 64KB is too
    // little (Sonic's 2 idle frames × 8 dirs × 64 tiles already fills
    // 32KB by itself). 1D_128 covers the full 128KB of bank B.
    // Per-asset byte start must be 128-byte aligned (multiple of 4 in
    // 32-byte tile units); the exporter aligns naturally because each
    // emitted unit is a frame × dir × tpf chunk.
    oamInit(&oamMain, SpriteMapping_1D_128, false);

    // Push the 3D layer (BG0) to the lowest priority so OBJ (default
    // priority 0) renders on top of the 3D scene.
    REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
    REG_DISPCNT |= DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;

#if AFN_ALL_TILES_LEN > 0
    {
        // Tightly-packed 4bpp tile data: each 8x8 tile = 32 bytes, sub-tiles
        // within a sprite read at 32-byte increments. The 1D_64 boundary only
        // scales the OAM tile-INDEX field (byte_addr = slot_index * 64), not
        // the inter-sub-tile stride. NDS VRAM rejects 8-bit writes → u32 copy.
        const uint32_t* src = afn_all_tiles;
        uint32_t* dst = (uint32_t*)0x06400000;
        int words = (AFN_ALL_TILES_LEN + 3) / 4;
        for (int i = 0; i < words; i++) dst[i] = src[i];
    }
#endif

#if AFN_ASSET_COUNT > 0
    // Upload each asset's 16-color palette into its OBJ palette bank.
    for (int ai = 0; ai < AFN_ASSET_COUNT; ai++) {
        int bank = afn_asset_desc[ai][4] & 0xF;
        uint16_t* dst = (uint16_t*)0x05000200 + bank * 16;
        for (int c = 0; c < 16; c++) dst[c] = (uint16_t)afn_pal[ai][c];
    }
#endif
#endif
}

// Pass-1 result: a sprite that passed depth+screen culling, ready for OAM
// emission once we sort by depth so nearer sprites get lower OAM slots
// (NDS OAM draws lower-indexed OBJs on top of higher-indexed ones).
typedef struct {
    int idx;          // sprite_data row index
    int depth;        // sort key (smaller = nearer = lower slot)
    int screenX, screenY;
    int matScale;
    int objSize;
    int palBank;
    int tileIdx;
} ProjSpr;

void afn_sprite_update(void)
{
#if defined(HAS_SPRITES)
    oamClear(&oamMain, 0, 128);

    // Global animation tick — assets sharing a default-anim fps stay in sync
    // across all instances. Per-sprite animation state would let different
    // instances diverge; punt that until script-driven anims (Phase 3) need it.
    static int s_animTick = 0;
    s_animTick++;

    static ProjSpr proj[AFN_SPRITE_COUNT];
    int projCount = 0;

    // Match gluPerspective(70°, 256/192): focal_px = 96/tan(35°) ≈ 137 in
    // both axes (the aspect ratio cancels through the X-axis half-width).
    const int focalLen = 137;
    const int screenHalfX = 128;
    // fps3d.c sets m7_horizon to the screen Y where the camera's horizon
    // lands after its downward pitch. Sharing that variable keeps sprites
    // glued to the meshes regardless of how the pitch multiplier is tuned.
    int screenHalfY = m7_horizon;

    for (int si = 0; si < AFN_SPRITE_COUNT; si++) {
        if (afn_sprite_data[si][9] >= 0) continue;  // mesh sprite — fps3d draws it
        int aIdx = afn_sprite_data[si][4];
        if (aIdx < 0 || aIdx >= AFN_ASSET_COUNT) continue;

        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0
        // Player sprite — read its mutable runtime position from fps3d.c so
        // the visible Sonic moves with the input that drives the camera.
        if (si == AFN_PLAYER_IDX) {
            wx = player_x;
            wy = player_y;
            wz = player_z;
        }
#endif

        int dx = wx - cam_x;
        int dy = wy - cam_h;
        int dz = wz - cam_z;

        // gluLookAt(eye, eye+(sin,0,cos), +Y) builds view_x = -cos*dx + sin*dz
        // (forward × up = (-cos,0,sin), so the camera's world-space right is
        // -X at cam_angle=0). Depth = -view_z = sin*dx + cos*dz.
        int depth = (dx * g_sinf + dz * g_cosf) >> 8;
        if (depth <= 64) continue;

        int viewX  = (-dx * g_cosf + dz * g_sinf) >> 8;
        int screenX = screenHalfX + (viewX * focalLen) / depth;
        int screenY = screenHalfY - (dy    * focalLen) / depth;

        int objSize = afn_asset_desc[aIdx][3];
        if (objSize <= 0) objSize = 16;
        // Screen bounds: keep sprites that have any chance of being visible.
        // OAM silently no-ops fully off-screen sprites, so bounds are only
        // here to avoid wasting OAM slots — no `scale < 1` far-distance cull,
        // which caused sprites to pop out when crossing depth > 137 editor px.
        if (screenX < -128 || screenX > 256 + 128) continue;
        if (screenY < -128 || screenY > 192 + 128) continue;

        int matScale = depth / 32;
        if (matScale < 128)  matScale = 128;
        if (matScale > 2048) matScale = 2048;

        // Direction picking: camera-space angle into the sprite's 8 facings.
        // Convention: dir 0 = sprite seen from south (+Z relative to sprite,
        // i.e. camera at +Z of sprite). Walking CCW around the sprite advances
        // through dirs 0..7. ForceStatic sprites (sprite_data[10]==1) always
        // use dir 0.
        int tileStart  = afn_asset_desc[aIdx][0];
        int tilesPerFr = afn_asset_desc[aIdx][1];
        int animCount  = afn_asset_desc[aIdx][2];
        int dirCount   = afn_asset_desc[aIdx][5];
        int animFps    = afn_asset_desc[aIdx][6];
        int animLoop   = afn_asset_desc[aIdx][7];
        int fStatic    = afn_sprite_data[si][10];
        int dir = 0;
        if (dirCount > 1 && !fStatic) {
            // 8-way direction from sprite-to-camera vector (-dx, -dz). Octant
            // 0 = camera at +Z (south of sprite), advancing CCW (looking down
            // +Y) through 1..7. Tan(22.5°) ≈ 0.414 ≈ 53/128 — used to test
            // whether a diagonal or a cardinal octant wins.
            int vx = -dx, vz = -dz;
            int ax = vx < 0 ? -vx : vx;
            int az = vz < 0 ? -vz : vz;
            // Compare ax vs az*tan(22.5°) and az vs ax*tan(22.5°) to find
            // which of the 8 octants holds the vector.
            int tanLo = (ax * 128) < (az * 53);   // ax dominated by az
            int tanHi = (az * 128) < (ax * 53);   // az dominated by ax
            if (vz >= 0 && vx >= 0) {                          // NE quadrant
                dir = tanLo ? 0 : (tanHi ? 2 : 1);
            } else if (vz < 0 && vx >= 0) {                    // SE quadrant
                dir = tanHi ? 2 : (tanLo ? 4 : 3);
            } else if (vz < 0 && vx < 0) {                     // SW quadrant
                dir = tanLo ? 4 : (tanHi ? 6 : 5);
            } else {                                           // NW quadrant
                dir = tanHi ? 6 : (tanLo ? 0 : 7);
            }
            if (dir >= dirCount) dir = 0;
        }

        // Animation cycle. Each anim frame holds for (60 / animFps) game
        // frames. Non-looping anims clamp at the last frame.
        int animFrame = 0;
        if (animCount > 1 && animFps > 0) {
            int hold = 60 / animFps; if (hold < 1) hold = 1;
            int step = s_animTick / hold;
            animFrame = animLoop ? (step % animCount)
                                 : (step >= animCount ? animCount - 1 : step);
        }

        proj[projCount].idx     = si;
        proj[projCount].depth   = depth;
        proj[projCount].screenX = screenX;
        proj[projCount].screenY = screenY;
        proj[projCount].matScale = matScale;
        proj[projCount].objSize = objSize;
        proj[projCount].palBank = afn_asset_desc[aIdx][4] & 0xF;
        proj[projCount].tileIdx = tileStart + (animFrame * dirCount + dir) * tilesPerFr;
        projCount++;
        if (projCount >= AFN_SPRITE_COUNT) break;
    }

    // Sort nearest first → lower OAM slot → drawn on top of farther sprites.
    // Insertion sort: N is small (≤ AFN_SPRITE_COUNT, typically <30).
    for (int i = 1; i < projCount; i++) {
        ProjSpr key = proj[i];
        int j = i - 1;
        while (j >= 0 && proj[j].depth > key.depth) {
            proj[j + 1] = proj[j];
            j--;
        }
        proj[j + 1] = key;
    }

    int oamSlot = 0, affineSlot = 0;
    for (int i = 0; i < projCount && oamSlot < 96 && affineSlot < 32; i++) {
        ProjSpr* p = &proj[i];
        SpriteSize sz = p->objSize == 8  ? SpriteSize_8x8   :
                        p->objSize == 16 ? SpriteSize_16x16 :
                        p->objSize == 32 ? SpriteSize_32x32 : SpriteSize_64x64;

        // Double-canvas: bottom-anchor needs scale compensation so feet stay
        // on the ground when the affine shrinks the sprite.
        int scaledHalfH = (p->objSize * 256) / (p->matScale * 2);
        int topLeftY = p->screenY - scaledHalfH - p->objSize;
        int topLeftX = p->screenX - p->objSize;
        int canvasSize = p->objSize * 2;

        // OAM Y is 8-bit (0–255) and wraps. A sprite whose render box
        // crosses the wrap edge gets its bottom half torn to the top of
        // the screen. Drop sprites in the unsafe zone instead of drawing.
        if (topLeftY + canvasSize <= 0 || topLeftY >= 192) continue;
        if (topLeftY >= 0 && topLeftY + canvasSize > 255) continue;
        if (topLeftX + canvasSize <= 0 || topLeftX >= 256) continue;

        oamRotateScale(&oamMain, affineSlot, 0, p->matScale, p->matScale);
        oamSet(&oamMain, oamSlot,
               topLeftX, topLeftY,
               0, p->palBank, sz, SpriteColorFormat_16Color,
               (void*)((u8*)0x06400000 + p->tileIdx * 32),
               affineSlot, true, false, false, false, false);
        oamSlot++;
        affineSlot++;
    }

    {
        static int s_dbgFrame = 0;
        s_dbgFrame++;
        if ((s_dbgFrame & 31) == 0)
            iprintf("\x1b[16;0Hsprites=%d ", oamSlot);
    }

    oamUpdate(&oamMain);
#else
    (void)0;
#endif
}

#if !defined(HAS_SPRITES)
void afn_sprite_init(void) {}
void afn_sprite_update(void) {}
#endif

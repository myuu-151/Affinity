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
    vramSetBankF(VRAM_F_MAIN_SPRITE);
    // 1D_32 matches our packed-4bpp emission (32 bytes per 8x8 tile).
    oamInit(&oamMain, SpriteMapping_1D_32, false);

    // Push the 3D layer (BG0) to the lowest priority so OBJ (default
    // priority 0) renders on top of the 3D scene.
    REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
    REG_DISPCNT |= DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;

#if AFN_ALL_TILES_LEN > 0
    {
        // VRAM rejects 8-bit writes — must copy as 32-bit words.
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

void afn_sprite_update(void)
{
#if defined(HAS_SPRITES)
    oamClear(&oamMain, 0, 128);

    int oamSlot = 0;
    int affineSlot = 0;
    // Match gluPerspective(70°, 256/192): focal_px = 96/tan(35°) ≈ 137 in
    // both axes (the aspect ratio cancels through the X-axis half-width).
    const int focalLen = 137;
    const int screenHalfX = 128;
    const int screenHalfY = 96;   // 3D scene horizon is always screen center

    for (int si = 0; si < AFN_SPRITE_COUNT && oamSlot < 96 && affineSlot < 32; si++) {
        if (afn_sprite_data[si][9] >= 0) continue;  // mesh sprite — fps3d draws it
        int aIdx = afn_sprite_data[si][4];
        if (aIdx < 0 || aIdx >= AFN_ASSET_COUNT) continue;

        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];

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
        int scale   = (FX_ONE   * focalLen) / depth;  // 1 world unit at this depth

        int objSize = afn_asset_desc[aIdx][3];
        if (objSize <= 0) objSize = 16;
        if (screenX < -objSize || screenX > 256 + objSize) continue;
        if (screenY < -objSize || screenY > 192 + objSize) continue;
        if (scale < 1) continue;

        int tileStart  = afn_asset_desc[aIdx][0];
        int tilesPerFr = afn_asset_desc[aIdx][1];
        int palBank    = afn_asset_desc[aIdx][4] & 0xF;
        int tileIdx    = tileStart + 0 * tilesPerFr;   // frame 0 for now

        SpriteSize sz = objSize == 8  ? SpriteSize_8x8  :
                        objSize == 16 ? SpriteSize_16x16 :
                        objSize == 32 ? SpriteSize_32x32 : SpriteSize_64x64;
        // Bottom-anchored: sprite feet at (screenX, screenY). Matches GBA's
        // proj[i].screenY semantics — projection returns the ground contact.
        oamSet(&oamMain, oamSlot,
               screenX - objSize/2, screenY - objSize,
               0, palBank, sz, SpriteColorFormat_16Color,
               (void*)((u8*)0x06400000 + tileIdx * 32),
               -1, false, false, false, false, false);

        if (oamSlot == 0) {
            static int s_f = 0;
            s_f++;
            if ((s_f & 15) == 0)
                iprintf("\x1b[17;0Hsp0 sX=%d sY=%d aI=%d ti=%d  ",
                        screenX, screenY, aIdx, tileIdx);
        }

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

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
    oamInit(&oamMain, SpriteMapping_1D_32, false);

    // Push the 3D layer (BG0) to the lowest priority so OBJ (default
    // priority 0) renders on top of the 3D scene.
    REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
    // Belt-and-braces: ensure OBJ enable + 1D mapping survive any later
    // bgInit() in the fps3d setup path.
    REG_DISPCNT |= DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;

#if AFN_ALL_TILES_LEN > 0
    // Upload the entire combined tile bank to OBJ VRAM at slot 0. byte-by-
    // byte copy avoids the dmaCopy-from-const-data cache-coherency issue
    // that left VRAM all zeros earlier.
    {
        const uint8_t* src = (const uint8_t*)afn_all_tiles;
        uint8_t* dst = (uint8_t*)0x06400000;
        for (int i = 0; i < AFN_ALL_TILES_LEN; i++) dst[i] = src[i];
    }
#endif

    // DEBUG: force every OBJ palette bank to {transparent, red, green, blue, ...}
    // so any sprite tile with a non-zero palette index renders as a clear color.
    {
        uint16_t fallback[16] = {
            0, RGB15(31, 0, 0), RGB15(0, 31, 0), RGB15(0, 0, 31),
            RGB15(31,31,0), RGB15(31,0,31), RGB15(0,31,31), RGB15(31,31,31),
            RGB15(15, 0, 0), RGB15(0,15,0), RGB15(0,0,15), RGB15(15,15,0),
            RGB15(15,0,15), RGB15(0,15,15), RGB15(20,20,20), RGB15(31,16,0),
        };
        for (int bank = 0; bank < 16; bank++) {
            uint16_t* dst = (uint16_t*)0x05000200 + bank * 16;
            for (int c = 0; c < 16; c++) dst[c] = fallback[c];
        }
    }
#endif
}

void afn_sprite_update(void)
{
#if defined(HAS_SPRITES)
    // Explicit clear of all 128 slots — count=0 to oamClear is ambiguous.
    for (int s = 0; s < 128; s++) oamSetHidden(&oamMain, s, true);

    // Anchor sprite at slot 0 — canary that OAM still renders.
    oamSet(&oamMain, 0, 8, 8, 0, afn_asset_desc[0][4] & 0xF,
           SpriteSize_64x64, SpriteColorFormat_16Color,
           (void*)((u8*)0x06400000 + afn_asset_desc[0][0] * 32),
           -1, false, false, false, false, false);

    int oamSlot = 1;
    int affineSlot = 0;
    int focalLen = 160;

    for (int si = 0; si < AFN_SPRITE_COUNT && oamSlot < 96; si++) {
        if (afn_sprite_data[si][9] >= 0) continue;  // mesh sprite — fps3d handles it
        int aIdx = afn_sprite_data[si][4];
        if (aIdx < 0 || aIdx >= AFN_ASSET_COUNT) continue;

        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];

        int dx = wx - cam_x;
        int dz = wz - cam_z;

        int fovLambda = (dx * g_sinf - dz * g_cosf) >> 8;
        if (fovLambda <= 64) continue;

        int side       = (dx * g_cosf + dz * g_sinf) >> 8;
        int heightDiff = cam_h - wy;
        int screenY = m7_horizon + (heightDiff * focalLen) / fovLambda;
        int screenX = 128 + (side * focalLen) / fovLambda;
        int scale   = (cam_h * focalLen) / fovLambda;

        if (screenX < -64 || screenX > 256+64) continue;
        if (screenY < -64 || screenY > 192+64) continue;
        if (scale < 1) continue;

        int objSize    = afn_asset_desc[aIdx][3];
        int tileStart  = afn_asset_desc[aIdx][0];
        int tilesPerFr = afn_asset_desc[aIdx][1];
        int palBank    = afn_asset_desc[aIdx][4] & 0xF;
        int tileIdx    = tileStart + 0 * tilesPerFr;   // frame 0; animation in Phase 3

        // Affine scale: NDS .8 fixed. base = 256 * objSize / scale
        // (smaller scale = farther away = bigger matScale = smaller on-screen).
        int matScale = (256 * objSize) / scale;
        if (matScale < 16)   matScale = 16;
        if (matScale > 8192) matScale = 8192;

        // Plain non-affine OAM for first cut. Affine canvas-clipping math
        // produced invisible sprites — return to it once non-affine works.
        oamSet(&oamMain, oamSlot,
               screenX - objSize/2, screenY - objSize/2,
               0, palBank,
               objSize == 8  ? SpriteSize_8x8 :
               objSize == 16 ? SpriteSize_16x16 :
               objSize == 32 ? SpriteSize_32x32 : SpriteSize_64x64,
               SpriteColorFormat_16Color,
               (void*)((u8*)0x06400000 + tileIdx * 32),
               -1, false, false, false, false, false);
        (void)matScale; (void)affineSlot;

        if (oamSlot == 0) {
            static int s_dbgF = 0;
            s_dbgF++;
            if ((s_dbgF & 15) == 0)
                iprintf("\x1b[17;0Hs0 sX=%d sY=%d sc=%d   ", screenX, screenY, scale);
        }
        oamSlot++;
        affineSlot++;
    }

    {
        static int s_dbgFrame = 0;
        s_dbgFrame++;
        if ((s_dbgFrame & 15) == 0) {
            iprintf("\x1b[16;0Hsprites visible=%d", oamSlot);
        }
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

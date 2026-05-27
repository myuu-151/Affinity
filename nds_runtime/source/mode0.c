// Affinity NDS — Mode 0 (tilemap + HUD) renderer.
//
// Mirrors GBA's mode0_init_scene (gba_runtime/source/main.c ~line 5950):
// - Uploads per-scene tile gfx, palette, screen map into BG VRAM
// - Renders non-Tile tm_objects (Player/NPC/etc.) as OAM sprites
// - Scrolls BG to keep player centered
//
// Tile-type objects ARE baked into BG tiles at editor export time (see
// frame_loop.cpp ~13440 — each unique sprite asset becomes one 8x8 BG tile +
// palette bank, then its cells get painted in tileIndices). Tilemap rendering
// thus shows the painted ground/grass/etc. without any per-frame work.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/video.h>
#include <nds/arm9/background.h>
#include <nds/arm9/sprite.h>
#include <nds/arm9/cache.h>
#include <nds/arm9/input.h>
#include <nds/arm9/trig_lut.h>
#include <string.h>

extern int afn_current_mode;
extern int afn_current_scene;

#ifdef AFN_HAS_MODE0

// VRAM_A layout in Mode 0:
//   0x06000000 .. 0x06004000 (16KB)  — BG0 tile gfx (CBB 0)
//   0x06004000 .. 0x06006000 (8KB)   — BG0 screen map (SBBs 8..11 for 64x64 4bpp)
// Tile/map bases below match — DON'T change one without the other.
#define MODE0_BG_TILE_BASE 0
#define MODE0_BG_MAP_BASE  8

// Current-scene shortcuts, set by mode0_init_scene().
int tm_scene_idx        = 0;
static const AfnTmObj *tm_cur_objs   = NULL;
static int            tm_cur_obj_count = 0;
static int            tm_cur_map_w   = 0;
static int            tm_cur_map_h   = 0;
static int            tm_tile_size   = 8;
static int            tm_cam_x       = 0;
static int            tm_cam_y       = 0;

void afn_mode0_init_scene(int sceneIdx);

void afn_mode0_init(void)
{
    // Mode 0 = pure 2D. BG0 will hold the tilemap; OBJ on top for HUD/NPCs.
    // VRAM_A serves as MAIN_BG (128KB at 0x06000000) — the Mode-4 boot path
    // claimed it for textures, so this assignment is what flips the engine
    // out of "3D ready" into "tilemap ready".
    vramSetBankA(VRAM_A_MAIN_BG);
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE |
                 DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);

    afn_current_mode  = 1;
    afn_current_scene = AFN_TM_START_SCENE;

    afn_mode0_init_scene(AFN_TM_START_SCENE);
}

// Build the NDS BG0CNT value for a given scene's bg-size flag and 4bpp text mode.
// bgSize: 0=32x32, 1=64x32, 2=32x64, 3=64x64.
static u16 mode0_bg0cnt(int bgSize)
{
    u16 sz;
    switch (bgSize) {
        case 3: sz = BG_64x64;  break;
        case 2: sz = BG_32x64;  break;
        case 1: sz = BG_64x32;  break;
        default: sz = BG_32x32; break;
    }
    // 4bpp text mode, priority 3 (sprites on top), our fixed tile+map bases.
    return BG_TILE_BASE(MODE0_BG_TILE_BASE) | BG_MAP_BASE(MODE0_BG_MAP_BASE) |
           BG_COLOR_16 | sz | BG_PRIORITY(3);
}

void afn_mode0_init_scene(int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= AFN_TM_SCENE_COUNT) sceneIdx = 0;
    tm_scene_idx     = sceneIdx;
    tm_cur_objs      = afn_tm_scene_objs[sceneIdx];
    tm_cur_obj_count = afn_tm_scene_obj_count[sceneIdx];
    tm_cur_map_w     = afn_tm_scene_w[sceneIdx];
    tm_cur_map_h     = afn_tm_scene_h[sceneIdx];
    tm_tile_size     = afn_tm_scene_tile_size[sceneIdx];

    int bgSz = afn_tm_scene_bg_size[sceneIdx];
    REG_BG0CNT  = mode0_bg0cnt(bgSz);
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;

    // Upload tile gfx (4bpp packed, 32 bytes per tile).
    {
        const u32 *src = afn_tm_scene_tiles[sceneIdx];
        int len = afn_tm_scene_tiles_len[sceneIdx];
        dmaCopy(src, (void*)(0x06000000 + MODE0_BG_TILE_BASE * 0x4000), len);
    }

    // Upload palette (256 entries, all 16 banks).
    {
        const u16 *pal = afn_tm_scene_pal[sceneIdx];
        dmaCopy(pal, BG_PALETTE, 256 * 2);
        // Bank 0 / index 0 stays the BG backdrop. Force pure black so the
        // backdrop doesn't leak whatever junk index 0 held in the asset.
        BG_PALETTE[0] = RGB15(0, 0, 0);
    }

    // Upload screen map. NDS BG screen base is 2KB per SBB. For sizes >32 in
    // either axis the hardware stitches multiple 32x32 SBBs together:
    //   64x32 -> SBBs [0,1]
    //   32x64 -> SBBs [0,1]
    //   64x64 -> SBBs [0,1,2,3]  (TL, TR, BL, BR)
    {
        int mapW = tm_cur_map_w;
        int mapH = tm_cur_map_h;
        int bgW, bgH, nSBB;
        if (bgSz == 3)      { bgW = 64; bgH = 64; nSBB = 4; }
        else if (bgSz == 2) { bgW = 32; bgH = 64; nSBB = 2; }
        else if (bgSz == 1) { bgW = 64; bgH = 32; nSBB = 2; }
        else                { bgW = 32; bgH = 32; nSBB = 1; }

        u16 *mapBase = (u16*)(0x06000000 + MODE0_BG_MAP_BASE * 0x800);
        // Wipe all SBBs we own to backdrop tile (0).
        memset(mapBase, 0, nSBB * 32 * 32 * 2);

        const u16 *src = afn_tm_scene_map[sceneIdx];
        for (int y = 0; y < mapH && y < bgH; y++)
            for (int x = 0; x < mapW && x < bgW; x++)
            {
                int sbbOfs = (x >> 5);
                if (y >= 32) sbbOfs += (bgW > 32) ? 2 : 1;
                u16 *sbb = mapBase + sbbOfs * (32 * 32);
                sbb[(y & 31) * 32 + (x & 31)] = src[y * mapW + x];
            }
    }

    // Center camera on first Player object if this scene owns one.
#if AFN_TM_PLAYER_OBJ >= 0
    if (sceneIdx == AFN_TM_PLAYER_SCENE) {
        int px = tm_cur_objs[AFN_TM_PLAYER_OBJ].tx * tm_tile_size;
        int py = tm_cur_objs[AFN_TM_PLAYER_OBJ].ty * tm_tile_size;
        tm_cam_x = px - 128 + tm_tile_size / 2;
        tm_cam_y = py -  96 + tm_tile_size / 2;
    } else
#endif
    {
        tm_cam_x = 0;
        tm_cam_y = 0;
    }
    REG_BG0HOFS = tm_cam_x;
    REG_BG0VOFS = tm_cam_y;
}

void afn_mode0_update(void)
{
    // scanKeys drives keysDown/Held/Up — every BP OnKeyPressed depends on it.
    scanKeys();

    // BG scroll. Player movement / camera follow still TODO; for now the
    // camera stays parked where mode0_init_scene placed it.
    REG_BG0HOFS = tm_cam_x;
    REG_BG0VOFS = tm_cam_y;
}

#else /* AFN_HAS_MODE0 not defined — minimal stubs */

int tm_scene_idx = 0;

void afn_mode0_init(void)
{
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE |
                 DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);
    REG_BG0CNT = BG_MAP_BASE(0) | BG_TILE_BASE(0) | BG_PRIORITY(3);
    BG_PALETTE[0] = RGB15(7, 7, 8);
    afn_current_mode = 1;
    afn_current_scene = 0;
}

void afn_mode0_update(void)
{
    scanKeys();
}

#endif /* AFN_HAS_MODE0 */

// Affinity NDS — Mode 0 (tilemap + HUD) renderer.
//
// Mirrors GBA's mode0_init_scene + Mode 0 per-frame update:
//   1. Upload per-scene tile gfx / palette / screen map (mode0_init_scene)
//   2. Drive tile-grid player movement from DPAD + collision
//   3. Render non-Tile tm_objects (Player / NPC / etc.) as OAM sprites
//   4. Scroll BG to keep the player centered
//
// Tile-type editor objects are baked into BG tiles at editor export time so
// the BG layer paints ground/grass/decorations for free; this module only
// has to render dynamic content (player + NPCs) on top.

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
#define MODE0_BG_TILE_BASE 0
#define MODE0_BG_MAP_BASE  8

// Current-scene shortcuts, refreshed by mode0_init_scene().
int tm_scene_idx        = 0;
static const AfnTmObj *tm_cur_objs   = NULL;
static int             tm_cur_obj_count = 0;
static int             tm_cur_map_w   = 0;
static int             tm_cur_map_h   = 0;
static int             tm_tile_size   = 8;
static int             tm_cur_logical_w = 0;
static int             tm_cur_logical_h = 0;

// Camera scroll (pixels into the BG).
static int tm_cam_x = 0;
static int tm_cam_y = 0;

// Player tile-grid state. tm_move_timer lives in script_glue.c so emitted
// scripts can read it (IsMoving gate etc.); the rest is local.
static int tm_player_tx = 0, tm_player_ty = 0;
static int tm_move_dx = 0, tm_move_dy = 0;
static int tm_move_frames = 8;                       // frames per tile step
static int tm_player_pixel_x = 0, tm_player_pixel_y = 0;
static int tm_turn_timer = 0;                        // tap-to-turn delay
static int tm_turn_facing = -1;
static unsigned int tm_frame_count = 0;              // anim ticker

// Per-tm_object anim state, mutable copies seeded from the scene's const
// AfnTmObj table on scene load. Lets scripts mutate animIdx/animPlay/etc.
#define TM_MAX_OBJS 64
static int  tm_obj_anim_idx[TM_MAX_OBJS];
static int  tm_obj_anim_play[TM_MAX_OBJS];
static int  tm_obj_facing[TM_MAX_OBJS];

// Per-tm_object active VRAM frame, -1 = none. Each object owns its own
// streaming slot rather than sharing with sprites.c's g_active_frame[ai]
// (two objects on the same asset would otherwise fight over which frame's
// data lives in VRAM).
static int  tm_obj_active_frame[TM_MAX_OBJS];

// Each tm_object that uses a direction asset gets a compact VRAM slot
// (1 facing's worth of tiles) so all NPCs fit alongside the player without
// chewing up VRAM at the 8-facings rate the bulk upload would need.
static int  tm_obj_vram_slot[TM_MAX_OBJS];
static int  tm_obj_vram_tiles[TM_MAX_OBJS];

// AFN_HUD_VRAM_OFFSET is the byte offset in sprite VRAM where the HUD reserves
// its area. Anything before that is tm_object territory.
#ifndef AFN_HUD_VRAM_OFFSET
#define AFN_HUD_VRAM_OFFSET 98304
#endif
#define TM_OBJ_VRAM_BUDGET_TILES ((AFN_HUD_VRAM_OFFSET) / 32)

void afn_mode0_init_scene(int sceneIdx);

void afn_mode0_init(void)
{
    // Mode 0 = pure 2D. BG0 holds the tilemap; OBJ draws HUD + NPCs on top.
    // VRAM_A serves as MAIN_BG (128KB at 0x06000000). The Mode-4 boot path
    // claimed it for textures, so this re-assignment is what flips the
    // engine out of "3D ready" into "tilemap ready".
    vramSetBankA(VRAM_A_MAIN_BG);
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE |
                 DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);

    afn_current_mode  = 1;
    afn_current_scene = AFN_TM_START_SCENE;

    afn_mode0_init_scene(AFN_TM_START_SCENE);
}

// Build BG0CNT for a given bg-size flag, 4bpp text mode.
static u16 mode0_bg0cnt(int bgSize)
{
    u16 sz;
    switch (bgSize) {
        case 3: sz = BG_64x64;  break;
        case 2: sz = BG_32x64;  break;
        case 1: sz = BG_64x32;  break;
        default: sz = BG_32x32; break;
    }
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
    // logical = map size in "editor tile" units (mapW already includes 2x
    // expansion for pixelScale=2 scenes). tile_size also doubles in that
    // case, so logical = map / (tile_size / 8).
    {
        int unit = tm_tile_size / 8; if (unit < 1) unit = 1;
        tm_cur_logical_w = tm_cur_map_w / unit;
        tm_cur_logical_h = tm_cur_map_h / unit;
    }

    int bgSz = afn_tm_scene_bg_size[sceneIdx];
    REG_BG0CNT  = mode0_bg0cnt(bgSz);
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;

    // Tile gfx.
    {
        const u32 *src = afn_tm_scene_tiles[sceneIdx];
        int len = afn_tm_scene_tiles_len[sceneIdx];
        dmaCopy(src, (void*)(0x06000000 + MODE0_BG_TILE_BASE * 0x4000), len);
    }
    // Palette (256 entries / 16 banks). Force backdrop to pure black so a
    // stray index-0 color from the asset doesn't leak through.
    {
        const u16 *pal = afn_tm_scene_pal[sceneIdx];
        dmaCopy(pal, BG_PALETTE, 256 * 2);
        BG_PALETTE[0] = RGB15(0, 0, 0);
    }
    // Screen map. Multi-SBB stitch for sizes >32 — matches GBA's layout.
    {
        int mapW = tm_cur_map_w;
        int mapH = tm_cur_map_h;
        int bgW, bgH, nSBB;
        if (bgSz == 3)      { bgW = 64; bgH = 64; nSBB = 4; }
        else if (bgSz == 2) { bgW = 32; bgH = 64; nSBB = 2; }
        else if (bgSz == 1) { bgW = 64; bgH = 32; nSBB = 2; }
        else                { bgW = 32; bgH = 32; nSBB = 1; }

        u16 *mapBase = (u16*)(0x06000000 + MODE0_BG_MAP_BASE * 0x800);
        memset(mapBase, 0, nSBB * 32 * 32 * 2);

        const u16 *src = afn_tm_scene_map[sceneIdx];
        for (int y = 0; y < mapH && y < bgH; y++)
            for (int x = 0; x < mapW && x < bgW; x++) {
                int sbbOfs = (x >> 5);
                if (y >= 32) sbbOfs += (bgW > 32) ? 2 : 1;
                u16 *sbb = mapBase + sbbOfs * (32 * 32);
                sbb[(y & 31) * 32 + (x & 31)] = src[y * mapW + x];
            }
    }

    // Seed mutable per-object state. Compact-allocate VRAM slots for any
    // direction-sprite object so all NPCs co-exist within our budget; the
    // streaming step DMAs the chosen facing's tiles into that slot each
    // time the facing/anim changes.
    int nextSlot = 0;
    for (int oi = 0; oi < tm_cur_obj_count && oi < TM_MAX_OBJS; oi++) {
        tm_obj_anim_idx[oi]    = tm_cur_objs[oi].animIdx;
        tm_obj_anim_play[oi]   = tm_cur_objs[oi].animPlay;
        tm_obj_facing[oi]      = tm_cur_objs[oi].facing;
        tm_obj_active_frame[oi] = -1;
        int ai = tm_cur_objs[oi].assetIdx;
        int tpf = 0;
#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
        if (ai >= 0 && ai < AFN_ASSET_COUNT)
            tpf = afn_asset_desc[ai][1];
#endif
        if (tpf > 0 && tm_cur_objs[oi].type != 6 /*Tile*/ &&
            nextSlot + tpf <= TM_OBJ_VRAM_BUDGET_TILES) {
            tm_obj_vram_slot[oi]  = nextSlot;
            tm_obj_vram_tiles[oi] = tpf;
            nextSlot += tpf;
        } else {
            tm_obj_vram_slot[oi]  = -1;
            tm_obj_vram_tiles[oi] = 0;
        }
    }

    // Player init from the scene's player object (if any).
#if AFN_TM_PLAYER_OBJ >= 0
    if (sceneIdx == AFN_TM_PLAYER_SCENE) {
        tm_player_tx = tm_cur_objs[AFN_TM_PLAYER_OBJ].tx;
        tm_player_ty = tm_cur_objs[AFN_TM_PLAYER_OBJ].ty;
    } else {
        tm_player_tx = 0;
        tm_player_ty = 0;
    }
#else
    tm_player_tx = 0;
    tm_player_ty = 0;
#endif
    tm_move_dx = 0; tm_move_dy = 0; tm_move_timer = 0;
    tm_turn_timer = 0; tm_turn_facing = -1;
    tm_player_pixel_x = tm_player_tx * tm_tile_size;
    tm_player_pixel_y = tm_player_ty * tm_tile_size;
    tm_cam_x = tm_player_pixel_x - 128 + tm_tile_size / 2;
    tm_cam_y = tm_player_pixel_y -  96 + tm_tile_size / 2;
    REG_BG0HOFS = tm_cam_x;
    REG_BG0VOFS = tm_cam_y;
}

#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0 && AFN_FRAME_STREAM_LEN > 0
// Stream one tm_object's frame data into its dedicated VRAM slot.
// globalFrame indexes afn_frame_rom_off/afn_frame_tile_count.
static void tm_stream_frame(int oi, int globalFrame)
{
    if (tm_obj_vram_slot[oi] < 0) return;
    if (tm_obj_active_frame[oi] == globalFrame) return;
    const u32 *src = afn_all_tiles + afn_frame_rom_off[globalFrame];
    u32 *dst = (u32*)(0x06400000 + tm_obj_vram_slot[oi] * 32);
    int tiles = afn_frame_tile_count[globalFrame];
    if (tiles > tm_obj_vram_tiles[oi]) tiles = tm_obj_vram_tiles[oi];
    dmaCopy(src, dst, tiles * 32);
    tm_obj_active_frame[oi] = globalFrame;
}
#endif

// Convert our 4-cardinal facing (0=N, 2=E, 4=S, 6=W) into the asset's
// dirCount index. Most assets are 4-dir (N,E,S,W → 0,1,2,3) or 8-dir
// (N,NE,E,SE,S,SW,W,NW → 0..7); the 4-dir case is /2.
static int tm_facing_to_dir(int facing, int dirCount)
{
    if (dirCount >= 8) return facing & 7;
    if (dirCount == 4) return (facing >> 1) & 3;
    return 0;
}

static void mode0_render_objects(int oamSlotStart, int *outOamSlot)
{
    int oamSlot = oamSlotStart;
#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
    // Render high layer first so it ends up at the lower OAM slot (NDS
    // draws lower slot on top of higher within the same priority).
    for (int layerPass = 3; layerPass >= 0; layerPass--)
    for (int oi = 0; oi < tm_cur_obj_count && oi < TM_MAX_OBJS && oamSlot < 96; oi++) {
        const AfnTmObj *obj = &tm_cur_objs[oi];
        if (obj->layer != layerPass) continue;
        if (obj->type == 6) continue;            // Tile — already in BG
        int ai = obj->assetIdx;
        if (ai < 0 || ai >= AFN_ASSET_COUNT) continue;
        if (tm_obj_vram_slot[oi] < 0) continue;  // no VRAM budget

        int objPx, objPy;
        if (oi == AFN_TM_PLAYER_OBJ && tm_scene_idx == AFN_TM_PLAYER_SCENE) {
            objPx = tm_player_pixel_x;
            objPy = tm_player_pixel_y;
        } else {
            objPx = obj->tx * tm_tile_size;
            objPy = obj->ty * tm_tile_size;
        }
        int screenX = objPx - tm_cam_x;
        int screenY = objPy - tm_cam_y;

        int objSize    = afn_asset_desc[ai][3];
        int palBank    = afn_asset_desc[ai][4] & 0xF;
        int frameCount = afn_asset_desc[ai][2];
        int dirCount   = afn_asset_desc[ai][5];
        int animBase   = afn_asset_desc[ai][6];
        int animCount  = afn_asset_desc[ai][7];
        int defaultAn  = afn_asset_desc[ai][8];
        int frameBase  = afn_asset_desc[ai][9];
        int tilesPerFr = afn_asset_desc[ai][1];

        // Pick facing.
        int facing = (oi == AFN_TM_PLAYER_OBJ) ? tm_player_facing : tm_obj_facing[oi];
        int dir = tm_facing_to_dir(facing, dirCount > 0 ? dirCount : 1);

        // Pick anim — PlayAnim wins for player, default for others.
        int animIdx = tm_obj_anim_idx[oi];
        if (animIdx < 0) animIdx = defaultAn;
#if defined(AFN_HAS_SCRIPT)
        extern int afn_play_anim;
        if (oi == AFN_TM_PLAYER_OBJ && afn_play_anim >= 0)
            animIdx = afn_play_anim;
#endif
        if (animIdx < 0 || animIdx >= animCount) animIdx = defaultAn;
        if (animIdx < 0) animIdx = 0;

        // Resolve anim → {start, length, fps, loop} via the global table.
        int frameStart = 0, animLen = frameCount, animFps = 0;
#if AFN_ANIM_TABLE_LEN > 0
        if (animCount > 0 && animIdx >= 0 && animIdx < animCount) {
            const int *row = afn_anim_table[animBase + animIdx];
            frameStart = row[0];
            animLen    = row[1];
            animFps    = row[2];
        }
#endif
        int animFrame = frameStart;
        int animPlay = (oi == AFN_TM_PLAYER_OBJ) ? 1 : tm_obj_anim_play[oi];
        if (animPlay && animLen > 1 && animFps > 0) {
            int hold = 60 / animFps; if (hold < 1) hold = 1;
            animFrame = frameStart + ((tm_frame_count / hold) % animLen);
        }
        if (animFrame >= frameCount) animFrame = frameCount - 1;
        if (animFrame < 0) animFrame = 0;

        // DMA the chosen frame into this object's VRAM slot.
        int globalFrame = frameBase + animFrame;
#if AFN_FRAME_STREAM_LEN > 0
        tm_stream_frame(oi, globalFrame);
#endif
        // Tile index inside the asset's frame (picks the right facing's
        // tiles within the frame layout the exporter baked).
#if AFN_FRAME_DIR_TILE_LEN > 0
        int dirOff = afn_frame_dir_tile[globalFrame][dir];
#else
        int dirOff = dir * tilesPerFr;
#endif
        int tileBaseSlot = tm_obj_vram_slot[oi] + dirOff;

        // Off-screen cull.
        if (screenX < -64 || screenX > 256 + 32) continue;
        if (screenY < -64 || screenY > 192 + 32) continue;

        // Scale8: 256 = 1x. Apply via affine rotscale.
        int sc8 = obj->scale8;
        if (sc8 < 1) sc8 = 256;
        // matScale: 256 = 1x in oamRotateScale (smaller = bigger sprite).
        int matScale = (256 * 256) / sc8;
        if (matScale < 64)   matScale = 64;
        if (matScale > 2048) matScale = 2048;

        SpriteSize sz = objSize == 8  ? SpriteSize_8x8   :
                        objSize == 16 ? SpriteSize_16x16 :
                        objSize == 32 ? SpriteSize_32x32 : SpriteSize_64x64;

        // Bottom-anchor the sprite to its tile, then expand to canvas
        // (double-size) so the affine matrix doesn't clip at the edges.
        int scaledH = (objSize * 256) / matScale;
        int canvasSize = objSize * 2;
        int topLeftX = screenX + (tm_tile_size - canvasSize) / 2;
        int topLeftY = screenY + tm_tile_size - scaledH - canvasSize / 2;

        oamRotateScale(&oamMain, oamSlot, 0, matScale, matScale);
        oamSet(&oamMain, oamSlot,
               topLeftX, topLeftY,
               (layerPass == 0) ? 3 : 1,           // BG priority
               palBank, sz, SpriteColorFormat_16Color,
               (void*)((u8*)0x06400000 + tileBaseSlot * 32),
               oamSlot, true, false, false, false, false);
        oamSlot++;
    }
#endif
    *outOamSlot = oamSlot;
}

void afn_mode0_update(void)
{
    // scanKeys drives keysDown/Held/Up — every BP OnKeyPressed depends on it.
    scanKeys();

    extern int afn_player_frozen;

    // Grid-step state machine. Mirrors GBA's mode0 update verbatim.
    if (tm_move_timer > 0) {
        tm_move_timer--;
        if (tm_move_timer == 0) {
            tm_player_tx += tm_move_dx;
            tm_player_ty += tm_move_dy;
            tm_move_dx = 0;
            tm_move_dy = 0;
        }
    }
    if (tm_move_timer == 0 && !afn_player_frozen) {
        int dx = 0, dy = 0;
        int newFacing = -1;
        int held = keysHeld();
        if      (held & KEY_LEFT)  { dx = -1; newFacing = 6; }
        else if (held & KEY_RIGHT) { dx = 1;  newFacing = 2; }
        else if (held & KEY_UP)    { dy = -1; newFacing = 0; }
        else if (held & KEY_DOWN)  { dy = 1;  newFacing = 4; }

        if (newFacing >= 0) {
            if (newFacing != tm_turn_facing) {
                int wasFacing = tm_player_facing;
                tm_turn_facing = newFacing;
                tm_player_facing = newFacing;
                // Already facing the requested direction → no turn delay,
                // walk immediately. New direction → 6-frame tap-to-turn.
                tm_turn_timer = (newFacing == wasFacing) ? 6 : 0;
            }
            tm_turn_timer++;
            if (tm_turn_timer > 6) {
                int nx = tm_player_tx + dx;
                int ny = tm_player_ty + dy;
                if (nx >= 0 && nx < tm_cur_logical_w &&
                    ny >= 0 && ny < tm_cur_logical_h) {
                    int blocked = 0;
                    for (int ci = 0; ci < tm_cur_obj_count; ci++) {
                        if (ci == AFN_TM_PLAYER_OBJ) continue;
                        if (!tm_cur_objs[ci].collision) continue;
                        int sc = tm_cur_objs[ci].scale8;
                        int cells = sc >> 8;
                        if (cells < 1) cells = 1;
                        int ox = tm_cur_objs[ci].tx;
                        int oy = tm_cur_objs[ci].ty;
                        int ddx = nx - ox; if (ddx < 0) ddx = -ddx;
                        int ddy = ny - oy; if (ddy < 0) ddy = -ddy;
                        if (ddx < cells && ddy < cells) { blocked = 1; break; }
                    }
                    if (!blocked) {
                        tm_move_dx = dx;
                        tm_move_dy = dy;
                        tm_move_timer = tm_move_frames;
                    }
                }
            }
        } else {
            tm_turn_timer = 0;
            tm_turn_facing = -1;
        }
    }

    // Interpolate pixel position during a move so the visible sprite glides
    // between tiles instead of teleporting.
    if (tm_move_timer > 0) {
        int fromX = tm_player_tx * tm_tile_size;
        int fromY = tm_player_ty * tm_tile_size;
        int toX = (tm_player_tx + tm_move_dx) * tm_tile_size;
        int toY = (tm_player_ty + tm_move_dy) * tm_tile_size;
        int t = tm_move_frames - tm_move_timer;
        tm_player_pixel_x = fromX + (toX - fromX) * t / tm_move_frames;
        tm_player_pixel_y = fromY + (toY - fromY) * t / tm_move_frames;
    } else {
        tm_player_pixel_x = tm_player_tx * tm_tile_size;
        tm_player_pixel_y = tm_player_ty * tm_tile_size;
    }

    // Camera follows player, centered on the 256x192 screen.
    tm_cam_x = tm_player_pixel_x - 128 + tm_tile_size / 2;
    tm_cam_y = tm_player_pixel_y -  96 + tm_tile_size / 2;
    REG_BG0HOFS = tm_cam_x;
    REG_BG0VOFS = tm_cam_y;

    // Emit OAM. We own slots 0..AFN_HUD_OAM_BASE-1; HUD takes over from
    // AFN_HUD_OAM_BASE up. Hide our trailing slots so stale state from a
    // previous frame doesn't render.
    oamClear(&oamMain, 0, 128);
    int oamSlot = 0;
    mode0_render_objects(0, &oamSlot);

    tm_frame_count++;
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

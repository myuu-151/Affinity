// Affinity NDS — Mode 0 (tilemap / HUD-only) scene.
//
// MVP scope: get the boot path off Mode 4 so HUD-only splash screens
// render. Mode 0 here means "no 3D engine + no fps3d sprite render";
// HUD/script/OAM still run via main.c's loop. Real tilemap rendering
// (BG layer with scrollable tile data) is the next chunk — needed for
// gameplay scenes that exit the splash.

#include "affinity.h"
#include <nds/arm9/video.h>
#include <nds/arm9/background.h>
#include <nds/arm9/sprite.h>
#include <nds/arm9/cache.h>
#include <nds/arm9/input.h>

extern int afn_current_mode;
extern int afn_current_scene;

void afn_mode0_init(void)
{
    // Top screen: pure 2D. Enable a BG layer (BG0 as text mode) — the NDS
    // 2D engine needs at least one BG active for OBJ compositing to show
    // up reliably. Even an empty BG0 satisfies that. Solid clear color
    // comes from BG palette index 0.
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE |
                 DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);
    // Empty BG0 — char base + screen base both at 0, lowest priority so
    // OBJ at priority 0 renders on top.
    REG_BG0CNT = BG_MAP_BASE(0) | BG_TILE_BASE(0) | BG_PRIORITY(3);
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
    BG_PALETTE[0] = RGB15(7, 7, 8); // dark grey
    afn_current_mode = 1;
    afn_current_scene = 0;
}

void afn_mode0_update(void)
{
    // No tilemap render yet — HUD draw in main.c handles everything
    // visible. When the tilemap subsystem lands, scroll + per-scene
    // map update happens here.
    // CRITICAL: scanKeys() drives keysDown/Held/Up. fps3d_update calls
    // it for Mode 4; we have to do the same in Mode 0 or every script
    // key check (BP OnKeyPressed, etc.) is a no-op.
    scanKeys();
}

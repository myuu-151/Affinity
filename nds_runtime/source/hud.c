// Affinity NDS — HUD elements + text (Phase 5, MVP).
//
// Current scope: console-text rendering of script-side HUD value
// slots (afn_hud_value[0..3]) when their corresponding visibility
// flag is set. Sufficient for the ring counter / score-style HUDs
// the project actually uses. Full composite-HUD (sprite pieces,
// cursor stops, menu navigation, font tile rendering) is still TODO
// and would mirror the GBA hud.c / hud_text.c machinery.

#include "affinity.h"
#include "mapdata.h"
#include <stdio.h>

#ifdef AFN_HAS_SCRIPT
extern int afn_hud_value[4];
extern unsigned char afn_hud_visible[4];
#endif

void afn_hud_init(void) {
#ifdef AFN_HAS_SCRIPT
    // Slot 0 visible by default — matches the ring_counter blueprint's
    // OnStart → ShowHUD pattern so the counter appears at boot without
    // needing the BP to fire first.
    afn_hud_visible[0] = 1;
#endif
}

void afn_hud_draw(void) {
#ifdef AFN_HAS_SCRIPT
    // Console row 0 = top of screen. iprintf is wired by main.c's
    // consoleDemoInit. Each slot gets its own row so multiple counters
    // don't collide.
    for (int s = 0; s < 4; s++) {
        if (!afn_hud_visible[s]) continue;
        iprintf("\x1b[%d;0H[%d] %-6d", s, s, afn_hud_value[s]);
    }
#endif
}

// Affinity NDS — HUD elements + text (Phase 5).
//
// Sprite-based MVP: bakes a small 8x8 digit font into reserved sprite
// VRAM at boot, then renders each visible HUD element's text rows as
// OAM sprites on the top screen. Source slots (0..3) pull from
// afn_hud_value[] each frame.
//
// Out of scope: composite sprite pieces, cursor stops / menu nav,
// keyframe animation, font 1 (4x5 small), arbitrary RGB text colors.
// All map to the GBA's full HUD system in mapdata.h's HUD tables but
// aren't rendered here yet.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/sprite.h>
#include <nds/arm9/video.h>
#include <stdio.h>
#include <string.h>

#ifdef AFN_HAS_SCRIPT
extern int afn_hud_value[4];
extern unsigned char afn_hud_visible[4];
#endif

// 1bpp 8x8 digit bitmaps — each byte is one row, bit 7 = leftmost pixel.
static const unsigned char k_digit_bits[10][8] = {
    { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 }, // 0
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 }, // 1
    { 0x3C, 0x66, 0x06, 0x1C, 0x30, 0x66, 0x7E, 0x00 }, // 2
    { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 }, // 3
    { 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00 }, // 4
    { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 }, // 5
    { 0x3C, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 }, // 6
    { 0x7E, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00 }, // 7
    { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 }, // 8
    { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00 }, // 9
};

// Reserved sprite-VRAM region for HUD. SpriteMapping_1D_128 (set in
// sprites.c) means each OAM tile slot covers 128 bytes — even though an
// 8x8 4bpp tile is only 32 bytes, the next addressable tile is 96 bytes
// further. So digit tiles must be spaced 128 bytes apart, not 32. Bytes
// 32..127 of each slot are wasted but unavoidable in this mapping.
// VRAM byte offset 786432 = tile slot 6144 in raw 32-byte units, or
// slot 6144 in our 128-byte-spaced layout starts at byte 786432 → past
// the 128KB bank. Use base byte offset 98304 (slot 768 in 128-byte
// units), well above the gameplay-sprite cursor (1620 raw tiles ≈
// 51840 bytes).
#define AFN_HUD_VRAM_OFFSET 98304            // bytes from sprite VRAM base
#define AFN_HUD_TILE_STRIDE 128               // 1D_128 OAM addressing
#define AFN_HUD_PAL_BANK    15
#define AFN_HUD_OAM_BASE    100               // gameplay uses <30 OAM slots

static void hud_bake_digits(void)
{
    // 4bpp tile = 32 bytes, 2 px per byte (low nibble = left pixel).
    // Expand the 1bpp bitmap row to 4bpp with color index 1 for set bits.
    // Each digit lives in its own 128-byte OAM slot; only the first 32
    // bytes hold pixel data.
    for (int d = 0; d < 10; d++) {
        unsigned short* vram = (unsigned short*)(0x06400000 + AFN_HUD_VRAM_OFFSET + d * AFN_HUD_TILE_STRIDE);
        for (int row = 0; row < 8; row++) {
            unsigned char bits = k_digit_bits[d][row];
            // 8 px row = 4 bytes = 2 u16 words. Each byte packs 2 px
            // (low nibble = left). Each word packs 4 px (low byte = left pair).
            // VRAM rejects 8-bit writes on NDS so the row is built as u16s.
            for (int half = 0; half < 2; half++) {
                int p0 = (bits >> (7 - half*4))     & 1;
                int p1 = (bits >> (7 - half*4 - 1)) & 1;
                int p2 = (bits >> (7 - half*4 - 2)) & 1;
                int p3 = (bits >> (7 - half*4 - 3)) & 1;
                unsigned short word =
                    ((p0 ? 1 : 0)      ) |
                    ((p1 ? 1 : 0) <<  4) |
                    ((p2 ? 1 : 0) <<  8) |
                    ((p3 ? 1 : 0) << 12);
                *vram++ = word;
            }
        }
    }
}

void afn_hud_init(void) {
#ifdef AFN_HAS_SCRIPT
    afn_hud_visible[0] = 1;
    hud_bake_digits();
    // Palette bank 15 (HUD): index 0 transparent, index 1 white.
    // Use pointer arithmetic (not int + cast) so the +16 advances by 16
    // u16-words = 32 bytes per bank, matching the OBJ palette layout.
    unsigned short* pal = (unsigned short*)0x05000200 + AFN_HUD_PAL_BANK * 16;
    pal[0] = 0;
    pal[1] = RGB15(31, 31, 31);
#endif
}

void afn_hud_draw(void) {
#if defined(AFN_HAS_SCRIPT) && defined(AFN_HUD_ELEM_COUNT) && AFN_HUD_ELEM_COUNT > 0
    int oamSlot = AFN_HUD_OAM_BASE;
    for (int e = 0; e < AFN_HUD_ELEM_COUNT; e++) {
        int slot = e; // visibility slot 0..3 maps 1:1 to element index for now
        if (slot < 4 && !afn_hud_visible[slot]) continue;
        int ex = afn_hud_elems[e].x;
        int ey = afn_hud_elems[e].y;
        int ts = afn_hud_elems[e].textStart;
        int tc = afn_hud_elems[e].textCount;
        for (int t = 0; t < tc; t++) {
            int ss = afn_hud_texts[ts + t].sourceSlot;
            if (ss < 0 || ss > 3) continue;
            int tx = ex + afn_hud_texts[ts + t].x;
            int ty = ey + afn_hud_texts[ts + t].y;
            int pad = afn_hud_texts[ts + t].pad;
            if (pad < 1) pad = 1;
            // Format the value, zero-padded, max 8 digits.
            char buf[12];
            int v = afn_hud_value[ss];
            if (v < 0) v = 0;
            snprintf(buf, sizeof(buf), "%0*d", pad, v);
            int n = (int)strlen(buf);
            if (n > 8) n = 8;
            for (int i = 0; i < n; i++) {
                int ch = buf[i] - '0';
                if (ch < 0 || ch > 9) continue;
                if (oamSlot >= 128) break;
                oamSet(&oamMain, oamSlot++,
                       tx + i * 8,                   // screen x
                       ty,                           // screen y
                       0,                            // priority
                       AFN_HUD_PAL_BANK,             // palette
                       SpriteSize_8x8,
                       SpriteColorFormat_16Color,
                       (void*)(0x06400000 + AFN_HUD_VRAM_OFFSET + ch * AFN_HUD_TILE_STRIDE),
                       -1, false, false, false, false, false);
            }
        }
    }
    // Park unused HUD OAM slots offscreen so stale entries from earlier
    // frames don't render.
    for (int i = oamSlot; i < 128; i++)
        oamSetHidden(&oamMain, i, true);
    // Flush our OAM writes to the hardware shadow. sprite_update already
    // ran oamUpdate before we got here, so without this our slots stay in
    // the user buffer and get wiped by next frame's oamClear.
    oamUpdate(&oamMain);
#endif
}

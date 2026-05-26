// Affinity NDS — HUD elements + text (Phase 5).
//
// Bakes libnds's default 8x8 ASCII font into reserved sprite VRAM at
// boot, then renders each visible HUD element's text rows + pieces
// (sprite-art tiles) as OAM sprites on the top screen. Source slots
// pull from afn_hud_value[]; static strings render the text field.
//
// Out of scope (still TODO): cursor stops / menu nav, bars, icons,
// keyframe animation, font 1/2 size variants, per-row RGB color.

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

// libnds ships its console's 8x8 1bpp font as default_font_bin — 256
// chars * 8 bytes each. Reused here so the HUD covers all of printable
// ASCII without a hand-baked font blob.
extern const unsigned char default_font_bin[];

// Reserved sprite-VRAM region for HUD. SpriteMapping_1D_128 (set in
// sprites.c) means each OAM tile slot covers 128 bytes — an 8x8 4bpp
// tile is 32 bytes but the next addressable tile is 96 bytes further,
// so font glyphs are spaced 128 bytes apart. Bytes 32..127 of each slot
// are wasted but unavoidable.
//
// Base byte offset 98304 (slot 768 in 128-byte units), well above the
// gameplay-sprite cursor (1620 raw tiles ≈ 51840 bytes). 256 glyphs *
// 128 bytes = 32768 bytes, ending at byte 131072 = the bank-B limit.
// If the gameplay cursor ever crosses 768 we need a different region.
#define AFN_HUD_VRAM_OFFSET   98304          // bytes into sprite VRAM
#define AFN_HUD_TILE_STRIDE   128             // 1D_128 OAM addressing
#define AFN_HUD_FONT_FIRST    0x20            // space
#define AFN_HUD_FONT_LAST     0x7E            // ~
#define AFN_HUD_FONT_COUNT    (AFN_HUD_FONT_LAST - AFN_HUD_FONT_FIRST + 1) // 95
// Pieces start after the font slots (95 * 128 = 12160 bytes).
#define AFN_HUD_PIECE_VRAM_OFFSET (AFN_HUD_VRAM_OFFSET + AFN_HUD_FONT_COUNT * AFN_HUD_TILE_STRIDE)
#define AFN_HUD_PAL_BANK      15
#define AFN_HUD_OAM_BASE      100

static void hud_bake_font(void)
{
    // Expand 1bpp -> 4bpp at color index 1 (white). Only printable ASCII
    // (0x20..0x7E) — full 256 chars would consume 32KB of bank B; this
    // covers 95 chars in 12160 bytes, leaving room for pieces.
    for (int g = 0; g < AFN_HUD_FONT_COUNT; g++) {
        int srcCh = AFN_HUD_FONT_FIRST + g;
        unsigned short* vram = (unsigned short*)
            (0x06400000 + AFN_HUD_VRAM_OFFSET + g * AFN_HUD_TILE_STRIDE);
        for (int row = 0; row < 8; row++) {
            unsigned char bits = default_font_bin[srcCh * 8 + row];
            for (int half = 0; half < 2; half++) {
                int p0 = (bits >> (7 - half*4 - 0)) & 1;
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

#if defined(AFN_HUD_PIECE_TILE_LEN) && AFN_HUD_PIECE_TILE_LEN > 0
extern const unsigned int afn_hud_piece_tiles[];
static void hud_bake_pieces(void)
{
    // Piece tile data is baked into mapdata.h as a flat u32 blob,
    // already in NDS sprite-tile format. Push it to VRAM right after
    // the font region. The exporter computes per-piece VRAM tile
    // offsets relative to AFN_HUD_PIECE_VRAM_OFFSET so the runtime
    // doesn't need a lookup table.
    dmaCopy(afn_hud_piece_tiles,
            (void*)(0x06400000 + AFN_HUD_PIECE_VRAM_OFFSET),
            AFN_HUD_PIECE_TILE_LEN * 4);
}
#endif

void afn_hud_init(void) {
#ifdef AFN_HAS_SCRIPT
    afn_hud_visible[0] = 1;
    hud_bake_font();
#if defined(AFN_HUD_PIECE_TILE_LEN) && AFN_HUD_PIECE_TILE_LEN > 0
    hud_bake_pieces();
#endif
    // Palette bank 15 (HUD): index 0 transparent, index 1 white.
    // Pointer arithmetic (not int + cast) so +16 advances 16 u16-words
    // = 32 bytes per palette bank.
    unsigned short* pal = (unsigned short*)0x05000200 + AFN_HUD_PAL_BANK * 16;
    pal[0] = 0;
    pal[1] = RGB15(31, 31, 31);
#endif
}

#if defined(AFN_HAS_SCRIPT) && defined(AFN_HUD_ELEM_COUNT) && AFN_HUD_ELEM_COUNT > 0
// Place a single 8x8 glyph at (sx, sy). Clamps to the printable-ASCII
// font range; codes outside it render as a transparent slot (no glyph).
static int hud_blit_glyph(int oamSlot, int sx, int sy, int ch)
{
    if (oamSlot >= 128) return oamSlot;
    if (ch < AFN_HUD_FONT_FIRST || ch > AFN_HUD_FONT_LAST) return oamSlot;
    int g = ch - AFN_HUD_FONT_FIRST;
    oamSet(&oamMain, oamSlot,
           sx, sy,
           0,                                  // priority
           AFN_HUD_PAL_BANK,                   // palette
           SpriteSize_8x8,
           SpriteColorFormat_16Color,
           (void*)(0x06400000 + AFN_HUD_VRAM_OFFSET + g * AFN_HUD_TILE_STRIDE),
           -1, false, false, false, false, false);
    return oamSlot + 1;
}

static int hud_blit_string(int oamSlot, int sx, int sy, const char* s)
{
    int n = 0;
    while (s[n] && n < 32) {
        oamSlot = hud_blit_glyph(oamSlot, sx + n * 8, sy, (unsigned char)s[n]);
        if (oamSlot >= 128) break;
        n++;
    }
    return oamSlot;
}
#endif

void afn_hud_draw(void) {
#if defined(AFN_HAS_SCRIPT) && defined(AFN_HUD_ELEM_COUNT) && AFN_HUD_ELEM_COUNT > 0
    int oamSlot = AFN_HUD_OAM_BASE;
    for (int e = 0; e < AFN_HUD_ELEM_COUNT; e++) {
        int slot = e;
        if (slot < 4 && !afn_hud_visible[slot]) continue;
        int ex = afn_hud_elems[e].x;
        int ey = afn_hud_elems[e].y;
#if defined(AFN_HUD_PIECE_COUNT) && AFN_HUD_PIECE_COUNT > 0
        // Pieces render BELOW text rows (lower OAM slot = drawn on top
        // for NDS-1D, but priority is what actually sorts). Same palette
        // bank (15) for now — composite tints / per-piece palettes TODO.
        int ps = afn_hud_elems[e].pieceStart;
        int pc = afn_hud_elems[e].pieceCount;
        for (int p = 0; p < pc; p++) {
            if (oamSlot >= 128) break;
            const struct AfnHudPiece* pi = &afn_hud_pieces[ps + p];
            int sz;
            switch (pi->size) {
                case 8:  sz = SpriteSize_8x8;   break;
                case 16: sz = SpriteSize_16x16; break;
                case 32: sz = SpriteSize_32x32; break;
                case 64: sz = SpriteSize_64x64; break;
                default: sz = SpriteSize_16x16; break;
            }
            oamSet(&oamMain, oamSlot++,
                   ex + pi->x, ey + pi->y,
                   1,                                    // priority below text
                   pi->palBank,                          // baked HUD palette bank
                   sz,
                   SpriteColorFormat_16Color,
                   (void*)(0x06400000 + AFN_HUD_PIECE_VRAM_OFFSET + pi->vramTile * AFN_HUD_TILE_STRIDE),
                   -1, false, false, false, false, false);
        }
#endif
        int ts = afn_hud_elems[e].textStart;
        int tc = afn_hud_elems[e].textCount;
        for (int t = 0; t < tc; t++) {
            int tx = ex + afn_hud_texts[ts + t].x;
            int ty = ey + afn_hud_texts[ts + t].y;
            int ss = afn_hud_texts[ts + t].sourceSlot;
            char buf[40];
            const char* s;
            if (ss >= 0 && ss <= 3) {
                int pad = afn_hud_texts[ts + t].pad;
                if (pad < 1) pad = 1;
                int v = afn_hud_value[ss]; if (v < 0) v = 0;
                snprintf(buf, sizeof(buf), "%0*d", pad, v);
                s = buf;
            } else {
                // Static string from the text field. Exported empty when
                // sourceSlot >= 0 so this path only fires for label rows.
                s = afn_hud_texts[ts + t].text;
            }
            oamSlot = hud_blit_string(oamSlot, tx, ty, s);
            if (oamSlot >= 128) break;
        }
    }
    // Park unused HUD slots so stale entries from earlier frames don't render.
    for (int i = oamSlot; i < 128; i++)
        oamSetHidden(&oamMain, i, true);
    // sprite_update flushed oamMain already; without our own flush the
    // HUD writes only land in the user buffer and get wiped next frame.
    oamUpdate(&oamMain);
#endif
}

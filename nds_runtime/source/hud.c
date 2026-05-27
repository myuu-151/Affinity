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
extern int afn_cursor_stop;
extern int afn_active_element;
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
// AFN_HUD_VRAM_OFFSET is emitted by the exporter, sized to start
// immediately after the gameplay-sprite pool. Fallback 98304 covers
// builds without the macro.
#ifndef AFN_HUD_VRAM_OFFSET
#define AFN_HUD_VRAM_OFFSET   98304
#endif
#define AFN_HUD_TILE_STRIDE   128             // 1D_128 OAM addressing
#define AFN_HUD_FONT_FIRST    0x20            // space
#define AFN_HUD_FONT_LAST     0x7E            // ~
#define AFN_HUD_FONT_COUNT    (AFN_HUD_FONT_LAST - AFN_HUD_FONT_FIRST + 1) // 95
// Pieces start after the font slots (95 * 128 = 12160 bytes).
#define AFN_HUD_PIECE_VRAM_OFFSET (AFN_HUD_VRAM_OFFSET + AFN_HUD_FONT_COUNT * AFN_HUD_TILE_STRIDE)
#define AFN_HUD_PAL_BANK      15
// Dedicated all-black palette for HUD pieces with blackTint set
// (drop shadows). All 15 colour slots populated so any palette index
// used by the source tile data renders as solid black.
#define AFN_HUD_BLACK_BANK    10
// HUD reserves slots 8..127 (120 slots). The 8 reserved low slots cover
// the player + any same-frame tm_objects on a typical Mode 0 scene;
// Mode 4 sprite_update also writes into the low range but Affinity
// projects rarely have >8 simultaneous projected sprites on-screen.
// Densely-decorated HUDs (splash logos with shadow + glow layers)
// regularly need 100+ pieces — 96 wasn't enough.
#define AFN_HUD_OAM_BASE      8

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
            // libnds default_font_bin packs pixels with bit 0 = leftmost.
            // 4bpp tile bytes want low nibble = leftmost screen pixel,
            // so read the 1bpp bits from LSB upward.
            for (int half = 0; half < 2; half++) {
                int p0 = (bits >> (half*4 + 0)) & 1;
                int p1 = (bits >> (half*4 + 1)) & 1;
                int p2 = (bits >> (half*4 + 2)) & 1;
                int p3 = (bits >> (half*4 + 3)) & 1;
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
// afn_hud_piece_tiles[] is defined as `static const` in mapdata.h (so
// every TU including it gets its own copy without link conflicts).
// This TU's copy is the one we upload.
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
    // All slots start hidden — matches GBA, where afn_hud_visible[] is
    // BSS-zero and ShowHUD/HideHUD nodes drive what's on screen. The old
    // "default all visible" was a stopgap for when ShowHUD didn't emit; it
    // pinned things like menu elements on at boot.
    for (int i = 0; i < 4; i++) afn_hud_visible[i] = 0;
    hud_bake_font();
#if defined(AFN_HUD_PIECE_TILE_LEN) && AFN_HUD_PIECE_TILE_LEN > 0
    hud_bake_pieces();
#endif
    // Palette bank 15 (HUD font): index 0 transparent, index 1 white.
    // Pointer arithmetic (not int + cast) so +16 advances 16 u16-words
    // = 32 bytes per palette bank.
    unsigned short* pal = (unsigned short*)0x05000200 + AFN_HUD_PAL_BANK * 16;
    pal[0] = 0;
    pal[1] = RGB15(31, 31, 31);
    // Bank 10 — all-black for blackTint pieces (drop shadows). Index 0
    // stays transparent; indices 1-15 all render solid black so the
    // piece's tile data shape blits as a silhouette.
    // dmaCopy avoids any cache / write-ordering surprises that plain
    // pointer stores hit (sprite_init's asset palette upload was
    // somehow winning over my plain writes here).
    {
        static const u16 black_bank[16] = {
            0,
            RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0),
            RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0),
            RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0),
            RGB15(0,0,0), RGB15(0,0,0), RGB15(0,0,0),
        };
        dmaCopy(black_bank, SPRITE_PALETTE + AFN_HUD_BLACK_BANK * 16, 32);
    }
#if defined(AFN_HUD_TEXT_PAL_COUNT) && AFN_HUD_TEXT_PAL_COUNT > 0
    // Per-text-row RGB color: dedup'd by the exporter into a small pool
    // of OBJ palette banks (14, 13, 12, 11). index 0 transparent,
    // index 1 = the row's color.
    for (int i = 0; i < AFN_HUD_TEXT_PAL_COUNT; i++) {
        unsigned short* p = (unsigned short*)0x05000200 + afn_hud_text_palettes[i].bank * 16;
        p[0] = 0;
        p[1] = afn_hud_text_palettes[i].color;
    }
#endif
#endif
}

#if defined(AFN_HAS_SCRIPT) && defined(AFN_HUD_ELEM_COUNT) && AFN_HUD_ELEM_COUNT > 0
// Place a single 8x8 glyph at (sx, sy). Clamps to the printable-ASCII
// font range; codes outside it render as a transparent slot (no glyph).
static int hud_blit_glyph(int oamSlot, int sx, int sy, int ch, int palBank)
{
    if (oamSlot >= 128) return oamSlot;
    if (ch < AFN_HUD_FONT_FIRST || ch > AFN_HUD_FONT_LAST) return oamSlot;
    int g = ch - AFN_HUD_FONT_FIRST;
    oamSet(&oamMain, oamSlot,
           sx, sy,
           0,                                  // priority
           palBank,                            // per-row palette
           SpriteSize_8x8,
           SpriteColorFormat_16Color,
           (void*)(0x06400000 + AFN_HUD_VRAM_OFFSET + g * AFN_HUD_TILE_STRIDE),
           -1, false, false, false, false, false);
    return oamSlot + 1;
}

static int hud_blit_string(int oamSlot, int sx, int sy, const char* s, int palBank)
{
    int n = 0;
    while (s[n] && n < 32) {
        oamSlot = hud_blit_glyph(oamSlot, sx + n * 8, sy, (unsigned char)s[n], palBank);
        if (oamSlot >= 128) break;
        n++;
    }
    return oamSlot;
}
#endif

#if defined(AFN_HUD_PIECE_COUNT) && AFN_HUD_PIECE_COUNT > 0
static int hud_blit_piece(int oamSlot, int sx, int sy, const struct AfnHudPiece* pi)
{
    if (oamSlot >= 128) return oamSlot;
    int sz;
    switch (pi->size) {
        case 8:  sz = SpriteSize_8x8;   break;
        case 16: sz = SpriteSize_16x16; break;
        case 32: sz = SpriteSize_32x32; break;
        case 64: sz = SpriteSize_64x64; break;
        default: sz = SpriteSize_16x16; break;
    }
    // blackTint pieces (drop shadows) override the asset palette with the
    // dedicated all-black bank — index 1..15 all render solid black so
    // the shape's silhouette draws regardless of which colour index the
    // source tile data used.
    int palBank = pi->blackTint ? AFN_HUD_BLACK_BANK : pi->palBank;
    oamSet(&oamMain, oamSlot++,
           sx, sy,
           0,                                              // priority 0 — same as font
           palBank,
           sz,
           SpriteColorFormat_16Color,
           (void*)(0x06400000 + AFN_HUD_PIECE_VRAM_OFFSET + pi->vramTile * AFN_HUD_TILE_STRIDE),
           -1, false, false, false, false, false);
    return oamSlot;
}
#endif

#if defined(AFN_HUD_LAYER_COUNT) && AFN_HUD_LAYER_COUNT > 0
// Per-layer animation state — frame counter (driven by speed = 60/fps)
// plus a sub-frame tick. NOT static so emitted PlayHudAnim /
// StopHudAnim / SetHudAnimSpeed bodies can mutate them via extern decls
// in mapdata.h.
int afn_hud_layer_frame [AFN_HUD_LAYER_COUNT];
int afn_hud_layer_tick  [AFN_HUD_LAYER_COUNT];
unsigned char afn_hud_layer_active[AFN_HUD_LAYER_COUNT];
unsigned char afn_hud_layer_speed_override[AFN_HUD_LAYER_COUNT]; // 0 = use exporter's speed

static void hud_layer_advance(void)
{
    for (int li = 0; li < AFN_HUD_LAYER_COUNT; li++) {
        if (!afn_hud_layer_active[li]) continue;
        int spd = afn_hud_layer_speed_override[li]
                ? afn_hud_layer_speed_override[li]
                : afn_hud_layers[li].speed;
        if (spd < 1) spd = 1;
        afn_hud_layer_tick[li]++;
        if (afn_hud_layer_tick[li] >= spd) {
            afn_hud_layer_tick[li] = 0;
            afn_hud_layer_frame[li]++;
            int len = afn_hud_layers[li].length;
            if (afn_hud_layers[li].loop && len > 0 && afn_hud_layer_frame[li] >= len)
                afn_hud_layer_frame[li] = 0;
        }
    }
}

// Resolve (offX, offY) for layer li at its current frame, lerping
// between the surrounding two keyframes.
static void hud_layer_offset(int li, int* outX, int* outY)
{
    *outX = 0; *outY = 0;
    int kS = afn_hud_layers[li].kfStart;
    int kN = afn_hud_layers[li].kfCount;
    if (kN < 1) return;
    int t = afn_hud_layer_frame[li];
    int lastF = afn_hud_layer_kf[kS + kN - 1].frame;
    // After the last keyframe, freeze on the last value (until loop wraps).
    if (t >= lastF) { *outX = afn_hud_layer_kf[kS + kN - 1].offX;
                      *outY = afn_hud_layer_kf[kS + kN - 1].offY; return; }
    if (t <= afn_hud_layer_kf[kS].frame) { *outX = afn_hud_layer_kf[kS].offX;
                                           *outY = afn_hud_layer_kf[kS].offY; return; }
    for (int i = 0; i + 1 < kN; i++) {
        int f0 = afn_hud_layer_kf[kS + i].frame;
        int f1 = afn_hud_layer_kf[kS + i + 1].frame;
        if (t >= f0 && t <= f1) {
            int span = f1 - f0; if (span < 1) span = 1;
            int interp = afn_hud_layers[li].interp;
            int x0 = afn_hud_layer_kf[kS + i].offX, x1 = afn_hud_layer_kf[kS + i + 1].offX;
            int y0 = afn_hud_layer_kf[kS + i].offY, y1 = afn_hud_layer_kf[kS + i + 1].offY;
            if (interp == 0) {
                // Constant — snap to f0's value.
                *outX = x0; *outY = y0;
            } else {
                int u = ((t - f0) * 256) / span; // 0..256
                if (interp == 2) {
                    // Smoothstep t*t*(3-2t)
                    u = (u * u * (768 - 2 * u)) >> 16;
                }
                *outX = x0 + ((x1 - x0) * u >> 8);
                *outY = y0 + ((y1 - y0) * u >> 8);
            }
            return;
        }
    }
}
#endif

#if defined(AFN_HUD_KF_COUNT) && AFN_HUD_KF_COUNT > 0
// Resolve (offX, offY) at the given absolute frame by walking the
// element's keyframe range and linearly interpolating between the
// surrounding two. Loops when kfLoop is set and t exceeds the last kf.
extern int afn_frame_count;
static void hud_kf_at(int e, int* outOffX, int* outOffY)
{
    *outOffX = 0; *outOffY = 0;
    int ks = afn_hud_elems[e].kfStart;
    int kc = afn_hud_elems[e].kfCount;
    if (kc <= 0) return;
    int t = afn_frame_count;
    if (afn_hud_elems[e].kfLoop) {
        int last = afn_hud_kf[ks + kc - 1].frame;
        if (last > 0) t = t % (last + 1);
    }
    // Before first keyframe → snap to its value.
    if (t <= afn_hud_kf[ks].frame) {
        *outOffX = afn_hud_kf[ks].offX;
        *outOffY = afn_hud_kf[ks].offY;
        return;
    }
    // After last keyframe → snap to it.
    if (t >= afn_hud_kf[ks + kc - 1].frame) {
        *outOffX = afn_hud_kf[ks + kc - 1].offX;
        *outOffY = afn_hud_kf[ks + kc - 1].offY;
        return;
    }
    // Find the surrounding pair.
    for (int i = 0; i + 1 < kc; i++) {
        int f0 = afn_hud_kf[ks + i].frame;
        int f1 = afn_hud_kf[ks + i + 1].frame;
        if (t >= f0 && t <= f1) {
            int span = f1 - f0; if (span < 1) span = 1;
            int u = ((t - f0) * 256) / span; // 0..256
            int x0 = afn_hud_kf[ks + i].offX, x1 = afn_hud_kf[ks + i + 1].offX;
            int y0 = afn_hud_kf[ks + i].offY, y1 = afn_hud_kf[ks + i + 1].offY;
            *outOffX = x0 + ((x1 - x0) * u >> 8);
            *outOffY = y0 + ((y1 - y0) * u >> 8);
            return;
        }
    }
}
#endif

void afn_hud_draw(void) {
#if defined(AFN_HAS_SCRIPT) && defined(AFN_HUD_ELEM_COUNT) && AFN_HUD_ELEM_COUNT > 0
    extern int afn_current_mode;
    extern int afn_current_scene;
#if defined(AFN_HUD_LAYER_COUNT) && AFN_HUD_LAYER_COUNT > 0
    hud_layer_advance();
#endif
    int oamSlot = AFN_HUD_OAM_BASE;
    for (int e = 0; e < AFN_HUD_ELEM_COUNT; e++) {
        int slot = e;
        if (slot < 4 && !afn_hud_visible[slot]) continue;
        // runtimeMode: 0 = both, 1 = Mode 4 only, 2 = Mode 0 only.
        // afn_current_mode: 0 = Mode 4 (3D), 1 = Mode 0 (tilemap).
        int rm = afn_hud_elems[e].runtimeMode;
        if (rm == 1 && afn_current_mode != 0) continue;
        if (rm == 2 && afn_current_mode != 1) continue;
        // Per-scene mask: skip if current scene's bit isn't set.
        unsigned int mask = (afn_current_mode == 0)
            ? afn_hud_elems[e].mode4Mask
            : afn_hud_elems[e].mode0Mask;
        if (!(mask & (1u << afn_current_scene))) continue;
        int ex = afn_hud_elems[e].x;
        int ey = afn_hud_elems[e].y;
#if defined(AFN_HUD_KF_COUNT) && AFN_HUD_KF_COUNT > 0
        int kfX = 0, kfY = 0;
        hud_kf_at(e, &kfX, &kfY);
        ex += kfX; ey += kfY;
#endif
        // Layer ordering — render the higher-layer category FIRST so it
        // takes the lower OAM slot (NDS draws lower slot on top within
        // the same priority). Tie-break order: pieces, sprites, text, cursor.
        signed char layers[4] = {
            afn_hud_elems[e].layerPieces,
            afn_hud_elems[e].layerSprites,
            afn_hud_elems[e].layerText,
            afn_hud_elems[e].layerCursor
        };
        int order[4] = { 0, 1, 2, 3 };
        // Tiny insertion sort, descending by layer.
        for (int a = 1; a < 4; a++) {
            int k = order[a]; signed char kl = layers[k];
            int b = a;
            while (b > 0 && layers[order[b-1]] < kl) { order[b] = order[b-1]; b--; }
            order[b] = k;
        }
        for (int oi = 0; oi < 4; oi++) {
            if (oamSlot >= 128) break;
            int cat = order[oi];
            if (cat == 0) {
#if defined(AFN_HUD_PIECE_COUNT) && AFN_HUD_PIECE_COUNT > 0
                int ps = afn_hud_elems[e].pieceStart;
                int pc = afn_hud_elems[e].pieceCount;
                for (int p = pc - 1; p >= 0; p--) {
                    if (oamSlot >= 128) break;
                    const struct AfnHudPiece* pi = &afn_hud_pieces[ps + p];
                    int aox = 0, aoy = 0;
#if defined(AFN_HUD_LAYER_COUNT) && AFN_HUD_LAYER_COUNT > 0
                    // Find any anim layer that targets this piece, accumulate offsets.
                    for (int li = 0; li < AFN_HUD_LAYER_COUNT; li++) {
                        if (afn_hud_layers[li].elemIdx != e) continue;
                        int iS = afn_hud_layers[li].itemStart;
                        int iN = afn_hud_layers[li].itemCount;
                        for (int ii = 0; ii < iN; ii++) {
                            if (afn_hud_layer_items[iS + ii].type == 0 &&
                                afn_hud_layer_items[iS + ii].index == p) {
                                int lx, ly; hud_layer_offset(li, &lx, &ly);
                                aox += lx; aoy += ly;
                                break;
                            }
                        }
                    }
#endif
                    // Wrap the LAYER offset itself at GBA screen width (240)
                    // so animations authored against the editor's 240-px canvas
                    // end where they started — a +240 keyframe means "one full
                    // screen of slide." Wrapping at 256 (NDS) leaves a 16-px gap.
                    int wrapMod = 240;
                    int aoxW = aox % wrapMod;
                    if (aoxW < 0) aoxW += wrapMod;
                    int px = ex + pi->x + aoxW;
                    int py = ey + pi->y + aoy;
                    oamSlot = hud_blit_piece(oamSlot, px, py, pi);
                    // Draw a second copy at px - wrapMod so the wrapped portion
                    // appears as the piece's "left half" sliding off the right.
                    if (aoxW != 0 && oamSlot < 128)
                        oamSlot = hud_blit_piece(oamSlot, px - wrapMod, py, pi);
                }
#endif
            } else if (cat == 1) {
#if defined(AFN_HUD_SPRITE_COUNT) && AFN_HUD_SPRITE_COUNT > 0
                int ss2 = afn_hud_elems[e].sprStart;
                int sc2 = afn_hud_elems[e].sprCount;
                for (int p = sc2 - 1; p >= 0; p--) {
                    if (oamSlot >= 128) break;
                    const struct AfnHudPiece* pi = &afn_hud_sprites[ss2 + p];
                    oamSlot = hud_blit_piece(oamSlot, ex + pi->x, ey + pi->y, pi);
                }
#endif
            } else if (cat == 2) {
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
                        // Static string from the text field.
                        s = afn_hud_texts[ts + t].text;
                    }
                    int palBank = afn_hud_texts[ts + t].palBank;
                    if (palBank < 0 || palBank > 15) palBank = AFN_HUD_PAL_BANK;
                    oamSlot = hud_blit_string(oamSlot, tx, ty, s, palBank);
                    if (oamSlot >= 128) break;
                }
            } else {
                // Cursor: render the active element's selected stop. The
                // cursor is a normal sprite asset (curAsset / curFrame in
                // the element) streamed into its asset's VRAM slot via the
                // shared sprite DMA cache.
#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
                if (e == afn_active_element && oamSlot < 128) {
                    int stopCount = afn_hud_elems[e].stopCount;
                    int cAi = afn_hud_elems[e].curAsset;
                    if (stopCount > 0 && cAi >= 0 && cAi < AFN_ASSET_COUNT) {
                        int stopIdx = afn_cursor_stop;
                        if (stopIdx < 0 || stopIdx >= stopCount) stopIdx = 0;
                        int si = afn_hud_elems[e].stopStart + stopIdx;
                        int csx = ex + afn_hud_stops[si].x + afn_hud_elems[e].curOffX;
                        int csy = ey + afn_hud_stops[si].y + afn_hud_elems[e].curOffY;

                        int cFrame   = afn_hud_elems[e].curFrame;
                        int vramBase = afn_asset_desc[cAi][0];
                        int tpf      = afn_asset_desc[cAi][1];
                        int objSize  = afn_asset_desc[cAi][3];
                        int palBank  = afn_asset_desc[cAi][4] & 0xF;
                        int frameBase = afn_asset_desc[cAi][9];

                        // DMA the cursor frame into the asset's VRAM slot
                        // (same per-asset streaming sprites.c uses). Skip
                        // when the active frame already matches.
                        extern int g_active_frame[AFN_ASSET_COUNT];
#if AFN_FRAME_STREAM_LEN > 0
                        int globalFrame = frameBase + cFrame;
                        if (globalFrame >= 0 && globalFrame < AFN_FRAME_STREAM_LEN
                            && g_active_frame[cAi] != globalFrame) {
                            const u32* src = afn_all_tiles + afn_frame_rom_off[globalFrame];
                            u32* dst = (u32*)(0x06400000 + vramBase * 32);
                            int tiles = afn_frame_tile_count[globalFrame];
                            dmaCopy(src, dst, tiles * 32);
                            g_active_frame[cAi] = globalFrame;
                        }
#endif
                        SpriteSize sz = objSize == 8  ? SpriteSize_8x8   :
                                        objSize == 16 ? SpriteSize_16x16 :
                                        objSize == 32 ? SpriteSize_32x32 : SpriteSize_64x64;
                        int tileSlot = vramBase + cFrame * tpf;
                        oamSet(&oamMain, oamSlot,
                               csx, csy,
                               0, palBank, sz, SpriteColorFormat_16Color,
                               (void*)((u8*)0x06400000 + tileSlot * 32),
                               -1, false, false, false, false, false);
                        oamSlot++;
                    }
                }
#endif
            }
        }
    }
    // Park unused HUD slots so stale entries from earlier frames don't render.
    // Skip slots flagged RotateScale — those belong to sprite_update (called
    // earlier this frame) and oamSetHidden asserts on RotateScale entries.
    // sprite_update's oamClear next frame will retire them properly.
    for (int i = oamSlot; i < 128; i++) {
        if (oamMain.oamMemory[i].isRotateScale) continue;
        oamSetHidden(&oamMain, i, true);
    }
    // sprite_update flushed oamMain already; without our own flush the
    // HUD writes only land in the user buffer and get wiped next frame.
    oamUpdate(&oamMain);
#endif
}

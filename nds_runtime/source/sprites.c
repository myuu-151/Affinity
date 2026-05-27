// Affinity NDS — OAM sprite projection + render.
// Mirrors GBA's update_sprites: per-frame world→screen projection with
// affine-scale OBJs for distance shrink. Sprite assets are uploaded once
// at boot from afn_all_tiles + afn_pal (emitted by nds_package).

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/sprite.h>
#include <nds/arm9/cache.h>
#include <stdio.h>

#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
#define HAS_SPRITES 1
// Per-asset currently-loaded VRAM frame, -1 = none. sprite_update() DMAs
// the active frame in when it changes (drives the streaming pipeline).
int g_active_frame[AFN_ASSET_COUNT];
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
    // 1D_128 mapping: 10-bit OAM tile field × 128-byte boundary = 128KB
    // addressable, matching bank B alone. Going wider would require
    // mapping bank A as the second sprite slot, but bank A is currently
    // tied up serving 3D textures (see main.c) and bank C/D both fail
    // as texture banks.
    oamInit(&oamMain, SpriteMapping_1D_128, false);

    // Push the 3D layer (BG0) to the lowest priority so OBJ (default
    // priority 0) renders on top of the 3D scene.
    REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
    REG_DISPCNT |= DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT;

    // DMA streaming: no bulk upload of afn_all_tiles. The runtime DMAs
    // each asset's active frame into its fixed VRAM slot on demand —
    // sprite_update() below tracks g_active_frame[] per asset and pushes
    // the current frame's tile data when it changes.
    for (int ai = 0; ai < AFN_ASSET_COUNT; ai++) g_active_frame[ai] = -1;

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
    int oamPrio;
} ProjSpr;

void afn_sprite_update(void)
{
#if defined(HAS_SPRITES)
    oamClear(&oamMain, 0, 128);

    // Per-sprite anim state — accumulator resets when the chosen anim
    // changes so each instance starts at its own frame 0 instead of jumping
    // to a global tick's modulo (which causes visible stutter on switch).
    static int s_animTick = 0;
    s_animTick++;
    static int s_animPrev[AFN_SPRITE_COUNT];   // last animIdx per sprite
    static int s_animStart[AFN_SPRITE_COUNT];  // tick when current anim started

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
#if defined(AFN_HAS_SCRIPT) && defined(NUM_SPRITES)
        // DestroyObject / SetVisible toggle this; skip rendering when hidden.
        if (si < NUM_SPRITES && !afn_sprite_visible[si]) continue;
#endif

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
        // Attached (sub) sprite: follow its parent every frame. parentIdx
        // is sprite_data column 12; offX/Y/Z in 13/14/15, grounded in 16.
        // Grounded subs (shadows) stay at offY in world space so they
        // stick to the ground while the parent jumps.
        {
            int parentIdx = afn_sprite_data[si][12];
            if (parentIdx >= 0 && parentIdx < AFN_SPRITE_COUNT) {
                int px = afn_sprite_data[parentIdx][0];
                int py = afn_sprite_data[parentIdx][1];
                int pz = afn_sprite_data[parentIdx][2];
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0
                if (parentIdx == AFN_PLAYER_IDX) {
                    px = player_x; py = player_y; pz = player_z;
                }
#endif
                int offX = afn_sprite_data[si][13];
                int offY = afn_sprite_data[si][14];
                int offZ = afn_sprite_data[si][15];
                int grounded = afn_sprite_data[si][16];
                wx = px + offX;
                wy = grounded ? offY : (py + offY);
                wz = pz + offZ;
            }
        }

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

        // GBA formula: invScale = depth * 14 / sprScale (when scaleSize ==
        // baseSize). Per-sprite scale = 256 → 1×; larger sprScale → bigger
        // sprite. Mirrors the rendering in gba_runtime/main.c:2946.
        int sprScale = afn_sprite_data[si][5];
        if (sprScale <= 0) sprScale = 256;
        int matScale = (depth * 14) / sprScale;
        if (matScale < 128)  matScale = 128;
        if (matScale > 2048) matScale = 2048;

        // Direction picking: camera-space angle into the sprite's 8 facings.
        // Convention: dir 0 = sprite seen from south (+Z relative to sprite,
        // i.e. camera at +Z of sprite). Walking CCW around the sprite advances
        // through dirs 0..7. ForceStatic sprites (sprite_data[10]==1) always
        // use dir 0.
        int tileStart  = afn_asset_desc[aIdx][0];
        int tilesPerFr = afn_asset_desc[aIdx][1];
        int frameCount = afn_asset_desc[aIdx][2];
        int dirCount   = afn_asset_desc[aIdx][5];
        int animBase   = afn_asset_desc[aIdx][6];
        int animCount  = afn_asset_desc[aIdx][7];
        int defaultAn  = afn_asset_desc[aIdx][8];
        int fStatic    = afn_sprite_data[si][10];
        int dir = 0;
        if (dirCount > 1 && !fStatic) {
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0
            if (si == AFN_PLAYER_IDX) {
                // Moving sprite is input-only (no orbit_angle) so holding
                // L/R while walking doesn't flicker NW/NE — base dir
                // stays stable, only the explicit L/R shift below changes
                // it. Idle has the 2x orbit multiplier so the sprite
                // rotates visibly as the camera orbits.
                uint16_t sprAngle = player_moving
                    ? player_move_angle
                    : (uint16_t)(player_move_angle - (orbit_angle << 1));
                int rawIdx = ((sprAngle + 0xC000 + 4096) >> 13) & 7;
                dir = (8 - rawIdx) & 7;
                // L/R shift only while moving — otherwise pressing L for
                // orbit would instantly slap the idle sprite to NE/NW
                // before the orbit-rotation actually moves the dir.
                if (player_moving) {
                    int held = keysHeld();
                    if (held & KEY_L)      dir = (dir - 1 + 8) & 7;  // UP+L → NW
                    else if (held & KEY_R) dir = (dir + 1) & 7;      // UP+R → NE
                }
                static int s_pickDbg = 0;
                s_pickDbg++;
                if ((s_pickDbg & 15) == 0) {
                    iprintf("\x1b[18;0Hmv=%d pma=%04X ora=%04X sa=%04X raw=%d dir=%d ",
                            player_moving, (unsigned)player_move_angle,
                            (unsigned)orbit_angle, (unsigned)sprAngle, rawIdx, dir);
                }
            } else
#endif
            {
                // Non-player: camera-position billboard (back-facing camera).
                int vx = -dx, vz = -dz;
                int ax = vx < 0 ? -vx : vx;
                int az = vz < 0 ? -vz : vz;
                int tanLo = (ax * 128) < (az * 53);
                int tanHi = (az * 128) < (ax * 53);
                if (vz >= 0 && vx >= 0) dir = tanLo ? 0 : (tanHi ? 2 : 1);
                else if (vz < 0 && vx >= 0) dir = tanHi ? 2 : (tanLo ? 4 : 3);
                else if (vz < 0 && vx < 0) dir = tanLo ? 4 : (tanHi ? 6 : 5);
                else dir = tanHi ? 6 : (tanLo ? 0 : 7);
            }
            if (dir >= dirCount) dir = 0;
        }

        // Pick which anim to play. Player sprite obeys afn_play_anim
        // (PlayAnim node); other sprites that match afn_sprite_anim_spr
        // obey afn_sprite_anim_val (SetSpriteAnim). Else asset default.
        int animIdx = defaultAn;
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0
        if (si == AFN_PLAYER_IDX && afn_play_anim >= 0)
            animIdx = afn_play_anim;
#endif
        if (afn_sprite_anim_spr == si && afn_sprite_anim_val >= 0)
            animIdx = afn_sprite_anim_val;
        if (animIdx < 0 || animIdx >= animCount) animIdx = defaultAn;

        // Resolve anim → {startFrame, count, fps, loop} via the global table.
        int frameStart = 0, animLen = frameCount, animFps = 0, animLoop = 1;
#if AFN_ANIM_TABLE_LEN > 0
        if (animCount > 0 && animIdx >= 0 && animIdx < animCount) {
            const int* row = afn_anim_table[animBase + animIdx];
            frameStart = row[0];
            animLen    = row[1];
            animFps    = row[2];
            animLoop   = row[3];
        }
#endif
        // Per-sprite anim phase. Reset start tick when the anim changes
        // so cycling begins from frame 0 of the new range, not from a
        // global modulo that'd jump mid-cycle.
        if (s_animPrev[si] != animIdx) {
            s_animPrev[si]  = animIdx;
            s_animStart[si] = s_animTick;
        }
        int animFrame = frameStart;
        int oneShotDone = 0;
        if (animLen > 1 && animFps > 0) {
            int hold = 60 / animFps; if (hold < 1) hold = 1;
            int elapsed = s_animTick - s_animStart[si];
            int step = elapsed / hold;
            if (!animLoop && step >= animLen) oneShotDone = 1;
            int local = animLoop ? (step % animLen)
                                 : (step >= animLen ? animLen - 1 : step);
            animFrame = frameStart + local;
        }
        if (animFrame >= frameCount) animFrame = frameCount - 1;
        // SetSpriteAnim one-shot revert (mirrors GBA main.c:8028-8033):
        // when a script-driven override finishes its non-looping cycle, clear
        // the override and snap back to the asset's default anim so it
        // doesn't stick on the last frame forever.
        if (oneShotDone && afn_sprite_anim_spr == si && afn_sprite_anim_val >= 0) {
            afn_sprite_anim_spr = -1;
            afn_sprite_anim_val = -1;
            animIdx = defaultAn;
            s_animPrev[si] = animIdx;
            s_animStart[si] = s_animTick;
            // Re-resolve frame to defaultAn's start so this frame already
            // renders the reverted anim, not the override's last frame.
#if AFN_ANIM_TABLE_LEN > 0
            if (animCount > 0 && animIdx >= 0 && animIdx < animCount) {
                const int* row2 = afn_anim_table[animBase + animIdx];
                animFrame = row2[0];
            } else {
                animFrame = 0;
            }
#else
            animFrame = 0;
#endif
        }

#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0
        if (si == AFN_PLAYER_IDX) {
            static int s_animDbgF = 0;
            s_animDbgF++;
            if ((s_animDbgF & 15) == 0) {
                iprintf("\x1b[12;0HaIdx=%d aB=%d aC=%d fS=%d aL=%d aF=%d fc=%d",
                        aIdx, animBase, animCount, frameStart, animLen, animFrame, frameCount);
                iprintf("\x1b[13;0Hpa=%d animIdx=%d ms=%d dep=%d",
                        afn_play_anim, animIdx, matScale, depth);
            }
        }
#endif

        // DMA streaming: if this asset's currently-loaded VRAM frame
        // doesn't match the frame we want to render, DMA in the new one.
        // g_active_frame tracks per-asset; the DMA only fires on frame
        // changes (which happens at the anim's fps cadence — every 5-12
        // game frames, not every frame).
        {
            int frameBase = afn_asset_desc[aIdx][9];
            int globalFrame = frameBase + animFrame;
            int vramTileBase = afn_asset_desc[aIdx][0];
#if AFN_FRAME_STREAM_LEN > 0
            extern int g_active_frame[AFN_ASSET_COUNT];
            if (g_active_frame[aIdx] != globalFrame) {
                const u32* src = afn_all_tiles + afn_frame_rom_off[globalFrame];
                u32* dst = (u32*)(0x06400000 + vramTileBase * 32);
                int tiles = afn_frame_tile_count[globalFrame];
                dmaCopy(src, dst, tiles * 32);
                g_active_frame[aIdx] = globalFrame;
            }
#endif
        }

        proj[projCount].idx     = si;
        proj[projCount].depth   = depth;
        proj[projCount].screenX = screenX;
        proj[projCount].screenY = screenY;
        proj[projCount].matScale = matScale;
        proj[projCount].objSize = objSize;
        proj[projCount].palBank = afn_asset_desc[aIdx][4] & 0xF;
        proj[projCount].oamPrio = afn_sprite_data[si][11] & 3;
        // tileIdx = vramTileBase + frame_dir_tile[frame][dir]  (frame_dir
        // is now VRAM-slot-relative since DMA put the frame's data at
        // vramTileBase). Per-frame dir-fallback baked in by exporter.
        {
            int frameBase = afn_asset_desc[aIdx][9];
            int globalFrame = frameBase + animFrame;
            int vramTileBase = afn_asset_desc[aIdx][0];
#if AFN_FRAME_DIR_TILE_LEN > 0
            int dirOff = afn_frame_dir_tile[globalFrame][dir];
#else
            int dirOff = (animFrame * dirCount + dir) * tilesPerFr;
#endif
            proj[projCount].tileIdx = vramTileBase + dirOff;
            (void)tileStart;
        }
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
               p->oamPrio, p->palBank, sz, SpriteColorFormat_16Color,
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


// Affinity NDS — FPS / 3D mesh scene (Mode 4 GBA equivalent on NDS hardware 3D).
// Owns: camera state, Mode 7 affine floor HBlank handler, mesh upload + render.

#include "affinity.h"
#include "mapdata.h"
#include "dsma.h"
#include <nds/arm9/videoGL.h>
#include <stdio.h>
#include <stdlib.h> // malloc/calloc/free for mesh spatial buckets
#include <math.h>   // sinf/cosf/sqrtf for slope-normal rig alignment

// ---------------------------------------------------------------------------
// Camera state — definitions (extern in affinity.h)
// ---------------------------------------------------------------------------
int cam_x, cam_z, cam_h;
uint16_t cam_angle;
int g_cosf, g_sinf;

int player_x, player_z, player_y;
// Render-smoothed player position. Physics (player_x/y/z) stays exactly on the
// rail for collision + camera; sprites.c draws the player from these instead so
// the sharp heading/Y kink at each hand-placed rail joint doesn't make the
// sprite snap against the smoothly-eased camera. Off the rail these track the
// exact position 1:1 (no input lag).
int player_render_x, player_render_y, player_render_z;
uint16_t orbit_angle;
int orbit_dist;
int player_sprite_idx = -1;
int player_moving;
// Init so the picker formula (sprAngle = pma - 2*orbit_angle) resolves to
// sprAngle = 0x4000 → dir 0 (N, back) regardless of what AFN_CAM_ANGLE
// the project ships with. Previously hard-coded to 0x4000, which only
// gave the back-facing pose for the default AFN_CAM_ANGLE — set the
// editor's Angle slider to anything else and sonic would start facing
// the camera (S sprite) until the first DPAD-UP snap.
uint16_t player_move_angle = (uint16_t)(0x4000 + (AFN_CAM_ANGLE << 1));
uint16_t orbit_angle = AFN_CAM_ANGLE;
// Last frame's world-space movement direction (un-normalized). The sprite
// dir picker reads these to face the player in the direction of motion.
int s_lastMoveDX, s_lastMoveDZ;
static int s_wasMoving = 0;

int m7_horizon = 60;
int m7_bg;

#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
static int gl_sky_tex_id = 0;
#endif

// ---------------------------------------------------------------------------
// Scene-transition state (defs match affinity.h externs)
// ---------------------------------------------------------------------------
int afn_pending_scene      = -1;
int afn_pending_scene_mode = 0;
int afn_current_scene      = 0;
int afn_current_mode       = 0;
// Fade state lives in script_glue.c when scripts are on (so SceneChange /
// FadeTo nodes can write it directly); fallback here otherwise.
#ifndef AFN_HAS_SCRIPT
int afn_fade_target        = 0;
int afn_fade_counter       = 0;
int afn_fade_frames        = 0;
#endif
static int s_fade_phase    = 0;   // 0 = idle, 1 = fading out, 2 = fading in

// ---------------------------------------------------------------------------
// Mode 7 floor — HBlank driven affine BG
// ---------------------------------------------------------------------------
void m7_hbl(void)
{
    int vc = REG_VCOUNT;
    int row = vc - m7_horizon;

    if (row <= 0)
    {
        REG_BG2PA = 0;
        REG_BG2PB = 0;
        REG_BG2PC = 0;
        REG_BG2PD = 0;
        REG_BG2X = 0;
        REG_BG2Y = 0;
        return;
    }

    int D = 160;
    int lam = (D << 8) / row;

    int lcf = FX_MUL(lam, g_cosf);
    int lsf = FX_MUL(lam, g_sinf);

    REG_BG2PA = (int16_t)(lcf >> 0);
    REG_BG2PC = (int16_t)(lsf >> 0);
    REG_BG2PB = (int16_t)(-lsf >> 0);
    REG_BG2PD = (int16_t)(lcf >> 0);

    int cx = cam_x >> FX_SHIFT;
    int cz = cam_z >> FX_SHIFT;

    int dx = 128 - FX_MUL(lcf, 128) + FX_MUL(lsf, 128);
    int dy = 128 - FX_MUL(lsf, 128) - FX_MUL(lcf, 128);

    REG_BG2X = (cx << 8) + (dx << 0);
    REG_BG2Y = (cz << 8) + (dy << 0);
}

// ---------------------------------------------------------------------------
// Floor tilemap (only present when the project exports one)
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
static void load_floor(void)
{
    dmaCopy(afn_floor_tiles, BG_TILE_RAM(0), AFN_FLOOR_TILES_LEN);
    dmaCopy(afn_floor_map, BG_MAP_RAM(4), AFN_FLOOR_MAP_LEN);
    dmaCopy(afn_floor_pal, BG_PALETTE, AFN_FLOOR_PAL_LEN);
}
#endif

// AFN_PLAYER_BASE_Y: spawn-Y of the player sprite (used as the ground
// reference for camera height + flat-ground fallback). Falls back to 0
// when a project has no sprites or no Player-typed sprite — keeps
// fps3d.c linkable for empty scenes that boot straight into Mode 0.
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
#define AFN_PLAYER_BASE_Y  afn_sprite_data[AFN_PLAYER_IDX][1]
#else
#define AFN_PLAYER_BASE_Y  0
#endif

// ---------------------------------------------------------------------------
// Mesh textures → VRAM
// ---------------------------------------------------------------------------
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
static int gl_tex_ids[AFN_MESH_COUNT];

static void load_mesh_textures(void)
{
    for (int i = 0; i < AFN_MESH_COUNT; i++)
    {
        gl_tex_ids[i] = 0;
        if (!afn_mesh_desc[i][8]) continue;

        int texW = afn_mesh_desc[i][9];
        // Slot [6] carries texH for non-square textures; fall back to
        // texW when 0 (older mapdata.h that pre-dates the split).
        int texH = afn_mesh_desc[i][6] > 0 ? afn_mesh_desc[i][6] : texW;
        if (texW == 0) continue;

        // glTexImage2D wants size as the TEXTURE_SIZE_* enum
        // (TEXTURE_SIZE_8=0, _16=1, _32=2, _64=3, _128=4, _256=5, _512=6, _1024=7),
        // not the raw pixel count. log2(N) - 3 gets us there for power-of-2 sizes.
        int sizeW = 0, tw = texW; while (tw > 8) { tw >>= 1; sizeW++; }
        int sizeH = 0, th = texH; while (th > 8) { th >>= 1; sizeH++; }

        glGenTextures(1, &gl_tex_ids[i]);
        glBindTexture(0, gl_tex_ids[i]);
        // Texture parameter flags:
        //   TEXGEN_TEXCOORD         — use UVs from glTexCoord2t16
        //   GL_TEXTURE_WRAP_S/_T    — tile when UVs go past 0..texSize
        //                             (default is CLAMP → bricks stop at edges)
        //   GL_TEXTURE_COLOR0_TRANSPARENT — palette index 0 = transparent
        int flags = TEXGEN_TEXCOORD | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
        if (afn_mesh_desc[i][11]) flags |= GL_TEXTURE_COLOR0_TRANSPARENT;
#ifdef AFN_MESH_HAS_TEX256
        int is256 = afn_mesh_tex256[i];
#else
        int is256 = 0;
#endif
        glTexImage2D(0, 0, is256 ? GL_RGB256 : GL_RGB16, sizeW, sizeH, 0, flags,
                     afn_mesh_tex_ptrs[i]);
        glColorTableEXT(0, 0, is256 ? 256 : 16, 0, 0, afn_mesh_tex_pal_ptrs[i]);
    }
}

// --- Spatial bucketing for per-region mesh culling -------------------------
// render_meshes submits every vertex in immediate mode (no batching), so an
// off-screen triangle still costs full CPU. To cull WITHIN a single mesh (so a
// whole level that's one giant subdivided mesh still only pays for what's on
// screen), each mesh is partitioned once into a GN×GN×GN grid of buckets by
// triangle centroid. Per frame we cull each bucket's bounding sphere against
// the frustum and only submit the survivors. A single huge mesh thus self-
// partitions into cullable chunks.
#define MESH_GN       4
#define MESH_NBUCKET  (MESH_GN * MESH_GN * MESH_GN)
typedef struct {
    int cx, cy, cz;            // local-space sphere center (fx8)
    int radius;                // local-space sphere radius (fx8)
    int triStart, triCount;    // span into s_tri_idx[mi]  (u16 indices, 3/tri)
    int quadStart, quadCount;  // span into s_quad_idx[mi] (u16 indices, 4/quad)
} MeshBucket;
static MeshBucket* s_buckets[AFN_MESH_COUNT];
static int         s_bucket_count[AFN_MESH_COUNT];
static uint16_t*   s_tri_idx[AFN_MESH_COUNT];
static uint16_t*   s_quad_idx[AFN_MESH_COUNT];

static void mesh_buckets_build(void)
{
    for (int mi = 0; mi < AFN_MESH_COUNT; mi++) {
        s_buckets[mi] = 0; s_bucket_count[mi] = 0;
        s_tri_idx[mi] = 0; s_quad_idx[mi] = 0;

        const int16_t* mv  = afn_mesh_vert_ptrs[mi];
        const uint16_t* ti = afn_mesh_idx_ptrs[mi];
        const uint16_t* qi = afn_mesh_qidx_ptrs[mi];
        int ic = afn_mesh_desc[mi][1];
        int qc = afn_mesh_desc[mi][2];
        int ntri = ic / 3, nquad = qc / 4;
        if (ntri <= 0 && nquad <= 0) continue;

        // Mesh AABB for the bucket grid.
        int mnx = 1<<30, mny = 1<<30, mnz = 1<<30;
        int mxx = -(1<<30), mxy = -(1<<30), mxz = -(1<<30);
        for (int k = 0; k < ic; k++) { int v = ti[k];
            int x = mv[v*3], y = mv[v*3+1], z = mv[v*3+2];
            if (x<mnx)mnx=x; if (x>mxx)mxx=x; if (y<mny)mny=y; if (y>mxy)mxy=y; if (z<mnz)mnz=z; if (z>mxz)mxz=z; }
        for (int k = 0; k < qc; k++) { int v = qi[k];
            int x = mv[v*3], y = mv[v*3+1], z = mv[v*3+2];
            if (x<mnx)mnx=x; if (x>mxx)mxx=x; if (y<mny)mny=y; if (y>mxy)mxy=y; if (z<mnz)mnz=z; if (z>mxz)mxz=z; }

        int exx = mxx-mnx; if (exx < 1) exx = 1;
        int exy = mxy-mny; if (exy < 1) exy = 1;
        int exz = mxz-mnz; if (exz < 1) exz = 1;
        #define CELL_OF(cx,cy,cz) ({ \
            int _gx=((cx)-mnx)*MESH_GN/(exx+1); if(_gx<0)_gx=0; if(_gx>=MESH_GN)_gx=MESH_GN-1; \
            int _gy=((cy)-mny)*MESH_GN/(exy+1); if(_gy<0)_gy=0; if(_gy>=MESH_GN)_gy=MESH_GN-1; \
            int _gz=((cz)-mnz)*MESH_GN/(exz+1); if(_gz<0)_gz=0; if(_gz>=MESH_GN)_gz=MESH_GN-1; \
            (_gx*MESH_GN + _gy)*MESH_GN + _gz; })

        MeshBucket* B = (MeshBucket*)calloc(MESH_NBUCKET, sizeof(MeshBucket));
        if (!B) continue;   // OOM → leave bucket_count 0, fall back to full draw

        // Pass A: count index entries per bucket (counts stored in index units).
        for (int t = 0; t < ntri; t++) {
            int a=ti[t*3], b=ti[t*3+1], c=ti[t*3+2];
            int cx=(mv[a*3]+mv[b*3]+mv[c*3])/3;
            int cy=(mv[a*3+1]+mv[b*3+1]+mv[c*3+1])/3;
            int cz=(mv[a*3+2]+mv[b*3+2]+mv[c*3+2])/3;
            B[CELL_OF(cx,cy,cz)].triCount += 3;
        }
        for (int q = 0; q < nquad; q++) {
            int a=qi[q*4], b=qi[q*4+1], c=qi[q*4+2], d=qi[q*4+3];
            int cx=(mv[a*3]+mv[b*3]+mv[c*3]+mv[d*3])/4;
            int cy=(mv[a*3+1]+mv[b*3+1]+mv[c*3+1]+mv[d*3+1])/4;
            int cz=(mv[a*3+2]+mv[b*3+2]+mv[c*3+2]+mv[d*3+2])/4;
            B[CELL_OF(cx,cy,cz)].quadCount += 4;
        }
        // Prefix sums → start offsets; reset counts to use as fill cursors.
        int triTotal=0, quadTotal=0;
        for (int c = 0; c < MESH_NBUCKET; c++) {
            B[c].triStart  = triTotal;  triTotal  += B[c].triCount;  B[c].triCount  = 0;
            B[c].quadStart = quadTotal; quadTotal += B[c].quadCount; B[c].quadCount = 0;
        }
        uint16_t* TI = triTotal  ? (uint16_t*)malloc(triTotal  * sizeof(uint16_t)) : 0;
        uint16_t* QI = quadTotal ? (uint16_t*)malloc(quadTotal * sizeof(uint16_t)) : 0;
        if ((triTotal && !TI) || (quadTotal && !QI)) { free(TI); free(QI); free(B); continue; }

        // Pass B: scatter indices into their buckets + accumulate bucket AABB.
        int bmnx[MESH_NBUCKET], bmny[MESH_NBUCKET], bmnz[MESH_NBUCKET];
        int bmxx[MESH_NBUCKET], bmxy[MESH_NBUCKET], bmxz[MESH_NBUCKET];
        for (int c = 0; c < MESH_NBUCKET; c++) {
            bmnx[c]=bmny[c]=bmnz[c]= 1<<30;
            bmxx[c]=bmxy[c]=bmxz[c]=-(1<<30);
        }
        #define ACC(cell,v) do { int _v=(v); \
            int _x=mv[_v*3],_y=mv[_v*3+1],_z=mv[_v*3+2]; \
            if(_x<bmnx[cell])bmnx[cell]=_x; if(_x>bmxx[cell])bmxx[cell]=_x; \
            if(_y<bmny[cell])bmny[cell]=_y; if(_y>bmxy[cell])bmxy[cell]=_y; \
            if(_z<bmnz[cell])bmnz[cell]=_z; if(_z>bmxz[cell])bmxz[cell]=_z; } while(0)
        for (int t = 0; t < ntri; t++) {
            int a=ti[t*3], b=ti[t*3+1], c=ti[t*3+2];
            int cx=(mv[a*3]+mv[b*3]+mv[c*3])/3;
            int cy=(mv[a*3+1]+mv[b*3+1]+mv[c*3+1])/3;
            int cz=(mv[a*3+2]+mv[b*3+2]+mv[c*3+2])/3;
            int cell=CELL_OF(cx,cy,cz);
            int p=B[cell].triStart + B[cell].triCount;
            TI[p]=a; TI[p+1]=b; TI[p+2]=c; B[cell].triCount += 3;
            ACC(cell,a); ACC(cell,b); ACC(cell,c);
        }
        for (int q = 0; q < nquad; q++) {
            int a=qi[q*4], b=qi[q*4+1], c=qi[q*4+2], d=qi[q*4+3];
            int cx=(mv[a*3]+mv[b*3]+mv[c*3]+mv[d*3])/4;
            int cy=(mv[a*3+1]+mv[b*3+1]+mv[c*3+1]+mv[d*3+1])/4;
            int cz=(mv[a*3+2]+mv[b*3+2]+mv[c*3+2]+mv[d*3+2])/4;
            int cell=CELL_OF(cx,cy,cz);
            int p=B[cell].quadStart + B[cell].quadCount;
            QI[p]=a; QI[p+1]=b; QI[p+2]=c; QI[p+3]=d; B[cell].quadCount += 4;
            ACC(cell,a); ACC(cell,b); ACC(cell,c); ACC(cell,d);
        }
        #undef ACC
        // Pass C: per-bucket center + radius from its AABB.
        for (int c = 0; c < MESH_NBUCKET; c++) {
            if (B[c].triCount==0 && B[c].quadCount==0) continue;
            int ccx=(bmnx[c]+bmxx[c])/2, ccy=(bmny[c]+bmxy[c])/2, ccz=(bmnz[c]+bmxz[c])/2;
            long long dx=bmxx[c]-ccx, dy=bmxy[c]-ccy, dz=bmxz[c]-ccz;
            B[c].cx=ccx; B[c].cy=ccy; B[c].cz=ccz;
            B[c].radius=(int)sqrtf((float)(dx*dx+dy*dy+dz*dz)) + 1;
        }
        #undef CELL_OF
        s_buckets[mi]=B; s_bucket_count[mi]=MESH_NBUCKET;
        s_tri_idx[mi]=TI; s_quad_idx[mi]=QI;
    }
}

static void render_meshes(void)
{
#if defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    static int s_buckets_ready = 0;
    if (!s_buckets_ready) { s_buckets_ready = 1; mesh_buckets_build(); }

    for (int si = 0; si < AFN_SPRITE_COUNT; si++)
    {
        int meshIdx = afn_sprite_data[si][9];
        if (meshIdx < 0 || meshIdx >= AFN_MESH_COUNT) continue;
        // visible == 0 → collision-only geometry; skip rendering (matches
        // the editor's "hidden" toggle that's already respected on GBA).
        if (!afn_mesh_desc[meshIdx][15]) continue;

        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];
        int spriteScale = afn_sprite_data[si][5];  // 256 = 1.0
        int rot  = afn_sprite_data[si][7];   // Y rotation (brad)
        int rotX = afn_sprite_data[si][17];  // X rotation (pitch, brad)
        int rotZ = afn_sprite_data[si][18];  // Z rotation (roll, brad)

        const int16_t* verts = afn_mesh_vert_ptrs[meshIdx];
        const uint16_t* idx  = afn_mesh_idx_ptrs[meshIdx];
        const int16_t* uvs   = afn_mesh_uv_ptrs[meshIdx];
#ifdef AFN_MESH_HAS_VCOL_PTRS
        const u8* vcol = afn_mesh_vcol_ptrs[meshIdx]; // OBJ 2.0 per-vertex colors (0 = none)
#else
        const u8* vcol = 0;
#endif

        int indexCount    = afn_mesh_desc[meshIdx][1];
        int quadIdxCount  = afn_mesh_desc[meshIdx][2];
        uint16_t color    = afn_mesh_desc[meshIdx][3];
        int cullMode      = afn_mesh_desc[meshIdx][4];
        int lit           = afn_mesh_desc[meshIdx][5];
        int textured      = afn_mesh_desc[meshIdx][8];
        int grayscale     = afn_mesh_desc[meshIdx][13];

        // Frustum + distance culling removed: every mesh is always submitted
        // regardless of camera position or the editor's draw distance. The DS
        // hardware still clips geometry outside the view volume.

        glPushMatrix();
        // Absolute world coords — gluLookAtf32 already applied the camera
        // transform.
        glTranslatef32(fx8_to_f32(wx),
                       fx8_to_f32(wy),
                       fx8_to_f32(wz));
        // Apply in Z, X, Y call order so the matrix is Rz*Rx*Ry — i.e. Y is
        // applied to the vertex first, then X, then Z, matching the editor's
        // scale→Y→X→Z composition (mode7_preview.cpp / OBJ export).
        // The exported value is a 16-bit brad (65536 = full circle). libnds
        // glRotate*i feeds the angle to sinLerp(s16), and DEGREES_IN_CIRCLE is
        // 32768 — so the correct angle is brad >> 1 (= degrees * 32768/360).
        // The old `<< 10` shifted the meaningful bits out of the s16 range and
        // aliased non-trivial angles (e.g. -88°) to ~0, leaving them unrotated.
        if (rotZ != 0)
            glRotateZi(rotZ >> 1);
        if (rotX != 0)
            glRotateXi(rotX >> 1);
        if (rot != 0)
            glRotateYi(rot >> 1);
        // Per-sprite scale (8.8 fixed, 256 = 1.0). gluPerspective + identity
        // fx8→v16 alone makes meshes microscopic — multiply by the sprite's
        // own scale field which the editor uses for the same purpose on GBA.
        // The extra << vshift undoes the per-mesh vertex downscale the exporter
        // applies to long meshes (afn_mesh_vshift) so they don't overflow s16.
        int vshift = afn_mesh_vshift[meshIdx];
        int s32 = (spriteScale << 4) << vshift;  // 8.8 → 20.12 f32, ×2^vshift
        glScalef32(s32, s32, s32);

        uint32_t polyFmt = POLY_ALPHA(31);
        if (cullMode == 0) polyFmt |= POLY_CULL_BACK;
        else if (cullMode == 1) polyFmt |= POLY_CULL_FRONT;
        else polyFmt |= POLY_CULL_NONE;
        // Vertex-colored meshes draw unlit so the per-vertex glColor shows
        // directly (DS lighting drives color from material regs, not glColor).
        if (lit && !vcol) polyFmt |= POLY_FORMAT_LIGHT0;
        glPolyFmt(polyFmt);

        int r, g, b;
        if (textured && gl_tex_ids[meshIdx]) {
            // Texture color is modulated by glColor; force white so the
            // texture comes through unmodified. Per-mesh color is ignored
            // for textured meshes (matches GBA behavior).
            r = g = b = 255;
            glBindTexture(0, gl_tex_ids[meshIdx]);
        } else {
            // Unbind any leftover texture from a previous mesh, otherwise the
            // texture lookup still happens with stale UVs/binding and the
            // mesh inherits a tint from whatever was last drawn.
            glBindTexture(0, 0);
            // Untextured meshes render WHITE by default — per-mesh RGB15 tints
            // were producing dark blues/greens because the editor stores its
            // "default" color as a dark hue and lighting on NDS makes it
            // worse. White lets the geometry's silhouette read clearly.
            r = g = b = 255;
            (void)color; (void)grayscale;
        }
        glColor3b(r, g, b);

        // Helper macro: emit (UV, vertex) for index i. UVs come from the
        // exporter in t16 format (.4 fixed texel units) so we pass directly.
        #define EMIT(i) do { \
            if (textured) glTexCoord2t16(uvs[(i)*2+0], uvs[(i)*2+1]); \
            if (vcol) glColor3b(vcol[(i)*3+0], vcol[(i)*3+1], vcol[(i)*3+2]); \
            glVertex3v16(fx8_to_v16(verts[(i)*3+0]), \
                         fx8_to_v16(verts[(i)*3+1]), \
                         fx8_to_v16(verts[(i)*3+2])); \
        } while (0)

        if (s_bucket_count[meshIdx] > 0)
        {
            // Frustum culling removed: submit every bucket's geometry. Buckets
            // still partition the mesh's triangles by centroid (the exporter's
            // index reordering lives in TI/QI), so iterating all of them draws
            // the whole mesh.
            const MeshBucket* B = s_buckets[meshIdx];
            const uint16_t* TI = s_tri_idx[meshIdx];
            const uint16_t* QI = s_quad_idx[meshIdx];
            int nb = s_bucket_count[meshIdx];

            if (TI) {
                glBegin(GL_TRIANGLES);
                for (int c = 0; c < nb; c++) {
                    int end = B[c].triStart + B[c].triCount;
                    for (int t = B[c].triStart; t < end; t += 3) {
                        EMIT(TI[t]); EMIT(TI[t+1]); EMIT(TI[t+2]);
                    }
                }
                glEnd();
            }
            if (QI) {
                glBegin(GL_QUADS);
                for (int c = 0; c < nb; c++) {
                    int end = B[c].quadStart + B[c].quadCount;
                    for (int q = B[c].quadStart; q < end; q += 4) {
                        EMIT(QI[q]); EMIT(QI[q+1]); EMIT(QI[q+2]); EMIT(QI[q+3]);
                    }
                }
                glEnd();
            }
        }
        else
        {
            // Fallback (bucket build OOM'd): draw the whole mesh unculled.
            if (indexCount > 0)
            {
                glBegin(GL_TRIANGLES);
                for (int t = 0; t + 3 <= indexCount; t += 3)
                {
                    EMIT(idx[t + 0]); EMIT(idx[t + 1]); EMIT(idx[t + 2]);
                }
                glEnd();
            }
            if (quadIdxCount > 0)
            {
                const uint16_t* qidx = afn_mesh_qidx_ptrs[meshIdx];
                glBegin(GL_QUADS);
                for (int q = 0; q + 4 <= quadIdxCount; q += 4)
                {
                    EMIT(qidx[q + 0]); EMIT(qidx[q + 1]);
                    EMIT(qidx[q + 2]); EMIT(qidx[q + 3]);
                }
                glEnd();
            }
        }
        #undef EMIT

        glPopMatrix(1);
    }
#endif
}
#endif

#ifdef AFN_HAS_PLAYER_RIG
// Player rendered as a DSMA skinned (rigged glTF) mesh — Mode 4. DSMA manages
// the bone matrix stack internally; we only set the modelview to the player's
// world transform. The animation frame advances each tick unless the player is
// frozen (node-driven, per the FreezePlayer node). Clip defaults to the editor
// selection; node-driven clip switching is a follow-up.
#ifndef AFN_PLAYER_RIG_SCALE_F32
#define AFN_PLAYER_RIG_SCALE_F32 64     // fallback (older exports): matches OBJ scale/64 sizing
#endif
#ifndef AFN_PLAYER_RIG_CULL
#define AFN_PLAYER_RIG_CULL 0           // 0 Back / 1 Front / 2 None
#endif
#define AFN_RIG_CULLFMT(c) ((c) == 1 ? POLY_CULL_FRONT : (c) == 2 ? POLY_CULL_NONE : POLY_CULL_BACK)
// Yaw correction so the model's authored forward aligns with the runtime heading.
//16384 = 90° in player_move_angle's 16-bit brad space; -16384 spins the player
// another 180° vs the importer's baked orientation (adjust if facing is wrong).
#define AFN_RIG_YAW_CORRECTION (-16384)
extern int afn_player_frozen;
extern int afn_rig_clip;                 // script-set skeletal clip (PlaySkelAnim node); -1 = leave default
extern int afn_skel_anim_obj;            // SetSkelAnim request: NPC sprite index (-1 = none)
extern int afn_skel_anim_clip;           // SetSkelAnim request: clip to set on that NPC
static int32_t s_rig_frame = 0;          // 20.12 fixed animation frame
static int     s_rig_clip  = AFN_PLAYER_RIG_DEFAULT_CLIP;

// One GL texture id per material group (0 = untextured group).
static int gl_rig_tex_id[AFN_PLAYER_RIG_MATCOUNT];
static void load_player_rig_texture(void)
{
    for (int g = 0; g < AFN_PLAYER_RIG_MATCOUNT; g++) {
        gl_rig_tex_id[g] = 0;
        if (!afn_player_rig_tex[g]) continue;
        int sizeW = 0, tw = afn_player_rig_texw[g]; while (tw > 8) { tw >>= 1; sizeW++; }
        int sizeH = 0, th = afn_player_rig_texh[g]; while (th > 8) { th >>= 1; sizeH++; }
        glGenTextures(1, &gl_rig_tex_id[g]);
        glBindTexture(0, gl_rig_tex_id[g]);
        // UV addressing per material group (editor rig material "Wrap" setting):
        //   0 Clip   = clamp (no WRAP flags) — edge texel, no atlas-overflow seams
        //   1 Extend = WRAP_S|WRAP_T (tile/repeat)
        //   2 Mirror = WRAP|FLIP (mirrored repeat)
        int texflags = TEXGEN_TEXCOORD;
        int rigwrap = afn_player_rig_texwrap[g];
        if (rigwrap == 1) texflags |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
        else if (rigwrap == 2) texflags |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T
                                         | GL_TEXTURE_FLIP_S | GL_TEXTURE_FLIP_T;
#ifdef AFN_PLAYER_RIG_ALPHA
        texflags |= GL_TEXTURE_COLOR0_TRANSPARENT;   // palette[0] = transparent
#endif
        glTexImage2D(0, 0, GL_RGB256, sizeW, sizeH, 0, texflags, afn_player_rig_tex[g]);
        glColorTableEXT(0, 0, 256, 0, 0, afn_player_rig_texpal[g]);
    }
}

static void render_player_rig(void)
{
    // A PlaySkelAnim node sets afn_rig_clip; switching clips restarts the frame.
    if (afn_rig_clip >= 0 && afn_rig_clip < AFN_PLAYER_RIG_CLIP_COUNT
        && afn_rig_clip != s_rig_clip) {
        s_rig_clip  = afn_rig_clip;
        s_rig_frame = 0;
    }
    int clip = s_rig_clip;
    if (clip < 0 || clip >= AFN_PLAYER_RIG_CLIP_COUNT) clip = 0;
    const u32* dsa = afn_player_rig_dsa[clip];

    int do_loop = 1;
#ifdef AFN_PLAYER_RIG_HAS_LOOP
    do_loop = afn_player_rig_loop[clip];
#endif
    uint32_t nframes = DSMA_GetNumFrames(dsa);
    if (!afn_player_frozen) {
        s_rig_frame += 1638;             // ~0.4 frame/tick = ~24 fps at 60 Hz
        if (nframes) {
            int32_t maxf = (int32_t)(nframes << 12);
            if (do_loop) {
                while (s_rig_frame >= maxf) s_rig_frame -= maxf;
            } else if (s_rig_frame > maxf - (1 << 12)) {
                s_rig_frame = maxf - (1 << 12);   // play once: hold last frame
            }
        }
    }

    // Ease the drawn Y so per-frame floor-height noise doesn't make the rig bob
    // climbing a slope (rig-only — player_y and sprites are untouched).
    // (Nebula snaps Y to the precise ground each frame and only smooths the
    // orientation — easing the position just adds lag/bob, so we draw at
    // player_render_y directly and smooth only the tilt below.)

    // --- Slope alignment: tilt the rig's up-axis to the floor it stands on,
    // then yaw (axis-angle via glRotatef32i — no matrix-layout guessing).
    // The target up eases toward the *standing triangle's* true geometric normal
    // (not a height-field finite difference, which sampled into adjacent vertical
    // wall faces and slanted the rig sideways near walls). When airborne — jumping
    // or walking off a slope edge — the target is straight up, so the rig rights
    // itself instead of staying slanted. Mirrors the PSP runtime (scene.c).
    static float s_upx = 0.0f, s_upy = 1.0f, s_upz = 0.0f;   // smoothed up vector
    float tnx = 0.0f, tny = 1.0f, tnz = 0.0f;                // target up (upright default)
    float upEase = 0.1f;                                     // airborne: ease back upright
#ifdef AFN_COL_FACE_COUNT
    if (player_on_ground) {
        extern int afn_floor_face;
        int fy;
        // One sample at the footing records the standing floor face; use that
        // triangle's normal (N = e1 x e2, forced +Y up) — robust against walls.
        if (afn_collide_floor(player_render_x, player_render_z, player_render_y, &fy)
            && afn_floor_face >= 0) {
            const CollFace* F = &afn_col_faces[afn_floor_face];
            float e1x = (float)(F->v1x - F->v0x), e1y = (float)(F->v1y - F->v0y), e1z = (float)(F->v1z - F->v0z);
            float e2x = (float)(F->v2x - F->v0x), e2y = (float)(F->v2y - F->v0y), e2z = (float)(F->v2z - F->v0z);
            float nx = e1y*e2z - e1z*e2y;
            float ny = e1z*e2x - e1x*e2z;
            float nz = e1x*e2y - e1y*e2x;
            if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
            float l = sqrtf(nx*nx + ny*ny + nz*nz);
            if (l > 0.0001f) { tnx = nx/l; tny = ny/l; tnz = nz/l; upEase = 0.2f; }
        }
    }
#endif
    // Ease the up-vector toward the target (snappier when grounded), renormalize.
    s_upx += (tnx - s_upx) * upEase;
    s_upy += (tny - s_upy) * upEase;
    s_upz += (tnz - s_upz) * upEase;
    { float l = sqrtf(s_upx*s_upx + s_upy*s_upy + s_upz*s_upz);
      if (l > 0.0001f) { s_upx/=l; s_upy/=l; s_upz/=l; } else { s_upx=0; s_upy=1; s_upz=0; } }

    uint16_t rig_face = player_moving
        ? (uint16_t)(player_move_angle + (orbit_angle << 1))
        : player_move_angle;
    // Tank controls: the rig always faces its heading (turned by Turn Player),
    // independent of the free camera. 0x4000 + heading<<1 matches the encoding
    // the formula above uses so movement direction and facing line up.
    if (afn_tank_camera)
        rig_face = (uint16_t)(0x4000 + ((uint16_t)afn_player_heading << 1));

    glPushMatrix();
    glTranslatef32(fx8_to_f32(player_render_x),
                   fx8_to_f32(player_render_y),
                   fx8_to_f32(player_render_z));
    // Tilt the up-axis to the floor normal: rotate (0,1,0)->N about axis
    // (0,1,0)xN = (Nz,0,-Nx) by the angle between them. Applied before the yaw
    // (world space) so the yawed model lays onto the slope.
    float horiz = sqrtf(s_upx*s_upx + s_upz*s_upz);
    if (horiz > 0.0001f) {
        int tiltDS = (int)(atan2f(horiz, s_upy) * (32768.0f / 6.28318531f));  // 32768 = full circle
        glRotatef32i(tiltDS, floattof32(s_upz), 0, floattof32(-s_upx));
    }
    glRotateYi(rig_face >> 1);                        // face movement heading
    glRotateYi(AFN_RIG_YAW_CORRECTION >> 1);          // align model forward to heading
    int s32 = AFN_PLAYER_RIG_SCALE_F32;
    glScalef32(s32, s32, s32);

    glPolyFmt(POLY_ALPHA(31) | AFN_RIG_CULLFMT(AFN_PLAYER_RIG_CULL) | POLY_FORMAT_LIGHT0);
    glColor3b(255, 255, 255);
#ifdef AFN_PLAYER_RIG_CAMLIGHT
#ifndef AFN_PLAYER_RIG_LIGHT_DX
#define AFN_PLAYER_RIG_LIGHT_DX (0.0f)
#define AFN_PLAYER_RIG_LIGHT_DY (0.0f)
#define AFN_PLAYER_RIG_LIGHT_DZ (-0.99f)
#endif
    // Headlamp from the camera (Light X/Y aim baked into the direction), set in
    // eye space (identity modelview) so the rig is always lit from the viewer.
    glPushMatrix();
    glLoadIdentity();
    glLight(0, RGB15(31, 31, 31),
            floattov10(AFN_PLAYER_RIG_LIGHT_DX),
            floattov10(AFN_PLAYER_RIG_LIGHT_DY),
            floattov10(AFN_PLAYER_RIG_LIGHT_DZ));
    glPopMatrix(1);
#endif

    // One draw per material group, binding that group's texture (0 = flat).
    for (int g = 0; g < AFN_PLAYER_RIG_MATCOUNT; g++) {
        glBindTexture(0, gl_rig_tex_id[g]);
        DSMA_DrawModel(afn_player_rig_dsm[g], dsa, s_rig_frame);
    }
    glPopMatrix(1);
#ifdef AFN_PLAYER_RIG_CAMLIGHT
    // Restore the scene's default directional light for the next frame's meshes.
    glPushMatrix();
    glLoadIdentity();
    glLight(0, RGB15(31, 31, 31), floattov10(-0.5f), floattov10(-0.7f), floattov10(-0.5f));
    glPopMatrix(1);
#endif
}
#endif

// Gravity / terminal-fall fallbacks (used by NPC physics below and the player
// update further down). Defined here so they precede the first use.
#ifndef AFN_GRAVITY
#define AFN_GRAVITY 23
#endif
#ifndef AFN_TERMINAL_VEL
#define AFN_TERMINAL_VEL 1536
#endif

#ifdef AFN_HAS_NPC_RIGS
// Rigged (DSMA skinned) NPC instances. Each instance references a rig asset
// blob (afn_npc_dsm/dsa/loop/tex...) and reads its world transform from the
// matching afn_sprite_data row, so it sits exactly where the sprite is placed.
// Per-instance s_npc_clip / s_npc_frame hold playback state; a SetSkelAnim node
// (afn_skel_anim_obj/clip) switches the clip for the NPC with a matching sprite
// index. Up to AFN_NPC_RIG_COUNT instances (editor caps this at 4).
#define AFN_NPC_MAX_GROUPS 8
static int32_t s_npc_frame[AFN_NPC_RIG_COUNT];
static int     s_npc_clip[AFN_NPC_RIG_COUNT];
static int     s_npc_tex_id[AFN_NPC_RIG_COUNT][AFN_NPC_MAX_GROUPS];
static int     s_npc_y[AFN_NPC_RIG_COUNT];        // 16.8 dynamic Y (gravity-driven)
static int     s_npc_vy[AFN_NPC_RIG_COUNT];       // 16.8 vertical velocity
static int     s_npc_on_ground[AFN_NPC_RIG_COUNT];
static int     s_npc_inited = 0;

static void load_npc_rig_textures(void)
{
    for (int i = 0; i < AFN_NPC_RIG_COUNT; i++) {
        s_npc_frame[i] = 0;
        s_npc_clip[i]  = afn_npc_defclip[i];
        // Seed physics from the placed Y; gravity settles it onto the floor.
        s_npc_y[i]  = afn_sprite_data[afn_npc_sprite[i]][1];
        s_npc_vy[i] = 0;
        s_npc_on_ground[i] = 0;
        int ng = afn_npc_matcount[i];
        for (int g = 0; g < ng && g < AFN_NPC_MAX_GROUPS; g++) {
            s_npc_tex_id[i][g] = 0;
            if (!afn_npc_tex[i][g]) continue;
            int sizeW = 0, tw = afn_npc_texw[i][g]; while (tw > 8) { tw >>= 1; sizeW++; }
            int sizeH = 0, th = afn_npc_texh[i][g]; while (th > 8) { th >>= 1; sizeH++; }
            glGenTextures(1, &s_npc_tex_id[i][g]);
            glBindTexture(0, s_npc_tex_id[i][g]);
            // UV addressing per group: 0 Clip=clamp, 1 Extend=tile, 2 Mirror.
            int npcflags = TEXGEN_TEXCOORD;
            int npcwrap = afn_npc_texwrap[i][g];
            if (npcwrap == 1) npcflags |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
            else if (npcwrap == 2) npcflags |= GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T
                                             | GL_TEXTURE_FLIP_S | GL_TEXTURE_FLIP_T;
            if (afn_npc_alpha[i]) npcflags |= GL_TEXTURE_COLOR0_TRANSPARENT;
            glTexImage2D(0, 0, GL_RGB256, sizeW, sizeH, 0, npcflags, afn_npc_tex[i][g]);
            glColorTableEXT(0, 0, 256, 0, 0, afn_npc_texpal[i][g]);
        }
    }
    s_npc_inited = 1;
}

static void render_npc_rigs(void)
{
    if (!s_npc_inited) load_npc_rig_textures();

    // Apply a pending SetSkelAnim request to the matching NPC, then consume it.
    if (afn_skel_anim_obj >= 0) {
        for (int i = 0; i < AFN_NPC_RIG_COUNT; i++) {
            if (afn_npc_sprite[i] == afn_skel_anim_obj && s_npc_clip[i] != afn_skel_anim_clip) {
                s_npc_clip[i]  = afn_skel_anim_clip;
                s_npc_frame[i] = 0;
            }
        }
        afn_skel_anim_obj = -1;
    }

    for (int i = 0; i < AFN_NPC_RIG_COUNT; i++) {
        int si   = afn_npc_sprite[i];
        int clip = s_npc_clip[i];
        if (clip < 0 || clip >= afn_npc_clipcount[i]) clip = 0;
        const u32* dsa = afn_npc_dsa[i][clip];

        int do_loop = afn_npc_loop[i][clip];
        uint32_t nframes = DSMA_GetNumFrames(dsa);
        s_npc_frame[i] += 1638;             // ~24 fps, matches the player rig
        if (nframes) {
            int32_t maxf = (int32_t)(nframes << 12);
            if (do_loop) { while (s_npc_frame[i] >= maxf) s_npc_frame[i] -= maxf; }
            else if (s_npc_frame[i] > maxf - (1 << 12)) s_npc_frame[i] = maxf - (1 << 12);
        }

        // World transform from the sprite's row (same as mesh sprites).
        int wx = afn_sprite_data[si][0];
        int wy = afn_sprite_data[si][1];
        int wz = afn_sprite_data[si][2];
        int spriteScale = afn_sprite_data[si][5];   // 256 = 1.0
        int rot = afn_sprite_data[si][7];            // Y rotation (brad)
#ifdef AFN_COL_FACE_COUNT
        // Per-NPC gravity + floor landing: enemies fall, settle on the ground,
        // and can be knocked airborne (set s_npc_vy[i] > 0 to launch one). Same
        // gravity/terminal constants as the player; the rig origin rests at the
        // floor surface. With no launch impulse an NPC just sits on the floor.
        int npcG    = afn_gravity      ? afn_gravity      : AFN_GRAVITY;
        int npcTerm = afn_terminal_vel ? afn_terminal_vel : AFN_TERMINAL_VEL;
        s_npc_vy[i] -= npcG;
        if (s_npc_vy[i] < -npcTerm) s_npc_vy[i] = -npcTerm;
        s_npc_y[i] += s_npc_vy[i];
        int npcFloorY;
        if (afn_collide_floor(wx, wz, s_npc_y[i] + afn_player_height, &npcFloorY)
            && s_npc_y[i] <= npcFloorY) {
            s_npc_y[i] = npcFloorY; s_npc_vy[i] = 0; s_npc_on_ground[i] = 1;
        } else {
            s_npc_on_ground[i] = 0;
        }
        wy = s_npc_y[i];
#endif

        glPushMatrix();
        glTranslatef32(fx8_to_f32(wx), fx8_to_f32(wy), fx8_to_f32(wz));
        // NPCs face their placed sprite rotation, exactly like mesh sprites — NO
        // AFN_RIG_YAW_CORRECTION. That -90° only exists to cancel the +0x4000
        // baseline baked into the player's player_move_angle; the NPC's `rot` is
        // the raw placement brad (no baseline), so applying the correction would
        // spin it 90° off its authored/editor orientation.
        if (rot != 0) glRotateYi(rot >> 1);
        int s32 = spriteScale >> 2;                  // 8.8 (256=1.0) -> scale*64 f32
        glScalef32(s32, s32, s32);

        glPolyFmt(POLY_ALPHA(31) | AFN_RIG_CULLFMT(afn_npc_cull[i]) | POLY_FORMAT_LIGHT0);
        glColor3b(255, 255, 255);
        // Per-NPC lighting, set in eye space (identity modelview) like the player
        // rig: a camera-light NPC uses its baked Light X/Y headlamp; otherwise the
        // scene's default directional light. Set explicitly each NPC so order/
        // a previous NPC's headlamp can't bleed into this one.
        glPushMatrix();
        glLoadIdentity();
        if (afn_npc_camlight[i])
            glLight(0, RGB15(31, 31, 31), afn_npc_lightdx[i], afn_npc_lightdy[i], afn_npc_lightdz[i]);
        else
            glLight(0, RGB15(31, 31, 31), floattov10(-0.5f), floattov10(-0.7f), floattov10(-0.5f));
        glPopMatrix(1);
        // One draw per material group, binding that group's texture (0 = flat).
        int ng = afn_npc_matcount[i];
        for (int g = 0; g < ng && g < AFN_NPC_MAX_GROUPS; g++) {
            glBindTexture(0, s_npc_tex_id[i][g]);
            DSMA_DrawModel(afn_npc_dsm[i][g], dsa, s_npc_frame[i]);
        }
        glPopMatrix(1);
    }
    // Restore the scene's default directional light for next frame's meshes.
    glPushMatrix();
    glLoadIdentity();
    glLight(0, RGB15(31, 31, 31), floattov10(-0.5f), floattov10(-0.7f), floattov10(-0.5f));
    glPopMatrix(1);
}
#endif

// ---------------------------------------------------------------------------
// Input + camera tick (WASD = D-pad: W/S = walk fwd/back, A/D = turn)
// Integer square root (for normalizing the rail-grind direction vector).
static int afn_isqrt(int n) {
    if (n <= 0) return 0;
    int x = n, r = 0;
    int b = 1 << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

#ifdef AFN_HAS_RAIL_PATH
// 64-bit integer sqrt. Rail points are 16.8 fixed WORLD coords (~100k+), so a
// squared segment length (dx*dx+dz*dz) easily exceeds 2^31 — and on ARM/NDS
// `long` is 32-bit, so the only safe accumulator is `long long`. Using the
// 32-bit afn_isqrt() here overflowed and fed garbage lengths into the grind
// loop, which froze the runtime the moment you stepped onto a rail.
static int afn_isqrt_ll(long long n) {
    if (n <= 0) return 0;
    long long x = n, r = 0, b = 1LL << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}
// Per-rail-point cumulative arc length, precomputed ONCE. The path functions
// below used to afn_isqrt_ll() every segment every call — with long rails that
// was the grind-catch lag (the magnet runs every frame near a rail). With the
// cache a segment length is a subtraction. NULL until built / on OOM, in which
// case afn_seg_len() falls back to the live sqrt so results stay correct.
static int* s_railCum = 0;
static int  s_railArcReady = 0;
static void afn_rail_arc_build(void) {
    s_railArcReady = 1;
    int total = 0;
    for (int r = 0; r < AFN_SPRITE_COUNT; r++) {
        int e = afn_rail_start[r] + afn_rail_count[r];
        if (e > total) total = e;
    }
    if (total <= 0) return;
    s_railCum = (int*)malloc(sizeof(int) * (unsigned)total);
    if (!s_railCum) return;
    for (int rail = 0; rail < AFN_SPRITE_COUNT; rail++) {
        int n = afn_rail_count[rail], start = afn_rail_start[rail];
        if (n < 1) continue;
        long long cum = 0;
        s_railCum[start] = 0;
        for (int i = 1; i < n; i++) {
            long long dx = afn_rail_pts[start+i][0]-afn_rail_pts[start+i-1][0];
            long long dz = afn_rail_pts[start+i][2]-afn_rail_pts[start+i-1][2];
            cum += afn_isqrt_ll(dx*dx+dz*dz);
            s_railCum[start+i] = (cum > 0x7fffffffLL) ? 0x7fffffff : (int)cum;
        }
    }
}
static inline int afn_seg_len(int start, int i) {
    if (s_railCum) return s_railCum[start+i] - s_railCum[start+i-1];
    long long dx = afn_rail_pts[start+i][0]-afn_rail_pts[start+i-1][0];
    long long dz = afn_rail_pts[start+i][2]-afn_rail_pts[start+i-1][2];
    return afn_isqrt_ll(dx*dx+dz*dz);
}
// Hand-authored grind rail path (exported per rail sprite). Deterministic slide
// along the exported centerline so thin/curved rails grind cleanly (no probe).
static int afn_railpath_len(int rail) {
    if (rail < 0 || rail >= AFN_SPRITE_COUNT) return 0;
    int n = afn_rail_count[rail]; if (n < 2) return 0;
    if (!s_railArcReady) afn_rail_arc_build();
    int start = afn_rail_start[rail];
    if (s_railCum) return s_railCum[start + n-1];
    long long total = 0;
    for (int i = 1; i < n; i++) total += afn_seg_len(start, i);
    if (total > 0x7fffffffLL) total = 0x7fffffffLL;
    return (int)total;
}
// Set at engage from afn_rail_spline[rail]: 1 = sample along a smooth Catmull-Rom
// curve through the points (rounded corners); 0 = straight segments.
static int s_railSplineOn = 0;
// Catmull-Rom position for one axis. tf/t2/t3 = t^1/t^2/t^3 in .8 fixed (0..256).
static inline int afn_catmull(int P0,int P1,int P2,int P3,long long tf,long long t2,long long t3){
    long long a1=(long long)P2-P0;
    long long a2=2LL*P0-5LL*P1+4LL*P2-P3;
    long long a3=-(long long)P0+3LL*P1-3LL*P2+P3;
    long long term=a1*tf + a2*t2 + a3*t3;   // 256 * (a1 t + a2 t^2 + a3 t^3)
    return (int)(P1 + (term>>9));            // >>8 undo .8, >>1 for the 0.5
}
static inline long long afn_catmull_tan(int P0,int P1,int P2,int P3,long long tf,long long t2){
    long long a1=(long long)P2-P0;
    long long a2=2LL*P0-5LL*P1+4LL*P2-P3;
    long long a3=-(long long)P0+3LL*P1-3LL*P2+P3;
    return a1 + ((2*a2*tf)>>8) + ((3*a3*t2)>>8);
}
static void afn_railpath_sample(int rail, int s, int* ox, int* oy, int* oz, int* tdx, int* tdz) {
    int n = afn_rail_count[rail]; int start = afn_rail_start[rail];
    if (n < 2) { *ox=*oy=*oz=0; *tdx=256; *tdz=0; return; }
    if (!s_railArcReady) afn_rail_arc_build();
    if (s < 0) s = 0; int acc = 0;
    for (int i = 1; i < n; i++) {
        int ax=afn_rail_pts[start+i-1][0],ay=afn_rail_pts[start+i-1][1],az=afn_rail_pts[start+i-1][2];
        int bx=afn_rail_pts[start+i][0],by=afn_rail_pts[start+i][1],bz=afn_rail_pts[start+i][2];
        long long dx=bx-ax, dz=bz-az; int seg=afn_seg_len(start,i); if(seg<1)seg=1;
        if (s <= acc+seg || i==n-1) {
            int t=(int)(((long long)(s-acc)<<8)/seg); if(t<0)t=0; if(t>256)t=256;
            if (s_railSplineOn && n >= 2) {
                // Smooth Catmull-Rom through P1=pt[i-1], P2=pt[i]; neighbours clamped at ends.
                int i0 = (i-2 >= 0) ? i-2 : i-1;
                int i3 = (i+1 < n)  ? i+1 : i;
                int P0x=afn_rail_pts[start+i0][0], P3x=afn_rail_pts[start+i3][0];
                int P0y=afn_rail_pts[start+i0][1], P3y=afn_rail_pts[start+i3][1];
                int P0z=afn_rail_pts[start+i0][2], P3z=afn_rail_pts[start+i3][2];
                long long tf=t, t2=(tf*tf)>>8, t3=(t2*tf)>>8;
                *ox = afn_catmull(P0x, ax, bx, P3x, tf,t2,t3);
                *oy = afn_catmull(P0y, ay, by, P3y, tf,t2,t3);
                *oz = afn_catmull(P0z, az, bz, P3z, tf,t2,t3);
                long long dtx = afn_catmull_tan(P0x, ax, bx, P3x, tf,t2);
                long long dtz = afn_catmull_tan(P0z, az, bz, P3z, tf,t2);
                int mag=afn_isqrt_ll(dtx*dtx+dtz*dtz); if(mag<1)mag=1;
                *tdx=(int)((dtx*256)/mag); *tdz=(int)((dtz*256)/mag);
                return;
            }
            *ox=ax+(int)(((bx-ax)*(long long)t)>>8); *oy=ay+(int)(((by-ay)*(long long)t)>>8); *oz=az+(int)(((bz-az)*(long long)t)>>8);
            int l=afn_isqrt_ll(dx*dx+dz*dz); if(l<1)l=1; *tdx=(int)((dx*256)/l); *tdz=(int)((dz*256)/l); return;
        }
        acc+=seg;
    }
}
// Returns the arc position of the closest point on the path. Also outputs the
// nearest authored POINT index (closer endpoint of the winning segment) and its
// squared XZ distance when the pointers are non-NULL — so callers needing those
// (the width-catch End check, range early-out) don't have to re-loop the path.
static int afn_railpath_nearest_ex(int rail, int px, int pz, int* outPt, long long* outD2) {
    int n=afn_rail_count[rail]; int start=afn_rail_start[rail];
    if(n<2){ if(outPt)*outPt=0; if(outD2)*outD2=0; return 0; }
    if (!s_railArcReady) afn_rail_arc_build();
    long long bestD=0x7fffffffffffffffLL; int bestArc=0, acc=0, bestPt=0;
    for(int i=1;i<n;i++){
        int ax=afn_rail_pts[start+i-1][0],az=afn_rail_pts[start+i-1][2];
        int bx=afn_rail_pts[start+i][0],bz=afn_rail_pts[start+i][2];
        long long ex=bx-ax,ez=bz-az; long long el2=ex*ex+ez*ez; if(el2<1)el2=1;
        long long tn=(long long)(px-ax)*ex+(long long)(pz-az)*ez; int t=(int)((tn<<8)/el2);
        if(t<0)t=0; if(t>256)t=256;
        int cx=ax+(int)((ex*t)>>8),cz=az+(int)((ez*t)>>8);
        long long dd=(long long)(px-cx)*(px-cx)+(long long)(pz-cz)*(pz-cz);
        int seg=afn_seg_len(start,i);
        if(dd<bestD){bestD=dd; bestArc=acc+(int)(((long long)seg*t)>>8); bestPt=(t<128)?(i-1):i;}
        acc+=seg;
    }
    if(outPt)*outPt=bestPt; if(outD2)*outD2=bestD;
    return bestArc;
}
static int afn_railpath_nearest(int rail, int px, int pz) {
    return afn_railpath_nearest_ex(rail, px, pz, 0, 0);
}
#endif

// ---------------------------------------------------------------------------
// Movement/jump state. Walk speed ramps with AFN_WALK_EASE_IN/OUT toward
// the target (walk or sprint). player_y is a vertical offset added to the
// base cam_h — jump impulse pushes it up, gravity pulls it back, cam_h
// follows with a separate smoothing delay (land vs air).
static int s_moveSpeed;             // current smoothed speed
// player_vy lives in script_glue.c when scripts are on (so scripts can
// read/write it); local fallback otherwise.
#ifndef AFN_HAS_SCRIPT
int player_vy;
#endif
static int s_playerY;               // 16.8 vertical offset from ground
static int s_camYSmooth;            // smoothed cam_h follow of s_playerY
static int s_groundSnapTol;         // per-frame ground-snap distance (downhill slopes)
// player_on_ground is the script-side global (defined in script_glue.c when
// AFN_HAS_SCRIPT, else here as a fallback so fps3d.c always has it).
#ifndef AFN_HAS_SCRIPT
int player_on_ground = 1;
#endif

#ifndef AFN_WALK_SPEED
#define AFN_WALK_SPEED 37
#endif
#ifndef AFN_SPRINT_SPEED
#define AFN_SPRINT_SPEED 56
#endif
#ifndef AFN_WALK_EASE_IN
#define AFN_WALK_EASE_IN 25
#endif
#ifndef AFN_WALK_EASE_OUT
#define AFN_WALK_EASE_OUT 25
#endif
#ifndef AFN_SPRINT_EASE_IN
#define AFN_SPRINT_EASE_IN 15
#endif
#ifndef AFN_SPRINT_EASE_OUT
#define AFN_SPRINT_EASE_OUT 10
#endif
#ifndef AFN_JUMP_VEL
#define AFN_JUMP_VEL 512
#endif
#ifndef AFN_GRAVITY
#define AFN_GRAVITY 23
#endif
#ifndef AFN_TERMINAL_VEL
#define AFN_TERMINAL_VEL 1536
#endif
#ifndef AFN_JUMP_CAM_LAND
#define AFN_JUMP_CAM_LAND 94
#endif
#ifndef AFN_JUMP_CAM_AIR
#define AFN_JUMP_CAM_AIR 30
#endif

#ifdef AFN_CAM_SLOT_COUNT
// Player camera presets (Mode 4). afn_active_camera (set by a SetCamera node) is
// 0 = scene default or 1..N = a slot in afn_cam_slots. We don't hard-cut: a blend
// weight ramps in/out and the slot target itself eases, so default<->slot and
// slot<->slot transitions all glide (~0.3s). At weight 0 the live default camera
// is used unchanged, so normal play feel is preserved.
static int s_camW = 0;                 // 0..256 blend weight
static int s_slotAngle, s_slotDist, s_slotHeightOff, s_slotHorizon;  // eased targets
static int s_camSlotInit = 0;
static void update_camera_slot(void)
{
    int active = afn_active_camera;
    if (active < 0 || active >= AFN_CAM_SLOT_COUNT) active = 0;
    int wt = (active > 0) ? 256 : 0;
    s_camW += (wt - s_camW) >> 3;
    if (wt == 0 && s_camW < 2) s_camW = 0;
    if (wt == 256 && s_camW > 254) s_camW = 256;
    const int* S = afn_cam_slots[active];
    if (!s_camSlotInit) {
        s_slotAngle = S[0]; s_slotDist = S[1]; s_slotHeightOff = S[2]; s_slotHorizon = S[3];
        s_camSlotInit = 1;
    }
    s_slotAngle     += (int)(int16_t)(S[0] - s_slotAngle) >> 3;   // brad, wrap-safe
    s_slotDist      += (S[1] - s_slotDist) >> 3;
    s_slotHeightOff += (S[2] - s_slotHeightOff) >> 3;
    s_slotHorizon   += (S[3] - s_slotHorizon) >> 3;
}
// Blend a default value toward the slot value by the ramp weight.
static inline int cam_blend(int def, int slot)     { return def + (((slot - def) * s_camW) >> 8); }
static inline int cam_blend_ang(int def, int slot) { return def + (((int)(int16_t)(slot - def) * s_camW) >> 8); }
#endif

static void update_camera(void)
{
    scanKeys();
    uint32_t held = keysHeld();
    uint32_t down = keysDown();
    {
        static int s_dbgFrame = 0;
        s_dbgFrame++;
        if ((s_dbgFrame & 15) == 0) {
            iprintf("\x1b[10;0Hkeys=%04x cx=%d cz=%d ca=%d   ",
                    (unsigned)held, cam_x >> FX_SHIFT, cam_z >> FX_SHIFT, (int)cam_angle);
        }
    }

#ifndef AFN_HAS_SCRIPT
    // Debug free-cam controls — only when no scripts. Once nodes exist,
    // OrbitCamera / Jump / etc. are the only thing that touches camera state.
    if (held & KEY_L) cam_h += AFN_WALK_SPEED;
    if (held & KEY_R) cam_h -= AFN_WALK_SPEED;
    if (held & KEY_LEFT)  orbit_angle += 512;
    if (held & KEY_RIGHT) orbit_angle -= 512;
#endif

#ifdef AFN_CAM_SLOT_COUNT
    // Camera slot. Distance/height/pitch are held at the slot while it's active
    // (blended below — they don't fight orbit input). Yaw is a ONE-SHOT: when the
    // active slot changes to a preset, we ease orbit_angle to that slot's yaw,
    // then release so the player can orbit freely from the new angle. Because we
    // move orbit_angle itself (the single camera+control angle), movement stays
    // camera-relative and the rig faces correctly during the swing — no spin.
    update_camera_slot();
    {
        static int s_prevCam = 0, s_yawEasing = 0;
        int active = afn_active_camera;
        if (active < 0 || active >= AFN_CAM_SLOT_COUNT) active = 0;
        if (active != s_prevCam) { s_prevCam = active; s_yawEasing = (active > 0); }
        if (s_yawEasing) {
            int d = (int)(int16_t)(afn_cam_slots[active][0] - orbit_angle);
            orbit_angle = (uint16_t)(orbit_angle + (d >> 3));
            if (d > -300 && d < 300) { orbit_angle = (uint16_t)afn_cam_slots[active][0]; s_yawEasing = 0; }
        }
    }
#endif

#ifdef AFN_ORBIT_MAX_DELTA
    // Clamp this frame's orbit_angle change so the camera lerp can keep
    // up and the player sprite stays centered. OrbitCamera scripts have
    // already applied their requested delta; we re-anchor to the prev
    // value and let through at most ±AFN_ORBIT_MAX_DELTA brads.
    {
        static uint16_t s_prev_orbit_angle = 0xFFFF;
        if (s_prev_orbit_angle == 0xFFFF) s_prev_orbit_angle = orbit_angle;
        int delta = (int16_t)(orbit_angle - s_prev_orbit_angle);
        if (delta >  AFN_ORBIT_MAX_DELTA) delta =  AFN_ORBIT_MAX_DELTA;
        if (delta < -AFN_ORBIT_MAX_DELTA) delta = -AFN_ORBIT_MAX_DELTA;
        orbit_angle = (uint16_t)(s_prev_orbit_angle + delta);
        s_prev_orbit_angle = orbit_angle;
    }
#endif
    // GBA writes cam_angle = orbit_angle once per frame (main.c:7946) — that's
    // how OrbitCamera scripts (which modify orbit_angle) actually propagate
    // to camera-direction-dependent code paths. Without this, manual orbit
    // changes orbit_angle but cam_angle / g_sinf / g_cosf stay stale.
    cam_angle = orbit_angle;
    g_cosf = brad_cos(cam_angle);
    g_sinf = brad_sin(cam_angle);

#ifdef AFN_HAS_SCRIPT
    // Script-driven path: MovePlayer nodes set afn_input_fwd/right, Walk/
    // Sprint nodes set afn_move_speed. Map view-space input → world XZ via
    // the camera basis (forward = (sin,cos), right = (cos,-sin) for our
    // gluLookAt convention). script_tick ran before fps3d_update so the
    // values are fresh.
    // Rail grinding: when afn_grinding, the player is locked to the rail
    // direction and slides on momentum instead of taking input movement.
    extern int afn_grinding, afn_grind_dx, afn_grind_dz, afn_grind_vel;
    extern int afn_grinding_active;
    extern int afn_grind_catch_y, afn_grind_catch_x; // GrindCatch node: re-catch window
    extern int afn_grind_power, afn_grind_boost; // GrindPower / GrindBoost nodes
    extern int afn_grind_bleed;                  // GrindBleed node: cap-bonus decay shift
    // Clamp to a safe shift range (0 = never bleeds, default 6). Done once so
    // both grind paths below share it without re-validating in the hot loop.
    int grindBleed = afn_grind_bleed; if (grindBleed < 0) grindBleed = 0; else if (grindBleed > 16) grindBleed = 16;
    extern int afn_player_vx_world, afn_player_vz_world;
    int fwd = afn_input_fwd, right = afn_input_right;
    if (fwd && right) { fwd = (fwd * 181) >> 8; right = (right * 181) >> 8; }
    int spd = afn_move_speed;
    // Tank controls: move relative to the player heading (turned by D-pad via a
    // Turn Player node), not the camera, so the camera can orbit independently.
    int bsin = g_sinf, bcos = g_cosf;
    if (afn_tank_camera) { bsin = brad_sin((uint16_t)afn_player_heading); bcos = brad_cos((uint16_t)afn_player_heading); }
    int dx = ((bsin * fwd + bcos * right) >> 8);
    int dz = ((bcos * fwd - bsin * right) >> 8);
    extern int afn_grind_rail;
    int mvX, mvZ;
    if (afn_grind_vel != 0) {
        // Engaged grind: locked to the rail axis, sliding on momentum. Player
        // input is FROZEN here (dx/dz ignored) — only the jump (handled below)
        // gets you off. Engage + direction seeding happen in the floor block,
        // once we confirm the player is actually standing on the rail's floor.
        mvX = FX_MUL(afn_grind_dx, afn_grind_vel);
        mvZ = FX_MUL(afn_grind_dz, afn_grind_vel);
    } else {
        mvX = FX_MUL(dx, spd);
        mvZ = FX_MUL(dz, spd);
    }
    player_x += mvX;
    player_z += mvZ;
    // Horizontal distance moved this frame (Manhattan) — used as the
    // ground-snap tolerance so walking DOWN a slope keeps the player glued
    // to the floor instead of float-then-landing each frame (which flickered
    // Is Jumping / Is Falling and stuttered the anim). A 45° slope drops ~one
    // horizontal step per frame, so this tolerance covers it; a real ledge
    // drop is far larger than one frame's move, so we won't snap off cliffs.
    s_groundSnapTol = (mvX < 0 ? -mvX : mvX) + (mvZ < 0 ? -mvZ : mvZ);
    s_groundSnapTol += s_groundSnapTol >> 1;  // ~1.5x margin
#ifdef AFN_HAS_SCRIPT
    // Forward decls in case the loaded mapdata.h was generated before
    // these externs were emitted. Real defs live in script_glue.c.
    extern int afn_player_vx_world;
    extern int afn_player_vz_world;
    extern int afn_velocity_falloff;
    extern int afn_pending_boost_fwd;
    // BoostForward(speed) wrote a pending magnitude — decompose it here
    // using the *current* view angle, then clear. Keeps emitted script
    // code view-agnostic.
    if (afn_pending_boost_fwd) {
        afn_player_vx_world = FX_MUL(g_sinf, afn_pending_boost_fwd);
        afn_player_vz_world = FX_MUL(g_cosf, afn_pending_boost_fwd);
        afn_pending_boost_fwd = 0;
    }
    // Node-driven world-axis push velocity (boost pads / knockback).
    // SetVelocityX/Z write the globals; VelocityFalloff(N) linearly ramps
    // them to 0 over N frames (vx -= vx/N as N decrements gives true linear).
    player_x += afn_player_vx_world;
    player_z += afn_player_vz_world;
    if (afn_velocity_falloff > 0) {
        afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
        afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
        if (--afn_velocity_falloff == 0) {
            afn_player_vx_world = 0;
            afn_player_vz_world = 0;
        }
    }
#endif
    player_moving = (fwd != 0 || right != 0);
    if (player_moving) { s_lastMoveDX = dx; s_lastMoveDZ = dz; }
    // GBA-style player facing tracking. player_move_angle is INPUT-space
    // while moving (atan2 of dpad input), then converted to world-space
    // at the moment of stopping. orbit_angle stays fixed at AFN_CAM_ANGLE
    // (matches GBA — manual L/R orbit modifies cam_angle, not orbit_angle).
    if (player_moving) {
        // Brad ArcTan2(inputRight=x, inputFwd=y). GBA convention: angle
        // measured from +X axis, so atan2(0,+y) = 0x4000 (DPAD-up = N
        // brad). Picker formula then maps brad 0x4000 → dir 0 (N image).
        // L/R input brad swapped: DPAD-LEFT → E image, DPAD-RIGHT → W image
        uint16_t ang = 0x4000;
        if (afn_input_fwd > 0 && afn_input_right == 0)      ang = 0x4000; // UP
        else if (afn_input_fwd > 0 && afn_input_right > 0)  ang = 0x6000; // UP+RIGHT
        else if (afn_input_fwd == 0 && afn_input_right > 0) ang = 0x8000; // RIGHT
        else if (afn_input_fwd < 0 && afn_input_right > 0)  ang = 0xA000; // DOWN+RIGHT
        else if (afn_input_fwd < 0 && afn_input_right == 0) ang = 0xC000; // DOWN
        else if (afn_input_fwd < 0 && afn_input_right < 0)  ang = 0xE000; // DOWN+LEFT
        else if (afn_input_fwd == 0 && afn_input_right < 0) ang = 0x0000; // LEFT
        else if (afn_input_fwd > 0 && afn_input_right < 0)  ang = 0x2000; // UP+LEFT
        player_move_angle = ang;
    } else if (s_wasMoving) {
        // On stop: bake current orbit into player_move_angle so the idle
        // formula `player_move_angle - 2*orbit_angle` gives the same dir
        // the moving picker just gave. Prevents a snap on key release.
        player_move_angle = player_move_angle + (orbit_angle << 1);
    }
    s_wasMoving = player_moving;
    (void)s_moveSpeed;
#else
    // No scripts — built-in WASD-style movement with ease ramp + sprint.
    int wantMove   = (held & (KEY_UP | KEY_DOWN)) != 0;
    int wantSprint = (held & KEY_B) && wantMove;
    int targetSpeed = wantMove ? (wantSprint ? AFN_SPRINT_SPEED : AFN_WALK_SPEED) : 0;
    int easeNum    = wantMove
        ? (wantSprint ? AFN_SPRINT_EASE_IN : AFN_WALK_EASE_IN)
        : (s_moveSpeed > AFN_WALK_SPEED ? AFN_SPRINT_EASE_OUT : AFN_WALK_EASE_OUT);
    s_moveSpeed += ((targetSpeed - s_moveSpeed) * easeNum) >> 8;
    if (s_moveSpeed < 0) s_moveSpeed = 0;

    int dx = 0, dz = 0;
    if (held & KEY_UP)    { dx += g_sinf; dz += g_cosf; }
    if (held & KEY_DOWN)  { dx -= g_sinf; dz -= g_cosf; }
    player_x += FX_MUL(dx, s_moveSpeed);
    player_z += FX_MUL(dz, s_moveSpeed);
    player_moving = (dx != 0 || dz != 0);
#endif

#ifndef AFN_HAS_SCRIPT
    // Built-in jump on KEY_A. With scripts, a Jump node sets player_vy.
    if ((down & KEY_A) && player_on_ground) {
        player_vy = AFN_JUMP_VEL;
        player_on_ground = 0;
    }
#else
    (void)down;
#endif
    // Gravity: scripts can override via SetGravity (afn_gravity); fallback
    // to the editor-exported AFN_GRAVITY constant otherwise.
#ifdef AFN_HAS_SCRIPT
    player_vy -= afn_gravity ? afn_gravity : AFN_GRAVITY;
    int term = afn_terminal_vel ? afn_terminal_vel : AFN_TERMINAL_VEL;
    if (player_vy < -term) player_vy = -term;
#else
    player_vy -= AFN_GRAVITY;
    if (player_vy < -AFN_TERMINAL_VEL) player_vy = -AFN_TERMINAL_VEL;
#endif
    player_y += player_vy;

    // Floor + wall collision against mesh data when the project exports it.
#ifdef AFN_COL_FACE_COUNT
    {
        extern int afn_floor_sprite;
        int wasGround = player_on_ground;
        int floorY;
        int onFloor = afn_collide_floor(player_x, player_z, player_y, &floorY);
        // "On the rail" = standing on a floor face that belongs to the rail
        // sprite (afn_grind_rail), captured by StartGrind. This is what makes
        // the grind follow JUST the pipe and end the instant you leave it.
        int onRail = (onFloor && afn_grind_rail >= 0 && afn_floor_sprite == afn_grind_rail);
        // Re-catch suppression after launching off an End-flagged terminus.
        // When you reach the rail end and launch off, the next frame you're still
        // over the rail mesh, so afn_grinding (StartGrind intent, re-set by On
        // Collision) is still true and the engage below — or the magnet — instantly
        // re-grabs you: you "portal" and never actually leave. The End flag means
        // "launch-off / clean release", so on launch (below) we set this window;
        // while >0 both the magnet catch and the engage are suppressed so you fly
        // off cleanly. Counts down each frame.
        static int s_railLaunchCD = 0;
        if (s_railLaunchCD > 0) s_railLaunchCD--;
        // Slope fix: collide_floor only accepts a floor within afn_player_height
        // (~12px) of the player's feet, so a tilted rail's surface — which moves
        // up or down by a whole horizontal grind step each frame — falls outside
        // that window and the grind detaches (only a flat beam stayed in range).
        // While grinding, re-query with the player Y biased UP by the horizontal
        // step (+margin) so an uphill OR downhill rail surface stays in range.
        // Gated on the rail sprite, so a non-rail floor that sneaks into the
        // wider window won't be mistaken for the rail.
        // Gate on the StartGrind INTENT (afn_grinding) so this widened probe is
        // active during the APPROACH — needed to catch fast entries like a spring
        // launch onto the rail, where the narrow floor window would tunnel past
        // between frames. It does NOT cause an early grind/SFX on a fly-over: the
        // engage below additionally requires player_y <= floorY (real surface
        // contact), and the SFX gate reads afn_grind_vel (only set once sliding).
        if (!onRail && (afn_grind_vel != 0 || afn_grinding) && afn_grind_rail >= 0) {
            int stepMag = (mvX < 0 ? -mvX : mvX) + (mvZ < 0 ? -mvZ : mvZ);
            int probeY = player_y + stepMag + stepMag / 2 + afn_player_height;
            int fY2;
            if (afn_collide_floor(player_x, player_z, probeY, &fY2) &&
                afn_floor_sprite == afn_grind_rail) {
                onFloor = 1; floorY = fY2; onRail = 1;
            }
        }
        // Set when the deliberate Width-grab magnet (below) is what put us onRail,
        // so the snap-up tightening doesn't undo an intentional catch.
        int magnetCaught = 0;
#ifdef AFN_HAS_RAIL_PATH
        // GrindCatch (Width): if you came down NEAR the rail path but not dead
        // over the thin mesh floor, snap-catch onto it. Only while descending,
        // with the rail captured, and within both the horizontal radius and the
        // vertical window — then treat it as onRail (the engage below pulls you
        // onto the path). Off when afn_grind_catch_x == 0.
        // PERF: gate cheaply (count check, not afn_railpath_len which sqrts every
        // segment), then ONE nearest pass that also yields the nearest point +
        // its squared distance — so we early-out on horizontal range before the
        // (also O(N)) sample, and the End check reuses the same point. This block
        // runs every frame you're near a rail, so the extra loops were the lag.
        if (!onRail && afn_grind_catch_x > 0 && afn_grinding && player_vy <= 0
            && s_railLaunchCD == 0
            && afn_grind_rail >= 0 && afn_grind_rail < AFN_SPRITE_COUNT
            && afn_rail_count[afn_grind_rail] >= 2) {
            int nearIdx = 0; long long horiz2 = 0;
            long long r = afn_grind_catch_x;
            int arc = afn_railpath_nearest_ex(afn_grind_rail, player_x, player_z, &nearIdx, &horiz2);
            if (horiz2 <= r * r) {                       // in horizontal range — now do the costlier bits
                int gx, gy, gz, tdx, tdz;
                afn_railpath_sample(afn_grind_rail, arc, &gx, &gy, &gz, &tdx, &tdz);
                int dyAbs = player_y - gy; if (dyAbs < 0) dyAbs = -dyAbs;
                int vWin = afn_player_height + afn_grind_catch_y;
                // End tip → only catch when moving TOWARD the rail (clean vault off).
                int movingToward = 1;
                if (afn_rail_pt_end[afn_rail_start[afn_grind_rail] + nearIdx]) {
                    long long ddx = player_x - gx, ddz = player_z - gz;
                    long long velX = mvX + afn_player_vx_world;
                    long long velZ = mvZ + afn_player_vz_world;
                    movingToward = (velX * (-ddx) + velZ * (-ddz)) >= 0;
                }
                if (dyAbs <= vWin && movingToward) {
                    onFloor = 1; floorY = gy; onRail = 1; magnetCaught = 1;
                }
            }
        }
#endif
        static int s_grindPrevFloorY = 0;
        // Persistent boosted-cap bonus. GrindBoost raises the grind speed
        // ceiling while descending; rather than snapping the cap back the
        // instant the rail levels out (a brief flat dip mid-descent would
        // jolt you), this bonus RAMPS UP toward the steepness-scaled target
        // while boosting downhill and DECAYS gradually otherwise — so speed
        // earned on a drop carries across flats and only bleeds off slowly.
        static int s_grindCapBonus = 0;
#ifdef AFN_HAS_RAIL_PATH
        static int s_railArc=0, s_railDir=1, s_railHas=0, s_railPrevY=0;
        // Smoothed arc-advance speed. afn_grind_vel is the PHYSICS velocity
        // (slope kicks + decay + cap), which jitters frame-to-frame on long
        // hand-placed segments where the slope changes abruptly at each point.
        // Advancing the arc by the raw value makes the step size oscillate; the
        // camera lag then amplifies that into a visible side-to-side shake on
        // the sprite (worse the faster you go). Advancing by a low-passed copy
        // gives a steady step so the slide reads smooth. Generated-from-mesh
        // rails never showed this because their many short segments already
        // keep the slope gradual.
        static int s_railVelSmooth = 0;
#endif

        // Don't get SUCKED UP onto the grind rail from below. collide_floor
        // accepts a floor up to a body-height (~12px) ABOVE the feet — fine for
        // stepping up terrain, but for a grind rail it means leaving a ledge
        // beside a slightly-higher rail snaps you straight up onto it the instant
        // you step off ("immediate connection from above"). Tighten that window
        // for the rail: only let it catch you when its top is at/just-below your
        // feet (a real landing or level run-on), not while you're still under it.
        // (Only when not already grinding, so following a rising rail mid-grind is
        // untouched; and the deliberate Width-grab magnet is exempt via
        // magnetCaught.) 256 = ~1px of upward slack.
        if (afn_grind_vel == 0 && onRail && !magnetCaught && (floorY - player_y) > 256) {
            onFloor = 0; onRail = 0;
        }

        if (afn_grind_vel != 0) {
            // --- Currently grinding ---
#ifdef AFN_HAS_RAIL_PATH
            if (s_railHas) {
                if (player_vy > 0) {
                    afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                    afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                    if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                    afn_grind_vel = 0; afn_grinding = 0; s_railHas = 0;
                } else {
                    int total = afn_railpath_len(afn_grind_rail);
                    // Advance by the smoothed speed (see s_railVelSmooth note).
                    s_railVelSmooth += (afn_grind_vel - s_railVelSmooth) >> 3;
                    if (s_railVelSmooth < 1) s_railVelSmooth = 1;
                    s_railArc += s_railDir * s_railVelSmooth;
                    int s_atEnd = (s_railArc <= 0 || s_railArc >= total);
                    if (s_atEnd) {
                        // Bounce point at this terminus? Reverse direction and keep
                        // grinding (bumper) instead of launching off. The jump-off
                        // (player_vy > 0) branch above runs first, so you can still
                        // jump off right before the bumper.
                        int rn2 = afn_rail_count[afn_grind_rail];
                        int rs2 = afn_rail_start[afn_grind_rail];
                        int termIdx2 = (s_railArc <= 0) ? 0 : (rn2 - 1);
                        if (rn2 > 0 && afn_rail_pt_bounce[rs2 + termIdx2]) {
                            s_railDir = -s_railDir;
                            s_railArc = (s_railArc <= 0) ? 0 : total;
                            s_atEnd = 0;
                        }
                    }
                    if (s_atEnd) {
                        afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                        afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                        if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                        afn_grind_vel = 0; afn_grinding = 0; s_railHas = 0;
                        // If the terminus we reached is End-flagged ("launch-off"),
                        // block re-catch/re-engage briefly so we actually leave the
                        // rail instead of being instantly re-grabbed (the "won't
                        // release" / portal bug). Non-flagged ends behave as before.
                        {
                            int rn3 = afn_rail_count[afn_grind_rail];
                            int rs3 = afn_rail_start[afn_grind_rail];
                            int termIdx3 = (s_railArc <= 0) ? 0 : (rn3 - 1);
                            if (rn3 > 0 && afn_rail_pt_end[rs3 + termIdx3])
                                s_railLaunchCD = 20;
                        }
                    } else {
                        int gx,gy,gz,tdx,tdz;
                        afn_railpath_sample(afn_grind_rail, s_railArc, &gx,&gy,&gz,&tdx,&tdz);
                        // Accelerate by GRADE (Y drop per unit arc), not by the
                        // raw per-frame Y delta. The per-frame delta scales with
                        // velocity (faster -> longer step -> bigger drop), so
                        // `vel += drop` was a positive-feedback loop that rang
                        // and shook the sprite on undulating hand-drawn rails.
                        // Dividing the drop by the arc step taken makes it a
                        // velocity-independent slope, so velocity converges.
                        int step = s_railVelSmooth; if (step < 1) step = 1;
                        int grade = ((s_railPrevY - gy) << 8) / step; // .8: +down/-up
                        s_railPrevY = gy;
                        if (grade >  256) grade =  256;
                        if (grade < -256) grade = -256;
                        // Downhill builds real momentum (Sonic rail launch): a
                        // steep grade adds (power+boost)/frame and friction is
                        // near-zero, so speed accumulates the longer the rail
                        // descends. Uphill bleeds gently. Grade is velocity-
                        // independent (drop per unit arc) so this stays stable.
                        // afn_grind_power (GrindPower node) sets the base gain
                        // (0 => default 24); afn_grind_boost (GrindBoost node,
                        // gated by a held key) adds extra ONLY while descending.
                        {
                            int gpow = afn_grind_power ? afn_grind_power : 24;
                            if (grade > 0) afn_grind_vel += (grade * gpow) >> 8;
                            else           afn_grind_vel += (grade * 4) >> 8;
                            // GrindBoost RAISES the speed ceiling while descending,
                            // scaled by steepness. The bonus ramps toward the target
                            // while boosting downhill and DECAYS gradually otherwise,
                            // so a flat dip mid-descent doesn't instantly cut speed —
                            // earned momentum carries across flats and bleeds slowly.
                            int target = (grade > 0 && afn_grind_boost > 0)
                                       ? ((grade * afn_grind_boost) >> 8) : 0;
                            if (target > s_grindCapBonus) s_grindCapBonus += (target - s_grindCapBonus) >> 2; // quick ramp up
                            else                          s_grindCapBonus -= s_grindCapBonus >> grindBleed;    // slow bleed down (GrindBleed node)
                        }
                        afn_grind_vel -= afn_grind_vel >> 10; // very slippery
                        if (afn_grind_vel < (AFN_WALK_SPEED >> 3)) afn_grind_vel = (AFN_WALK_SPEED >> 3);
                        { int gcap = AFN_SPRINT_SPEED * 5 + s_grindCapBonus;
                          if (afn_grind_vel > gcap) afn_grind_vel = gcap; }
                        player_x = gx; player_z = gz; player_y = gy;
                        player_vy = 0; player_on_ground = 1;
                        afn_grind_dx = tdx * s_railDir; afn_grind_dz = tdz * s_railDir;
                    }
                }
            } else
#endif

            if (player_vy > 0) {
                // Jump node fired → hop off, carry momentum into world velocity.
                afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                afn_grind_vel = 0; afn_grinding = 0;
            } else if (!onRail) {
                // Slid off the edge / end of the rail — release momentum and let
                // it decay so you fly off keeping speed then ramp down (Sonic rail
                // launch), instead of sliding forever. A VelocityFalloff node can
                // override the 30-frame default.
                afn_player_vx_world = FX_MUL(afn_grind_dx, afn_grind_vel);
                afn_player_vz_world = FX_MUL(afn_grind_dz, afn_grind_vel);
                if (afn_velocity_falloff <= 0) afn_velocity_falloff = 30;
                afn_grind_vel = 0; afn_grinding = 0;
            } else {
                // Stick to the rail; slope drives momentum. slope > 0 means the
                // floor dropped this frame (going DOWNHILL) → accelerate harder;
                // slope < 0 is uphill → bleed speed. Asymmetric so a downhill
                // run visibly ramps up like a Sonic rail.
                int slope = s_grindPrevFloorY - floorY;
                s_grindPrevFloorY = floorY;
                // GrindPower scales the downhill slope gain (default 24 ~= 1x of
                // the raw slope step). GrindBoost RAISES the speed ceiling while
                // descending (scaled by slope), so holding the button lets you
                // exceed the normal cap — steeper = more.
                // Boosted-cap bonus ramps up while boosting downhill, decays
                // gradually otherwise (so a flat dip doesn't instantly cut it).
                {
                    int target = (slope > 0 && afn_grind_boost > 0)
                               ? ((slope * afn_grind_boost) / 24) : 0;
                    if (target > s_grindCapBonus) s_grindCapBonus += (target - s_grindCapBonus) >> 2;
                    else                          s_grindCapBonus -= s_grindCapBonus >> grindBleed; // GrindBleed node
                }
                int gcap = AFN_SPRINT_SPEED * 3 + s_grindCapBonus;
                if (slope > 0) {
                    int gpow = afn_grind_power ? afn_grind_power : 24;
                    afn_grind_vel += (slope * gpow) / 24;     // downhill
                } else {
                    afn_grind_vel += slope >> 1;             // uphill: lose half the climb
                }
                afn_grind_vel -= afn_grind_vel >> 9;         // tiny friction (very slippery)
                if (afn_grind_vel < (AFN_WALK_SPEED >> 3)) afn_grind_vel = (AFN_WALK_SPEED >> 3);
                if (afn_grind_vel > gcap) afn_grind_vel = gcap;

                // --- Re-center between rail edges + steer to follow the curve ---
                // The grind heading is frozen at entry, so on a CURVED rail going
                // straight walks you off the side — that's why a long-turn rail
                // drops you immediately. Fix both every frame by probing the rail
                // surface itself:
                //   1. Lateral re-center: scan perpendicular to the heading, find
                //      how far the rail extends each way, snap to the midpoint.
                //      This ALONE makes falling off the side impossible — you're
                //      always pinned to the rail's middle, curve or not.
                //   2. Steer: re-center a probe ~16px ahead; the vector from the
                //      player to that ahead-center is the rail's local tangent.
                //      Adopt it as the new heading so we round the bend.
                // afn_collide_floor sets afn_floor_sprite as a side effect; the
                // final height sample below is at the player center, so it leaves
                // afn_floor_sprite correctly pointing at the rail for next frame.
                {
                    int probeBias = afn_player_height + 4096;   // search well above feet
                    int perpx = -afn_grind_dz, perpz = afn_grind_dx; // 256-normalized
                    int rfy;
                    const int STEP = 512, MAXW = 20 * 256;       // 2px steps, 20px half-width
                    // Helper inline: find rail-center offset (in +perp units) at (cx,cz).
                    #define GRIND_CENTER_OFF(cx, cz, outOff) do {                       \
                        int mp = 0, mn = 0;                                              \
                        for (int s = STEP; s <= MAXW; s += STEP) {                       \
                            int tx = (cx) + ((perpx * s) >> 8);                          \
                            int tz = (cz) + ((perpz * s) >> 8);                          \
                            if (afn_collide_floor(tx, tz, player_y + probeBias, &rfy)    \
                                && afn_floor_sprite == afn_grind_rail) mp = s; else break;\
                        }                                                                \
                        for (int s = STEP; s <= MAXW; s += STEP) {                       \
                            int tx = (cx) - ((perpx * s) >> 8);                          \
                            int tz = (cz) - ((perpz * s) >> 8);                          \
                            if (afn_collide_floor(tx, tz, player_y + probeBias, &rfy)    \
                                && afn_floor_sprite == afn_grind_rail) mn = s; else break;\
                        }                                                                \
                        (outOff) = (mp - mn) / 2;                                        \
                    } while (0)

                    // 1. Lateral re-center at the player.
                    int offHere;
                    GRIND_CENTER_OFF(player_x, player_z, offHere);
                    player_x += (perpx * offHere) >> 8;
                    player_z += (perpz * offHere) >> 8;

                    // 2. Steer toward the rail center ~16px ahead.
                    int ahead = 16 * 256;
                    int acx = player_x + ((afn_grind_dx * ahead) >> 8);
                    int acz = player_z + ((afn_grind_dz * ahead) >> 8);
                    int offAhead;
                    GRIND_CENTER_OFF(acx, acz, offAhead);
                    acx += (perpx * offAhead) >> 8;
                    acz += (perpz * offAhead) >> 8;
                    int ndx = acx - player_x, ndz = acz - player_z;
                    int nlen = afn_isqrt(ndx * ndx + ndz * ndz);
                    if (nlen > 0) {
                        afn_grind_dx = (ndx * 256) / nlen;
                        afn_grind_dz = (ndz * 256) / nlen;
                    }
                    #undef GRIND_CENTER_OFF

                    // Height: sample at the centered position. The tube-top center
                    // is the consistent crest, so this is far steadier than an
                    // off-center read. Leaves afn_floor_sprite = rail for next frame.
                    if (afn_collide_floor(player_x, player_z, player_y + probeBias, &rfy)
                        && afn_floor_sprite == afn_grind_rail)
                        floorY = rfy;
                }
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            }
        } else if (afn_grinding && onRail && player_vy <= 0 && s_railLaunchCD == 0
                   && player_y <= floorY + afn_grind_catch_y) {
            // NOTE: `player_y <= floorY` (actually reached the rail surface), not
            // merely onRail (within the detection window). Without it, arcing a
            // HIGH jump OVER the rail passes the feet through the window above the
            // surface with vy<=0, engaging mid-jump and retriggering the grind SFX
            // before you land. A real landing crosses floorY at the rail's XZ; a
            // jump that clears the rail moves past in XZ before reaching it.
            // GrindCatch (Height) adds afn_grind_catch_y so you can snap on from a
            // bit above the surface (0 = strict).
            // --- Engage: StartGrind fired AND we're standing on the rail ---
            // Lock the slide to the rail's mesh axis (so you follow the pipe even
            // if you landed at an angle), seed speed from current move speed.
            int rdx = s_lastMoveDX, rdz = s_lastMoveDZ;
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
            if (afn_grind_rail < AFN_SPRITE_COUNT) {
                int mi = afn_sprite_data[afn_grind_rail][9];
                if (mi >= 0 && mi < AFN_MESH_COUNT) {
                    const short* v = afn_mesh_vert_ptrs[mi];
                    int vc = afn_mesh_desc[mi][0];
                    if (v && vc > 0) {
                        int mnx=v[0],mxx=v[0],mnz=v[2],mxz=v[2];
                        for (int k = 1; k < vc; k++) {
                            int x=v[k*3], z=v[k*3+2];
                            if(x<mnx)mnx=x; if(x>mxx)mxx=x;
                            if(z<mnz)mnz=z; if(z>mxz)mxz=z;
                        }
                        int ex = mxx-mnx, ez = mxz-mnz;
                        long d1 = (long)ex*s_lastMoveDX + (long)ez*s_lastMoveDZ;
                        long d2 = (long)ex*s_lastMoveDX - (long)ez*s_lastMoveDZ;
                        int ddx = ex, ddz = (d1 >= d2) ? ez : -ez;
                        long dot = (long)ddx*s_lastMoveDX + (long)ddz*s_lastMoveDZ;
                        if (dot < 0) { ddx = -ddx; ddz = -ddz; }
                        int len = afn_isqrt(ddx*ddx + ddz*ddz); if (len < 1) len = 1;
                        rdx = (ddx * 256) / len;
                        rdz = (ddz * 256) / len;
                    }
                }
            }
#endif
#ifdef AFN_HAS_RAIL_PATH
            s_railHas = (afn_grind_rail >= 0 && afn_railpath_len(afn_grind_rail) > 0);
            if (s_railHas) {
                s_railSplineOn = afn_rail_spline[afn_grind_rail];
                s_railArc = afn_railpath_nearest(afn_grind_rail, player_x, player_z);
                int gx,gy,gz,tdx,tdz;
                afn_railpath_sample(afn_grind_rail, s_railArc, &gx,&gy,&gz,&tdx,&tdz);
                long fdot = (long)s_lastMoveDX*tdx + (long)s_lastMoveDZ*tdz;
                s_railDir = (fdot >= 0) ? 1 : -1;
                rdx = tdx * s_railDir; rdz = tdz * s_railDir;
                s_railPrevY = gy;
                player_x = gx; player_z = gz; player_y = gy;
            }
#endif
            afn_grind_dx = rdx; afn_grind_dz = rdz;
            // Seed the grind speed from the player's ACTUAL momentum entering the
            // rail (input movement + any boost-pad world velocity), projected
            // onto the rail axis — so sprinting / boosting onto a rail carries
            // that speed instead of snapping to a flat default. rdx/rdz are
            // 256-normalized, so the >>8 yields displacement units (same as
            // afn_grind_vel). Fold the world velocity in and clear it so it
            // doesn't keep adding drift off the rail while grinding.
            {
                int tvx = mvX + afn_player_vx_world;
                int tvz = mvZ + afn_player_vz_world;
                int mom = (tvx * rdx + tvz * rdz) >> 8;
                if (mom < 0) mom = -mom;
                afn_grind_vel = mom;
                if (afn_grind_vel < (AFN_WALK_SPEED >> 3))
                    afn_grind_vel = (AFN_WALK_SPEED >> 3);
                // Carry high entry momentum (boost pad / downhill sprint) past the
                // normal grind speed cap by seeding the boosted-cap bonus from it.
                // Without this the continue branch clamps afn_grind_vel to
                // SPRINT*N next frame and the boost is lost instantly; with it the
                // speed RIDES then bleeds off (GrindBleed), so a boost-into-rail
                // keeps its momentum.
                {
                    int overCap = mom - AFN_SPRINT_SPEED * 3;
                    s_grindCapBonus = (overCap > 0) ? overCap : 0;
                }
                afn_player_vx_world = 0; afn_player_vz_world = 0;
                afn_velocity_falloff = 0;
#ifdef AFN_HAS_RAIL_PATH
                // Seed the smoothed arc speed so the first frames don't ramp
                // up from zero (which would look like a brief stall on engage).
                s_railVelSmooth = afn_grind_vel;
#endif
            }
            s_grindPrevFloorY = floorY;
            // Initial heading is the rail axis (rdx/rdz, set above). From here the
            // grind frames re-center + steer off the rail surface each frame, so
            // no analytic anchor is needed — the curve is followed live.
            player_y = floorY; player_vy = 0; player_on_ground = 1;
        } else {
            // --- Not grinding: normal floor resolution ---
            if (onFloor && player_y <= floorY) {
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            } else if (onFloor && wasGround && player_vy <= 0
                       && (player_y - floorY) <= s_groundSnapTol) {
                player_y = floorY; player_vy = 0; player_on_ground = 1;
            } else {
                player_on_ground = 0;
            }
            // Don't let a StartGrind intent linger when we're not on the rail
            // (e.g. brushed the rail's bounding box from the side without
            // standing on it) — otherwise IsGrinding would stay true.
            if (!onRail) afn_grinding = 0;
        }
    }
    // Skip wall collision WHILE grinding — a thin pipe's own side walls would
    // shove the player off the rail. The grind already locks XZ to the axis.
    if (afn_grind_vel == 0)
        afn_collide_walls(&player_x, &player_z, player_y);
#ifdef AFN_HAS_NPC_RIGS
    // Bump the player out of NPC collision boxes (enemies block movement).
    afn_collide_npc_boxes(&player_x, &player_z, player_y);
#endif
#else
    {
        // No mesh collision data — fall back to flat ground at player init Y.
        int groundY = AFN_PLAYER_BASE_Y;
        if (player_y <= groundY) {
            player_y = groundY;
            player_vy = 0;
            player_on_ground = 1;
        } else {
            player_on_ground = 0;
        }
    }
#endif
    // Mirror the ACTUAL grind state for the script gates (Is Grinding / Is Not
    // Grinding) to read next frame. Use afn_grind_vel != 0, NOT afn_grinding:
    // afn_grinding is the StartGrind INTENT (set by On Collision) and lingers at
    // 1 whenever onRail's detection window is satisfied — including while you arc
    // a high jump OVER the rail without landing on it, which made the grind SFX
    // retrigger early. afn_grind_vel is only non-zero once you've truly engaged
    // and are sliding, so it cleanly means "grinding for real".
    { extern int afn_grinding_active, afn_grind_vel; afn_grinding_active = (afn_grind_vel != 0); }
    s_playerY = player_y - AFN_PLAYER_BASE_Y;

    // Render-smoothed player position. While grinding, low-pass toward the exact
    // position so the sharp per-vertex kink in a straight-segment rail path
    // doesn't snap the sprite against the eased camera. Off the rail, snap 1:1
    // (full input responsiveness — no smoothing lag when walking/jumping).
    {
        extern int afn_grinding, afn_grind_vel;
        int grinding = (afn_grinding || afn_grind_vel != 0);
        static int inited = 0;
        if (!inited) {
            player_render_x = player_x; player_render_y = player_y; player_render_z = player_z;
            inited = 1;
        }
        if (grinding) {
            // ~1/4 catch-up per frame: smooths vertex kinks without visible lag.
            player_render_x += (player_x - player_render_x) >> 2;
            player_render_y += (player_y - player_render_y) >> 2;
            player_render_z += (player_z - player_render_z) >> 2;
        } else {
            player_render_x = player_x;
            player_render_y = player_y;
            player_render_z = player_z;
        }
    }

    // 3rd-person camera: target = player - orbit_dist * view-forward. Lerp
    // cam_x/z toward target with the same ease rate as movement so the cam
    // glides into position rather than rubber-banding.
    {
#ifdef AFN_CAM_SLOT_COUNT
        orbit_dist = cam_blend(AFN_ORBIT_DIST, s_slotDist);   // blend zoom toward slot
#endif
        int targetX = player_x - ((g_sinf * orbit_dist) >> 8);
        int targetZ = player_z - ((g_cosf * orbit_dist) >> 8);
        int ddx = targetX - cam_x;
        int ddz = targetZ - cam_z;
        if (ddx > -16 && ddx < 16 && ddz > -16 && ddz < 16) {
            cam_x = targetX;
            cam_z = targetZ;
        } else {
            // Ease rate based on whether the player is actually moving.
            // Without scripts we have a richer state (wantSprint), with
            // scripts we just use afn_move_speed as a heuristic.
            int moving, sprintLike;
#ifdef AFN_HAS_SCRIPT
            // Node-driven sprint: the Sprint node sets afn_speed_prio = 1 when it
            // runs (reset to 0 each script tick), so this is true exactly when the
            // player is sprinting THIS frame — via whatever key the script wired
            // into Sprint, not a hardcoded button. Script tick runs before this
            // camera update, so the flag is current.
            moving = (afn_input_fwd != 0 || afn_input_right != 0);
            sprintLike = afn_speed_prio != 0;
#else
            int wantMove2   = (held & (KEY_UP | KEY_DOWN)) != 0;
            int wantSprint2 = (held & KEY_B) && wantMove2;
            moving = wantMove2;
            sprintLike = wantSprint2;
#endif
            int ease = sprintLike
                ? (moving ? AFN_SPRINT_EASE_IN : AFN_SPRINT_EASE_OUT)
                : (moving ? AFN_WALK_EASE_IN   : AFN_WALK_EASE_OUT);
            // Orbit-camera ease: in-rate while L/R held (ramping into
            // orbit), out-rate after release (settling). Picks the max
            // vs the walk/sprint ease so the camera never lags BEHIND
            // those defaults — only catches up faster when orbiting.
#ifdef AFN_ORBIT_EASE_IN
            if (held & (KEY_L | KEY_R)) {
                if (AFN_ORBIT_EASE_IN > ease) ease = AFN_ORBIT_EASE_IN;
            } else {
                if (AFN_ORBIT_EASE_OUT > ease) ease = AFN_ORBIT_EASE_OUT;
            }
#endif
            cam_x += (ddx * ease) >> 8;
            cam_z += (ddz * ease) >> 8;
            {
                static int s_dbgF = 0;
                s_dbgF++;
                if ((s_dbgF & 15) == 0)
                    iprintf("\x1b[14;0Hca=%d e=%d dx=%d dz=%d cx=%d cz=%d  ",
                            (int)cam_angle, ease, ddx >> 8, ddz >> 8,
                            cam_x >> 8, cam_z >> 8);
            }
        }
    }

    // Smooth cam_h Y follow — quick on the way down (landing), lazy in the
    // air so the camera lags a beat behind a jump's apex.
    {
        int dy = s_playerY - s_camYSmooth;
        int rate = player_on_ground ? AFN_JUMP_CAM_LAND : AFN_JUMP_CAM_AIR;
        s_camYSmooth += (dy * rate) >> 8;
        if (dy > -4 && dy < 4) s_camYSmooth = s_playerY;
    }
    // cam_h is the camera's world Y. AFN_CAM_H is the camera's offset above
    // the player. s_camYSmooth is the SMOOTHED player-Y offset (driven by
    // AFN_JUMP_CAM_LAND / AFN_JUMP_CAM_AIR rates) so the camera lags through
    // jumps instead of snapping. baseline = the player's spawn Y; adding the
    // smoothed delta gives the camera's tracked world Y.
    cam_h = AFN_PLAYER_BASE_Y + s_camYSmooth + AFN_CAM_H;
#ifdef AFN_CAM_SLOT_COUNT
    // Blend the height OFFSET so the camera still follows the player's Y.
    cam_h = AFN_PLAYER_BASE_Y + s_camYSmooth + cam_blend(AFN_CAM_H, s_slotHeightOff);
#endif

    // GrindBoost is a one-frame request: clear it every frame so it only applies
    // on frames the GrindBoost node actually fired. Gate the node with a held key
    // (On Update -> Is Key Held -> Grind Boost) for continuous hold-to-boost.
    { extern int afn_grind_boost; afn_grind_boost = 0; }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
static void load_sky_texture(void)
{
    glGenTextures(1, &gl_sky_tex_id);
    glBindTexture(0, gl_sky_tex_id);
    // 256x256 8bpp paletted (GL_RGB256). TEXTURE_SIZE_256 = 5.
    // GL_TEXTURE_WRAP_S = horizontal wrap so the panorama can scroll past
    // its 256-px edge as cam_angle rotates past 360° without clamping or
    // garbage. Vertical wrap intentionally off (top of sky is top).
    glTexImage2D(0, 0, GL_RGB256, 5, 5, 0,
                 TEXGEN_TEXCOORD | GL_TEXTURE_WRAP_S, afn_sky_tex);
    glColorTableEXT(0, 0, 256, 0, 0, afn_sky_pal);
}

// Draw the sky panorama as a view-space quad behind the 3D scene.
// UV.u scrolls with cam_angle so the panorama appears to wrap as you turn.
// UV.v covers the top half of the 256-tall texture, mapped to the top
// portion of the screen (above the horizon).
static void render_sky(void)
{
    if (!gl_sky_tex_id) return;
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    // Push the quad to ~900 DS units away (near the far plane = 1024) so
    // meshes in the normal scene scale always pass depth test against it.
    // Then scale by 200x so the small v16 vertex coords cover the screen
    // at that depth: visible half-width at depth 900 with 70°fov+4:3 ≈ 840
    // DS units, so verts of ±5 × 200 = ±1000 over-covers slightly.
    glTranslatef32(0, 0, -inttof32(900));
    glScalef32(inttof32(200), inttof32(200), inttof32(200));
    glBindTexture(0, gl_sky_tex_id);
    glColor3b(255, 255, 255);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

    // cam_angle 0..65535 = full 360° panorama wrap.
    // Map to t16 (.4 fixed texel units): one full wrap = 256 px = 4096 t16.
    // Screen shows the full 256-px panorama at 1:1 (matches GBA's Mode 7
    // sky which just scrolls a 256-px tilemap; a perspective-correct
    // ~86°/360° slice looked too stretched compared to the reference).
    // Match GBA's update_sky_scroll exactly: pixScroll = (cam_ang * 256) >> 16
    // and 1:1 panorama-to-screen mapping (one texture pixel = one screen
    // pixel, full 256-px panorama spans full 256-px screen).
    // -1280 t16 = -80 px shift in source = panorama appears 80 px RIGHT.
    int uOffset = ((int)cam_angle * 4096) >> 16;
    int uLeft  = uOffset - 1280;
    int uRight = uOffset - 1280 + 4096;  // 1:1 panorama mapping (matches GBA)
    int vTop   = 0;
    int vBot   = 4096;             // full 256-row panorama — anything smaller stretches each texel taller

    // Quad pushed to the far depth so all meshes draw on top.
    // v16 range is ±8 (4.12 fixed); z = -7.9 is as far as a vertex can go.
    // Then the quad's X/Y extent must be large enough to fill the screen at
    // that depth: with 70° FOV, halfTan ≈ 0.7 → half-extent = 7.9 * 0.7 ≈ 5.5.
    // Use 7.9 to be safe.
    // Local v16 coords scaled by 200x. Quad sized to EXACTLY match the
    // visible screen at depth 900 (half-width = 900*tan(43°) ≈ 840 DS,
    // half-height = 900*tan(35°) ≈ 630). With the quad exactly visible,
    // uRight-uLeft = 4096 maps the full 256-px panorama 1:1 to screen
    // width (was ~215 px visible before because the over-sized quad
    // wasted texture coords past the screen edges).
    int16_t qZ  = floattov16( 0.0f);
    int16_t qXl = floattov16(-4.2f), qXr = floattov16( 4.2f);
    int16_t qYt = floattov16( 3.15f), qYb = floattov16(-3.15f);

    glBegin(GL_QUADS);
        glTexCoord2t16(uLeft,  vTop); glVertex3v16(qXl, qYt, qZ);
        glTexCoord2t16(uRight, vTop); glVertex3v16(qXr, qYt, qZ);
        glTexCoord2t16(uRight, vBot); glVertex3v16(qXr, qYb, qZ);
        glTexCoord2t16(uLeft,  vBot); glVertex3v16(qXl, qYb, qZ);
    glEnd();

    glPopMatrix(1);
    glBindTexture(0, 0);
}
#endif

void afn_fps3d_init(void)
{
#if defined(AFN_HAS_FLOOR) && AFN_HAS_FLOOR
    vramSetBankB(VRAM_B_MAIN_BG);
    m7_bg = bgInit(2, BgType_Rotation, BgSize_R_256x256, 4, 0);
    bgSetPriority(m7_bg, 3);
    load_floor();
#endif
    // Load the sky panorama FIRST. It's a single 256x256 texture (64KB) and
    // needs one CONTIGUOUS 64KB block in texture VRAM bank A (128KB total).
    // The mesh textures are small 4bpp tiles (~46KB total); if they load first
    // they scatter across bank A and leave no contiguous 64KB hole, so the sky
    // upload fails silently (glGenTextures succeeds but glTexImage2D can't
    // place it) and the sky vanishes once a scene has enough meshes. Grabbing
    // the big block first guarantees it; the small mesh textures then fill the
    // remaining ~64KB. (Bank B is the Mode-7 floor, banks C/D fail as 3D
    // texture banks, so we can't just add VRAM — order is what matters here.)
#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
    load_sky_texture();
#endif
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    load_mesh_textures();
#endif
#ifdef AFN_HAS_PLAYER_RIG
    load_player_rig_texture();
#endif
#ifdef AFN_HAS_NPC_RIGS
    load_npc_rig_textures();
#endif

    cam_h     = AFN_CAM_H;
    cam_angle = AFN_CAM_ANGLE;
    g_cosf    = brad_cos(cam_angle);
    g_sinf    = brad_sin(cam_angle);

    // Initialize player position from the player sprite (3rd-person camera
    // follows). Fall back to AFN_CAM_X/Z (free-cam start) if no player sprite.
#if defined(AFN_PLAYER_IDX) && AFN_PLAYER_IDX >= 0 && defined(AFN_SPRITE_COUNT) && AFN_SPRITE_COUNT > 0
    player_x = afn_sprite_data[AFN_PLAYER_IDX][0];
    player_y = afn_sprite_data[AFN_PLAYER_IDX][1];
    player_z = afn_sprite_data[AFN_PLAYER_IDX][2];
#else
    player_x = AFN_CAM_X;
    player_z = AFN_CAM_Z;
    player_y = 0;
#endif
#ifdef AFN_HAS_SCRIPT
    // Seed Respawn defaults at player spawn so the Respawn node works
    // before any UpdateRespawnPos / checkpoint hit.
    afn_start_x = player_x;
    afn_start_y = player_y;
    afn_start_z = player_z;
#endif
    orbit_dist = AFN_ORBIT_DIST;
    afn_player_heading = AFN_CAM_ANGLE;   // tank heading starts facing forward
    // Camera = player - orbit_dist along view forward. NDS convention:
    // forward = (sin, cos), so camera sits at -(sin, cos) * dist behind player.
    cam_x = player_x - ((g_sinf * orbit_dist) >> 8);
    cam_z = player_z - ((g_cosf * orbit_dist) >> 8);

    glLight(0,
            RGB15(31, 31, 31),
            floattov10(-0.5f),
            floattov10(-0.7f),
            floattov10(-0.5f));
    glMaterialf(GL_AMBIENT,  RGB15(8, 8, 8));
    glMaterialf(GL_DIFFUSE,  RGB15(28, 28, 28));
    glMaterialf(GL_SPECULAR, RGB15(0, 0, 0));
    glMaterialf(GL_EMISSION, RGB15(0, 0, 0));
}

// ---------------------------------------------------------------------------
// Scene transitions — brightness-ramp fade out → swap → fade back in.
// ---------------------------------------------------------------------------
void afn_scene_start_transition(int sceneIdx, int sceneMode, int fadeFrames)
{
    // Idempotent: if a transition is already running toward the same
    // target, don't restart the fade counter. ChangeScene fires from
    // OnCollision2D every frame the player stays in the radius — without
    // this guard the counter never reached 0 and the swap only happened
    // after the player backed out of the trigger.
    if (s_fade_phase != 0 &&
        afn_pending_scene == sceneIdx && afn_pending_scene_mode == sceneMode)
        return;
    if (fadeFrames < 1) fadeFrames = 15;
    afn_pending_scene      = sceneIdx;
    afn_pending_scene_mode = sceneMode;
    afn_fade_frames        = fadeFrames;
    afn_fade_counter       = fadeFrames;
    afn_fade_target        = -16;
    s_fade_phase           = 1;
}

void afn_scene_tick(void)
{
    if (s_fade_phase == 0) {
        REG_MASTER_BRIGHT = 0;
        return;
    }

    int cur;
    if (afn_fade_counter > 0) afn_fade_counter--;

    if (s_fade_phase == 1) {
        int t = afn_fade_frames - afn_fade_counter;
        cur = (afn_fade_target * t) / (afn_fade_frames ? afn_fade_frames : 1);
        if (afn_fade_counter == 0) {
            afn_current_scene = afn_pending_scene;
            afn_current_mode  = afn_pending_scene_mode;
            afn_pending_scene = -1;
            afn_fade_counter  = afn_fade_frames;
            s_fade_phase      = 2;
#ifdef AFN_HAS_SCRIPT
            // Scene-load reset: respawn player, clear momentum + transient
            // anim state, restore sprite visibility / collision so the new
            // scene starts fresh. Mirrors GBA's scene_load() but keeps
            // afn_flags / afn_score / afn_vars persistent across scenes.
            player_x = afn_start_x;
            player_y = afn_start_y;
            player_z = afn_start_z;
            player_vy = 0;
            player_moving = 0;
            afn_play_anim = -1;
            afn_anim_prio = 0;
            afn_collided_sprite = -1;
            afn_frame_count = 0;
            afn_player_frozen = 0;
            afn_shake_frames = 0;
            for (int i = 0; i < NUM_SPRITES; i++) {
                afn_sprite_visible[i] = 1;
                afn_collision_enabled[i] = 1;
                afn_hp[i] = 100;
                afn_state_timer[i] = 0;
                afn_sprite_flip[i] = 0;
            }
#endif
#ifdef AFN_HAS_MODE0
            // If swapping into Mode 0, load the destination scene's tilemap
            // (tile gfx + palette + map) into BG VRAM. Without this the BG
            // still shows the previous scene (or boot scene).
            if (afn_current_mode == 1) {
                extern void afn_mode0_init_scene(int sceneIdx);
                afn_mode0_init_scene(afn_current_scene);
            }
#endif
            // If swapping back into Mode 4 from Mode 0, restore the 3D
            // video mode + texture VRAM and re-run fps3d_init so all
            // textures / floor / sky reload. mode0_init clobbered VRAM_A
            // (it was MAIN_BG holding tilemap tiles). Re-init OAM too —
            // videoSetMode clears the sprite mapping size bits to 1D_32
            // default, which made our 1D_128-addressed sprite tile
            // pointers land on garbage data (sprites rendered "snapped
            // in half"). oamInit reasserts SpriteMapping_1D_128 and the
            // OBJ-on-top priority dance.
            if (afn_current_mode == 0) {
                // Replay the full boot 3D init sequence — videoSetMode +
                // glInit leaves the geometry engine in a state that needs
                // every step. Earlier attempts that only restored viewport
                // / VRAM / OAM left the right ~64px of the screen black
                // (3D output drew into only the first 192px wide region).
                videoSetMode(MODE_0_3D | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);
                vramSetBankA(VRAM_A_TEXTURE);
                vramSetBankD(VRAM_D_TEXTURE);
                vramSetBankE(VRAM_E_TEX_PALETTE);
                oamInit(&oamMain, SpriteMapping_1D_128, false);
                REG_BG0CNT = (REG_BG0CNT & ~3) | 3;
                // Mode 0 was scrolling BG0 to track the player camera; for
                // Mode 4 the same BG0 is the 3D layer, and a non-zero scroll
                // shifts the entire 3D output sideways — that's what made
                // the right ~64px render black.
                REG_BG0HOFS = 0;
                REG_BG0VOFS = 0;
                // Don't re-call glInit() — second invocation leaves the
                // GE half-initialised. But the texture allocator's
                // internal bookkeeping still points at VRAM_A from before
                // it was reassigned to MAIN_BG, so glGenTextures returns
                // handles backed by stale/unmapped memory and the next
                // load_mesh_textures uploads silently fail (the whole
                // backdrop renders white on the 2nd Mode 0 -> Mode 4 swap
                // even though meshes are still drawn). glResetTextures
                // wipes that bookkeeping so the re-upload lands cleanly.
                glResetTextures();
                glResetMatrixStack();
                glEnable(GL_TEXTURE_2D);
#if defined(AFN_NDS_AA) && AFN_NDS_AA
                glEnable(GL_ANTIALIAS);
#else
                glDisable(GL_ANTIALIAS);
#endif
                glClearColor(10, 18, 31, 31);
                glClearPolyID(63);
                glClearDepth(0x7FFF);
                glViewport(0, 0, 255, 191);
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                gluPerspective(70, 256.0 / 192.0, 0.1, 1024);
                glFlush(0);
                afn_fps3d_init();
                // Sprite VRAM still holds whatever Mode 0 last DMA'd, but
                // sprite_update / mode0 share g_active_frame[] as their
                // "what's loaded" cache. Reset so each asset re-DMAs its
                // proper Mode-4 frame on the next render.
#if defined(AFN_ASSET_COUNT) && AFN_ASSET_COUNT > 0
                extern int g_active_frame[AFN_ASSET_COUNT];
                for (int ai = 0; ai < AFN_ASSET_COUNT; ai++) g_active_frame[ai] = -1;
#endif
            }
#ifdef AFN_HAS_SCRIPT
            // Re-fire OnStart for BPs that live in the new scene. Without
            // this, scene-1 BPs only ran once at boot and never re-armed on
            // ChangeScene (e.g. the song never restarted in the new scene).
            extern void afn_bp_dispatch_start(void);
            afn_bp_dispatch_start();
#endif
        }
    } else {
        int t = afn_fade_counter;
        cur = (afn_fade_target * t) / (afn_fade_frames ? afn_fade_frames : 1);
        if (afn_fade_counter == 0) s_fade_phase = 0;
    }

    int level = cur < 0 ? -cur : cur;
    if (level > 16) level = 16;
    REG_MASTER_BRIGHT = (cur < 0)
        ? (level | (1 << 14))
        : (cur > 0 ? (level | (2 << 14)) : 0);
}

// ---------------------------------------------------------------------------
// Per-frame render: FPS camera + project meshes.
// ---------------------------------------------------------------------------
void afn_fps3d_update(void)
{
    update_camera();

    // Re-set projection every frame (init-time set doesn't survive to first
    // user frame — cheap to redo each frame and always correct).
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0 / 192.0, 0.1, 1024);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    int lookX = cam_x + (g_sinf << 2);
    int lookZ = cam_z + (g_cosf << 2);

    // Pitch the camera down so the horizon lands at m7_horizon (in NDS
    // screen pixels). sprites.c reads the same m7_horizon to align its
    // sprite projection — single source of truth keeps meshes + OAM
    // sprites lined up regardless of the pitch tuning constant.
    //   pitch_rad = screenOffPx * (70° / 192) * (π/180) ≈ screenOffPx * 0.00637
    //   lookY_offset_fx8 = look_dist_fx8 * pitch_rad
    //                    = 1024 * 0.00637 * screenOffPx ≈ 6.5 * screenOffPx
    // Round to 7 for cheap integer math (~6% high but visually fine).
    int lookY = cam_h;
    {
        m7_horizon = (AFN_CAM_HORIZON * 6) / 5;     // GBA px → NDS px
#ifdef AFN_CAM_SLOT_COUNT
        m7_horizon = cam_blend((AFN_CAM_HORIZON * 6) / 5, s_slotHorizon);  // blend pitch toward slot
#endif
        int screenOffPx = 96 - m7_horizon;          // +ve = look down
        lookY -= screenOffPx * 7;
    }

    gluLookAtf32(
        fx8_to_f32(cam_x), fx8_to_f32(cam_h), fx8_to_f32(cam_z),
        fx8_to_f32(lookX), fx8_to_f32(lookY), fx8_to_f32(lookZ),
        0, inttof32(1), 0
    );

#if defined(AFN_HAS_SKY) && AFN_HAS_SKY
    render_sky();
#endif
#if defined(AFN_MESH_COUNT) && AFN_MESH_COUNT > 0
    render_meshes();
#endif
#ifdef AFN_HAS_PLAYER_RIG
    render_player_rig();
#endif
#ifdef AFN_HAS_NPC_RIGS
    render_npc_rigs();
#endif

    glFlush(0);
}

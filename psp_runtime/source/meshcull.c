// Affinity PSP runtime — bucketed frustum culling + boundary clipping.
//
// The PSP GE has no guard-band CLIPPING: a triangle whose projected vertex
// overflows the guard band (a big triangle with a corner near the camera and/or
// far off-axis) is DROPPED whole, not clipped (the DS clips; the PSP doesn't).
// On a coarse floor that's large wedges of missing geometry at grazing angles.
//
// History of failed fixes: bucket culling with a bounding-SPHERE test mis-culled
// (dropped buckets that were actually on-screen); removing culling exposed the
// guard-band drops; load-time tessellation either exploded the triangle count
// (~4fps) or left leaves big enough to still drop; per-frame per-triangle frustum
// clipping fixed the wedges but classifying all ~7k triangles × 5 planes EVERY
// frame is CPU-bound — smooth facing away (triangles reject on the first plane)
// but it tanks FPS facing the scene (every triangle passes all five tests).
//
// This combines the two: a coarse bucket grid culls/accepts most geometry with
// an ACCURATE AABB-vs-frustum test (no sphere over-approximation, so no false
// culls), and only the few buckets straddling a frustum plane drop to exact
// per-triangle Sutherland-Hodgman clipping. So:
//   * bucket fully inside  -> draw its tris straight, no per-tri work, no clip
//                             (nothing in it can overflow the guard band)
//   * bucket fully outside  -> skip
//   * bucket straddles      -> per-triangle: inside tris drawn indexed, the few
//                             straddlers clipped into a small scratch buffer
// Result: culling-cheap when facing the scene, wedge-free, no mis-culls.
#include "meshcull.h"
#include "affinity_psp.h"
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>

// Must match the near distance in scene.c's sceGumPerspective(...).
#define NEAR_CLIP 4.0f

// Spatial grid resolution per axis (GN^3 buckets). Finer = more buckets fully
// inside/outside (cheap) and a thinner straddling shell (less per-tri clipping),
// at a little more per-bucket bookkeeping. 6^3 = 216 is a good fit for a level
// that's wide and flat.
#define MESH_GN      6
#define MESH_NBUCKET (MESH_GN * MESH_GN * MESH_GN)

typedef struct {
    float bmin[3], bmax[3];   // local-space AABB
    int   triStart;           // offset into s_idx[mi], in index units
    int   triCount;           // index count (3 per triangle)
} PspBucket;

static PspBucket*      s_buckets[256];
static int             s_bucketCount[256];
static unsigned short* s_idx[256];          // indices reordered grouped by bucket

// Per-frame shared scratch (one mesh drawn at a time):
static unsigned short* s_drawIdx = 0;       // collected inside-tri indices
static int             s_drawIdxCap = 0;
static AfnVertex*      s_scratch = 0;        // clipped straddler verts (non-indexed)
static int             s_scratchCap = 0;
static int             s_ready = 0;

static int cell_of(float cx, float cy, float cz,
                   float mnx, float mny, float mnz,
                   float ex, float ey, float ez) {
    int gx = (int)((cx - mnx) / ex * MESH_GN); if (gx < 0) gx = 0; if (gx >= MESH_GN) gx = MESH_GN - 1;
    int gy = (int)((cy - mny) / ey * MESH_GN); if (gy < 0) gy = 0; if (gy >= MESH_GN) gy = MESH_GN - 1;
    int gz = (int)((cz - mnz) / ez * MESH_GN); if (gz < 0) gz = 0; if (gz >= MESH_GN) gz = MESH_GN - 1;
    return (gx * MESH_GN + gy) * MESH_GN + gz;
}

void meshcull_build(void) {
    if (s_ready) return;
    s_ready = 1;
    int mc = afn_mesh_count;
    if (mc > 256) mc = 256;

    int maxIdx = 0, maxTri = 0;
    for (int mi = 0; mi < mc; mi++) {
        s_buckets[mi] = 0; s_bucketCount[mi] = 0; s_idx[mi] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        int ntri = m->indexCount / 3;
        if (ntri <= 0 || !m->verts || !m->indices) continue;
        if (m->indexCount > maxIdx) maxIdx = m->indexCount;
        if (ntri > maxTri) maxTri = ntri;
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;

        // Mesh AABB (to lay out the grid).
        float mnx=1e30f, mny=1e30f, mnz=1e30f, mxx=-1e30f, mxy=-1e30f, mxz=-1e30f;
        for (int v = 0; v < m->vertCount; v++) {
            float x = V[v].x, y = V[v].y, z = V[v].z;
            if (x<mnx)mnx=x; if (x>mxx)mxx=x;
            if (y<mny)mny=y; if (y>mxy)mxy=y;
            if (z<mnz)mnz=z; if (z>mxz)mxz=z;
        }
        float ex = mxx-mnx; if (ex<1e-4f) ex=1e-4f;
        float ey = mxy-mny; if (ey<1e-4f) ey=1e-4f;
        float ez = mxz-mnz; if (ez<1e-4f) ez=1e-4f;

        PspBucket* B = (PspBucket*)calloc(MESH_NBUCKET, sizeof(PspBucket));
        if (!B) continue;

        // Pass A: count indices per bucket (by triangle centroid).
        for (int t = 0; t < ntri; t++) {
            int a=I[t*3], b=I[t*3+1], c=I[t*3+2];
            float cx=(V[a].x+V[b].x+V[c].x)/3.0f;
            float cy=(V[a].y+V[b].y+V[c].y)/3.0f;
            float cz=(V[a].z+V[b].z+V[c].z)/3.0f;
            B[cell_of(cx,cy,cz,mnx,mny,mnz,ex,ey,ez)].triCount += 3;
        }
        int total = 0;
        for (int c = 0; c < MESH_NBUCKET; c++) {
            B[c].triStart = total; total += B[c].triCount; B[c].triCount = 0;
            B[c].bmin[0]=B[c].bmin[1]=B[c].bmin[2]= 1e30f;
            B[c].bmax[0]=B[c].bmax[1]=B[c].bmax[2]=-1e30f;
        }
        unsigned short* OI = (unsigned short*)malloc(total * sizeof(unsigned short));
        if (!OI) { free(B); continue; }

        // Pass B: scatter indices into per-bucket spans + grow per-bucket AABB.
        for (int t = 0; t < ntri; t++) {
            int a=I[t*3], b=I[t*3+1], c=I[t*3+2];
            float cx=(V[a].x+V[b].x+V[c].x)/3.0f;
            float cy=(V[a].y+V[b].y+V[c].y)/3.0f;
            float cz=(V[a].z+V[b].z+V[c].z)/3.0f;
            int cell = cell_of(cx,cy,cz,mnx,mny,mnz,ex,ey,ez);
            int p = B[cell].triStart + B[cell].triCount;
            OI[p]=a; OI[p+1]=b; OI[p+2]=c; B[cell].triCount += 3;
            int ids[3] = { a, b, c };
            for (int k = 0; k < 3; k++) {
                float x=V[ids[k]].x, y=V[ids[k]].y, z=V[ids[k]].z;
                if (x<B[cell].bmin[0]) B[cell].bmin[0]=x;
                if (x>B[cell].bmax[0]) B[cell].bmax[0]=x;
                if (y<B[cell].bmin[1]) B[cell].bmin[1]=y;
                if (y>B[cell].bmax[1]) B[cell].bmax[1]=y;
                if (z<B[cell].bmin[2]) B[cell].bmin[2]=z;
                if (z>B[cell].bmax[2]) B[cell].bmax[2]=z;
            }
        }

        sceKernelDcacheWritebackRange(OI, total * sizeof(unsigned short));
        s_buckets[mi] = B; s_bucketCount[mi] = MESH_NBUCKET; s_idx[mi] = OI;
    }

    if (maxIdx <= 0) return;
    s_drawIdxCap = maxIdx;
    s_drawIdx = (unsigned short*)memalign(16, sizeof(unsigned short) * s_drawIdxCap);
    if (!s_drawIdx) s_drawIdxCap = 0;
    s_scratchCap = maxTri * 2 * 3;   // straddlers fan a bit; generous
    s_scratch = (AfnVertex*)memalign(16, sizeof(AfnVertex) * s_scratchCap);
    if (!s_scratch) s_scratchCap = 0;
}

// Per-channel clamped lerp of two packed RGBA8888 colors.
static unsigned clerp(unsigned a, unsigned b, float t) {
    unsigned r = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        int v = ca + (int)((cb - ca) * t);
        if (v < 0) v = 0; else if (v > 255) v = 255;
        r |= ((unsigned)v) << s;
    }
    return r;
}

static AfnVertex vlerp(const AfnVertex* a, const AfnVertex* b, float t) {
    AfnVertex m;
    m.u = a->u + (b->u - a->u) * t;
    m.v = a->v + (b->v - a->v) * t;
    m.color = clerp(a->color, b->color, t);
    m.x = a->x + (b->x - a->x) * t;
    m.y = a->y + (b->y - a->y) * t;
    m.z = a->z + (b->z - a->z) * t;
    return m;
}

static int clip_plane(const AfnVertex* in, int nin, AfnVertex* out, const float* p) {
    int nout = 0;
    for (int i = 0; i < nin; i++) {
        const AfnVertex* cur = &in[i];
        const AfnVertex* nxt = &in[(i + 1 == nin) ? 0 : i + 1];
        float dc = p[0]*cur->x + p[1]*cur->y + p[2]*cur->z + p[3];
        float dn = p[0]*nxt->x + p[1]*nxt->y + p[2]*nxt->z + p[3];
        int ci = dc >= 0.0f, ni = dn >= 0.0f;
        if (ci) out[nout++] = *cur;
        if (ci != ni) { float t = dc / (dc - dn); out[nout++] = vlerp(cur, nxt, t); }
    }
    return nout;
}

static void clip_tri(const AfnVertex* a, const AfnVertex* b, const AfnVertex* c,
                     const float planes[][4], int nplanes,
                     AfnVertex* dst, int* wc, int cap) {
    AfnVertex buf0[24], buf1[24];
    AfnVertex* poly = buf0; AfnVertex* tmp = buf1;
    poly[0]=*a; poly[1]=*b; poly[2]=*c;
    int n = 3;
    for (int pi = 0; pi < nplanes && n >= 3; pi++) {
        n = clip_plane(poly, n, tmp, planes[pi]);
        AfnVertex* sw = poly; poly = tmp; tmp = sw;
    }
    if (n < 3) return;
    for (int i = 1; i + 1 < n; i++) {
        if (*wc + 3 > cap) return;
        dst[*wc]=poly[0]; dst[*wc+1]=poly[i]; dst[*wc+2]=poly[i+1]; *wc += 3;
    }
}

void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float fwdX, float fwdY, float fwdZ,
                   float rgtX, float rgtY, float rgtZ,
                   float upX,  float upY,  float upZ,
                   float tanH, float tanV, float drawDist) {
    (void)drawDist;
    const AfnMesh* m = &afn_meshes[meshIdx];
    PspBucket* B = s_buckets[meshIdx];
    unsigned short* MI = s_idx[meshIdx];

    // Fall back to a plain indexed draw when we can't bucket/clip safely.
    int rotated = (rotX != 0.0f || rotY != 0.0f || rotZ != 0.0f);
    if (!B || !MI || !s_drawIdx || !s_scratch || rotated) {
        sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS | GU_INDEX_16BIT,
                        m->indexCount, m->indices, m->verts);
        return;
    }

    // Camera into mesh-local space (translate + uniform scale; rotation excluded
    // above). near is in world units -> /scale for local.
    float s = (scale > 1e-6f) ? scale : 1.0f;
    float inv = 1.0f / s;
    float ex = (camX - ix) * inv, ey = (camY - iy) * inv, ez = (camZ - iz) * inv;
    float nearL = NEAR_CLIP * inv;

    // Frustum planes in local space: inside = n·v + d >= 0.
    float planes[5][4];
    planes[0][0]=fwdX; planes[0][1]=fwdY; planes[0][2]=fwdZ;
    planes[0][3]=-(fwdX*ex+fwdY*ey+fwdZ*ez) - nearL;
    { float nx=rgtX+tanH*fwdX, ny=rgtY+tanH*fwdY, nz=rgtZ+tanH*fwdZ;
      planes[1][0]=nx;planes[1][1]=ny;planes[1][2]=nz;planes[1][3]=-(nx*ex+ny*ey+nz*ez); }
    { float nx=-rgtX+tanH*fwdX, ny=-rgtY+tanH*fwdY, nz=-rgtZ+tanH*fwdZ;
      planes[2][0]=nx;planes[2][1]=ny;planes[2][2]=nz;planes[2][3]=-(nx*ex+ny*ey+nz*ez); }
    { float nx=upX+tanV*fwdX, ny=upY+tanV*fwdY, nz=upZ+tanV*fwdZ;
      planes[3][0]=nx;planes[3][1]=ny;planes[3][2]=nz;planes[3][3]=-(nx*ex+ny*ey+nz*ez); }
    { float nx=-upX+tanV*fwdX, ny=-upY+tanV*fwdY, nz=-upZ+tanV*fwdZ;
      planes[4][0]=nx;planes[4][1]=ny;planes[4][2]=nz;planes[4][3]=-(nx*ex+ny*ey+nz*ez); }

    const AfnVertex* V = m->verts;
    unsigned short* dIdx = s_drawIdx; int dCap = s_drawIdxCap; int dn = 0;
    AfnVertex* dst = s_scratch;       int sCap = s_scratchCap;  int wc = 0;
    int nb = s_bucketCount[meshIdx];

    for (int bi = 0; bi < nb; bi++) {
        PspBucket* bk = &B[bi];
        if (bk->triCount == 0) continue;

        // Accurate AABB-vs-frustum classify (p/n-vertex per plane: no false cull).
        int culled = 0, straddle = 0;
        for (int pi = 0; pi < 5; pi++) {
            const float* p = planes[pi];
            float px = (p[0]>=0.0f)?bk->bmax[0]:bk->bmin[0];
            float py = (p[1]>=0.0f)?bk->bmax[1]:bk->bmin[1];
            float pz = (p[2]>=0.0f)?bk->bmax[2]:bk->bmin[2];
            if (p[0]*px + p[1]*py + p[2]*pz + p[3] < 0.0f) { culled = 1; break; }
            float nx = (p[0]>=0.0f)?bk->bmin[0]:bk->bmax[0];
            float ny = (p[1]>=0.0f)?bk->bmin[1]:bk->bmax[1];
            float nz = (p[2]>=0.0f)?bk->bmin[2]:bk->bmax[2];
            if (p[0]*nx + p[1]*ny + p[2]*nz + p[3] < 0.0f) straddle = 1;
        }
        if (culled) continue;

        const unsigned short* span = &MI[bk->triStart];
        int sc = bk->triCount;

        if (!straddle) {
            // Whole bucket inside the frustum: draw its tris directly (indexed),
            // no per-triangle work, no clipping.
            if (dn + sc > dCap) sc = dCap - dn;
            for (int i = 0; i < sc; i++) dIdx[dn + i] = span[i];
            dn += sc;
            continue;
        }

        // Boundary bucket: per-triangle classify just these.
        for (int t = 0; t + 2 < sc; t += 3) {
            unsigned short ia = span[t], ib = span[t+1], ic = span[t+2];
            const AfnVertex* a = &V[ia];
            const AfnVertex* b = &V[ib];
            const AfnVertex* c = &V[ic];
            int allIn = 1, rej = 0;
            for (int pi = 0; pi < 5; pi++) {
                const float* p = planes[pi];
                float da = p[0]*a->x + p[1]*a->y + p[2]*a->z + p[3];
                float db = p[0]*b->x + p[1]*b->y + p[2]*b->z + p[3];
                float dc = p[0]*c->x + p[1]*c->y + p[2]*c->z + p[3];
                if (da<0.0f && db<0.0f && dc<0.0f) { rej = 1; break; }
                if (da<0.0f || db<0.0f || dc<0.0f) allIn = 0;
            }
            if (rej) continue;
            if (allIn) {
                if (dn + 3 > dCap) continue;
                dIdx[dn]=ia; dIdx[dn+1]=ib; dIdx[dn+2]=ic; dn += 3;
            } else {
                clip_tri(a, b, c, planes, 5, dst, &wc, sCap);
            }
        }
    }

    // Pass 1: all inside triangles, indexed into the original const verts.
    if (dn > 0) {
        sceKernelDcacheWritebackRange(dIdx, sizeof(unsigned short) * dn);
        const int ICHUNK = 65532;
        for (int off = 0; off < dn; off += ICHUNK) {
            int n = dn - off; if (n > ICHUNK) n = ICHUNK;
            sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS | GU_INDEX_16BIT,
                            n, dIdx + off, V);
        }
    }
    // Pass 2: clipped straddlers, non-indexed (thin shell, usually tiny).
    if (wc > 0) {
        sceKernelDcacheWritebackRange(dst, sizeof(AfnVertex) * wc);
        const int CHUNK = 65532;
        for (int off = 0; off < wc; off += CHUNK) {
            int n = wc - off; if (n > CHUNK) n = CHUNK;
            sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS, n, 0, dst + off);
        }
    }
}

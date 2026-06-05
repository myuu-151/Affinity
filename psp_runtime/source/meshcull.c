// Affinity PSP runtime — load-time triangle tessellation (see meshcull.h).
//
// Why this exists: the PSP GE has no near-plane / guard-band CLIPPING. When a
// triangle's projected vertex overflows the guard band — which happens for a
// big triangle that has one corner near the camera AND far off the view axis —
// the GE discards the WHOLE triangle rather than clipping it to the screen edge
// (the DS hardware clips; the PSP doesn't). On a coarse floor that reads as
// large wedges of missing geometry whenever you look across it at a grazing
// angle. No amount of frustum culling fixes it (culling only ever made it
// worse). The robust fix is to make the triangles small: subdivide any triangle
// whose world-space edge exceeds MESH_MAX_EDGE. Then the on-screen part of the
// floor is always tiled by small triangles whose vertices stay on-screen (no
// overflow), and only the tiny slivers straddling the off-screen corner can
// drop — and those are off-screen anyway, so nothing visible is lost.
//
// Done once at load (no per-frame cost). Already-fine meshes are left as their
// original indexed buffers; only meshes that actually contain oversized
// triangles are rebuilt into a flat (non-indexed) tessellated buffer.
#include "meshcull.h"
#include "affinity_psp.h"
#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>

// World-space edge length above which a triangle is subdivided. The scene spans
// hundreds of units with the camera ~18 units from the player, so 20 keeps the
// near floor finely tiled while leaving the already-dense detail untouched.
// Raise it if fill/transform cost ever shows; lower it if any wedge survives.
#define MESH_MAX_EDGE  20.0f
#define MESH_MAX_DEPTH 4          // 4^4 = up to 256x per original triangle

static AfnVertex* s_tess[256];    // flat (3 verts/tri) tessellated buffer, or 0
static int        s_tessVc[256];  // vertex count in s_tess[mi]
static int        s_ready = 0;

// First instance scale that references this mesh (1.0 if none / unscaled).
static float mesh_scale(int mi) {
    for (int si = 0; si < afn_sprite_count; si++)
        if (afn_sprites[si].meshIdx == mi) return afn_sprites[si].scale;
    return 1.0f;
}

// Midpoint vertex: pos/uv interpolate linearly (exact across a planar triangle),
// color averaged per channel.
static AfnVertex vmid(const AfnVertex* a, const AfnVertex* b) {
    AfnVertex m;
    m.u = (a->u + b->u) * 0.5f;
    m.v = (a->v + b->v) * 0.5f;
    unsigned ca = a->color, cb = b->color, mc = 0;
    for (int s = 0; s < 32; s += 8)
        mc |= ((((ca >> s) & 0xFF) + ((cb >> s) & 0xFF)) >> 1) << s;
    m.color = mc;
    m.x = (a->x + b->x) * 0.5f;
    m.y = (a->y + b->y) * 0.5f;
    m.z = (a->z + b->z) * 0.5f;
    return m;
}

// True if any edge (squared) exceeds the local-space limit.
static int tri_big(const AfnVertex* a, const AfnVertex* b, const AfnVertex* c, float s2max) {
    float dx, dy, dz;
    dx=a->x-b->x; dy=a->y-b->y; dz=a->z-b->z; if (dx*dx+dy*dy+dz*dz > s2max) return 1;
    dx=b->x-c->x; dy=b->y-c->y; dz=b->z-c->z; if (dx*dx+dy*dy+dz*dz > s2max) return 1;
    dx=c->x-a->x; dy=c->y-a->y; dz=c->z-a->z; if (dx*dx+dy*dy+dz*dz > s2max) return 1;
    return 0;
}

// Recurse: split into 4 until small enough or at depth cap. If out!=0, write 3
// verts per leaf triangle at *oc; otherwise just advance *oc (counting pass).
static void tess_rec(AfnVertex* out, int* oc,
                     const AfnVertex* a, const AfnVertex* b, const AfnVertex* c,
                     float s2max, int depth) {
    if (depth >= MESH_MAX_DEPTH || !tri_big(a, b, c, s2max)) {
        if (out) { out[*oc] = *a; out[*oc+1] = *b; out[*oc+2] = *c; }
        *oc += 3;
        return;
    }
    AfnVertex m0 = vmid(a, b), m1 = vmid(b, c), m2 = vmid(c, a);
    tess_rec(out, oc, a,   &m0, &m2, s2max, depth+1);
    tess_rec(out, oc, &m0, b,   &m1, s2max, depth+1);
    tess_rec(out, oc, &m2, &m1, c,   s2max, depth+1);
    tess_rec(out, oc, &m0, &m1, &m2, s2max, depth+1);
}

void meshcull_build(void) {
    if (s_ready) return;
    s_ready = 1;
    int mc = afn_mesh_count; if (mc > 256) mc = 256;

    for (int mi = 0; mi < mc; mi++) {
        s_tess[mi] = 0; s_tessVc[mi] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        int ntri = m->indexCount / 3;
        if (ntri <= 0 || !m->verts || !m->indices) continue;

        // Compare edges in local space (verts are pre-scale): the world edge is
        // local*scale, so the local limit is MAX_EDGE/scale.
        float scale = mesh_scale(mi);
        float lim   = (scale > 1e-6f) ? (MESH_MAX_EDGE / scale) : MESH_MAX_EDGE;
        float s2max = lim * lim;

        const AfnVertex*      V = m->verts;
        const unsigned short* I = m->indices;

        // Already fine? Leave the original indexed buffer in place (no memory).
        int anyBig = 0;
        for (int t = 0; t < ntri && !anyBig; t++)
            anyBig = tri_big(&V[I[t*3]], &V[I[t*3+1]], &V[I[t*3+2]], s2max);
        if (!anyBig) continue;

        // Pass 1: count output verts.
        int oc = 0;
        for (int t = 0; t < ntri; t++)
            tess_rec(0, &oc, &V[I[t*3]], &V[I[t*3+1]], &V[I[t*3+2]], s2max, 0);

        AfnVertex* out = (AfnVertex*)memalign(16, sizeof(AfnVertex) * oc);
        if (!out) continue;   // OOM: fall back to the original (still drawable)

        // Pass 2: fill.
        int wc = 0;
        for (int t = 0; t < ntri; t++)
            tess_rec(out, &wc, &V[I[t*3]], &V[I[t*3+1]], &V[I[t*3+2]], s2max, 0);

        sceKernelDcacheWritebackRange(out, sizeof(AfnVertex) * oc);
        s_tess[mi] = out; s_tessVc[mi] = oc;
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
    (void)ix;(void)iy;(void)iz;(void)scale;(void)rotY;(void)rotX;(void)rotZ;
    (void)camX;(void)camY;(void)camZ;(void)fwdX;(void)fwdY;(void)fwdZ;
    (void)rgtX;(void)rgtY;(void)rgtZ;(void)upX;(void)upY;(void)upZ;
    (void)tanH;(void)tanV;(void)drawDist;

    if (s_tess[meshIdx]) {
        // Flat, non-indexed: oversized triangles were subdivided at load. The GE
        // primitive count is 16-bit (max 65535), and tessellation easily blows
        // past that, so submit in <=65532-vertex chunks (multiple of 3).
        int total = s_tessVc[meshIdx];
        AfnVertex* base = s_tess[meshIdx];
        const int CHUNK = 65532;
        for (int off = 0; off < total; off += CHUNK) {
            int n = total - off; if (n > CHUNK) n = CHUNK;
            sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS, n, 0, base + off);
        }
    } else {
        const AfnMesh* m = &afn_meshes[meshIdx];
        sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS | GU_INDEX_16BIT,
                        m->indexCount, m->indices, m->verts);
    }
}

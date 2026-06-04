// Affinity PSP runtime — spatial bucketing + frustum culling (see meshcull.h).
#include "meshcull.h"
#include "affinity_psp.h"
#include <pspgu.h>
#include <pspgum.h>
#include <stdlib.h>
#include <math.h>

#define MESH_GN      4
#define MESH_NBUCKET (MESH_GN * MESH_GN * MESH_GN)

// Frustum half-angle tangents for sceGumPerspective(75, 480/272). Padded a
// touch wider than the true cone so visible chunks never pop at the edges.
#define TAN_H 1.50f   // horizontal (true ~1.35)
#define TAN_V 0.90f   // vertical   (true ~0.77)
#define NEAR_EPS 0.5f

typedef struct {
    float cx, cy, cz, radius;   // local-space bounding sphere
    int   triStart, triCount;   // span into s_idx[mi] (index units, 3 per tri)
} PspBucket;

static PspBucket*      s_buckets[256];
static int             s_bucketCount[256];
static unsigned short* s_idx[256];          // reordered indices grouped by bucket

static int s_ready = 0;

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

    for (int mi = 0; mi < mc; mi++) {
        s_buckets[mi] = 0; s_bucketCount[mi] = 0; s_idx[mi] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        int ntri = m->indexCount / 3;
        if (ntri <= 0 || !m->verts || !m->indices) continue;
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;

        // Mesh AABB.
        float mnx = 1e30f, mny = 1e30f, mnz = 1e30f;
        float mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
        for (int v = 0; v < m->vertCount; v++) {
            float x = V[v].x, y = V[v].y, z = V[v].z;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
            if (z < mnz) mnz = z;
            if (z > mxz) mxz = z;
        }
        float ex = mxx - mnx; if (ex < 1e-4f) ex = 1e-4f;
        float ey = mxy - mny; if (ey < 1e-4f) ey = 1e-4f;
        float ez = mxz - mnz; if (ez < 1e-4f) ez = 1e-4f;

        PspBucket* B = (PspBucket*)calloc(MESH_NBUCKET, sizeof(PspBucket));
        if (!B) continue;

        // Pass A: count indices per bucket.
        for (int t = 0; t < ntri; t++) {
            int a = I[t*3], b = I[t*3+1], c = I[t*3+2];
            float cx = (V[a].x + V[b].x + V[c].x) / 3.0f;
            float cy = (V[a].y + V[b].y + V[c].y) / 3.0f;
            float cz = (V[a].z + V[b].z + V[c].z) / 3.0f;
            B[cell_of(cx,cy,cz,mnx,mny,mnz,ex,ey,ez)].triCount += 3;
        }
        int total = 0;
        for (int c = 0; c < MESH_NBUCKET; c++) {
            B[c].triStart = total; total += B[c].triCount; B[c].triCount = 0;
        }
        unsigned short* OI = (unsigned short*)malloc(total * sizeof(unsigned short));
        if (!OI) { free(B); continue; }

        // Per-bucket AABB while scattering.
        float bmnx[MESH_NBUCKET], bmny[MESH_NBUCKET], bmnz[MESH_NBUCKET];
        float bmxx[MESH_NBUCKET], bmxy[MESH_NBUCKET], bmxz[MESH_NBUCKET];
        for (int c = 0; c < MESH_NBUCKET; c++) {
            bmnx[c]=bmny[c]=bmnz[c]= 1e30f;
            bmxx[c]=bmxy[c]=bmxz[c]=-1e30f;
        }
        for (int t = 0; t < ntri; t++) {
            int a = I[t*3], b = I[t*3+1], c = I[t*3+2];
            float cx = (V[a].x + V[b].x + V[c].x) / 3.0f;
            float cy = (V[a].y + V[b].y + V[c].y) / 3.0f;
            float cz = (V[a].z + V[b].z + V[c].z) / 3.0f;
            int cell = cell_of(cx,cy,cz,mnx,mny,mnz,ex,ey,ez);
            int p = B[cell].triStart + B[cell].triCount;
            OI[p] = a; OI[p+1] = b; OI[p+2] = c; B[cell].triCount += 3;
            int ids[3] = { a, b, c };
            for (int k = 0; k < 3; k++) {
                float x = V[ids[k]].x, y = V[ids[k]].y, z = V[ids[k]].z;
                if (x < bmnx[cell]) bmnx[cell] = x;
                if (x > bmxx[cell]) bmxx[cell] = x;
                if (y < bmny[cell]) bmny[cell] = y;
                if (y > bmxy[cell]) bmxy[cell] = y;
                if (z < bmnz[cell]) bmnz[cell] = z;
                if (z > bmxz[cell]) bmxz[cell] = z;
            }
        }
        for (int c = 0; c < MESH_NBUCKET; c++) {
            if (B[c].triCount == 0) continue;
            float ccx=(bmnx[c]+bmxx[c])*0.5f, ccy=(bmny[c]+bmxy[c])*0.5f, ccz=(bmnz[c]+bmxz[c])*0.5f;
            float dx=bmxx[c]-ccx, dy=bmxy[c]-ccy, dz=bmxz[c]-ccz;
            B[c].cx=ccx; B[c].cy=ccy; B[c].cz=ccz;
            B[c].radius = sqrtf(dx*dx+dy*dy+dz*dz);
        }
        s_buckets[mi] = B; s_bucketCount[mi] = MESH_NBUCKET; s_idx[mi] = OI;
    }
}

void meshcull_draw(int meshIdx,
                   float ix, float iy, float iz,
                   float scale, float rotY, float rotX, float rotZ,
                   float camX, float camY, float camZ,
                   float camSin, float camCos, float drawDist) {
    const AfnMesh* m = &afn_meshes[meshIdx];
    int nb = (meshIdx < 256) ? s_bucketCount[meshIdx] : 0;

    if (nb <= 0 || !s_buckets[meshIdx] || !s_idx[meshIdx]) {
        // Fallback: no buckets (OOM) — draw the whole mesh.
        sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS | GU_INDEX_16BIT,
                        m->indexCount, m->indices, m->verts);
        return;
    }

    const PspBucket* B = s_buckets[meshIdx];
    const unsigned short* OI = s_idx[meshIdx];
    float ry = rotY * (3.14159265f/180.0f);
    float rx = rotX * (3.14159265f/180.0f);
    float rz = rotZ * (3.14159265f/180.0f);
    float cY=cosf(ry), sY=sinf(ry), cX=cosf(rx), sX=sinf(rx), cZ=cosf(rz), sZ=sinf(rz);

    for (int c = 0; c < nb; c++) {
        const PspBucket* bk = &B[c];
        if (bk->triCount == 0) continue;

        // Local center -> world (scale -> Ry -> Rx -> Rz -> translate).
        float lx = bk->cx * scale, ly = bk->cy * scale, lz = bk->cz * scale;
        float ax =  lx*cY + lz*sY,  az = -lx*sY + lz*cY,  ay = ly;
        float ay2 = ay*cX - az*sX,  az2 = ay*sX + az*cX;
        float ax2 = ax*cZ - ay2*sZ, ay3 = ax*sZ + ay2*cZ;
        float wx = ix + ax2, wy = iy + ay3, wz = iz + az2;
        float r = bk->radius * scale;

        float dx = wx - camX, dy = wy - camY, dz = wz - camZ;
        float depth = camSin*dx + camCos*dz;
        if (depth + r < NEAR_EPS) continue;
        if (drawDist > 0.0f && depth - r > drawDist) continue;
        float viewX = -camCos*dx + camSin*dz; if (viewX < 0) viewX = -viewX;
        if (viewX - r > depth * TAN_H) continue;
        float viewY = dy < 0 ? -dy : dy;
        if (viewY - r > depth * TAN_V) continue;

        sceGumDrawArray(GU_TRIANGLES, AFN_VERTEX_FLAGS | GU_INDEX_16BIT,
                        bk->triCount, &OI[bk->triStart], m->verts);
    }
}

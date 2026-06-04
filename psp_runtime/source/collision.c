// Affinity PSP runtime — mesh collision (see collision.h).
// Faces are built once from the exported mesh geometry: each mesh-instance
// triangle is transformed to world space (same scale->Ry->Rx->Rz->translate as
// the renderer), classified floor/wall/ceiling by its normal, and bucketed into
// an XZ grid for cheap per-cell queries. Float port of nds_runtime/collision.c.
#include "collision.h"
#include "affinity_psp.h"
#include <math.h>
#include <stdlib.h>

#define COL_GN     16
#define COL_NCELL  (COL_GN * COL_GN)

typedef struct {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    float nx, ny, nz;   // full unit face normal
    int   flags;        // 1 = floor, 2 = ceiling, 4 = wall
} ColFace;

static ColFace* s_faces = 0;
static int      s_faceCount = 0;
static int*     s_cellStart = 0;
static int*     s_cellCount = 0;
static int*     s_cellFaces = 0;
static float    s_minX, s_minZ, s_cellSize;
static int      s_ready = 0;

static int cell_x(float x) { int c = (int)((x - s_minX) / s_cellSize); return c < 0 ? 0 : (c >= COL_GN ? COL_GN-1 : c); }
static int cell_z(float z) { int c = (int)((z - s_minZ) / s_cellSize); return c < 0 ? 0 : (c >= COL_GN ? COL_GN-1 : c); }

void collide_build(void) {
    if (s_ready) return;
    s_ready = 1;
#if defined(AFN_SPRITE_COUNT) || 1
    // Count triangles across all mesh instances.
    int total = 0;
    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        total += afn_meshes[mi].indexCount / 3;
    }
    if (total <= 0) return;
    s_faces = (ColFace*)malloc(sizeof(ColFace) * total);
    if (!s_faces) return;

    float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;

    for (int si = 0; si < afn_sprite_count; si++) {
        const AfnSpriteInst* sp = &afn_sprites[si];
        int mi = sp->meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;

        float ry = sp->rotY * (3.14159265f/180.0f);
        float rx = sp->rotX * (3.14159265f/180.0f);
        float rz = sp->rotZ * (3.14159265f/180.0f);
        float cY=cosf(ry), sY=sinf(ry), cX=cosf(rx), sX=sinf(rx), cZ=cosf(rz), sZ=sinf(rz);
        float scl = sp->scale;

        for (int t = 0; t + 3 <= m->indexCount; t += 3) {
            float wp[9];
            for (int k = 0; k < 3; k++) {
                const AfnVertex* vv = &V[I[t+k]];
                float lx = vv->x * scl, ly = vv->y * scl, lz = vv->z * scl;
                float ax =  lx*cY + lz*sY, az = -lx*sY + lz*cY, ay = ly;     // Ry
                float ay2 = ay*cX - az*sX, az2 = ay*sX + az*cX;              // Rx
                float ax2 = ax*cZ - ay2*sZ, ay3 = ax*sZ + ay2*cZ;           // Rz
                wp[k*3+0] = sp->x + ax2;
                wp[k*3+1] = sp->y + ay3;
                wp[k*3+2] = sp->z + az2;
            }
            // Normal.
            float e1x=wp[3]-wp[0], e1y=wp[4]-wp[1], e1z=wp[5]-wp[2];
            float e2x=wp[6]-wp[0], e2y=wp[7]-wp[1], e2z=wp[8]-wp[2];
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float len=sqrtf(nx*nx+ny*ny+nz*nz);
            if (len < 1e-6f) continue;
            nx/=len; ny/=len; nz/=len;
            ColFace* F = &s_faces[s_faceCount++];
            F->ax=wp[0];F->ay=wp[1];F->az=wp[2];
            F->bx=wp[3];F->by=wp[4];F->bz=wp[5];
            F->cx=wp[6];F->cy=wp[7];F->cz=wp[8];
            F->flags = (ny > 0.3f) ? 1 : (ny < -0.7f) ? 2 : 4;
            F->nx = nx; F->ny = ny; F->nz = nz;   // full unit normal
            for (int k=0;k<3;k++){ float X=wp[k*3], Z=wp[k*3+2];
                if(X<mnx)mnx=X; if(X>mxx)mxx=X; if(Z<mnz)mnz=Z; if(Z>mxz)mxz=Z; }
        }
    }
    if (s_faceCount <= 0) return;

    // Grid over the world XZ bounds.
    s_minX = mnx; s_minZ = mnz;
    float span = (mxx-mnx) > (mxz-mnz) ? (mxx-mnx) : (mxz-mnz);
    s_cellSize = span / COL_GN; if (s_cellSize < 1.0f) s_cellSize = 1.0f;

    s_cellStart = (int*)calloc(COL_NCELL, sizeof(int));
    s_cellCount = (int*)calloc(COL_NCELL, sizeof(int));
    if (!s_cellStart || !s_cellCount) return;

    // Pass A: count (a face goes in every cell its XZ AABB overlaps).
    for (int i = 0; i < s_faceCount; i++) {
        const ColFace* F = &s_faces[i];
        float x0=F->ax,x1=F->bx,x2=F->cx, z0=F->az,z1=F->bz,z2=F->cz;
        float mnX=fminf(x0,fminf(x1,x2)), mxX=fmaxf(x0,fmaxf(x1,x2));
        float mnZ=fminf(z0,fminf(z1,z2)), mxZ=fmaxf(z0,fmaxf(z1,z2));
        for (int gz=cell_z(mnZ); gz<=cell_z(mxZ); gz++)
            for (int gx=cell_x(mnX); gx<=cell_x(mxX); gx++)
                s_cellCount[gz*COL_GN+gx]++;
    }
    int totalEntries = 0;
    for (int c = 0; c < COL_NCELL; c++) { s_cellStart[c]=totalEntries; totalEntries+=s_cellCount[c]; s_cellCount[c]=0; }
    s_cellFaces = (int*)malloc(sizeof(int) * (totalEntries > 0 ? totalEntries : 1));
    if (!s_cellFaces) return;
    // Pass B: fill.
    for (int i = 0; i < s_faceCount; i++) {
        const ColFace* F = &s_faces[i];
        float x0=F->ax,x1=F->bx,x2=F->cx, z0=F->az,z1=F->bz,z2=F->cz;
        float mnX=fminf(x0,fminf(x1,x2)), mxX=fmaxf(x0,fmaxf(x1,x2));
        float mnZ=fminf(z0,fminf(z1,z2)), mxZ=fmaxf(z0,fmaxf(z1,z2));
        for (int gz=cell_z(mnZ); gz<=cell_z(mxZ); gz++)
            for (int gx=cell_x(mnX); gx<=cell_x(mxX); gx++) {
                int c = gz*COL_GN+gx;
                s_cellFaces[s_cellStart[c] + s_cellCount[c]++] = i;
            }
    }
#endif
}

#define PLAYER_RADIUS 6.0f
#define PLAYER_HEIGHT 24.0f

int collide_floor(float x, float z, float py, float* outY, float* outN) {
    if (!s_cellFaces) return 0;
    int c = cell_z(z)*COL_GN + cell_x(x);
    int start = s_cellStart[c], count = s_cellCount[c];
    float bestY = 0; int found = 0; const ColFace* bestF = 0;
    for (int i = 0; i < count; i++) {
        const ColFace* F = &s_faces[s_cellFaces[start+i]];
        if (!(F->flags & 1)) continue;
        // Barycentric in XZ.
        float c0 = (F->bx-F->ax)*(z-F->az) - (F->bz-F->az)*(x-F->ax);
        float c1 = (F->cx-F->bx)*(z-F->bz) - (F->cz-F->bz)*(x-F->bx);
        float c2 = (F->ax-F->cx)*(z-F->cz) - (F->az-F->cz)*(x-F->cx);
        if (!((c0>=0&&c1>=0&&c2>=0)||(c0<=0&&c1<=0&&c2<=0))) continue;
        float cs = c0+c1+c2;
        float fy = (cs==0) ? (F->ay+F->by+F->cy)/3.0f
                           : (c1*F->ay + c2*F->by + c0*F->cy)/cs;
        if (fy > py + PLAYER_HEIGHT) continue;   // ignore floors above the head
        if (!found || fy > bestY) { bestY = fy; found = 1; bestF = F; }
    }
    *outY = bestY;
    if (outN) {
        if (bestF) { outN[0]=bestF->nx; outN[1]=bestF->ny; outN[2]=bestF->nz; }
        else { outN[0]=0; outN[1]=1; outN[2]=0; }
    }
    return found;
}

void collide_walls(float* x, float* z, float py) {
    if (!s_cellFaces) return;
    float ppx = *x, ppz = *z;
    int c = cell_z(ppz)*COL_GN + cell_x(ppx);
    int start = s_cellStart[c], count = s_cellCount[c];
    for (int i = 0; i < count; i++) {
        const ColFace* F = &s_faces[s_cellFaces[start+i]];
        if (!(F->flags & 4)) continue;
        float fMinY = fminf(F->ay, fminf(F->by, F->cy));
        float fMaxY = fmaxf(F->ay, fmaxf(F->by, F->cy));
        if (py + PLAYER_HEIGHT < fMinY || py >= fMaxY) continue;
        // XZ-normalized wall normal (face normal stored full).
        float xl = sqrtf(F->nx*F->nx + F->nz*F->nz);
        if (xl < 1e-4f) continue;
        float wnx = F->nx/xl, wnz = F->nz/xl;
        float dist = (ppx - F->ax)*wnx + (ppz - F->az)*wnz;
        float ad = dist < 0 ? -dist : dist;
        if (ad >= PLAYER_RADIUS) continue;
        // XZ AABB pad pre-check.
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        if (ppx<mnX-PLAYER_RADIUS||ppx>mxX+PLAYER_RADIUS||ppz<mnZ-PLAYER_RADIUS||ppz>mxZ+PLAYER_RADIUS) continue;
        float push = PLAYER_RADIUS - ad;
        float s = dist >= 0 ? 1.0f : -1.0f;
        ppx += wnx * push * s;
        ppz += wnz * push * s;
    }
    *x = ppx; *z = ppz;
}

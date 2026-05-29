// Affinity NDS — mesh collision (ported from gba_runtime/main.c).
//
// Two ops: collide_walls (push player out of wall faces in XZ) and
// collide_floor (find highest floor under player + interpolate Y).
// Both walk a per-cell list of face indices via a fixed 8x8 spatial grid
// over the world XZ plane (afn_col_grid_*). Player radius / height are
// constants; tunable later if needed.

#include "affinity.h"
#include "mapdata.h"

#ifdef AFN_COL_FACE_COUNT

#define COL_PLAYER_RADIUS 768   // 3 px in 16.8
// afn_player_height moved to script-side afn_player_height (set by
// SetPlayerHeight node). Default lives in script_glue.c at 3072.

int afn_wall_collided_sprite = -1;
int afn_floor_sprite = -1;   // sprite index of the floor face under the player (-1 = none)

#ifndef AFN_COL_GRID_ORIGIN_X
#define AFN_COL_GRID_ORIGIN_X 0
#define AFN_COL_GRID_ORIGIN_Z 0
#endif

void afn_collide_walls(int *px, int *pz, int py)
{
    afn_wall_collided_sprite = -1;
    int gx = (*px - AFN_COL_GRID_ORIGIN_X) >> AFN_COL_GRID_SHIFT;
    int gz = (*pz - AFN_COL_GRID_ORIGIN_Z) >> AFN_COL_GRID_SHIFT;
    if (gx < 0) gx = 0; if (gx >= AFN_COL_GRID_SIZE) gx = AFN_COL_GRID_SIZE - 1;
    if (gz < 0) gz = 0; if (gz >= AFN_COL_GRID_SIZE) gz = AFN_COL_GRID_SIZE - 1;

    int ci    = gz * AFN_COL_GRID_SIZE + gx;
    int start = afn_col_grid_start[ci];
    int count = afn_col_grid_count[ci];
    int ppx = *px, ppz = *pz;

    for (int i = 0; i < count; i++) {
        const CollFace *face = &afn_col_faces[afn_col_grid_faces[start + i]];
        if (!(face->flags & 4)) continue;   // wall faces only

        int y0 = face->v0y, y1 = face->v1y, y2 = face->v2y;
        int fMinY = y0 < y1 ? y0 : y1; if (y2 < fMinY) fMinY = y2;
        int fMaxY = y0 > y1 ? y0 : y1; if (y2 > fMaxY) fMaxY = y2;
        // py > fMaxY: player above the wall entirely. Use >= so a wall whose
        // top is flush with the player's feet (e.g. you standing on top of a
        // submesh box, trying to step onto the adjacent submesh) doesn't
        // block — without this the 90° side wall of each submesh stops you
        // from walking off the top edge of a separated-but-coplanar mesh.
        if (py + afn_player_height < fMinY || py >= fMaxY) continue;

        int dist = (((ppx - face->v0x) >> 4) * face->nx +
                    ((ppz - face->v0z) >> 4) * face->nz) >> 4;
        int absDist = dist < 0 ? -dist : dist;
        if (absDist >= COL_PLAYER_RADIUS) continue;

        // Quick XZ AABB pre-check.
        int fx0 = face->v0x, fx1 = face->v1x, fx2 = face->v2x;
        int fz0 = face->v0z, fz1 = face->v1z, fz2 = face->v2z;
        int fMinX = fx0 < fx1 ? fx0 : fx1; if (fx2 < fMinX) fMinX = fx2;
        int fMaxX = fx0 > fx1 ? fx0 : fx1; if (fx2 > fMaxX) fMaxX = fx2;
        int fMinZ = fz0 < fz1 ? fz0 : fz1; if (fz2 < fMinZ) fMinZ = fz2;
        int fMaxZ = fz0 > fz1 ? fz0 : fz1; if (fz2 > fMaxZ) fMaxZ = fz2;
        int pad = COL_PLAYER_RADIUS;
        if (ppx < fMinX - pad || ppx > fMaxX + pad ||
            ppz < fMinZ - pad || ppz > fMaxZ + pad) continue;

        int push = COL_PLAYER_RADIUS - absDist;
        if (dist >= 0) {
            ppx += (face->nx * push) >> 8;
            ppz += (face->nz * push) >> 8;
        } else {
            ppx -= (face->nx * push) >> 8;
            ppz -= (face->nz * push) >> 8;
        }
        if (face->sprIdx >= 0) afn_wall_collided_sprite = face->sprIdx;
    }
    *px = ppx; *pz = ppz;
}

int afn_collide_floor(int px, int pz, int py, int *outY)
{
    int gx = (px - AFN_COL_GRID_ORIGIN_X) >> AFN_COL_GRID_SHIFT;
    int gz = (pz - AFN_COL_GRID_ORIGIN_Z) >> AFN_COL_GRID_SHIFT;
    if (gx < 0) gx = 0; if (gx >= AFN_COL_GRID_SIZE) gx = AFN_COL_GRID_SIZE - 1;
    if (gz < 0) gz = 0; if (gz >= AFN_COL_GRID_SIZE) gz = AFN_COL_GRID_SIZE - 1;

    int ci    = gz * AFN_COL_GRID_SIZE + gx;
    int start = afn_col_grid_start[ci];
    int count = afn_col_grid_count[ci];

    int bestY = 0, found = 0, bestSpr = -1;
    int ppx = px >> 8, ppz = pz >> 8;       // integer pixels for cross-product math

    for (int i = 0; i < count; i++) {
        const CollFace *face = &afn_col_faces[afn_col_grid_faces[start + i]];
        if (!(face->flags & 1)) continue;   // floor faces only

        int ax = face->v0x >> 8, az = face->v0z >> 8;
        int bx = face->v1x >> 8, bz = face->v1z >> 8;
        int cx = face->v2x >> 8, cz = face->v2z >> 8;
        int c0 = (bx - ax) * (ppz - az) - (bz - az) * (ppx - ax);
        int c1 = (cx - bx) * (ppz - bz) - (cz - bz) * (ppx - bx);
        int c2 = (ax - cx) * (ppz - cz) - (az - cz) * (ppx - cx);
        if (!((c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0)))
            continue;

        int floorY;
        int cSum = c0 + c1 + c2;
        if (cSum == 0) {
            floorY = ((face->v0y + face->v1y + face->v2y) * 341) >> 10;
        } else {
            int c0s = c0 >> 4, c1s = c1 >> 4, c2s = c2 >> 4;
            int cs = c0s + c1s + c2s;
            if (cs == 0)
                floorY = ((face->v0y + face->v1y + face->v2y) * 341) >> 10;
            else
                floorY = (c1s * face->v0y + c2s * face->v1y + c0s * face->v2y) / cs;
        }

        if (floorY > py + afn_player_height) continue;
        if (!found || floorY > bestY) { bestY = floorY; found = 1; bestSpr = face->sprIdx; }
    }
    *outY = bestY;
    afn_floor_sprite = found ? bestSpr : -1;   // which sprite's floor we're on (grind needs this)
    return found;
}

#endif // AFN_COL_FACE_COUNT

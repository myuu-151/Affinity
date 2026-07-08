/*
 * nav_bridge.cpp — Detour navmesh query wrapper for PS Vita.
 * Ported from Nebula-Dreamcast-Engine DetourBridge.cpp (Dreamcast/KOS); the
 * only changes are the afn_nav_* names, Affinity-scale query extents, and a
 * 2048 node pool (the Vita is not memory-starved like the DC's 16 MB).
 *
 * All public functions are extern "C" so the C runtime (main.c) calls them
 * directly. The navmesh blob lives in psv_nav.h (afn_navmesh_bin), baked by
 * the editor's Recast build at export time.
 */

#include "nav_bridge.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "DetourStatus.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- internal state ---- */
static dtNavMesh*      sNavMesh  = nullptr;
static dtNavMeshQuery* sQuery    = nullptr;

static const int kMaxNodePool  = 2048;
static const int kMaxPathPolys = 256;   /* max polys in a single path query */

/* Query extents in PSV world units (world ~256 across, player radius 6). */
static const float kHalfExt[3] = { 16.0f, 48.0f, 16.0f };

/* ---- helpers ---- */

/* Simple xorshift32 PRNG (deterministic, no libc rand state coupling). */
static unsigned int sRandState = 1;
static float NavRandFloat(void)
{
    sRandState ^= sRandState << 13;
    sRandState ^= sRandState >> 17;
    sRandState ^= sRandState << 5;
    return (float)(sRandState & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

/* ---- public API ---- */

extern "C" int afn_nav_init(const void* navData, int navDataSize)
{
    afn_nav_free();

    if (!navData || navDataSize <= 0) return 0;

    /* dtNavMesh::init() takes ownership of the data pointer and will free it
       with dtFree().  We must give it a dtAlloc'd copy. */
    unsigned char* copy = (unsigned char*)dtAlloc(navDataSize, DT_ALLOC_PERM);
    if (!copy) return 0;
    memcpy(copy, navData, (size_t)navDataSize);

    sNavMesh = dtAllocNavMesh();
    if (!sNavMesh) { dtFree(copy); return 0; }

    dtStatus st = sNavMesh->init(copy, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(st))
    {
        dtFreeNavMesh(sNavMesh);
        sNavMesh = nullptr;
        return 0;
    }

    sQuery = dtAllocNavMeshQuery();
    if (!sQuery)
    {
        dtFreeNavMesh(sNavMesh);
        sNavMesh = nullptr;
        return 0;
    }

    st = sQuery->init(sNavMesh, kMaxNodePool);
    if (dtStatusFailed(st))
    {
        dtFreeNavMeshQuery(sQuery);
        sQuery = nullptr;
        dtFreeNavMesh(sNavMesh);
        sNavMesh = nullptr;
        return 0;
    }

    return 1;
}

extern "C" void afn_nav_free(void)
{
    if (sQuery) { dtFreeNavMeshQuery(sQuery); sQuery = nullptr; }
    if (sNavMesh) { dtFreeNavMesh(sNavMesh); sNavMesh = nullptr; }
}

extern "C" int afn_nav_is_ready(void)
{
    return (sNavMesh && sQuery) ? 1 : 0;
}

extern "C" int afn_nav_find_path(float sx, float sy, float sz,
                                 float gx, float gy, float gz,
                                 float* outPath, int maxPoints)
{
    if (!sQuery || !outPath || maxPoints <= 0) return 0;

    dtQueryFilter filter;

    float startPos[3] = { sx, sy, sz };
    float goalPos[3]  = { gx, gy, gz };
    dtPolyRef startRef = 0, goalRef = 0;
    float nearStart[3], nearGoal[3];

    sQuery->findNearestPoly(startPos, kHalfExt, &filter, &startRef, nearStart);
    sQuery->findNearestPoly(goalPos,  kHalfExt, &filter, &goalRef,  nearGoal);

    if (!startRef || !goalRef) return 0;

    /* Find polygon corridor */
    dtPolyRef polyPath[kMaxPathPolys];
    int polyCount = 0;
    sQuery->findPath(startRef, goalRef, nearStart, nearGoal, &filter,
                     polyPath, &polyCount, kMaxPathPolys);
    if (polyCount <= 0) return 0;

    /* Convert polygon corridor to straight-line waypoints */
    float straightPath[kMaxPathPolys * 3];
    unsigned char straightFlags[kMaxPathPolys];
    dtPolyRef straightPolys[kMaxPathPolys];
    int straightCount = 0;

    sQuery->findStraightPath(nearStart, nearGoal, polyPath, polyCount,
                             straightPath, straightFlags, straightPolys,
                             &straightCount, kMaxPathPolys, 0);

    if (straightCount <= 0) return 0;

    int outCount = straightCount < maxPoints ? straightCount : maxPoints;
    memcpy(outPath, straightPath, (size_t)outCount * 3 * sizeof(float));
    return outCount;
}

extern "C" int afn_nav_find_random_point(float outPos[3])
{
    if (!sQuery) return 0;

    dtQueryFilter filter;
    dtPolyRef randRef = 0;
    float pt[3];

    dtStatus st = sQuery->findRandomPoint(&filter, NavRandFloat, &randRef, pt);
    if (dtStatusFailed(st) || !randRef) return 0;

    outPos[0] = pt[0];
    outPos[1] = pt[1];
    outPos[2] = pt[2];
    return 1;
}

extern "C" int afn_nav_find_closest_point(float px, float py, float pz, float outPos[3])
{
    if (!sQuery) return 0;

    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float nearest[3];
    float pos[3] = { px, py, pz };

    sQuery->findNearestPoly(pos, kHalfExt, &filter, &ref, nearest);
    if (!ref) return 0;

    outPos[0] = nearest[0];
    outPos[1] = nearest[1];
    outPos[2] = nearest[2];
    return 1;
}

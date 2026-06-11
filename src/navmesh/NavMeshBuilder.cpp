// Ported from Nebula-Dreamcast-Engine src/navmesh/NavMeshBuilder.cpp — same
// Recast build pipeline + Detour query wrappers + raw-tile binary
// serialization, retargeted to Affinity types (NavVec3) and world scale.
#include "NavMeshBuilder.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

// ---------------------------------------------------------------------------
// Global navmesh state (one navmesh at a time — one scene = one navmesh)
// ---------------------------------------------------------------------------

static dtNavMesh*      sNavMesh  = nullptr;
static dtNavMeshQuery* sNavQuery = nullptr;
static std::vector<float> sDebugTris;   // detail-surface triangles for the editor overlay

// Query extents for nearest-poly lookups, in Affinity world units (player is
// radius 6 / height 24 — Nebula used 2/4/2 at metre scale).
static constexpr float kNavQueryExtX = 16.0f;
static constexpr float kNavQueryExtY = 48.0f;
static constexpr float kNavQueryExtZ = 16.0f;

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void NavMeshClear()
{
    if (sNavQuery) { dtFreeNavMeshQuery(sNavQuery); sNavQuery = nullptr; }
    if (sNavMesh)  { dtFreeNavMesh(sNavMesh);       sNavMesh  = nullptr; }
    sDebugTris.clear();
}

bool NavMeshIsReady()
{
    return sNavMesh != nullptr && sNavQuery != nullptr;
}

const std::vector<float>& NavMeshDebugTris()
{
    return sDebugTris;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

bool NavMeshBuild(const float* verts, int vertCount,
                  const int* tris, int triCount,
                  const NavMeshParams& p,
                  const unsigned char* triFlags,
                  const float* negBoxes, int negBoxCount)
{
    NavMeshClear();

    if (!verts || vertCount <= 0 || !tris || triCount <= 0)
        return false;

    // Compute bounds.
    float bmin[3], bmax[3];
    rcCalcBounds(verts, vertCount, bmin, bmax);

    // Fill Recast config.
    rcConfig cfg{};
    cfg.cs = p.cellSize;
    cfg.ch = p.cellHeight;
    cfg.walkableSlopeAngle = p.walkableSlopeDeg;
    cfg.walkableHeight     = (int)ceilf(p.walkableHeight / cfg.ch);
    cfg.walkableClimb      = (int)floorf(p.walkableClimb / cfg.ch);
    cfg.walkableRadius     = (int)ceilf(p.walkableRadius / cfg.cs);
    cfg.maxEdgeLen         = (int)(p.maxEdgeLen / cfg.cs);
    cfg.maxSimplificationError = p.maxSimplError;
    cfg.minRegionArea      = p.minRegionArea;
    cfg.mergeRegionArea    = p.mergeRegionArea;
    cfg.maxVertsPerPoly    = p.maxVertsPerPoly;
    cfg.detailSampleDist   = p.detailSampleDist;
    cfg.detailSampleMaxError = p.detailSampleMaxErr;

    rcVcopy(cfg.bmin, bmin);
    rcVcopy(cfg.bmax, bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext ctx(false);

    // Allocate heightfield.
    rcHeightfield* solid = rcAllocHeightfield();
    if (!solid) return false;

    bool ok = false;
    unsigned char* triAreas = nullptr;
    rcCompactHeightfield* chf  = nullptr;
    rcContourSet*         cset = nullptr;
    rcPolyMesh*           pmesh = nullptr;
    rcPolyMeshDetail*     dmesh = nullptr;

    printf("[NavMeshBuild] grid=%dx%d bmin=(%.2f,%.2f,%.2f) bmax=(%.2f,%.2f,%.2f)\n",
           cfg.width, cfg.height, cfg.bmin[0], cfg.bmin[1], cfg.bmin[2],
           cfg.bmax[0], cfg.bmax[1], cfg.bmax[2]);

    do {
        if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height,
                                 cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        { printf("[NavMeshBuild] FAIL: rcCreateHeightfield\n"); break; }

        const int ntris = triCount;
        triAreas = new unsigned char[ntris];
        memset(triAreas, 0, ntris * sizeof(unsigned char));

        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle,
                                verts, vertCount,
                                tris, ntris, triAreas);

        // Apply per-triangle flags: force non-walkable (bit 0) for obstacle-
        // only tris, force walkable (bit 1) for nav-bounds-box coverage.
        if (triFlags)
        {
            for (int i = 0; i < ntris; ++i) {
                if (triFlags[i] & 1) triAreas[i] = 0;
                else if (triFlags[i] & 2) triAreas[i] = RC_WALKABLE_AREA;
            }
        }

        // Count walkable triangles; if zero, flip winding and retry
        int nWalkable = 0;
        for (int i = 0; i < ntris; ++i) if (triAreas[i]) ++nWalkable;
        printf("[NavMeshBuild] walkable tris: %d / %d\n", nWalkable, ntris);

        if (nWalkable == 0)
        {
            // Flip winding order: swap first two indices of each triangle
            int* mutableTris = const_cast<int*>(tris);
            for (int i = 0; i < ntris; ++i)
            {
                int tmp = mutableTris[i * 3 + 0];
                mutableTris[i * 3 + 0] = mutableTris[i * 3 + 1];
                mutableTris[i * 3 + 1] = tmp;
            }
            memset(triAreas, 0, ntris * sizeof(unsigned char));
            rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle,
                                    verts, vertCount,
                                    tris, ntris, triAreas);
            if (triFlags)
            {
                for (int i = 0; i < ntris; ++i) {
                    if (triFlags[i] & 1) triAreas[i] = 0;
                    else if (triFlags[i] & 2) triAreas[i] = RC_WALKABLE_AREA;
                }
            }
            nWalkable = 0;
            for (int i = 0; i < ntris; ++i) if (triAreas[i]) ++nWalkable;
            printf("[NavMeshBuild] after winding flip: walkable tris: %d / %d\n", nWalkable, ntris);
        }

        if (!rcRasterizeTriangles(&ctx, verts, vertCount,
                                  tris, triAreas, ntris,
                                  *solid, cfg.walkableClimb))
        { printf("[NavMeshBuild] FAIL: rcRasterizeTriangles\n"); break; }

        // Filter obstacles.
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

        // Compact heightfield.
        chf = rcAllocCompactHeightfield();
        if (!chf) { printf("[NavMeshBuild] FAIL: alloc CHF\n"); break; }
        if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
        { printf("[NavMeshBuild] FAIL: rcBuildCompactHeightfield\n"); break; }

        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
        { printf("[NavMeshBuild] FAIL: rcErodeWalkableArea\n"); break; }

        // Negator boxes: carve the walkable area per-voxel — exact holes for
        // intricate path shaping, independent of source triangle size.
        for (int nb = 0; nb < negBoxCount; ++nb)
            rcMarkBoxArea(&ctx, &negBoxes[nb*6], &negBoxes[nb*6+3], RC_NULL_AREA, *chf);

        if (!rcBuildDistanceField(&ctx, *chf))
        { printf("[NavMeshBuild] FAIL: rcBuildDistanceField\n"); break; }
        if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        { printf("[NavMeshBuild] FAIL: rcBuildRegions\n"); break; }

        // Build contours.
        cset = rcAllocContourSet();
        if (!cset) { printf("[NavMeshBuild] FAIL: alloc contour set\n"); break; }
        if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
        { printf("[NavMeshBuild] FAIL: rcBuildContours\n"); break; }

        // Build polygon mesh.
        pmesh = rcAllocPolyMesh();
        if (!pmesh) { printf("[NavMeshBuild] FAIL: alloc poly mesh\n"); break; }
        if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
        { printf("[NavMeshBuild] FAIL: rcBuildPolyMesh\n"); break; }

        printf("[NavMeshBuild] polys: %d verts: %d\n", pmesh->npolys, pmesh->nverts);

        // Build detail mesh.
        dmesh = rcAllocPolyMeshDetail();
        if (!dmesh) { printf("[NavMeshBuild] FAIL: alloc detail mesh\n"); break; }
        if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                   cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        { printf("[NavMeshBuild] FAIL: rcBuildPolyMeshDetail\n"); break; }

        // Mark all polys as walkable.
        for (int i = 0; i < pmesh->npolys; ++i)
        {
            pmesh->flags[i] = 1;
            pmesh->areas[i] = 0;
        }

        // Capture the detail surface for the editor overlay before the
        // intermediates are freed below.
        sDebugTris.clear();
        for (int i = 0; i < dmesh->nmeshes; ++i)
        {
            const unsigned int* m = &dmesh->meshes[i * 4];
            const unsigned int bverts = m[0], btris = m[2], ntrisSub = m[3];
            const float* dverts = &dmesh->verts[bverts * 3];
            const unsigned char* dtris = &dmesh->tris[btris * 4];
            for (unsigned int t = 0; t < ntrisSub; ++t)
                for (int k = 0; k < 3; ++k)
                {
                    const float* v = &dverts[dtris[t * 4 + k] * 3];
                    sDebugTris.push_back(v[0]);
                    sDebugTris.push_back(v[1]);
                    sDebugTris.push_back(v[2]);
                }
        }

        // Create Detour navmesh data.
        dtNavMeshCreateParams params{};
        params.verts            = pmesh->verts;
        params.vertCount        = pmesh->nverts;
        params.polys            = pmesh->polys;
        params.polyAreas        = pmesh->areas;
        params.polyFlags        = pmesh->flags;
        params.polyCount        = pmesh->npolys;
        params.nvp              = pmesh->nvp;
        params.detailMeshes     = dmesh->meshes;
        params.detailVerts      = dmesh->verts;
        params.detailVertsCount = dmesh->nverts;
        params.detailTris       = dmesh->tris;
        params.detailTriCount   = dmesh->ntris;
        params.walkableHeight   = p.walkableHeight;
        params.walkableRadius   = p.walkableRadius;
        params.walkableClimb    = p.walkableClimb;
        rcVcopy(params.bmin, pmesh->bmin);
        rcVcopy(params.bmax, pmesh->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;

        unsigned char* navData = nullptr;
        int navDataSize = 0;
        if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
        { printf("[NavMeshBuild] FAIL: dtCreateNavMeshData (polys=%d)\n", pmesh->npolys); break; }

        sNavMesh = dtAllocNavMesh();
        if (!sNavMesh) { dtFree(navData); break; }
        if (dtStatusFailed(sNavMesh->init(navData, navDataSize, DT_TILE_FREE_DATA)))
        {
            dtFree(navData);
            break;
        }

        sNavQuery = dtAllocNavMeshQuery();
        if (!sNavQuery) break;
        if (dtStatusFailed(sNavQuery->init(sNavMesh, 2048))) break;

        ok = true;
    } while (false);

    // Cleanup Recast intermediates.
    delete[] triAreas;
    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);
    rcFreeContourSet(cset);
    rcFreeCompactHeightfield(chf);
    rcFreeHeightField(solid);

    if (!ok)
        NavMeshClear();

    return ok;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool NavMeshFindPath(const NavVec3& start, const NavVec3& goal,
                     std::vector<NavVec3>& outPath)
{
    outPath.clear();
    if (!NavMeshIsReady()) return false;

    const float ext[3] = { kNavQueryExtX, kNavQueryExtY, kNavQueryExtZ };
    const float startPt[3] = { start.x, start.y, start.z };
    const float endPt[3]   = { goal.x,  goal.y,  goal.z  };

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0, endRef = 0;
    float nearestStart[3], nearestEnd[3];

    if (dtStatusFailed(sNavQuery->findNearestPoly(startPt, ext, &filter, &startRef, nearestStart)) || startRef == 0)
        return false;
    if (dtStatusFailed(sNavQuery->findNearestPoly(endPt, ext, &filter, &endRef, nearestEnd)) || endRef == 0)
        return false;

    constexpr int kMaxPathPolys = 2048;
    std::vector<dtPolyRef> polys(kMaxPathPolys);
    int npolys = 0;

    if (dtStatusFailed(sNavQuery->findPath(startRef, endRef, nearestStart, nearestEnd,
                                           &filter, polys.data(), &npolys, kMaxPathPolys)) || npolys <= 0)
        return false;

    std::vector<float> straight(kMaxPathPolys * 3);
    std::vector<unsigned char> straightFlags(kMaxPathPolys);
    std::vector<dtPolyRef> straightPolys(kMaxPathPolys);
    int nstraight = 0;

    if (dtStatusFailed(sNavQuery->findStraightPath(
            nearestStart, nearestEnd, polys.data(), npolys,
            straight.data(), straightFlags.data(), straightPolys.data(),
            &nstraight, kMaxPathPolys, DT_STRAIGHTPATH_ALL_CROSSINGS)) || nstraight <= 0)
        return false;

    outPath.reserve((size_t)nstraight);
    for (int i = 0; i < nstraight; ++i)
    {
        NavVec3 p;
        p.x = straight[i * 3 + 0];
        p.y = straight[i * 3 + 1];
        p.z = straight[i * 3 + 2];
        outPath.push_back(p);
    }
    return !outPath.empty();
}

bool NavMeshFindRandomPoint(NavVec3& outPoint)
{
    if (!NavMeshIsReady()) return false;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef ref = 0;
    float pt[3] = {};
    auto frand = []() -> float { return (float)rand() / (float)RAND_MAX; };

    if (dtStatusFailed(sNavQuery->findRandomPoint(&filter, frand, &ref, pt)) || ref == 0)
        return false;

    outPoint.x = pt[0];
    outPoint.y = pt[1];
    outPoint.z = pt[2];
    return true;
}

bool NavMeshFindClosestPoint(const NavVec3& pos, NavVec3& outPoint)
{
    if (!NavMeshIsReady()) return false;

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    const float ext[3] = { kNavQueryExtX, kNavQueryExtY, kNavQueryExtZ };
    const float inPt[3] = { pos.x, pos.y, pos.z };
    dtPolyRef ref = 0;
    float outPt[3] = {};

    if (dtStatusFailed(sNavQuery->findNearestPoly(inPt, ext, &filter, &ref, outPt)) || ref == 0)
        return false;

    outPoint.x = outPt[0];
    outPoint.y = outPt[1];
    outPoint.z = outPt[2];
    return true;
}

// ---------------------------------------------------------------------------
// Binary serialization (raw Detour tile bytes — the PSV export blob)
// ---------------------------------------------------------------------------

bool NavMeshSaveBinary(std::vector<uint8_t>& outBlob)
{
    outBlob.clear();
    if (!sNavMesh) return false;

    // For a single-tile navmesh, tile (0,0,0) holds all the data.
    const dtMeshTile* tile = sNavMesh->getTileAt(0, 0, 0);
    if (!tile || !tile->header || !tile->data || tile->dataSize <= 0)
        return false;

    outBlob.resize((size_t)tile->dataSize);
    memcpy(outBlob.data(), tile->data, (size_t)tile->dataSize);
    return true;
}

bool NavMeshLoadBinary(const uint8_t* data, int dataSize)
{
    NavMeshClear();
    if (!data || dataSize <= 0) return false;

    // Detour takes ownership of the data buffer when DT_TILE_FREE_DATA is set,
    // so we must give it a dtAlloc'd copy.
    unsigned char* navData = (unsigned char*)dtAlloc(dataSize, DT_ALLOC_PERM);
    if (!navData) return false;
    memcpy(navData, data, (size_t)dataSize);

    sNavMesh = dtAllocNavMesh();
    if (!sNavMesh) { dtFree(navData); return false; }

    if (dtStatusFailed(sNavMesh->init(navData, dataSize, DT_TILE_FREE_DATA)))
    {
        dtFree(navData);
        dtFreeNavMesh(sNavMesh);
        sNavMesh = nullptr;
        return false;
    }

    sNavQuery = dtAllocNavMeshQuery();
    if (!sNavQuery)
    {
        dtFreeNavMesh(sNavMesh);
        sNavMesh = nullptr;
        return false;
    }

    if (dtStatusFailed(sNavQuery->init(sNavMesh, 2048)))
    {
        NavMeshClear();
        return false;
    }

    return true;
}

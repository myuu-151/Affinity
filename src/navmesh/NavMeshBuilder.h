#pragma once

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// NavMeshBuilder — builds and queries a Recast/Detour navigation mesh.
// Ported from Nebula-Dreamcast-Engine src/navmesh/NavMeshBuilder.* (same
// pipeline: Recast voxelize/region/contour/poly offline, Detour queries).
//
// Usage:
//   1. Gather world-space triangles from the scene's collision mesh instances
//      (same transform math as the PSV runtime's mesh_collision build)
//   2. Call NavMeshBuild() with the vertex/index arrays
//   3. Query with NavMeshFindPath / NavMeshFindRandomPoint / NavMeshFindClosestPoint
//      (editor preview), or NavMeshSaveBinary for the PSV export blob
//   4. Call NavMeshClear() to free the current navmesh
//
// The module holds a single global navmesh at a time (one scene = one navmesh).
// ---------------------------------------------------------------------------

struct NavVec3 { float x, y, z; };

// Recast build parameters, tuned for Affinity world scale: the world spans
// roughly ±512 units and the PSV player capsule is radius 6 / height 24
// (psv_runtime/main.c COL_RADIUS / COL_TOP).
struct NavMeshParams
{
    // Tuned for narrow ramps/bridges: 1-unit voxels and a small erosion
    // radius — radius 6 (the player capsule) at 2-unit cells eroded 3 cells
    // per side, which DELETED any walkable strip under ~14 units wide
    // (ramps, catwalks). Waypoint following doesn't need full capsule
    // clearance baked in; the runtime's own collision handles the rest.
    float cellSize          = 1.0f;   // XZ voxel size; finer = narrow strips survive
    float cellHeight        = 0.5f;   // Y voxel size
    float walkableSlopeDeg  = 60.0f;  // steeper than the runtime's walkable slopes
    float walkableHeight    = 24.0f;  // agent height (player COL_TOP)
    float walkableClimb     = 8.0f;   // max step height bridged at seams (ramp joins)
    float walkableRadius    = 3.0f;   // light edge erosion (3 cells)
    float maxEdgeLen        = 48.0f;
    float maxSimplError     = 1.0f;   // low = preserve detail at edge seams
    int   minRegionArea     = 8;      // allow small walkable patches
    int   mergeRegionArea   = 400;    // rcSqr(20)
    int   maxVertsPerPoly   = 6;
    float detailSampleDist  = 12.0f;
    float detailSampleMaxErr= 2.0f;
};

// Build navmesh from world-space triangles.
// verts:   packed float array [x0,y0,z0, x1,y1,z1, ...] — (count) vertices
// tris:    packed int array [i0,i1,i2, ...] — (count) triangles indexing into verts
// triFlags: optional per-triangle flags (triCount entries).
//   bit 0 = force non-walkable (obstacle only — shapes navmesh boundary but AI won't walk on it)
//   bit 1 = force walkable (overrides the slope test — nav bounds boxes use
//           this so ramps/objects inside a box join the walkable surface)
//   Pass nullptr to use default slope-based walkability.
// Returns true on success.
// negBoxes: optional world-space negator AABBs (packed minx,miny,minz,
//   maxx,maxy,maxz per box, negBoxCount boxes). Carved out of the walkable
//   area per-voxel (rcMarkBoxArea) AFTER erosion — exact holes regardless of
//   triangle size, for intricate path shaping.
bool NavMeshBuild(const float* verts, int vertCount,
                  const int* tris, int triCount,
                  const NavMeshParams& params = NavMeshParams{},
                  const unsigned char* triFlags = nullptr,
                  const float* negBoxes = nullptr, int negBoxCount = 0);

// Free the current navmesh.
void NavMeshClear();

// Is a navmesh currently loaded?
bool NavMeshIsReady();

// Find a path between two world-space points.
// Returns true if a path was found; outPath receives the waypoints.
bool NavMeshFindPath(const NavVec3& start, const NavVec3& goal,
                     std::vector<NavVec3>& outPath);

// Return a random navigable point on the navmesh.
bool NavMeshFindRandomPoint(NavVec3& outPoint);

// Project a world-space position onto the nearest navmesh surface.
bool NavMeshFindClosestPoint(const NavVec3& pos, NavVec3& outPoint);

// Serialize the built Detour navmesh to a binary blob (for PSV packaging).
// Returns true on success. The blob can be loaded at runtime with NavMeshLoadBinary
// (editor) or afn_nav_init (PSV nav_bridge).
bool NavMeshSaveBinary(std::vector<uint8_t>& outBlob);

// Load a previously serialized navmesh binary blob.
bool NavMeshLoadBinary(const uint8_t* data, int dataSize);

// Editor preview: triangles of the navmesh detail surface (packed xyz per
// vertex, 3 verts per tri), filled by NavMeshBuild. Empty when no navmesh.
const std::vector<float>& NavMeshDebugTris();

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nav_bridge — C-callable wrapper around Detour navmesh queries for PSV.
 * Ported from Nebula-Dreamcast-Engine DetourBridge (same API shape).
 *
 * The editor builds the navmesh with Recast at export time and bakes the raw
 * Detour tile bytes into psv_nav.h (afn_navmesh_bin). main.c passes that blob
 * to afn_nav_init() at boot; npc_nav_update() then paths NPCs across it.
 */

/* Initialize Detour from a serialized navmesh blob (raw tile bytes).
 * Returns 1 on success, 0 on failure. */
int afn_nav_init(const void* navData, int navDataSize);

/* Tear down Detour state (frees dtNavMesh and dtNavMeshQuery). */
void afn_nav_free(void);

/* Returns 1 if Detour is initialized and ready for queries. */
int afn_nav_is_ready(void);

/* Find a path between two world-space points.
 * Writes up to maxPoints xyz waypoints into outPath (packed: x0,y0,z0, ...).
 * Returns the number of waypoints written, or 0 if no path found. */
int afn_nav_find_path(float sx, float sy, float sz,
                      float gx, float gy, float gz,
                      float* outPath, int maxPoints);

/* Pick a random navigable point on the navmesh.
 * Writes xyz into outPos. Returns 1 on success, 0 on failure. */
int afn_nav_find_random_point(float outPos[3]);

/* Project a world position onto the nearest navmesh surface.
 * Writes xyz into outPos. Returns 1 on success, 0 on failure. */
int afn_nav_find_closest_point(float px, float py, float pz, float outPos[3]);

#ifdef __cplusplus
}
#endif

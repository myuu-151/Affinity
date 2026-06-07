// Affinity PS Vita runtime — shared data contract.
// Mirrors psp_runtime/include/affinity_psp.h but WITHOUT the PSP GU dependency:
// the Vita renders with vitaGL (fixed-function GL), so vertices go through GL
// vertex arrays instead of sceGumDrawArray, and there are no GU_* flag macros.
// The struct layouts are identical to the PSP ones so the generated scene data
// (psv_mapdata.h, a copy of psp_mapdata.h) drops in unchanged.
#pragma once

// One interleaved scene vertex. color is 0xAABBGGRR (== GL RGBA byte order in
// little-endian memory, so glColorPointer(4, GL_UNSIGNED_BYTE, ...) reads it).
typedef struct {
    float        u, v;     // texcoord
    unsigned int color;    // 0xAABBGGRR
    float        x, y, z;  // position
} AfnVertex;

// Rig vertex: carries a normal for lighting (camera headlamp).
typedef struct {
    float        u, v;
    unsigned int color;
    float        nx, ny, nz;
    float        x, y, z;
} AfnRigVertex;

// A static scene mesh (level chunk, prop). One draw per referencing instance.
typedef struct {
    int                   vertCount;
    int                   indexCount;   // triangle indices (16-bit)
    const AfnVertex*      verts;
    const unsigned short* indices;
    int                   textured;
    int                   texW, texH;
    const unsigned int*   texPixels;    // RGBA8888 linear
    int                   texHasAlpha;  // 1 = blend, 0 = opaque
    int                   cullMode;     // 0 back, 1 front, 2 none
    int                   lit;          // 1 = lit, 0 = unlit
    int                   visible;      // 0 = collision-only
} AfnMesh;

// A placed instance of a mesh in the world.
typedef struct {
    float x, y, z;
    float scale;
    float rotY, rotX, rotZ;   // degrees
    int   meshIdx;            // index into afn_meshes (-1 = none)
} AfnSpriteInst;

// ---- Generated data (psv_mapdata.h) -------------------------------------
extern const int            afn_mesh_count;
extern const AfnMesh        afn_meshes[];
extern const int            afn_sprite_count;
extern const AfnSpriteInst  afn_sprites[];

extern const float afn_cam_start_x, afn_cam_start_z, afn_cam_start_h;
extern const float afn_cam_start_angle;   // radians
extern const float afn_orbit_dist;
extern const float afn_draw_distance;     // 0 = unlimited
extern const float afn_walk_speed, afn_sprint_speed;

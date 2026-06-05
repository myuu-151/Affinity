// Affinity PSP runtime — shared data contract between the exporter-generated
// psp_mapdata.h and the runtime (scene.c / meshcull.c).
//
// Geometry is plain floats: the PSP has an FPU + hardware T&L, so we feed
// sceGumDrawArray interleaved vertices and let the GE transform them. The
// vertex layout matches the GU flags in AFN_VERTEX_FLAGS exactly (the GE reads
// fields in the fixed order weights→texcoord→color→normal→position).
#pragma once

#include <pspgu.h>

// One interleaved vertex. Order is mandated by the GE: texcoord, then color,
// then position. Untextured meshes still carry (u,v) (ignored) so the runtime
// has a single vertex path.
typedef struct {
    float        u, v;     // GU_TEXTURE_32BITF
    unsigned int color;    // GU_COLOR_8888 (0xAABBGGRR)
    float        x, y, z;  // GU_VERTEX_32BITF
} AfnVertex;

#define AFN_VERTEX_FLAGS \
    (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

// Rig vertex: like AfnVertex but carries a normal, so the player rig can be lit
// by the GE's hardware lighting (camera headlamp). The GE field order is fixed:
// texcoord -> color -> normal -> position.
typedef struct {
    float        u, v;        // GU_TEXTURE_32BITF
    unsigned int color;       // GU_COLOR_8888
    float        nx, ny, nz;  // GU_NORMAL_32BITF
    float        x, y, z;     // GU_VERTEX_32BITF
} AfnRigVertex;

#define AFN_RIG_VERTEX_FLAGS \
    (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

// A static scene mesh (level chunk, prop, player placeholder). One draw per
// sprite instance that references it.
typedef struct {
    int                  vertCount;
    int                  indexCount;   // triangle indices (GU_INDEX_16BIT)
    const AfnVertex*     verts;
    const unsigned short* indices;
    int                  textured;
    int                  texW, texH;
    const unsigned int*  texPixels;    // RGBA8888, linear (runtime swizzles at load)
    int                  texHasAlpha;  // 1 = blend, 0 = opaque
    int                  cullMode;     // 0 back, 1 front, 2 none
    int                  lit;          // 1 = lit, 0 = unlit (flat/vertex color)
    int                  visible;      // 0 = collision-only, never drawn
} AfnMesh;

// A placed instance of a mesh in the world.
typedef struct {
    float x, y, z;
    float scale;
    float rotY, rotX, rotZ;   // degrees
    int   meshIdx;            // index into afn_meshes (-1 = none)
} AfnSpriteInst;

// ---- Generated data (psp_mapdata.h) -------------------------------------
extern const int            afn_mesh_count;
extern const AfnMesh        afn_meshes[];
extern const int            afn_sprite_count;
extern const AfnSpriteInst  afn_sprites[];

// Camera / movement start state.
extern const float afn_cam_start_x, afn_cam_start_z, afn_cam_start_h;
extern const float afn_cam_start_angle;   // radians
extern const float afn_orbit_dist;
extern const float afn_draw_distance;     // 0 = unlimited
extern const float afn_walk_speed, afn_sprint_speed;

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

// A skinnable rig asset (player or NPC). The scene can carry several distinct
// rigs (psv_rig.h emits one per used model). All geometry is raw model space;
// the runtime CPU-skins (skinned = animPose[bone]·baseVert) per instance.
typedef struct {
    int   bones, verts, mats, clips, cull;   // counts + cull mode (0 back/1 front/2 none)
    float scale;                             // model -> world base scale
    int   camlight;                          // 1 = headlamp follows the camera
    float ldx, ldy, ldz;                     // baked eye-space headlamp direction
    float yawOff;                            // model forward correction (radians, added to all yaw)
    float shadowAmb;                         // headlamp ambient floor (Shadow Intensity, baked at export; 8/31 = default)
    const float*                 vpos;       // [verts*3] model-space positions
    const float*                 vnorm;      // [verts*3] model-space normals
    const float*                 vuv;        // [verts*2] texcoords
    const unsigned char*         vbone;      // [verts] bone index per vertex
    const unsigned short* const* idx;        // [mats] triangle indices per material
    const int*                   idxcount;   // [mats] index count per material
    const unsigned int*   const* tex;        // [mats] RGBA8888 texture per material (0 = flat)
    const int*                   texw;       // [mats]
    const int*                   texh;       // [mats]
    const float*          const* clip;       // [clips] flattened {px,py,pz,qw,qx,qy,qz}
    const int*                   clipframes; // [clips]
    const unsigned char*         cliploop;   // [clips]
    const float*                 clipspeed;  // [clips] playback rate multiplier (0..2, 1 = normal)
} AfnRig;

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
    int                   blend;        // 1 = alpha-blend (attached-model soft alpha), 0 = opaque/cutout
    // Multi-material (OBJ usemtl): mats>0 => draw group-per-slot (each slot binds
    // its own texture), ignoring the single texture fields above. 0 => single-tex.
    int                          mats;
    const unsigned short* const* slotIdx;      // [mats] indices per slot
    const int*                   slotIdxCount; // [mats]
    const unsigned int*   const* slotTex;      // [mats] RGBA8888 per slot (0 = flat)
    const int*                   slotTexW;      // [mats]
    const int*                   slotTexH;      // [mats]
    // OBJ 2.0 scene lighting (vitaGL GL_LIGHTING): per-vertex normals
    // [vertCount*3], or 0 when the scene has no light rig / this mesh is unlit —
    // the mesh then draws unlit-vertex-colored exactly as before.
    const float*                 normals;
    // OBJ 2.0 lightmap: full-color RGBA8 texture multiplied over the mesh via
    // a SECOND UV set in a second draw pass (blend DST_COLOR x ZERO, depth
    // EQUAL). 0/NULL = no lightmap.
    const float*                 uv2;      // [vertCount*2]
    const unsigned char*         lmTex;    // RGBA8 bytes (lmW*lmH*4)
    int                          lmW, lmH;
    // Editor/imported lights baked for a LIGHTMAPPED mesh: per-vertex additive
    // light term (0xAABBGGRR), drawn as fb += albedo × lcol in a third pass on
    // top of the lightmap multiply. 0/NULL = none.
    const unsigned int*          addCol;   // [vertCount]
    // AO map (OBJ 2.0 #aomap): GRAYSCALE occlusion through a THIRD UV set,
    // fb *= lerp(1, ao, aoStrength) between the lightmap multiply and the
    // additive lights. 0/NULL = none.
    const float*                 uv3;      // [vertCount*2]
    const unsigned char*         aoTex;    // grayscale bytes (aoW*aoH)
    int                          aoW, aoH;
    float                        aoStrength; // AO multiplier (folded at upload)
    // MAP GROUPS (OBJ 2.0 v1.5): 2+ lightmap/AO pairs in ONE mesh, applied per
    // face group — each group's triangles multiply that group's textures
    // through the shared UV2/UV3 channels. 0 = single-slot fields above.
    int                          lmgCount;
    const unsigned short* const* lmgIdx;      // [lmgCount] triangle indices per group
    const int*                   lmgIdxCount; // [lmgCount]
    const unsigned char* const*  lmgLm;       // [lmgCount] RGBA lightmap (0 = none)
    const int*                   lmgLmW;
    const int*                   lmgLmH;
    const unsigned char* const*  lmgAo;       // [lmgCount] grayscale AO (0 = none)
    const int*                   lmgAoW;
    const int*                   lmgAoH;
    // Editor "Collision" checkbox. 1 = this mesh contributes floor/wall collision
    // faces; 0 = walk-through (grass, flowers, decorative props). Read by
    // collide_build(): a 0 mesh is skipped entirely.
    int                          collision;
    // Editor "Filtered" checkbox: 1 = sample the diffuse with GL_LINEAR
    // (smooth), 0 = GL_NEAREST (crisp pixels). Applied at upload_textures().
    int                          texFiltered;
} AfnMesh;

// A static scene light (OBJ 2.0 "#light"/"#sun" lines, exporter-placed in world
// space). col carries the Blender energy + unit conversion pre-folded, so the
// runtime feeds these fields straight into glLightfv/glLightf.
typedef struct {
    float pos[4];      // world xyz + w (1 = point, 0 = directional "toward the light")
    float col[4];      // diffuse RGB (pre-scaled) + 1
    float kc, kl, kq;  // GL constant / linear / quadratic attenuation
} AfnLight;

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
// Scene lights — defined only when the export carries an OBJ 2.0 light rig
// (guard uses with #ifdef AFN_HAS_LIGHTS from psv_mapdata.h).
extern const int            afn_light_count;
extern const AfnLight       afn_lights[];
extern const float          afn_light_ambient[4];

extern const float afn_cam_start_x, afn_cam_start_z, afn_cam_start_h;
extern const float afn_cam_start_angle;   // radians
extern const float afn_orbit_dist;
extern const float afn_draw_distance;     // 0 = unlimited
extern const float afn_walk_speed, afn_sprint_speed;

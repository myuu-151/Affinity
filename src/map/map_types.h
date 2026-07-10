#pragma once

#include <cstdint>
#include <vector>
#include <string>

// Scene viewport dimensions (DS: 256×192)
static constexpr int kGBAWidth  = 256;
static constexpr int kGBAHeight = 192;

// Tile constants
static constexpr int kTileSize = 8;   // 8x8 pixels per tile

// A single 8x8 tile stored as 4bpp (32 bytes on GBA, but we store as 8-bit indices in editor)
struct Tile8
{
    uint8_t pixels[kTileSize * kTileSize]; // palette indices 0-15
};

// A tileset: collection of 8x8 tiles + a 256-color palette
struct Tileset
{
    std::vector<Tile8>   tiles;
    uint32_t             palette[256] = {}; // RGBA8 colors (editor side)
    std::string          name;
};

// A tilemap layer: grid of tile indices
struct TilemapLayer
{
    int width  = 32;  // in tiles
    int height = 32;
    std::vector<uint16_t> tileIndices; // index into Tileset::tiles
};

// ---- Sprite Asset System ----
// A sprite frame: 4bpp pixel data at a specific size (8, 16, or 32 px square)
static constexpr int kMaxFrameSize = 960; // max pixel dimension; holds the PSV full-screen 960x544 background frame

struct SpriteFrame
{
    uint8_t pixels[kMaxFrameSize * kMaxFrameSize] = {}; // palette indices 0-15
    int width  = 8;
    int height = 8;
    // Optional per-pixel alpha (row-major width*height, 0-255), captured at
    // import when the asset's Use Alpha is on. Empty = no soft alpha: the pixel
    // is fully opaque unless its palette index is 0 (the binary cutout).
    std::vector<uint8_t> alpha;
};

// Game states that can be assigned to animation slots
enum class AnimState : int
{
    None = 0,   // no game state — manual only
    Idle,       // plays when not moving
    Walk,       // plays when moving (no sprint)
    Run,        // plays when moving (no sprint) — alias for walk
    Sprint,     // plays when moving + sprint key
    Count
};

static const char* const kAnimStateNames[] = {
    "None", "Idle", "Walk", "Run", "Sprint"
};

// An animation clip: named sequence of frames
struct SpriteAnim
{
    std::string name = "idle";
    int startFrame = 0;
    int endFrame   = 0;
    int fps        = 8;
    bool loop      = true;
    bool stepAnim  = false; // true = advance frame per tile step (tilemap), false = time-based
    float speed    = 1.0f;  // playback speed multiplier (0..10, default 1)
    AnimState gameState = AnimState::None; // which game state triggers this anim
};

// LOD tier: at what distance to switch to a smaller sprite size
struct SpriteLOD
{
    int   size = 8;         // 8, 16, or 32
    int   frameStart = 0;   // first frame index for this LOD's tiles
    int   frameCount = 0;   // number of frames at this LOD
    float maxDist = 9999.0f; // use this LOD when distance < maxDist
};

// A sprite asset: defines a sprite type with frames, animations, LOD, palette
static constexpr int kMaxSpriteAssets = 32;
static constexpr int kMaxSpriteFrames = 64;
static constexpr int kMaxSpriteAnims  = 16;
static constexpr int kMaxSpriteLODs   = 3;

struct SpriteAsset
{
    std::string name = "Sprite";
    uint32_t palette[16] = {};     // 4bpp OBJ palette (16 colors, index 0 = transparent)
    int palBank = 1;               // GBA OBJ palette bank (0-15)
    int paletteSrc = -1;           // -1 = own palette, >= 0 = share palette from asset index

    // PSV-only higher-color graphics. Parallel to the 16-color frames/palette
    // above (which GBA/NDS keep using). psvColors = 16/32/64/128; when > 16 these
    // hold a re-quantization of the source PNG that the PSV preview + exporter
    // use. Index 0 = transparent. psvFrames pixels index into psvPalette.
    int psvColors = 16;            // 16 = use the normal 16-color path (default)
    uint32_t psvPalette[128] = {};
    std::vector<SpriteFrame> psvFrames;

    // PSV soft alpha: when on, import captures the source PNG's per-pixel alpha
    // into each frame's alpha[] and the PSV exporter emits it (the runtime alpha-
    // blends), so feathered/glowing edges fade smoothly instead of the binary
    // 50% cutout. Off = today's crisp pixel-art behavior (no alpha plane).
    bool useAlpha = false;

    std::vector<SpriteFrame> frames;
    std::vector<SpriteAnim>  anims;

    SpriteLOD lod[kMaxSpriteLODs] = {};
    int lodCount = 1;              // 1-3 LOD tiers

    int defaultAnim = 0;           // which animation plays by default
    int baseSize = 8;              // base rendering size (8, 16, 32)

    // Source image path (for re-import)
    std::string sourceImagePath;
    int stripFrameW = 0;           // frame width in source strip
    int stripFrameH = 0;           // frame height in source strip

    // 8-directional sprite animation sets (e.g., idle, running)
    static constexpr int kDirCount = 8;
    static constexpr int kMaxDirAnimSets = 8;

    struct DirAnimSet
    {
        std::string name = "idle";
        std::string dirPaths[8]; // PNG file paths per direction
    };

    std::vector<DirAnimSet> dirAnimSets;
    bool hasDirections = false;        // true if any direction image is loaded
};

// Object types for Mode 4 scene entities
enum class SpriteType : int
{
    Prop = 0,       // static decoration (trees, signs, rocks)
    Player,         // player spawn point
    Enemy,          // hostile NPC
    NPC,            // friendly/neutral character
    Trigger,        // invisible zone (events, warps, doors)
    Waypoint,       // pathfinding/patrol node
    Mesh,           // 3D mesh object
    Count
};

static const char* const kSpriteTypeNames[] = {
    "Prop", "Player", "Enemy", "NPC", "Trigger", "Waypoint", "Mesh"
};

// ---- 3D Mesh Asset System ----
struct MeshVertex
{
    float px, py, pz;   // position
    float nx, ny, nz;   // normal
    float r, g, b;      // vertex color (default white)
    float u, v;         // texture coordinates (0..1)
    float u2 = 0, v2 = 0; // lightmap UV (OBJ 2.0 4-component vt); unused when the mesh has no lightmap
    float u3 = 0, v3 = 0; // AO-map UV (OBJ 2.0 6-component vt); unused when the mesh has no AO map
    int   objPosIdx = -1; // original OBJ 'v' index (for vertex welding)
};

// Backface culling mode for mesh rendering
enum class CullMode : int
{
    Back  = 0,  // cull back faces (default — standard solid mesh)
    Front = 1,  // cull front faces (inside-out rendering)
    None  = 2,  // no culling (double-sided, renders both faces)
};

static const char* const kCullModeNames[] = { "Back", "Front", "None" };

// Export quality mode for mesh rendering
enum class MeshExportMode : int
{
    Quality     = 0,  // per-face normals, no welding (accurate shading, slower)
    Performance = 1,  // welded vertices, averaged normals (faster, blended shading)
    Barebones   = 2,  // welded + force unlit + pre-sorted tris (fastest, flat color)
};

static const char* const kMeshExportModeNames[] = { "Quality", "Performance", "Barebones" };

// One material slot's base-color texture (palettized). Used for both rig slots
// (glTF) and static-mesh slots (OBJ usemtl). Slot 0 is stored inline on the
// owning asset (legacy single-texture layout); slots 1..N live in extraMaterials.
struct RigMaterial
{
    std::string name;                        // material slot name (glTF material / OBJ usemtl)
    bool textured = false;
    bool textureManual = false;              // true = user-assigned PNG (persisted by path)
    std::string texturePath;
    std::vector<uint8_t> texturePixels;      // indexed pixels (texW * texH), 0..255
    uint32_t texturePalette[256] = {};       // 256-colour (GL_RGB256) palette
    int texW = 0, texH = 0;
    unsigned int glTexID = 0;                // OpenGL texture for editor preview
    int wrapMode = 0;                        // UV addressing: 0=Clip(clamp) 1=Extend(tile) 2=Mirror
};

struct MeshAsset
{
    std::string name = "Mesh";
    std::string sourcePath;            // original .obj file path
    std::vector<MeshVertex> vertices;  // vertex buffer
    std::vector<uint32_t>   indices;   // triangle index buffer
    float boundsMin[3] = {};           // AABB min
    float boundsMax[3] = {};           // AABB max
    CullMode cullMode = CullMode::Back; // backface culling mode
    MeshExportMode exportMode = MeshExportMode::Quality; // export quality
    bool lit = true; // false = unlit (flat color, no shading calc)
    bool halfRes = false; // true = rasterize every other scanline (2x fill speed)
    bool wireframe = false; // true = wireframe overlay (draw triangle edges)
    bool grayscale = false; // true = grayscale shaded faces (combine with wireframe for editor look)
    bool useQuads = true;  // true = export quads natively to GBA, false = fan-triangulate quads
    float drawDistance = 0.0f; // per-mesh draw distance (0 = use global/unlimited)
    bool collision = true; // true = generate collision faces for this mesh
    int drawPriority = 0; // 0 = draws on top (last), higher = draws first (behind)
    bool visible = true; // false = invisible collision-only mesh (saves CPU on GBA)
    bool hasVertexColor = false; // true = OBJ 2.0 per-vertex colors loaded (v x y z r g b)

    // OBJ 2.0 light rig (github.com/myuu-151/OBJ-2.0): "#light"/"#sun"/"#ambient"
    // lines parsed from the source OBJ (Blender's lighting setup travels with the
    // model). Retained as data: the editor previews them with GL lighting and the
    // PSV exporter emits them as afn_lights[] for runtime vitaGL GL_LIGHTING.
    struct MeshLight
    {
        int   type = 0;                  // 0 = point (x/y/z = position), 1 = sun (x/y/z = travel direction)
        float x = 0, y = 0, z = 0;
        float r = 1, g = 1, b = 1;       // light color 0..1
        float energy = 1000.0f;          // point: watts; sun: W/m^2 (Blender values)
        float radius = 0.0f;             // point only: custom-distance cutoff (0 = none)
    };
    std::vector<MeshLight> lights;       // empty = mesh contributes no lights
    float lightAmbient[3] = { 0.05f, 0.05f, 0.05f }; // "#ambient" world term
    float lightBake = 1.0f;              // intensity multiplier (editor tunable, persisted)
    bool lightsEdited = false;           // user deleted imported lights in-editor —
                                         // persist the survivors instead of re-parsing the OBJ

    // Lightmap (OBJ 2.0 "#lightmap file.png" + 4-component vt lines): a full-
    // color texture multiplied over the mesh through the SECOND UV set, drawn
    // as a second multiply pass (editor preview and PSV runtime identically).
    // A lightmapped mesh skips the vertex-color light bake (it owns lighting).
    bool hasLightmap = false;
    bool hasUV2 = false;                 // OBJ carried 4-component vt (lightmap UV channel)
    bool lmManual = false;               // user assigned/removed the lightmap in-editor (persisted)
    std::string lightmapPath;            // lightmap PNG path (kept on load failure for the warning)
    std::vector<uint8_t> lmPixels;       // RGBA8 (lmW * lmH * 4)
    int lmW = 0, lmH = 0;
    unsigned int lmGlTex = 0;            // editor preview texture (LINEAR)

    // AO map (OBJ 2.0 "#aomap file.png" + 6-component vt): GRAYSCALE occlusion
    // multiplied over the mesh through the THIRD UV set, between the lightmap
    // multiply and the additive lights — so AO darkens baked lighting but a
    // live point light still reaches occluded corners.
    bool hasAOMap = false;
    bool hasUV3 = false;                 // OBJ carried 6-component vt (AO UV channel)
    bool aoManual = false;               // user assigned/removed the AO map in-editor (persisted)
    std::string aoMapPath;               // AO PNG path (kept on load failure for the warning)
    std::vector<uint8_t> aoPixels;       // grayscale bytes (aoW * aoH)
    int aoW = 0, aoH = 0;
    unsigned int aoGlTex = 0;            // editor preview texture (LINEAR)
    float aoStrength = 1.0f;             // AO multiplier 0..2: effective = clamp(1 - k + k*ao).
                                         // 1 = baked AO, <1 fades out, >1 multiplies DARKER.
                                         // Folded into the texture at upload (FFP color caps at 1).
                                         // Applies to every map group too.

    // Bitmask overlay (Paint2Blend / OBJ 2.0 "#overlay" + "#bitmask"): a
    // second TILING texture blended OVER the base by a painted mask. UV pair
    // 2 = overlay tiling ("UV2"), UV pair 3 = mask UV ("Bitmask"); the mask
    // PNG is sampled PER-VERTEX at import into maskW, so previews and the
    // runtime just draw a second alpha-blended pass — no extra texture unit.
    // Mutually exclusive with lightmap/AO on the same mesh (shared UV pairs).
    bool hasOverlay = false;
    bool ovManual = false;                   // user assigned/removed in-editor (persisted)
    int ovUVSrc = 1, maskUVSrc = 2;          // UV pair each samples: 0=UV1 1=UV2 2=UV3
    std::string overlayPath, bitmaskPath;    // as loaded (for panel warnings)
    std::vector<uint32_t> overlayRGBA;       // 0xAABBGGRR (ovW * ovH)
    int ovW = 0, ovH = 0;
    unsigned int ovGlTex = 0;                // editor preview texture (LINEAR + REPEAT)
    std::vector<uint8_t> bmPixels;           // bitmask grayscale (bmW*bmH) — kept so the
    int bmW = 0, bmH = 0;                    // mask UV can be re-picked without the PNG
    std::vector<float> maskW;                // per-vertex blend weight 0..1 (parallel to vertices)

    // MAP GROUPS (OBJ 2.0 v1.5): one OBJ can carry SEVERAL lightmap/AO pairs —
    // each "#lightmap"/"#aomap" after faces have been emitted starts a new
    // group, and the faces that follow belong to it. Used when the model was
    // split into sections for lightmap density but authored as a single OBJ.
    // Single-pair OBJs keep using the legacy hasLightmap/hasAOMap fields above;
    // mapGroups is populated only when there are 2+ groups.
    struct MeshMapGroup
    {
        std::string lmPath, aoPath;          // as referenced by the OBJ (may be empty)
        std::vector<uint8_t> lmPixels;       // RGBA8 (lmW*lmH*4)
        int lmW = 0, lmH = 0;
        unsigned int lmGlTex = 0;
        std::vector<uint8_t> aoPixels;       // grayscale (aoW*aoH)
        int aoW = 0, aoH = 0;
        unsigned int aoGlTex = 0;            // aoStrength-folded, like the single slot
    };
    std::vector<MeshMapGroup> mapGroups;     // 2+ entries = multi-map mesh
    std::vector<uint8_t> triMapGroup;        // group per triangle (parallel to indices/3)
    std::vector<uint8_t> quadMapGroup;       // group per quad (parallel to quadIndices/4)

    // Quad index buffer — 4 consecutive indices per quad face from OBJ
    // OBJ quads are preserved as-is, not force-triangulated
    std::vector<uint32_t> quadIndices;

    // Texture mapping
    bool textured = false;                    // true = use texture, false = flat shaded
    std::string texturePath;                  // source PNG path
    std::vector<uint8_t> texturePixels;       // quantized indexed pixels (texW * texH), 0..255
    uint32_t texturePalette[256] = {};        // RGBA8 palette (16 or 256 entries used)
    bool texture256 = false;                  // false = 16-colour (GL_RGB16), true = 256 (GL_RGB256)
    bool texture512 = false;                  // PSV: store the FULL 24-bit RGBA (quantized to 512
                                              // colours) in texRGBA instead of the 256-colour RGB15
                                              // palette — finer gradients on the Vita (truecolor).
    std::vector<uint32_t> texRGBA;            // full RGBA8888 texture when texture512 (else empty)
    int texW = 0, texH = 0;                  // texture dimensions (power of 2, max 512)
    bool textureUseAlpha = false;             // true = treat alpha=0 as transparent (NDS reserves
                                              // palette[0] and sets GL_TEXTURE_COLOR0_TRANSPARENT).
                                              // Off by default — leave it off unless you actually want
                                              // cutout transparency. Textures with transparent padding
                                              // around opaque content should keep this off so the padding
                                              // doesn't bleed into face UVs near the edge.
    // Attached-model sprite-style texture options (ignored by normal meshes):
    bool useSoftAlpha = false;                // per-pixel (blended) alpha like sprite assets — drawn
                                              // with GL_BLEND instead of a 1-bit cutout
    std::vector<uint8_t> texAlpha;            // per-pixel alpha plane (texW*texH) when useSoftAlpha
    int  psvColors = 0;                       // 0 = legacy 16/256 path (texture256); >0 = quantize to N
                                              // colors sprite-style (attached models default to 128)
    bool ignoreVertexColor = false;           // export/preview: drop the mesh's per-vertex colors and
                                              // modulate the texture with white. Rescues OBJs whose
                                              // color layer is black (Blender placeholder) -> black mesh.
    unsigned int glTexID = 0;                 // OpenGL texture for editor preview
    bool texFiltered = false;                 // true = GL_LINEAR, false = GL_NEAREST
    bool texInIwram = false;                  // true = copy this texture into the IWRAM cache at boot (faster ldrb, shared 4KB budget)
    bool perspCorrect = false;               // true = perspective-corrected texture mapping (slower)
    int subdivide = 0;                       // 0=off, N=subdivide each face into NxN grid at export
    bool clampAbove = false;                 // true = clamp vertices to never project above horizon (prevents "under mesh" warp)
    bool nearClip = false;                   // true = view-space near-plane clipping (fixes slope walling)
    bool faceCull = false;                   // true = skip faces with vertices above camera (hard cutoff)
    bool removeDoubles = false;              // true = weld identical (pos+uv+normal) verts at export (fewer runtime verts)

    // Multi-material (OBJ usemtl): the inline texture fields above are slot 0;
    // additional usemtl materials live here as slots 1..N. triMaterial holds the
    // slot index per triangle (size = indices.size()/3); empty = all slot 0.
    // PSV draws each slot as its own group with its own texture bound.
    std::string materialName;                // slot 0 material name (from usemtl)
    int wrapMode = 0;                        // slot 0 UV addressing: 0=Clip 1=Extend 2=Mirror
    std::vector<RigMaterial> extraMaterials; // slots 1..N
    std::vector<uint8_t>     triMaterial;    // slot index per triangle (parallel to indices/3; empty = all slot 0)
    std::vector<uint8_t>     quadMaterial;   // slot index per quad (parallel to quadIndices/4; empty = all slot 0)
    int matCount() const { return 1 + (int)extraMaterials.size(); }
    bool matTextured(int s) const { return s == 0 ? textured : extraMaterials[s-1].textured; }
    const std::vector<uint8_t>& matPixels(int s) const { return s == 0 ? texturePixels : extraMaterials[s-1].texturePixels; }
    const uint32_t* matPalette(int s) const { return s == 0 ? texturePalette : extraMaterials[s-1].texturePalette; }
    int matTexW(int s) const { return s == 0 ? texW : extraMaterials[s-1].texW; }
    int matTexH(int s) const { return s == 0 ? texH : extraMaterials[s-1].texH; }
    unsigned int matGlTexID(int s) const { return s == 0 ? glTexID : extraMaterials[s-1].glTexID; }
    const std::string& matName(int s) const { return s == 0 ? materialName : extraMaterials[s-1].name; }
    const std::string& matPath(int s) const { return s == 0 ? texturePath : extraMaterials[s-1].texturePath; }
    int  matWrap(int s) const { return s == 0 ? wrapMode : extraMaterials[s-1].wrapMode; }
    void setMatWrap(int s, int w) { if (s == 0) wrapMode = w; else extraMaterials[s-1].wrapMode = w; }
};

static constexpr int kMaxMeshAssets = 32;

// ---------------------------------------------------------------------------
// Rigged (skinned) mesh — DSMA skeletal animation imported from glTF/GLB.
// RIGID skinning: each vertex is bound to a single bone. The PSV/PSP runtime
// CPU-skins per instance, so bone count is only capped by the 8-bit per-vertex
// bone index (<=255) — the old <=29 DS matrix-stack limit is retired. Vertex
// positions are stored in their bone's local space; bone transforms (bind pose
// + per-frame) are absolute (hierarchy already composed), per tools/gltf_to_dsma.py.
// ---------------------------------------------------------------------------

// A bone transform: translation + unit quaternion (no scale — DSMA has none).
struct BonePose
{
    float px = 0, py = 0, pz = 0;            // translation
    float qw = 1, qx = 0, qy = 0, qz = 0;   // orientation (quaternion)
};

// One animation clip: frameCount frames, each with boneCount BonePose entries.
// frames is flattened: frame f, bone b -> frames[f * boneCount + b].
struct RigAnimClip
{
    std::string name = "anim";
    int frameCount = 0;
    std::vector<BonePose> frames;
    bool loop = true;   // true = loop, false = play once then hold last frame
    float speed = 1.0f; // playback rate multiplier (0..2, 1 = authored fps) — driven at runtime
};

struct RiggedMeshAsset
{
    std::string name = "Rig";
    std::string sourcePath;                  // original .glb/.gltf path (re-imported on load)
    std::string materialName;                // glTF material slot name (for the import UI)

    int boneCount = 0;
    std::vector<MeshVertex> baseVerts;       // pos/normal in their bone's LOCAL space, uv in 0..1
    std::vector<int>        vertBone;         // parallel to baseVerts: bone index per vertex
    std::vector<uint32_t>   indices;          // triangle index buffer into baseVerts
    bool smoothShading = false;              // true = per-vertex normals (smooth), false = flat
    int  cullMode = 0;                       // backface culling: 0 = Back, 1 = Front, 2 = None
    bool useAlpha = false;                    // true = palette index 0 (alpha=0 src) renders transparent
    bool cameraLight = false;                // true = light follows the camera (headlamp)
    float lightX = 50.0f, lightY = 180.0f;   // headlamp aim: pitch/yaw in degrees off the camera
    float shadowIntensity = 1.0f;            // editor rig shading depth: 0 = flat (no shadow), 1 = default, 2 = black shadow side
    float yawOffset = 0.0f;                  // model forward correction, degrees — added to ALL
                                             // instance yaw (set 180 for glTFs authored facing -Z,
                                             // fixes movement-facing "moonwalk")

    std::vector<BonePose>   bindPose;         // boneCount entries, absolute bind transforms
    std::vector<int>        boneParent;       // boneCount entries, parent bone index (-1 = root)
    std::vector<std::string> boneNames;       // boneCount entries, joint names (bone-attach dropdown)

    std::vector<RigAnimClip> clips;

    float boundsMin[3] = {};                 // AABB of the bind pose (for framing/scale)
    float boundsMax[3] = {};

    // Optional base-color texture (palettized like MeshAsset, for preview + export).
    bool textured = false;
    bool textureManual = false;              // true = user-assigned PNG (persisted by path);
                                             // false = decoded from the glTF (re-imported on load)
    std::string texturePath;
    std::vector<uint8_t> texturePixels;      // indexed pixels (texW * texH), 0..255
    uint32_t texturePalette[256] = {};       // 256-colour (GL_RGB256) palette
    int texW = 0, texH = 0;
    unsigned int glTexID = 0;                // OpenGL texture for editor preview
    int wrapMode = 0;                        // slot 0 UV addressing: 0=Clip 1=Extend 2=Mirror

    // Multi-material: the fields above are material slot 0; additional glTF
    // materials live here as slots 1..N. triMaterial holds the slot index per
    // triangle (size = indices.size()/3); empty means every triangle is slot 0.
    // On the DS each slot is drawn as its own group with its own texture bound.
    std::vector<RigMaterial> extraMaterials;
    std::vector<uint8_t>     triMaterial;
    int matCount() const { return 1 + (int)extraMaterials.size(); }
    // Uniform read access to a slot's texture data (slot 0 = inline fields).
    bool matTextured(int s) const { return s == 0 ? textured : extraMaterials[s-1].textured; }
    const std::vector<uint8_t>& matPixels(int s) const { return s == 0 ? texturePixels : extraMaterials[s-1].texturePixels; }
    const uint32_t* matPalette(int s) const { return s == 0 ? texturePalette : extraMaterials[s-1].texturePalette; }
    int matTexW(int s) const { return s == 0 ? texW : extraMaterials[s-1].texW; }
    int matTexH(int s) const { return s == 0 ? texH : extraMaterials[s-1].texH; }
    unsigned int matGlTexID(int s) const { return s == 0 ? glTexID : extraMaterials[s-1].glTexID; }
    const std::string& matName(int s) const { return s == 0 ? materialName : extraMaterials[s-1].name; }
    const std::string& matPath(int s) const { return s == 0 ? texturePath : extraMaterials[s-1].texturePath; }
    // UV wrap mode per slot (0=Clip/clamp, 1=Extend/tile, 2=Mirror).
    int  matWrap(int s) const { return s == 0 ? wrapMode : extraMaterials[s-1].wrapMode; }
    void setMatWrap(int s, int w) { if (s == 0) wrapMode = w; else extraMaterials[s-1].wrapMode = w; }

    // Collision proxy box. Authored here, drawn as a wireframe in the 3D tab.
    // Local-space (follows the placed glTF's transform); static — does not
    // deform with the animation. collisionType: 0 = None, 1 = Box (AABB).
    // (Use-mesh-as-collision stays a separate feature for static props.)
    int   collisionType = 0;
    float colCenter[3]  = {0.0f, 0.0f, 0.0f};   // box center, rig local space
    float colExtents[3] = {1.0f, 1.0f, 1.0f};   // box half-extents
};

static constexpr int kMaxRiggedMeshAssets = 16;

// A sprite object placed on the Mode 4 floor
// A named camera preset attached to the player object. Runtime slot 0 is always
// the scene default (the CameraStartObject); these are the extra slots a
// SetCamera node switches to on an event. The camera still orbit-follows the
// player, but with the slot's angle/pitch/distance/height (smoothly blended).
struct CameraSlot
{
    std::string name = "Camera";  // label shown in the player panel + node dropdown
    float angle    = 0.0f;        // orbit yaw, degrees
    float horizon  = 60.0f;       // NDS Mode-4 horizon line (editor px); PSV uses orbitPitch
    float distance = 0.0f;        // orbit distance, editor units (0 = keep scene default)
    float height   = 14.0f;       // camera height, editor units
    float orbitPitch = 0.0f;      // orbit pitch in degrees (0 = auto from height/dist);
                                  // PSV, identical authoring to CameraStartObject.orbitPitch
    float lookYaw = 0.0f;         // horizontal AIM rotation in degrees — pans the camera's
                                  // look direction left/right (subject off-center), independent
                                  // of the orbit Yaw (which moves the eye). PSV.
    bool  lockAware = false;      // PSV: while locked-on, ON = keep the lock target framed (camera
                                  // faces the enemy at this slot's dist/height/pan); OFF = point
                                  // where the slot's Yaw says relative to the model.
    float hOffset = 0.0f;         // horizontal TRANSLATE of the framing, editor units — slides the
                                  // camera sideways (left/right) so the framing moves laterally,
                                  // distinct from H Rotation (which spins the aim). PSV.
    float depthOffset = 0.0f;     // forward/back TRANSLATE (dolly), editor units — slides the camera
                                  // along its look direction (+ = toward the subject, - = away). PSV.
    float lookPitch = 0.0f;       // vertical AIM rotation in degrees — tilts the camera's look
                                  // up/down (+ = up) so the subject sits higher/lower in frame,
                                  // without moving the eye (that's Pitch). PSV.
    float vOffset = 0.0f;         // vertical TRANSLATE of the framing, editor units — slides the
                                  // camera (and look point) up/down in world Y so the whole shot
                                  // moves vertically without tilting. + = up. PSV.
};

// One shot in a keyframed camera path (cutscene). Snapshotted from the editor's
// 3D viewport camera ("Add Keyframe from Camera"), then editable in the viewport.
// eye = world position; yaw/pitch = the LOOK direction in radians, stored as
// atan2(f.x,f.z)/asin(f.y) from f = normalize(target - eye) so they stay valid
// after the eye point is moved (decoupled from any orbit target).
struct CameraKeyframe
{
    int   frame  = 0;            // timeline position (ticks @ the path's authored fps)
    float ex = 0.0f, ey = 0.0f, ez = 0.0f;  // eye world position (editor units)
    float yaw    = 0.0f;         // look yaw, radians
    float pitch  = 0.0f;         // look pitch, radians
    float fov    = 45.0f;        // field of view, degrees (reserved; 0 = scene default)
    int   interp = 2;            // EYE-PATH shape: 0 = Constant, 1 = Linear, 2 = Curved (Catmull-Rom)
    float speed  = 1.0f;         // playback-rate multiplier for the SEGMENT starting at this keyframe
                                 // (2 = that leg plays twice as fast; unused on the last keyframe)
    bool  smoothIn  = true;      // ease TIME as the camera ARRIVES at this keyframe (decelerate in)
    bool  smoothOut = true;      // ease TIME as the camera LEAVES this keyframe (accelerate out)
                                 // independent toggles; apply in Linear eye mode too
};

// A named keyframed camera path (one cutscene, e.g. "intro" / "victory" / "failure").
// A player can hold several; the Play Camera Anim node picks one by index (Anim pin).
struct CameraAnim
{
    char name[32] = "Camera Anim";
    int  fps = 30;                       // playback fps (speed/step derivation like HUD layers)
    bool smoothPath = false;             // true = traverse ALL points as one continuous motion
                                         // (uniform-knot Catmull-Rom + a single global ease in/out);
                                         // ignores per-keyframe interp/ease/speed. Kills the per-point
                                         // "ramp/stop" jitter for fly-throughs.
    std::vector<CameraKeyframe> keyframes;
};

struct FloorSprite
{
    float x = 0.0f;          // world X
    float y = 0.0f;          // world Y (height above floor)
    float z = 0.0f;          // world Z
    float scale = 1.0f;      // size multiplier (R + drag to adjust, 1.0 = default)
    float rotation = 0.0f;   // Y-axis rotation in degrees (0=N, 90=E, 180=S, 270=W)
    float rotationX = 0.0f;  // X-axis rotation in degrees (pitch)
    float rotationZ = 0.0f;  // Z-axis rotation in degrees (roll)
    SpriteType type = SpriteType::Prop; // object type
    int   spriteId = 0;      // which sprite graphic (legacy)
    int   assetIdx = -1;     // index into sprite asset list (-1 = none)
    int   meshIdx  = -1;     // index into mesh asset list (-1 = none, used when type==Mesh)
    int   animIdx  = 0;      // which animation to play
    bool  animEnabled = true; // false = static (no animation cycling)
    bool  forceStatic = false; // compact: show same frame at all angles (saves VRAM)
    bool  drawBehind = false;  // true = draw behind meshes (OAM priority 2)
    bool  drawBehindNoSky = false; // true = don't draw behind skybox (clear sky pixels in sprite rect)
    uint32_t drawBehindExceptions = 0; // bitmask: bit N = sprite[N] is exempt (draw in front of)
    uint32_t drawBehindClipPlane = 0;  // bitmask: bit N = sprite[N] clips via plane (same-side check)
    bool  skipProximity = false; // true = always render regardless of draw distance
    bool  billboard = false;     // mesh only: always face camera (Y-axis billboard)
    float drawBehindThreshold = 0.0f; // Y offset added to sprite for above/below mesh check
    int   meshSpriteIdx = -1;    // mesh only: sprite asset to display on top of the mesh (-1 = none)
    int   spriteDrawPriority = 0; // -8..+8: higher = drawn over other sprites (OAM order)
    int   blitSlot = -1;          // -1 = auto-assign, 0/1/2 = manual palette slot for blit
    // Navigation (PSV navmesh, NPC/Enemy only): the exported NPC paths across
    // the baked Recast navmesh at runtime.
    int   navMode = 0;            // 0 = off, 1 = follow player, 2 = wander
    float navSpeed = 5.0f;        // editor units per frame (5 ~= player walk speed)
    float navStopDist = 32.0f;    // stop this close to the goal, editor units (follow mode)
    int   navRepath = 30;         // frames between path recomputes
    int   navMoveClip = -1;       // rig clip to play while moving (-1 = keep current)
    int   navPauseMin = 60;       // wander: min idle frames between legs
    int   navPauseMax = 180;      // wander: max idle frames between legs (random in [min,max])
    // Nav bounds box (Nebula NavMesh3DNode semantics): an axis-aligned volume
    // centered on x/y/z. When any exist, only scene geometry with a vertex
    // inside a box participates in the navmesh build, so the walkable surface
    // conforms over the actual objects inside it. No boxes = whole scene.
    bool  isNavPlane = false;     // (field name kept for save compat)
    float navPlaneW = 64.0f;      // box extent X, editor units
    float navPlaneD = 64.0f;      // box extent Z, editor units
    float navPlaneH = 64.0f;      // box extent Y, editor units
    bool  navNegate = false;      // negator: carve walkable area OUT (wins over walkable boxes)
    uint32_t color = 0xFFFF00FF; // tint color (ABGR) — used for editor preview
    bool  selected = false;
    // Attached sub-sprites (extra sprite layers with local offsets)
    struct SubSprite {
        int   assetIdx = -1;
        int   animIdx  = 0;
        bool  animEnabled = true;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        int   drawOrder = 1; // 0 = behind parent, 1 = in front
        float scale = 1.0f;  // size multiplier
        bool  forceStatic = false; // render as static (same frame at all angles)
        bool  grounded = false;    // stay on ground (Y=0) instead of following parent Y
        bool  hidden = false;      // start invisible (editor + runtime); a Cast Effect node
                                   // shows it, plays its anim once, then auto-hides it
        int   boneIdx = -1;        // -1 = anchor to parent origin; >=0 = ride this rig bone
                                   // (player rig only for now); offset X/Y/Z is bone-relative
        bool  driveThroughElement = false; // run a HUD element's keyframe animation
                                   // (rotation/scale) on this sub-sprite's own graphic,
                                   // keeping its exact 3D/bone position
        int   driveElementIdx = -1;        // which HUD element supplies the animation (-1 = none)
        int   effectKind = 0;              // 0 = draw the sprite graphic; >0 = draw a procedural
                                           // effect at this sub-sprite instead (1 = Aura Sphere).
                                           // Lets an attached orb (e.g. the focus ball) be a glow FX.
    };
    static constexpr int kMaxSubSprites = 4;
    SubSprite subSprites[kMaxSubSprites];
    int subSpriteCount = 0;
    // Attached models (extra 3D mesh layers riding the object, e.g. a charge aura).
    // Imported as a single-texture OBJ (MeshAsset); the texture uses the sprite-style
    // 128-color + soft-alpha pipeline. Optionally bone-driven like SubSprite.
    struct SubModel {
        int   meshIdx = -1;        // index into sMeshAssets (-1 = none)
        float offsetX = 0.0f;      // local offset (bone-relative if boneIdx>=0)
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        float scale = 1.0f;        // size multiplier
        float yaw = 0.0f;          // extra spin about Y (degrees)
        bool  grounded = false;    // stay on ground (Y=0) instead of following parent Y
        bool  hidden = false;      // start invisible at RUNTIME; a Cast Effect node reveals it
        bool  editorHide = false;  // hide in the EDITOR preview only (declutter); still shows in-game
        int   boneIdx = -1;        // -1 = anchor to parent origin; >=0 = ride this rig bone
                                   // (player rig only for now); offset X/Y/Z is bone-relative
        float shadowIntensity = -1.0f; // -1 = inherit the parent rig's; >=0 = own shading depth (0..2)
        bool  customLight = false;     // true = own camera-light aim (below) instead of the parent rig's
        float lightX = 50.0f, lightY = 180.0f; // own headlamp aim (pitch/yaw deg off camera)
    };
    static constexpr int kMaxSubModels = 2;
    SubModel subModels[kMaxSubModels];
    int subModelCount = 0;
    // Blueprint script attachment
    int   blueprintIdx = -1;         // index into sBlueprintAssets (-1 = none)
    struct { int paramIdx; int value; } instanceParams[8] = {};
    int   instanceParamCount = 0;

    // Grind rail: when isGrindRail, this mesh object is a grindable rail and
    // railPath holds the hand-authored centerline the player slides along (in
    // world units, same space as x/y/z). Edited per-object in the 3D tab's mesh
    // panel; exported into mapdata.h so the runtime follows it directly instead
    // of deriving a centerline from faceted geometry at 60fps.
    bool  isGrindRail = false;
    bool  railSpline = false;   // runtime follows a smooth Catmull-Rom curve through the points
    struct RailPoint { float x = 0.0f, y = 0.0f, z = 0.0f; bool isEnd = false; bool isBounce = false; bool isStart = false; };
    // isEnd: a launch-off terminus. The runtime's width-catch only re-grabs an
    // End point when you're moving TOWARD the rail, so you vault off cleanly but
    // can still re-catch from the approach side.
    // isBounce: a bumper terminus. Reaching it reverses the grind direction
    // (bounce back) instead of launching off — unless you jump off just before.
    // isStart: a clean-exit terminus (same no-re-grab behavior as End) — where
    // you slide off smoothly after bouncing back, without the re-catch teleport.
    static constexpr int kMaxRailPoints = 512;
    RailPoint railPath[kMaxRailPoints];
    int   railPointCount = 0;

    // Rigged (skinned glTF) mesh — DSMA skeletal animation. -1 = none.
    // When set, this object renders as an animated skinned mesh (Mode 4) instead
    // of (or in addition to) its sprite, both in the editor preview and on NDS.
    int   riggedMeshIdx = -1;    // index into sRiggedMeshAssets
    int   rigAnimIdx = 0;        // which animation clip to play
    bool  rigAnimPlay = true;    // play the clip in the editor preview
    float modelYaw = 0.0f;       // PER-OBJECT yaw correction (deg), added on top of the
                                 // rig asset's shared yawOffset — so two objects sharing a
                                 // glTF can face different ways without rotating each other.
    float rigAnimClock = 0.0f;   // transient editor-only playback frame (NOT serialized)

    // Player camera presets (Mode 4). Only meaningful on the player object
    // (type == Player). Runtime slot 0 = scene default; these are slots 1..N a
    // SetCamera node switches to. See CameraSlot.
    std::vector<CameraSlot> cameraSlots;

    // Keyframed camera path for cutscenes. Player object only. Authored in the
    // Meshes tab (Add Keyframe from Camera) + editable in the 3D viewport; played
    // by the Play Camera Anim node. One path per player (v1).
    static constexpr int kMaxCameraKeyframes = 64;
    static constexpr int kMaxCameraAnims = 16;
    std::vector<CameraAnim> cameraAnims;   // named cutscene paths (intro/victory/...)
    int cameraAnimSel = 0;                 // editor: which animation is being edited (not exported)
};

static constexpr int kMaxFloorSprites = 256; // supports up to 256 objects per scene, nearest 32 rendered via OAM

// Camera start object — the "game camera" position for Play mode
struct CameraStartObject
{
    float x = 0.0f;
    float z = 0.0f;
    float height = 14.0f;
    float angle = 0.0f;
    float horizon = 60.0f;
    // Initial orbit pitch in degrees (positive = look down). 0 = auto: derive
    // from camera height/distance (the legacy behavior). PSV-consumed.
    float orbitPitch = 0.0f;
    // Movement speeds
    float walkSpeed   = 35.0f;
    float sprintSpeed = 53.0f;
    // Camera follow rates (percentage per frame, 1-100)
    float walkEaseIn  = 19.0f;
    float walkEaseOut = 19.0f;
    float sprintEaseIn  = 6.0f;
    float sprintEaseOut = 12.0f;
    // Jump physics
    float jumpForce    = 2.0f;   // initial upward velocity (pixels)
    float gravity      = 0.09f;  // downward accel per frame (pixels)
    float maxFallSpeed = 6.0f;   // terminal velocity (pixels)
    // Jump camera follow
    float jumpCamLand  = 37.0f;  // camera Y catch-up % when grounded
    float jumpCamAir   = 12.0f;  // camera Y catch-up % when airborne
    // Auto-orbit speed (brads per frame, 0 = disabled)
    float autoOrbitSpeed = 205.0f;
    // Jump dampen factor (0-1, applied each frame while A released and rising)
    float jumpDampen = 0.75f;
    // Orbit camera lerp speed (% per frame). Ease-in applies while L/R
    // is held (camera follows the moving orbit target). Ease-out applies
    // after release so the camera settles. Higher = camera tracks the
    // target faster, keeping the player sprite more centered. NDS-only.
    float orbitCamEaseIn  = 25.0f;
    float orbitCamEaseOut = 50.0f;
    // Per-frame cap on orbit_angle change. The OrbitCamera script can
    // request a large rotation per frame, but if the camera lerp can't
    // keep up the player sprite drifts toward the screen edge. Capping
    // the per-frame delta forces orbit to ramp smoothly within what the
    // lerp can handle. 0 = uncapped, higher = faster max orbit.
    int orbitMaxDelta = 0;
    bool orbitSnapCam = false;  // legacy field (unused)
    // Draw distance (0 = unlimited)
    float drawDistance = 0.0f;
    float spriteDrawDistance = 0.0f;
    // Performance toggles
    int smallTriCull = 0;   // min screen-space area to render (0=off)
    bool skipFloor = false; // skip floor rendering entirely
    bool coverageBuf = false; // front-to-back with coverage buffer (reduces overdraw)
    // Camera pitch (per-vertex depth-based tilt, .8 fixed on GBA)
    float camPitch = 0.0f;
    bool autoPitch = false;    // dynamically compute pitch from floor slope
    bool horizonClamp = false; // clamp vertices above camera to horizon
    bool faceCull = false;     // skip faces with any vertex above camera
    bool dynamicHorizon = false; // shift horizon line based on floor slope
    bool wallAware = false;    // PSV: pull the orbit camera in when a wall is between it and the player (no clip-through)
};

// Map data — the floor plane rendered by Mode 4
struct Mode7Map
{
    Tileset      tileset;
    TilemapLayer floor;      // BG2 affine layer
};

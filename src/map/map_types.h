#pragma once

#include <cstdint>
#include <vector>
#include <string>

// GBA screen dimensions
static constexpr int kGBAWidth  = 240;
static constexpr int kGBAHeight = 160;

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
static constexpr int kMaxFrameSize = 64; // max 64x64 pixels

struct SpriteFrame
{
    uint8_t pixels[kMaxFrameSize * kMaxFrameSize] = {}; // palette indices 0-15
    int width  = 8;
    int height = 8;
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

// Object types for Mode 7 scene entities
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

    // Texture mapping
    bool textured = false;                    // true = use texture, false = flat shaded
    std::string texturePath;                  // source PNG path
    std::vector<uint8_t> texturePixels;       // quantized indexed pixels (texW * texH)
    uint32_t texturePalette[16] = {};         // RGBA8 palette for the texture
    int texW = 0, texH = 0;                  // texture dimensions (power of 2, max 256)
    unsigned int glTexID = 0;                 // OpenGL texture for editor preview
    bool texFiltered = false;                 // true = GL_LINEAR, false = GL_NEAREST
};

static constexpr int kMaxMeshAssets = 32;

// A sprite object placed on the Mode 7 floor
struct FloorSprite
{
    float x = 0.0f;          // world X
    float y = 0.0f;          // world Y (height above floor)
    float z = 0.0f;          // world Z
    float scale = 1.0f;      // size multiplier (R + drag to adjust, 1.0 = default)
    float rotation = 0.0f;   // world-space facing angle in degrees (0=N, 90=E, 180=S, 270=W)
    SpriteType type = SpriteType::Prop; // object type
    int   spriteId = 0;      // which sprite graphic (legacy)
    int   assetIdx = -1;     // index into sprite asset list (-1 = none)
    int   meshIdx  = -1;     // index into mesh asset list (-1 = none, used when type==Mesh)
    int   animIdx  = 0;      // which animation to play
    bool  animEnabled = true; // false = static (no animation cycling)
    uint32_t color = 0xFFFF00FF; // tint color (ABGR) — used for editor preview
    bool  selected = false;
};

static constexpr int kMaxFloorSprites = 64; // GBA OAM has 128 slots, reserve half

// Camera start object — the "game camera" position for Play mode
struct CameraStartObject
{
    float x = 0.0f;
    float z = 0.0f;
    float height = 14.0f;
    float angle = 0.0f;
    float horizon = 60.0f;
    // Movement speeds
    float walkSpeed   = 35.0f;
    float sprintSpeed = 53.0f;
    // Camera follow rates (percentage per frame, 1-100)
    float walkEaseIn  = 19.0f;
    float walkEaseOut = 19.0f;
    float sprintEaseIn  = 6.0f;
    float sprintEaseOut = 12.0f;
    // Draw distance (0 = unlimited)
    float drawDistance = 0.0f;
    // Performance toggles
    int smallTriCull = 0;   // min screen-space area to render (0=off)
    bool skipFloor = false; // skip floor rendering entirely
    bool coverageBuf = false; // front-to-back with coverage buffer (reduces overdraw)
};

// Map data — the floor plane rendered by Mode 7
struct Mode7Map
{
    Tileset      tileset;
    TilemapLayer floor;      // BG2 affine layer
};

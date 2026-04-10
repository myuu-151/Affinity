#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Affinity
{

// Sprite data for GBA export
struct GBASpriteExport
{
    float x, y, z;     // world position (editor coords, ±512)
    float scale;
    float rotation;     // world-space facing angle in degrees
    int   palIdx;       // OBJ palette color index (1-5)
    int   assetIdx;     // sprite asset index (-1 = none/placeholder)
    int   animIdx;      // default animation index
    int   spriteType;   // SpriteType enum (0=Prop, 1=Player, ...)
    bool  animEnabled;  // false = static, no animation cycling
    int   meshIdx = -1; // mesh asset index (-1 = none)
};

// Player direction sprite for GBA export (RGBA8 image)
struct GBAPlayerDirExport
{
    const unsigned char* pixels;  // RGBA8 data (nullptr if empty)
    int width, height;
};

// Sprite asset frame for GBA export — 4bpp pixel data
static constexpr int kExportMaxFrameSize = 64;

struct GBASpriteFrameExport
{
    uint8_t pixels[kExportMaxFrameSize * kExportMaxFrameSize]; // palette indices 0-15
    int width, height;
};

// Sprite asset animation for GBA export
struct GBASpriteAnimExport
{
    int startFrame, endFrame;
    int fps;
    bool loop;
    float speed = 1.0f;
    int gameState = 0; // 0=None, 1=Idle, 2=Walk, 3=Run, 4=Sprint
};

// Sprite asset for GBA export — tile data + palette + animations
struct GBASpriteAssetExport
{
    std::string name;
    int baseSize;          // 8, 16, or 32
    int palBank;           // GBA OBJ palette bank (0-15)
    uint32_t palette[16];  // RGBA8 colors (index 0 = transparent)
    std::vector<GBASpriteFrameExport> frames;
    std::vector<GBASpriteAnimExport>  anims;
    int defaultAnim;

    // Directional sprite animation sets (each set = 8 direction RGBA8 images)
    struct DirAnimSetExport
    {
        std::string name;
        GBAPlayerDirExport dirImages[8] = {};
    };
    bool hasDirections = false;
    int paletteSrc = -1;       // -1 = own palette, >= 0 = share from asset index
    std::vector<DirAnimSetExport> dirAnimSets;
};

// Camera start data for GBA export
struct GBACameraExport
{
    float x, z;
    float height;
    float angle;        // radians
    float horizon;
    float walkSpeed   = 35.0f;
    float sprintSpeed = 53.0f;
    float walkEaseIn  = 19.0f;
    float walkEaseOut = 19.0f;
    float sprintEaseIn  = 6.0f;
    float sprintEaseOut = 12.0f;
    float jumpForce     = 2.0f;   // initial upward velocity (pixels)
    float gravity       = 0.09f;  // downward accel per frame (pixels)
    float maxFallSpeed  = 6.0f;   // terminal velocity (pixels)
    float jumpCamLand   = 37.0f;  // camera Y catch-up % when grounded
    float jumpCamAir    = 12.0f;  // camera Y catch-up % when airborne
    float autoOrbitSpeed = 205.0f; // brads per frame when strafing (0 = disabled)
    float jumpDampen = 0.75f;     // velocity multiplier per frame while A released + rising
    float drawDistance  = 0.0f;   // 0 = unlimited
    int   smallTriCull  = 0;      // min screen-space area to render (0=off)
    bool  skipFloor     = false;  // skip floor rendering entirely
    bool  coverageBuf   = false;  // front-to-back rendering with coverage buffer
};

// Mesh asset for GBA export
struct GBAMeshExport
{
    std::vector<float> positions; // px, py, pz per vertex (flat)
    std::vector<float> normals;   // nx, ny, nz per vertex (flat)
    std::vector<int>   objPosIdx; // original OBJ 'v' index per vertex (for welding)
    std::vector<uint32_t> indices;      // triangle indices (3 per face)
    std::vector<uint32_t> quadIndices;  // quad indices (4 per face)
    uint16_t colorRGB15;          // base color for shading
    int cullMode = 0;             // 0=Back, 1=Front, 2=None
    int exportMode = 0;           // 0=Quality (no weld), 1=Performance (welded)
    int lit = 1;                  // 1=lit (shaded), 0=unlit (flat color)
    int halfRes = 0;              // 1=rasterize every other scanline
    int wireframe = 0;            // 1=wireframe overlay
    int grayscale = 0;            // 1=grayscale shaded faces
    float drawDistance = 0.0f;    // per-mesh draw distance (0 = use global/unlimited)
    int collision = 1;            // 1 = generate collision faces, 0 = no collision
    int drawPriority = 0;         // 0 = draws on top (last), higher = draws first
    int visible = 1;              // 1 = render, 0 = invisible collision-only
    // Texture mapping
    std::vector<float> uvs;       // u, v per vertex (flat, interleaved)
    int textured = 0;             // 1 = textured, 0 = flat shaded
    int texW = 0, texH = 0;      // texture dimensions
    std::vector<uint8_t> texPixels;   // quantized indexed pixels (texW * texH)
    uint16_t texPalette[16] = {}; // RGB15 palette for this texture
};

// ---- Visual Script Export ----

// Node types matching editor VsNodeType enum
enum class GBAScriptNodeType : int {
    OnKeyPressed = 0, OnKeyReleased, OnKeyHeld,
    OnCollision, OnStart,
    Branch, CompareVar,
    MovePlayer, LookDirection, ChangeScene, SetVariable, AddVariable,
    PlaySound, Wait, Jump,
    Walk, Sprint, OrbitCamera, PlayAnim,
    SetGravity, SetMaxFall, DestroyObject,
    AutoOrbit, DampenJump,
    Integer, Key, Direction, Animation, Float,
    OnUpdate,
    Group,
    Object,
    CustomCode,
    COUNT
};

struct GBAScriptNodeExport {
    int id;
    GBAScriptNodeType type;
    int paramInt[4];  // per-node params (key index, value, IEEE754 float bits, etc.)
    char customCode[512] = {};  // user-editable code override (empty = use default)
    char funcName[64] = {};     // custom function name (empty = use default)
};

struct GBAScriptLinkExport {
    int fromNodeId;
    int fromPinType;  // 0=execOut, 2=dataOut
    int fromPinIdx;
    int toNodeId;
    int toPinType;    // 1=execIn, 3=dataIn
    int toPinIdx;
};

struct GBAScriptExport {
    std::vector<GBAScriptNodeExport> nodes;
    std::vector<GBAScriptLinkExport> links;
};

// ---- Blueprint Export ----

struct GBABlueprintParamExport {
    int dataType;      // 0=int, 1=float, 2=key, 3=direction, 4=animation
    int defaultValue;
};

struct GBABlueprintExport {
    std::string name;
    GBAScriptExport script;  // nodes + links
    std::vector<GBABlueprintParamExport> params;
};

struct GBABlueprintInstanceExport {
    int blueprintIdx;
    int spriteIdx;      // -1 if tilemap object
    int tmObjIdx;       // -1 if 3D sprite
    int paramValues[8]; // resolved values (override or default)
    int paramCount;
};

// Package the current map into a .gba ROM.
// runtimeDir: path to gba_runtime/ directory
// outputPath: where to write the final .gba
// Returns true on success. errorMsg receives details on failure.
bool PackageGBA(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                const GBAScriptExport& script,
                const std::vector<GBABlueprintExport>& blueprints,
                const std::vector<GBABlueprintInstanceExport>& bpInstances,
                std::string& errorMsg);

} // namespace Affinity

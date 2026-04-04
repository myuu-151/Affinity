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
    std::vector<uint32_t> indices;
    uint16_t colorRGB15;          // base color for shading
    int cullMode = 0;             // 0=Back, 1=Front, 2=None
    int exportMode = 0;           // 0=Quality (no weld), 1=Performance (welded)
    int lit = 1;                  // 1=lit (shaded), 0=unlit (flat color)
    int halfRes = 0;              // 1=rasterize every other scanline
    int wireframe = 0;            // 1=grayscale wireframe only
    // Texture mapping
    std::vector<float> uvs;       // u, v per vertex (flat, interleaved)
    int textured = 0;             // 1 = textured, 0 = flat shaded
    int texW = 0, texH = 0;      // texture dimensions
    std::vector<uint8_t> texPixels;   // quantized indexed pixels (texW * texH)
    uint16_t texPalette[16] = {}; // RGB15 palette for this texture
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
                std::string& errorMsg);

} // namespace Affinity

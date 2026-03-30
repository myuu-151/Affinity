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
    int   palIdx;       // OBJ palette color index (1-5)
    int   assetIdx;     // sprite asset index (-1 = none/placeholder)
    int   animIdx;      // default animation index
};

// Sprite asset frame for GBA export — 4bpp pixel data
static constexpr int kExportMaxFrameSize = 32;

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
};

// Camera start data for GBA export
struct GBACameraExport
{
    float x, z;
    float height;
    float angle;        // radians
    float horizon;
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
                std::string& errorMsg);

} // namespace Affinity

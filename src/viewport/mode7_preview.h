#pragma once

#include "../math/math_types.h"
#include "../map/map_types.h"

// Software Mode 7 rasterizer — renders the GBA-resolution preview into an RGB buffer.
// The buffer is 240x160x3 bytes (RGB8). Upload to an OpenGL texture for display.

namespace Mode7
{

// Initialize the preview (creates checkerboard tileset if no map loaded)
void Init();

// Player directional sprite data for the renderer
struct PlayerDirImage
{
    const unsigned char* pixels; // RGBA8 data (or nullptr if not assigned)
    int width, height;
};
constexpr int kPlayerDirCount = 8;

// Per-asset directional sprite images (8 directions each)
struct AssetDirImages
{
    PlayerDirImage dirs[8]; // N, NE, E, SE, S, SW, W, NW
};

// Render one frame of Mode 7 into the pixel buffer
void Render(const Mode7Camera& cam, const Mode7Map* map = nullptr,
            const FloorSprite* sprites = nullptr, int spriteCount = 0,
            const CameraStartObject* camObj = nullptr, float camObjScale = 1.0f,
            const SpriteAsset* assets = nullptr, int assetCount = 0,
            float animTime = 0.0f, bool playing = false,
            const PlayerDirImage* playerDirs = nullptr, float playerOrbitAngle = 0.0f,
            const AssetDirImages* assetDirImages = nullptr, int assetDirCount = 0);

// Projected sprite screen position (set after Render)
struct SpriteScreenPos { int screenX, screenY, halfW, halfH, spriteIdx; };

// Get the last frame's projected sprite positions (count returned)
int GetProjectedSprites(const SpriteScreenPos** out);

// Get the pixel buffer (240 * 160 * 3 bytes, RGB8)
const unsigned char* GetFrameBuffer();

// Get the OpenGL texture ID (created/updated each frame)
unsigned int GetTexture();

// Update the GL texture from the frame buffer
void UploadTexture();

} // namespace Mode7

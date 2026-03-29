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

// A sprite object placed on the Mode 7 floor
struct FloorSprite
{
    float x = 0.0f;          // world X
    float y = 0.0f;          // world Y (height above floor)
    float z = 0.0f;          // world Z
    float scale = 1.0f;      // size multiplier (R + drag to adjust)
    int   spriteId = 0;      // which sprite graphic (index into sprite sheet)
    uint32_t color = 0xFFFF00FF; // tint color (ABGR) — used for editor preview
    bool  selected = false;
};

static constexpr int kMaxFloorSprites = 64; // GBA OAM has 128 slots, reserve half

// Camera start object — the "game camera" position for Play mode
struct CameraStartObject
{
    float x = 0.0f;
    float z = 0.0f;
    float height = 64.0f;
    float angle = 0.0f;
    float horizon = 54.0f;
};

// Map data — the floor plane rendered by Mode 7
struct Mode7Map
{
    Tileset      tileset;
    TilemapLayer floor;      // BG2 affine layer
};

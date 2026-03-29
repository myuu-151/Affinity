#pragma once

#include "../math/math_types.h"
#include "../map/map_types.h"

// Software Mode 7 rasterizer — renders the GBA-resolution preview into an RGB buffer.
// The buffer is 240x160x3 bytes (RGB8). Upload to an OpenGL texture for display.

namespace Mode7
{

// Initialize the preview (creates checkerboard tileset if no map loaded)
void Init();

// Render one frame of Mode 7 into the pixel buffer
void Render(const Mode7Camera& cam, const Mode7Map* map = nullptr);

// Get the pixel buffer (240 * 160 * 3 bytes, RGB8)
const unsigned char* GetFrameBuffer();

// Get the OpenGL texture ID (created/updated each frame)
unsigned int GetTexture();

// Update the GL texture from the frame buffer
void UploadTexture();

} // namespace Mode7

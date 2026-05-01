#pragma once

#include <cstdint>

// Fixed-point type matching GBA 8.8 format
using Fixed8  = int16_t;
// Fixed-point 16.16 for higher precision editor math
using Fixed16 = int32_t;

struct Vec2i { int x, y; };
struct Vec2f { float x, y; };

// Mode 4 camera state
struct Mode7Camera
{
    float x     = 0.0f;   // world X
    float z     = 0.0f;   // world Z (forward)
    float height = 64.0f; // camera height above ground plane
    float angle   = 0.0f;  // yaw in radians
    float fov     = 128.0f; // focal distance D (perspective strength)
    float horizon = 54.0f;  // horizon scanline (pitch control)
};

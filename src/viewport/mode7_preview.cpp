#include "mode7_preview.h"
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

namespace Mode7
{

static unsigned char sFrameBuf[kGBAWidth * kGBAHeight * 3];
static GLuint        sTexture = 0;

// Default checkerboard palette
static const uint8_t kCheckA[3] = { 80, 160, 80 };   // green
static const uint8_t kCheckB[3] = { 40,  80, 40 };   // dark green
static const uint8_t kSkyCol[3] = { 100, 140, 200 }; // sky blue

void Init()
{
    memset(sFrameBuf, 0, sizeof(sFrameBuf));

    if (!sTexture)
    {
        glGenTextures(1, &sTexture);
        glBindTexture(GL_TEXTURE_2D, sTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, kGBAWidth, kGBAHeight, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, sFrameBuf);
    }
}

// Sample the floor plane at world coordinates (wx, wz).
// If a map is provided, sample from its tilemap; otherwise use a checkerboard.
static void SampleFloor(float wx, float wz, const Mode7Map* map,
                         uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (map && !map->tileset.tiles.empty())
    {
        // Map pixel coordinates from world space
        int px = (int)floorf(wx) & ((map->floor.width * kTileSize) - 1);
        int py = (int)floorf(wz) & ((map->floor.height * kTileSize) - 1);
        if (px < 0) px += map->floor.width * kTileSize;
        if (py < 0) py += map->floor.height * kTileSize;

        int tileX = px / kTileSize;
        int tileY = py / kTileSize;
        int localX = px % kTileSize;
        int localY = py % kTileSize;

        int tileIdx = 0;
        int mapIdx = tileY * map->floor.width + tileX;
        if (mapIdx >= 0 && mapIdx < (int)map->floor.tileIndices.size())
            tileIdx = map->floor.tileIndices[mapIdx];

        uint8_t palIdx = 0;
        if (tileIdx >= 0 && tileIdx < (int)map->tileset.tiles.size())
            palIdx = map->tileset.tiles[tileIdx].pixels[localY * kTileSize + localX];

        uint32_t rgba = map->tileset.palette[palIdx];
        r = (rgba >>  0) & 0xFF;
        g = (rgba >>  8) & 0xFF;
        b = (rgba >> 16) & 0xFF;
    }
    else
    {
        // Checkerboard fallback — 16x16 pixel squares
        int cx = ((int)floorf(wx / 16.0f)) & 1;
        int cz = ((int)floorf(wz / 16.0f)) & 1;
        const uint8_t* col = (cx ^ cz) ? kCheckA : kCheckB;
        r = col[0]; g = col[1]; b = col[2];
    }
}

void Render(const Mode7Camera& cam, const Mode7Map* map)
{
    float cosA = cosf(-cam.angle);
    float sinA = sinf(-cam.angle);

    // Horizon line — where lambda would go to infinity
    int horizon = kGBAHeight / 3; // top third is sky

    for (int y = 0; y < kGBAHeight; y++)
    {
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;

        if (y <= horizon)
        {
            // Sky
            for (int x = 0; x < kGBAWidth; x++)
            {
                row[x * 3 + 0] = kSkyCol[0];
                row[x * 3 + 1] = kSkyCol[1];
                row[x * 3 + 2] = kSkyCol[2];
            }
            continue;
        }

        // Mode 7 per-scanline math (matches Tonc HBlank ISR)
        // lambda = camHeight / (scanline - horizon)
        float lambda = cam.height / (float)(y - horizon);

        // Direction vectors scaled by lambda
        float lcf = lambda * cosA;
        float lsf = lambda * sinA;

        // Left edge of this scanline in world space
        float startX = cam.x + (-120.0f * lcf) + (cam.fov * lsf);
        float startZ = cam.z + (-120.0f * lsf) - (cam.fov * lcf);

        // Per-pixel step
        float stepX = lcf;
        float stepZ = lsf;

        float wx = startX;
        float wz = startZ;

        for (int x = 0; x < kGBAWidth; x++)
        {
            uint8_t r, g, b;
            SampleFloor(wx, wz, map, r, g, b);

            // Distance fog — fade to sky color at far distances
            float fog = lambda / 300.0f;
            if (fog > 1.0f) fog = 1.0f;
            r = (uint8_t)(r * (1.0f - fog) + kSkyCol[0] * fog);
            g = (uint8_t)(g * (1.0f - fog) + kSkyCol[1] * fog);
            b = (uint8_t)(b * (1.0f - fog) + kSkyCol[2] * fog);

            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;

            wx += stepX;
            wz += stepZ;
        }
    }
}

const unsigned char* GetFrameBuffer()
{
    return sFrameBuf;
}

unsigned int GetTexture()
{
    return sTexture;
}

void UploadTexture()
{
    if (!sTexture) return;
    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGBAWidth, kGBAHeight,
                    GL_RGB, GL_UNSIGNED_BYTE, sFrameBuf);
}

} // namespace Mode7

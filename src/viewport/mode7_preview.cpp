#include "mode7_preview.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <GL/gl.h>

namespace Mode7
{

static unsigned char sFrameBuf[kGBAWidth * kGBAHeight * 3];
static GLuint        sTexture = 0;

// Last frame's projected sprites for viewport click-to-select
static SpriteScreenPos sLastProj[kMaxFloorSprites];
static int sLastProjCount = 0;

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

// Draw a filled rect into the framebuffer with alpha blend
static void DrawRect(int rx, int ry, int rw, int rh,
                     uint8_t cr, uint8_t cg, uint8_t cb, float alpha = 1.0f)
{
    int x0 = std::max(0, rx);
    int y0 = std::max(0, ry);
    int x1 = std::min(kGBAWidth,  rx + rw);
    int y1 = std::min(kGBAHeight, ry + rh);
    for (int y = y0; y < y1; y++)
    {
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = x0; x < x1; x++)
        {
            if (alpha >= 1.0f)
            {
                row[x*3+0] = cr; row[x*3+1] = cg; row[x*3+2] = cb;
            }
            else
            {
                row[x*3+0] = (uint8_t)(row[x*3+0]*(1-alpha) + cr*alpha);
                row[x*3+1] = (uint8_t)(row[x*3+1]*(1-alpha) + cg*alpha);
                row[x*3+2] = (uint8_t)(row[x*3+2]*(1-alpha) + cb*alpha);
            }
        }
    }
}

// Draw a diamond shape (sprite placeholder)
static void DrawDiamond(int cx, int cy, int halfW, int halfH,
                        uint8_t cr, uint8_t cg, uint8_t cb, float fogAlpha)
{
    int y0 = std::max(0, cy - halfH);
    int y1 = std::min(kGBAHeight, cy + halfH);
    for (int y = y0; y < y1; y++)
    {
        float fy = 1.0f - fabsf((float)(y - cy) / (float)halfH);
        int hw = (int)(halfW * fy);
        int x0 = std::max(0, cx - hw);
        int x1 = std::min(kGBAWidth, cx + hw);
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = x0; x < x1; x++)
        {
            // Blend with fog
            uint8_t fr = (uint8_t)(cr*(1-fogAlpha) + kSkyCol[0]*fogAlpha);
            uint8_t fg = (uint8_t)(cg*(1-fogAlpha) + kSkyCol[1]*fogAlpha);
            uint8_t fb = (uint8_t)(cb*(1-fogAlpha) + kSkyCol[2]*fogAlpha);
            row[x*3+0] = fr; row[x*3+1] = fg; row[x*3+2] = fb;
        }
    }
    // Outline
    if (halfW >= 2 && halfH >= 2)
    {
        uint8_t olr = (uint8_t)(cr * 0.5f);
        uint8_t olg = (uint8_t)(cg * 0.5f);
        uint8_t olb = (uint8_t)(cb * 0.5f);
        // Top and bottom points
        for (int dy = -halfH; dy <= halfH; dy++)
        {
            float fy = 1.0f - fabsf((float)dy / (float)halfH);
            int hw = (int)(halfW * fy);
            int py = cy + dy;
            if (py < 0 || py >= kGBAHeight) continue;
            uint8_t* row = sFrameBuf + py * kGBAWidth * 3;
            // Left edge
            int lx = cx - hw;
            if (lx >= 0 && lx < kGBAWidth)
            { row[lx*3+0] = olr; row[lx*3+1] = olg; row[lx*3+2] = olb; }
            // Right edge
            int rx2 = cx + hw - 1;
            if (rx2 >= 0 && rx2 < kGBAWidth)
            { row[rx2*3+0] = olr; row[rx2*3+1] = olg; row[rx2*3+2] = olb; }
        }
    }
}

// Sprite projection data for sorting
struct SpriteProj
{
    float depth;   // distance from camera (for sorting)
    int   screenX; // center X on screen
    int   screenY; // base Y on screen (foot position)
    float scale;   // size scale factor
    float fog;     // fog amount
    int   idx;     // index into sprite array
};

// Draw a small square (camera icon placeholder)
static void DrawSquare(int cx, int cy, int half, uint8_t cr, uint8_t cg, uint8_t cb, float fogAlpha)
{
    int x0 = std::max(0, cx - half);
    int y0 = std::max(0, cy - half);
    int x1 = std::min(kGBAWidth, cx + half);
    int y1 = std::min(kGBAHeight, cy + half);
    uint8_t fr = (uint8_t)(cr*(1-fogAlpha) + kSkyCol[0]*fogAlpha);
    uint8_t fg = (uint8_t)(cg*(1-fogAlpha) + kSkyCol[1]*fogAlpha);
    uint8_t fb = (uint8_t)(cb*(1-fogAlpha) + kSkyCol[2]*fogAlpha);
    for (int y = y0; y < y1; y++)
    {
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = x0; x < x1; x++)
        {
            // Border
            if (x == x0 || x == x1-1 || y == y0 || y == y1-1)
            { row[x*3+0] = 0; row[x*3+1] = 0; row[x*3+2] = 0; }
            else
            { row[x*3+0] = fr; row[x*3+1] = fg; row[x*3+2] = fb; }
        }
    }
    // Direction notch at top center
    if (cx >= 0 && cx < kGBAWidth && cy - half - 1 >= 0)
    {
        uint8_t* row = sFrameBuf + (cy - half - 1) * kGBAWidth * 3;
        int nx0 = std::max(0, cx - 1);
        int nx1 = std::min(kGBAWidth, cx + 2);
        for (int x = nx0; x < nx1; x++)
        { row[x*3+0] = fr; row[x*3+1] = fg; row[x*3+2] = fb; }
    }
}

void Render(const Mode7Camera& cam, const Mode7Map* map,
            const FloorSprite* sprites, int spriteCount,
            const CameraStartObject* camObj, float camObjScale)
{
    float cosA = cosf(-cam.angle);
    float sinA = sinf(-cam.angle);

    // Horizon line — controlled by camera pitch (I/K keys)
    int horizon = (int)cam.horizon;

    // --- Render floor ---
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
        float lambda = cam.height / (float)(y - horizon);

        float lcf = lambda * cosA;
        float lsf = lambda * sinA;

        float startX = cam.x + (-120.0f * lcf) + (cam.fov * lsf);
        float startZ = cam.z + (-120.0f * lsf) - (cam.fov * lcf);

        float stepX = lcf;
        float stepZ = lsf;

        float wx = startX;
        float wz = startZ;

        for (int x = 0; x < kGBAWidth; x++)
        {
            uint8_t r, g, b;
            SampleFloor(wx, wz, map, r, g, b);

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

    // --- Render sprites as scaled billboards ---
    if ((!sprites || spriteCount <= 0) && !camObj) return;
    if (!sprites) spriteCount = 0;

    // Project each sprite to screen space
    SpriteProj projected[kMaxFloorSprites];
    int projCount = 0;

    for (int i = 0; i < spriteCount && i < kMaxFloorSprites; i++)
    {
        // Vector from camera to sprite in world space
        float dx = sprites[i].x - cam.x;
        float dz = sprites[i].z - cam.z;

        // Invert the Mode 7 floor mapping to find screen position.
        // Floor forward: wx = cam.x + (px-120)*lambda*cosA + fov*lambda*sinA
        //                wz = cam.z + (px-120)*lambda*sinA - fov*lambda*cosA
        // Solving for lambda: fov*lambda = dx*sinA - dz*cosA
        //   => lambda = (dx*sinA - dz*cosA) / fov
        // Solving for px: (px-120)*lambda = dx*cosA + dz*sinA
        //   => px = 120 + (dx*cosA + dz*sinA) / lambda

        float fovLambda = dx * sinA - dz * cosA;

        // Skip if behind camera (fovLambda must be positive for objects in front)
        if (fovLambda <= 0.5f) continue;

        float lambda = fovLambda / cam.fov;

        // screenY from lambda = height / (y - horizon) => y = horizon + height / lambda
        int screenY = horizon + (int)((cam.height - sprites[i].y) / lambda);

        // screenX from the side component
        float sideComponent = dx * cosA + dz * sinA;
        int screenX = 120 + (int)(sideComponent / lambda);

        // Skip if completely off-screen
        if (screenY < 0 || screenY >= kGBAHeight) continue;
        if (screenX < -32 || screenX >= kGBAWidth + 32) continue;

        // Scale: sprite base size is 16px, scaled inversely by distance
        float scale = cam.height / lambda;
        float fog = lambda / 300.0f;
        if (fog > 1.0f) fog = 1.0f;

        // Don't draw if too fogged out
        if (fog > 0.95f) continue;

        projected[projCount].depth   = lambda;
        projected[projCount].screenX = screenX;
        projected[projCount].screenY = screenY;
        projected[projCount].scale   = scale;
        projected[projCount].fog     = fog;
        projected[projCount].idx     = i;
        projCount++;
    }

    // Sort back-to-front (farthest first)
    for (int i = 0; i < projCount - 1; i++)
        for (int j = i + 1; j < projCount; j++)
            if (projected[i].depth < projected[j].depth)
                std::swap(projected[i], projected[j]);

    // Draw each sprite as a diamond billboard and store projection for click-select
    sLastProjCount = 0;
    for (int i = 0; i < projCount; i++)
    {
        const SpriteProj& sp = projected[i];
        const FloorSprite& fs = sprites[sp.idx];

        // Extract color (ABGR format)
        uint8_t cr = (fs.color >>  0) & 0xFF;
        uint8_t cg = (fs.color >>  8) & 0xFF;
        uint8_t cb = (fs.color >> 16) & 0xFF;

        int halfW = std::max(2, (int)(8.0f * sp.scale / cam.height * 16.0f * fs.scale));
        int halfH = std::max(3, (int)(12.0f * sp.scale / cam.height * 16.0f * fs.scale));

        // Sprite draws upward from its foot position
        int drawCenterY = sp.screenY - halfH;

        DrawDiamond(sp.screenX, drawCenterY, halfW, halfH, cr, cg, cb, sp.fog);

        // Selection highlight: bright outline
        if (fs.selected)
        {
            DrawRect(sp.screenX - halfW - 1, drawCenterY - halfH - 1,
                     halfW * 2 + 2, 1, 255, 255, 255);
            DrawRect(sp.screenX - halfW - 1, drawCenterY + halfH,
                     halfW * 2 + 2, 1, 255, 255, 255);
        }

        // Store for viewport click-to-select
        sLastProj[sLastProjCount].screenX = sp.screenX;
        sLastProj[sLastProjCount].screenY = drawCenterY;
        sLastProj[sLastProjCount].halfW = halfW;
        sLastProj[sLastProjCount].halfH = halfH;
        sLastProj[sLastProjCount].spriteIdx = sp.idx;
        sLastProjCount++;
    }

    // --- Render camera start object on the floor ---
    if (camObj)
    {
        float dx = camObj->x - cam.x;
        float dz = camObj->z - cam.z;

        float fovLambda = dx * sinA - dz * cosA;
        if (fovLambda > 1.0f)
        {
            float lambda = fovLambda / cam.fov;
            int screenY = horizon + (int)(cam.height / lambda);
            float sideComponent = dx * cosA + dz * sinA;
            int screenX = 120 + (int)(sideComponent / lambda);

            float fog = lambda / 300.0f;
            if (fog > 1.0f) fog = 1.0f;

            float scale = cam.height / lambda;
            int half = std::max(2, (int)(10.0f * scale / cam.height * 16.0f * camObjScale));

            // Draw at foot position — bright white/cyan
            DrawSquare(screenX, screenY - half, half, 100, 220, 255, fog);
        }
    }
}

int GetProjectedSprites(const SpriteScreenPos** out)
{
    *out = sLastProj;
    return sLastProjCount;
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

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
        // World bounds match tilemap panel: ±512
        int mapW = map->floor.width * kTileSize;
        int mapH = map->floor.height * kTileSize;
        static constexpr float kHalf = 512.0f;
        if (wx < -kHalf || wx > kHalf || wz < -kHalf || wz > kHalf)
        {
            r = 30; g = 30; b = 35;
            return;
        }
        int px = ((int)floorf(wx) % mapW + mapW) % mapW;
        int py = ((int)floorf(wz) % mapH + mapH) % mapH;

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
        // Checkerboard fallback — bounded to default world (±512)
        static constexpr float defHalf = 512.0f;
        if (wx < -defHalf || wx > defHalf || wz < -defHalf || wz > defHalf)
        {
            r = 30; g = 30; b = 35;
            return;
        }
        int cx = ((int)floorf(wx / 32.0f)) & 1;
        int cz = ((int)floorf(wz / 32.0f)) & 1;
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

// Draw a filled triangle into the framebuffer (scanline rasterizer)
static void DrawTriangle(float x0, float y0, float x1, float y1, float x2, float y2,
                         uint8_t cr, uint8_t cg, uint8_t cb, float fogAlpha)
{
    // Sort vertices by Y
    if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
    if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
    if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }

    int iy0 = std::max(0, (int)ceilf(y0));
    int iy2 = std::min(kGBAHeight - 1, (int)floorf(y2));

    float totalH = y2 - y0;
    if (totalH < 0.5f) return;

    uint8_t fr = (uint8_t)(cr * (1 - fogAlpha) + kSkyCol[0] * fogAlpha);
    uint8_t fg = (uint8_t)(cg * (1 - fogAlpha) + kSkyCol[1] * fogAlpha);
    uint8_t fb = (uint8_t)(cb * (1 - fogAlpha) + kSkyCol[2] * fogAlpha);

    for (int y = iy0; y <= iy2; y++)
    {
        float t = (y - y0) / totalH;
        // Long edge: x0 -> x2
        float xLong = x0 + (x2 - x0) * t;
        // Short edges
        float xShort;
        if ((float)y < y1)
        {
            float segH = y1 - y0;
            if (segH < 0.5f) xShort = x0;
            else xShort = x0 + (x1 - x0) * ((y - y0) / segH);
        }
        else
        {
            float segH = y2 - y1;
            if (segH < 0.5f) xShort = x1;
            else xShort = x1 + (x2 - x1) * ((y - y1) / segH);
        }

        int left = std::max(0, (int)ceilf(std::min(xLong, xShort)));
        int right = std::min(kGBAWidth - 1, (int)floorf(std::max(xLong, xShort)));

        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = left; x <= right; x++)
        {
            row[x * 3 + 0] = fr;
            row[x * 3 + 1] = fg;
            row[x * 3 + 2] = fb;
        }
    }
}

// Draw a wireframe triangle edge
static void DrawLine(int ax, int ay, int bx, int by,
                     uint8_t cr, uint8_t cg, uint8_t cb)
{
    int dx = abs(bx - ax), dy = abs(by - ay);
    int sx = (ax < bx) ? 1 : -1;
    int sy = (ay < by) ? 1 : -1;
    int err = dx - dy;
    for (int steps = 0; steps < 1000; steps++)
    {
        if (ax >= 0 && ax < kGBAWidth && ay >= 0 && ay < kGBAHeight)
        {
            uint8_t* p = sFrameBuf + (ay * kGBAWidth + ax) * 3;
            p[0] = cr; p[1] = cg; p[2] = cb;
        }
        if (ax == bx && ay == by) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; ax += sx; }
        if (e2 <  dx) { err += dx; ay += sy; }
    }
}

// Project a 3D world-space point to Mode 7 screen space
// Returns false if behind camera
static bool ProjectPoint(float wx, float wy, float wz,
                         const Mode7Camera& cam, float cosA, float sinA,
                         float& sx, float& sy)
{
    float dx = wx - cam.x;
    float dz = wz - cam.z;

    float fovLambda = dx * sinA - dz * cosA;
    if (fovLambda <= 0.5f) return false;

    float lambda = fovLambda / cam.fov;
    if (lambda < 0.01f) lambda = 0.01f;

    sy = cam.horizon + (cam.height - wy) / lambda;
    float sideComponent = dx * cosA + dz * sinA;
    sx = 120.0f + sideComponent / lambda;
    return true;
}

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
    if (cx >= 0 && cx < kGBAWidth && cy - half - 1 >= 0 && cy - half - 1 < kGBAHeight)
    {
        uint8_t* row = sFrameBuf + (cy - half - 1) * kGBAWidth * 3;
        int nx0 = std::max(0, cx - 1);
        int nx1 = std::min(kGBAWidth, cx + 2);
        for (int x = nx0; x < nx1; x++)
        { row[x*3+0] = fr; row[x*3+1] = fg; row[x*3+2] = fb; }
    }
}

// Draw a sprite asset frame as a scaled billboard
static void DrawSpriteFrame(int cx, int cy, int halfW, int halfH,
                            const SpriteFrame& frame, const uint32_t* palette,
                            float fogAlpha)
{
    int fSize = frame.width;
    if (fSize <= 0) return;

    // Map frame pixels to screen rect
    int drawW = halfW * 2;
    int drawH = halfH * 2;
    int sx0 = cx - halfW;
    int sy0 = cy - halfH;

    for (int dy = 0; dy < drawH; dy++)
    {
        int sy = sy0 + dy;
        if (sy < 0 || sy >= kGBAHeight) continue;
        uint8_t* row = sFrameBuf + sy * kGBAWidth * 3;

        int fy = dy * fSize / drawH;
        if (fy >= fSize) fy = fSize - 1;

        for (int dx = 0; dx < drawW; dx++)
        {
            int sx = sx0 + dx;
            if (sx < 0 || sx >= kGBAWidth) continue;

            int fx = dx * fSize / drawW;
            if (fx >= fSize) fx = fSize - 1;

            uint8_t palIdx = frame.pixels[fy * kMaxFrameSize + fx];
            if (palIdx == 0) continue; // transparent

            uint32_t col = palette[palIdx & 0xF];
            uint8_t cr = (col >> 0) & 0xFF;
            uint8_t cg = (col >> 8) & 0xFF;
            uint8_t cb = (col >> 16) & 0xFF;

            // Fog blend
            row[sx*3+0] = (uint8_t)(cr*(1-fogAlpha) + kSkyCol[0]*fogAlpha);
            row[sx*3+1] = (uint8_t)(cg*(1-fogAlpha) + kSkyCol[1]*fogAlpha);
            row[sx*3+2] = (uint8_t)(cb*(1-fogAlpha) + kSkyCol[2]*fogAlpha);
        }
    }
}

// Draw an RGBA image scaled into the framebuffer with alpha transparency
static void DrawRGBASprite(int cx, int cy, int halfW, int halfH,
                           const unsigned char* rgba, int imgW, int imgH,
                           float fogAlpha)
{
    if (!rgba || imgW <= 0 || imgH <= 0) return;

    int drawW = halfW * 2;
    int drawH = halfH * 2;
    int sx0 = cx - halfW;
    int sy0 = cy - halfH;

    for (int dy = 0; dy < drawH; dy++)
    {
        int sy = sy0 + dy;
        if (sy < 0 || sy >= kGBAHeight) continue;
        uint8_t* row = sFrameBuf + sy * kGBAWidth * 3;

        int iy = dy * imgH / drawH;
        if (iy >= imgH) iy = imgH - 1;

        for (int dx = 0; dx < drawW; dx++)
        {
            int sx = sx0 + dx;
            if (sx < 0 || sx >= kGBAWidth) continue;

            int ix = dx * imgW / drawW;
            if (ix >= imgW) ix = imgW - 1;

            const unsigned char* px = rgba + (iy * imgW + ix) * 4;
            if (px[3] < 128) continue; // alpha test

            uint8_t cr = px[0], cg = px[1], cb = px[2];
            row[sx*3+0] = (uint8_t)(cr*(1-fogAlpha) + kSkyCol[0]*fogAlpha);
            row[sx*3+1] = (uint8_t)(cg*(1-fogAlpha) + kSkyCol[1]*fogAlpha);
            row[sx*3+2] = (uint8_t)(cb*(1-fogAlpha) + kSkyCol[2]*fogAlpha);
        }
    }
}

void Render(const Mode7Camera& cam, const Mode7Map* map,
            const FloorSprite* sprites, int spriteCount,
            const CameraStartObject* camObj, float camObjScale,
            const SpriteAsset* assets, int assetCount,
            float animTime, bool playing,
            const PlayerDirImage* playerDirs, float playerOrbitAngle,
            const AssetDirImages* assetDirImages, int assetDirCount,
            const AssetDirImages* spriteDirImages, int spriteDirCount,
            const MeshAsset* meshAssets, int meshAssetCount)
{
    float cosA = cosf(-cam.angle);
    float sinA = sinf(-cam.angle);

    // Horizon line — controlled by camera pitch (I/K keys)
    int horizon = (int)cam.horizon;

    // --- Clear to sky ---
    for (int y = 0; y < kGBAHeight; y++)
    {
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = 0; x < kGBAWidth; x++)
        {
            row[x * 3 + 0] = kSkyCol[0];
            row[x * 3 + 1] = kSkyCol[1];
            row[x * 3 + 2] = kSkyCol[2];
        }
    }

    // --- Draw ground grid at origin (wireframe lines) ---
    {
        static const float kGridSpacing = 32.0f;
        static const int kGridLines = 17; // -256 to +256 in steps of 32
        static const float kGridHalf = 256.0f;
        static const int kSegments = 32;  // segments per grid line

        for (int axis = 0; axis < 2; axis++)
        {
            for (int i = 0; i < kGridLines; i++)
            {
                float offset = -kGridHalf + i * kGridSpacing;
                bool isCenter = (i == kGridLines / 2);
                uint8_t gr = isCenter ? 180 : 60;
                uint8_t gg = isCenter ? 180 : 70;
                uint8_t gb = isCenter ? 180 : 60;

                int prevSX = 0, prevSY = 0;
                bool prevVis = false;

                for (int s = 0; s <= kSegments; s++)
                {
                    float t = -kGridHalf + s * (kGridHalf * 2.0f / kSegments);
                    float wx = axis == 0 ? t : offset;
                    float wz = axis == 0 ? offset : t;

                    float ddx = wx - cam.x;
                    float ddz = wz - cam.z;
                    float fovLambda = ddx * sinA - ddz * cosA;
                    if (fovLambda <= 0.5f) { prevVis = false; continue; }
                    float lambda = fovLambda / cam.fov;
                    if (lambda < 0.01f) lambda = 0.01f;

                    float fog = lambda / 300.0f;
                    if (fog > 0.95f) { prevVis = false; continue; }

                    int screenY = horizon + (int)(cam.height / lambda);
                    float side = ddx * cosA + ddz * sinA;
                    int screenX = 120 + (int)(side / lambda);

                    // Fog-blend the color
                    uint8_t fr = (uint8_t)(gr * (1.0f - fog) + kSkyCol[0] * fog);
                    uint8_t fg = (uint8_t)(gg * (1.0f - fog) + kSkyCol[1] * fog);
                    uint8_t fb = (uint8_t)(gb * (1.0f - fog) + kSkyCol[2] * fog);

                    if (prevVis)
                        DrawLine(prevSX, prevSY, screenX, screenY, fr, fg, fb);

                    prevSX = screenX;
                    prevSY = screenY;
                    prevVis = true;
                }
            }
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
        if (lambda < 0.01f) lambda = 0.01f; // prevent overflow when camera is very close

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

    // Draw each sprite as a diamond billboard (or asset frame) and store projection for click-select
    sLastProjCount = 0;
    for (int i = 0; i < projCount; i++)
    {
        const SpriteProj& sp = projected[i];
        const FloorSprite& fs = sprites[sp.idx];

        // Extract color (ABGR format)
        uint8_t cr = (fs.color >>  0) & 0xFF;
        uint8_t cg = (fs.color >>  8) & 0xFF;
        uint8_t cb = (fs.color >> 16) & 0xFF;

        int halfW = std::clamp((int)(8.0f * sp.scale / cam.height * 1.6f * fs.scale), 2, 200);
        int halfH = std::clamp((int)(12.0f * sp.scale / cam.height * 1.6f * fs.scale), 3, 200);
        int meshSelCX = sp.screenX, meshSelCY = 0;
        bool hasMeshBounds = false;

        // Sprite draws upward from its foot position
        int drawCenterY = sp.screenY - halfH;

        // Check if this sprite has a linked asset with directional images
        bool drewSprite = false;
        if (fs.assetIdx >= 0 && fs.assetIdx < assetCount && assets
            && assetDirImages && fs.assetIdx < assetDirCount
            && assets[fs.assetIdx].hasDirections)
        {
            int dirIdx;
            if (fs.type == SpriteType::Player)
            {
                // Player direction: based on movement/orbit angle (same as GBA runtime)
                float a = playerOrbitAngle;
                const float PI2 = 6.28318530f;
                a = fmodf(a, PI2);
                if (a < 0.0f) a += PI2;
                dirIdx = ((int)((a + 0.39269908f) / 0.78539816f)) % 8;
            }
            else
            {
                // Non-player: compute angle from camera to sprite
                float dx = fs.x - cam.x;
                float dz = fs.z - cam.z;
                float angleToSprite = atan2f(dx, -dz);
                float rotRad = fs.rotation * 3.14159265f / 180.0f;
                float relAngle = -angleToSprite + 3.14159265f + rotRad;

                const float PI2 = 6.28318530f;
                relAngle = fmodf(relAngle, PI2);
                if (relAngle < 0.0f) relAngle += PI2;

                dirIdx = ((int)((relAngle + 0.39269908f) / 0.78539816f)) % 8;
            }

            // Use per-sprite dir images if available, else fall back to per-asset
            const PlayerDirImage& adi = (spriteDirImages && sp.idx < spriteDirCount)
                ? spriteDirImages[sp.idx].dirs[dirIdx]
                : assetDirImages[fs.assetIdx].dirs[dirIdx];
            if (adi.pixels && adi.width > 0 && adi.height > 0)
            {
                int halfS = std::max(halfW, halfH);
                DrawRGBASprite(sp.screenX, drawCenterY, halfS, halfS,
                               adi.pixels, adi.width, adi.height, sp.fog);
                drewSprite = true;
            }
        }

        // Check if this sprite has a linked asset with frames
        if (!drewSprite && fs.assetIdx >= 0 && fs.assetIdx < assetCount && assets)
        {
            const SpriteAsset& asset = assets[fs.assetIdx];
            if (!asset.frames.empty())
            {
                // Determine which frame to show (animate if anim linked)
                int frameIdx = 0;
                if (fs.animIdx >= 0 && fs.animIdx < (int)asset.anims.size())
                {
                    const SpriteAnim& anim = asset.anims[fs.animIdx];
                    int frameCount = anim.endFrame - anim.startFrame + 1;
                    if (frameCount > 0 && anim.fps > 0)
                    {
                        float frameTime = 1.0f / anim.fps;
                        int animFrame = (int)(animTime / frameTime) % frameCount;
                        frameIdx = anim.startFrame + animFrame;
                    }
                    else
                        frameIdx = anim.startFrame;
                }
                if (frameIdx >= (int)asset.frames.size())
                    frameIdx = 0;

                // Make half sizes square for pixel art
                int halfS = std::max(halfW, halfH);
                DrawSpriteFrame(sp.screenX, drawCenterY, halfS, halfS,
                                asset.frames[frameIdx], asset.palette, sp.fog);
                drewSprite = true;
            }
        }
        // Render mesh geometry for Mesh-type sprites
        if (!drewSprite && fs.type == SpriteType::Mesh
            && fs.meshIdx >= 0 && fs.meshIdx < meshAssetCount && meshAssets)
        {
            const MeshAsset& ma = meshAssets[fs.meshIdx];
            if (!ma.vertices.empty() && !ma.indices.empty())
            {
                float meshScale = fs.scale;
                float rotRad = fs.rotation * 3.14159265f / 180.0f;
                float cr2 = cosf(rotRad), sr = sinf(rotRad);

                // Project all vertices to screen space
                int nv = (int)ma.vertices.size();
                // Use dynamic alloc only for large meshes, stack for small
                float scrX[256], scrY[256];
                bool  vis[256];
                float* pSX = (nv <= 256) ? scrX : new float[nv];
                float* pSY = (nv <= 256) ? scrY : new float[nv];
                bool*  pVis = (nv <= 256) ? vis : new bool[nv];

                for (int v = 0; v < nv; v++)
                {
                    const MeshVertex& mv = ma.vertices[v];
                    // Local -> world: rotate around Y axis, scale, translate
                    float lx = mv.px * meshScale;
                    float ly = mv.py * meshScale;
                    float lz = mv.pz * meshScale;
                    float wx = fs.x + lx * cr2 + lz * sr;
                    float wy = fs.y + ly;
                    float wz = fs.z - lx * sr + lz * cr2;
                    pVis[v] = ProjectPoint(wx, wy, wz, cam, cosA, sinA, pSX[v], pSY[v]);
                }

                // Simple flat shading: use a directional light
                float lightDirX = 0.3f, lightDirY = -0.8f, lightDirZ = 0.5f;
                float ll = sqrtf(lightDirX*lightDirX + lightDirY*lightDirY + lightDirZ*lightDirZ);
                lightDirX /= ll; lightDirY /= ll; lightDirZ /= ll;

                // Draw filled triangles
                int ntri = (int)ma.indices.size() / 3;
                for (int t = 0; t < ntri; t++)
                {
                    int i0 = ma.indices[t * 3 + 0];
                    int i1 = ma.indices[t * 3 + 1];
                    int i2 = ma.indices[t * 3 + 2];
                    if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                    if (!pVis[i0] && !pVis[i1] && !pVis[i2]) continue;

                    // Backface culling via screen-space cross product
                    if (ma.cullMode != CullMode::None) {
                        float cross = (pSX[i1] - pSX[i0]) * (pSY[i2] - pSY[i0])
                                    - (pSY[i1] - pSY[i0]) * (pSX[i2] - pSX[i0]);
                        if (ma.cullMode == CullMode::Back  && cross <= 0.0f) continue;
                        if (ma.cullMode == CullMode::Front && cross >= 0.0f) continue;
                    }

                    // Face normal for shading (average vertex normals or compute from positions)
                    float nx = (ma.vertices[i0].nx + ma.vertices[i1].nx + ma.vertices[i2].nx) / 3.0f;
                    float ny = (ma.vertices[i0].ny + ma.vertices[i1].ny + ma.vertices[i2].ny) / 3.0f;
                    float nz = (ma.vertices[i0].nz + ma.vertices[i1].nz + ma.vertices[i2].nz) / 3.0f;
                    // Rotate normal by sprite rotation
                    float rnx = nx * cr2 + nz * sr;
                    float rny = ny;
                    float rnz = -nx * sr + nz * cr2;

                    float dot = -(rnx * lightDirX + rny * lightDirY + rnz * lightDirZ);
                    float shade = 0.3f + 0.7f * std::max(0.0f, dot); // ambient + diffuse

                    uint8_t tr = (uint8_t)(cr * shade);
                    uint8_t tg = (uint8_t)(cg * shade);
                    uint8_t tb = (uint8_t)(cb * shade);

                    DrawTriangle(pSX[i0], pSY[i0], pSX[i1], pSY[i1], pSX[i2], pSY[i2],
                                 tr, tg, tb, sp.fog);
                }

                // Draw wireframe edges on top for definition
                for (int t = 0; t < ntri; t++)
                {
                    int i0 = ma.indices[t * 3 + 0];
                    int i1 = ma.indices[t * 3 + 1];
                    int i2 = ma.indices[t * 3 + 2];
                    if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                    if (!pVis[i0] || !pVis[i1] || !pVis[i2]) continue;

                    // Match backface culling from fill pass
                    if (ma.cullMode != CullMode::None) {
                        float cross = (pSX[i1] - pSX[i0]) * (pSY[i2] - pSY[i0])
                                    - (pSY[i1] - pSY[i0]) * (pSX[i2] - pSX[i0]);
                        if (ma.cullMode == CullMode::Back  && cross <= 0.0f) continue;
                        if (ma.cullMode == CullMode::Front && cross >= 0.0f) continue;
                    }
                    uint8_t wr = (uint8_t)(cr * 0.3f);
                    uint8_t wg = (uint8_t)(cg * 0.3f);
                    uint8_t wb = (uint8_t)(cb * 0.3f);
                    DrawLine((int)pSX[i0], (int)pSY[i0], (int)pSX[i1], (int)pSY[i1], wr, wg, wb);
                    DrawLine((int)pSX[i1], (int)pSY[i1], (int)pSX[i2], (int)pSY[i2], wr, wg, wb);
                    DrawLine((int)pSX[i2], (int)pSY[i2], (int)pSX[i0], (int)pSY[i0], wr, wg, wb);
                }

                // Compute screen-space bounding box from projected vertices
                float minSX = 9999, maxSX = -9999, minSY = 9999, maxSY = -9999;
                for (int v = 0; v < nv; v++)
                {
                    if (!pVis[v]) continue;
                    if (pSX[v] < minSX) minSX = pSX[v];
                    if (pSX[v] > maxSX) maxSX = pSX[v];
                    if (pSY[v] < minSY) minSY = pSY[v];
                    if (pSY[v] > maxSY) maxSY = pSY[v];
                }
                if (maxSX > minSX && maxSY > minSY)
                {
                    meshSelCX = (int)((minSX + maxSX) * 0.5f);
                    meshSelCY = (int)((minSY + maxSY) * 0.5f);
                    halfW = (int)((maxSX - minSX) * 0.5f) + 2;
                    halfH = (int)((maxSY - minSY) * 0.5f) + 2;
                    drawCenterY = meshSelCY;
                    hasMeshBounds = true;
                }

                if (nv > 256) { delete[] pSX; delete[] pSY; delete[] pVis; }
                drewSprite = true;
            }
        }

        if (!drewSprite)
            DrawDiamond(sp.screenX, drawCenterY, halfW, halfH, cr, cg, cb, sp.fog);

        // Selection highlight: bright outline
        if (fs.selected)
        {
            int selCX = hasMeshBounds ? meshSelCX : sp.screenX;
            DrawRect(selCX - halfW - 1, drawCenterY - halfH - 1,
                     halfW * 2 + 2, 1, 255, 255, 255);
            DrawRect(selCX - halfW - 1, drawCenterY + halfH,
                     halfW * 2 + 2, 1, 255, 255, 255);
        }

        // Store for viewport click-to-select
        sLastProj[sLastProjCount].screenX = hasMeshBounds ? meshSelCX : sp.screenX;
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
        if (fovLambda > 2.0f)
        {
            float lambda = fovLambda / cam.fov;
            if (lambda < 0.01f) lambda = 0.01f; // prevent overflow when camera is very close

            int screenY = horizon + (int)(cam.height / lambda);
            float sideComponent = dx * cosA + dz * sinA;
            int screenX = 120 + (int)(sideComponent / lambda);

            // Skip if way off screen
            if (screenY > -200 && screenY < kGBAHeight + 200 &&
                screenX > -200 && screenX < kGBAWidth + 200)
            {
                float fog = lambda / 300.0f;
                if (fog > 1.0f) fog = 1.0f;

                float scale = cam.height / lambda;
                int half = std::clamp((int)(10.0f * scale / cam.height * 16.0f * camObjScale), 2, 200);

                DrawSquare(screenX, screenY - half, half, 100, 220, 255, fog);
            }
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

void DrawAxisGuide(const Mode7Camera& cam, float spriteX, float spriteY, float spriteZ, char axis)
{
    float cosA = cosf(-cam.angle);
    float sinA = sinf(-cam.angle);

    // Colors: X=red, Y=green, Z=blue
    uint8_t cr = 0, cg = 0, cb = 0;
    if (axis == 'X') { cr = 255; cg = 60; cb = 60; }
    else if (axis == 'Y') { cr = 60; cg = 255; cb = 60; }
    else if (axis == 'Z') { cr = 60; cg = 60; cb = 255; }

    // Draw a line of sample points along the axis through the sprite's position
    constexpr float extent = 300.0f;
    constexpr int steps = 60;
    float prevSX, prevSY;
    bool prevVis = false;

    for (int i = 0; i <= steps; i++)
    {
        float t = -extent + (2.0f * extent * i) / steps;
        float wx = spriteX, wy = spriteY, wz = spriteZ;
        if (axis == 'X') wx += t;
        else if (axis == 'Y') wy += t;
        else if (axis == 'Z') wz += t;

        float sx, sy;
        bool vis = ProjectPoint(wx, wy, wz, cam, cosA, sinA, sx, sy);
        if (vis && prevVis)
            DrawLine((int)prevSX, (int)prevSY, (int)sx, (int)sy, cr, cg, cb);
        prevSX = sx;
        prevSY = sy;
        prevVis = vis;
    }
}

void UploadTexture()
{
    if (!sTexture) return;
    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGBAWidth, kGBAHeight,
                    GL_RGB, GL_UNSIGNED_BYTE, sFrameBuf);
}

} // namespace Mode7

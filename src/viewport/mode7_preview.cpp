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
            const AssetDirImages* assetDirImages, int assetDirCount)
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

        int halfW = std::max(2, (int)(8.0f * sp.scale / cam.height * 1.6f * fs.scale));
        int halfH = std::max(3, (int)(12.0f * sp.scale / cam.height * 1.6f * fs.scale));

        // Sprite draws upward from its foot position
        int drawCenterY = sp.screenY - halfH;

        // Check if this is a Player sprite with directional images
        bool drewSprite = false;
        if (fs.type == SpriteType::Player && playerDirs)
        {
            // Map orbit angle to 8 directions
            // Normalize angle to [0, 2*PI)
            float a = playerOrbitAngle;
            const float PI2 = 6.28318530f;
            a = fmodf(a, PI2);
            if (a < 0.0f) a += PI2;

            // Each direction covers PI/4 (45 degrees), offset by half
            // 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
            int dirIdx = ((int)((a + 0.39269908f) / 0.78539816f)) % 8;

            const PlayerDirImage& pdi = playerDirs[dirIdx];
            if (pdi.pixels && pdi.width > 0 && pdi.height > 0)
            {
                int halfS = std::max(halfW, halfH);
                DrawRGBASprite(sp.screenX, drawCenterY, halfS, halfS,
                               pdi.pixels, pdi.width, pdi.height, sp.fog);
                drewSprite = true;
            }
        }

        // Check if this sprite has a linked asset with directional images
        if (!drewSprite && fs.assetIdx >= 0 && fs.assetIdx < assetCount && assets
            && assetDirImages && fs.assetIdx < assetDirCount
            && assets[fs.assetIdx].hasDirections)
        {
            // Compute angle from camera to sprite
            float dx = fs.x - cam.x;
            float dz = fs.z - cam.z;
            float angleToSprite = atan2f(dx, -dz); // angle from camera to sprite
            float rotRad = fs.rotation * 3.14159265f / 180.0f;
            float relAngle = angleToSprite + 3.14159265f - rotRad;

            const float PI2 = 6.28318530f;
            relAngle = fmodf(relAngle, PI2);
            if (relAngle < 0.0f) relAngle += PI2;

            // 4 directions: N(0), E(2), S(4), W(6) — each covers 90°
            int quadrant = ((int)((relAngle + 0.78539816f) / 1.57079632f)) % 4;
            int dirIdx = quadrant * 2; // map to 0, 2, 4, 6

            const PlayerDirImage& adi = assetDirImages[fs.assetIdx].dirs[dirIdx];
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
        if (!drewSprite)
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
                int half = std::max(2, (int)(10.0f * scale / cam.height * 16.0f * camObjScale));

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

void UploadTexture()
{
    if (!sTexture) return;
    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGBAWidth, kGBAHeight,
                    GL_RGB, GL_UNSIGNED_BYTE, sFrameBuf);
}

} // namespace Mode7

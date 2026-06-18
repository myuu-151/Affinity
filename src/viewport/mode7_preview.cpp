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
// Per-pixel depth buffer for the mesh rasterizer. We store 1/z (smaller = farther)
// to skip a divide per pixel — the existing perspective-correct interp already
// produces 1/z linearly. A pixel passes the test iff its 1/z >= the stored value
// (i.e. closer than what's already there). Cleared to 0.0 each frame so the
// first opaque pixel always wins. Used only by the mesh path; sprites and HUD
// still composite on top in their existing painter order.
static float sZBuf[kGBAWidth * kGBAHeight];

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
            r = kSkyCol[0]; g = kSkyCol[1]; b = kSkyCol[2];
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
            r = kSkyCol[0]; g = kSkyCol[1]; b = kSkyCol[2];
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
    float depth;      // distance from camera (for sorting)
    int   screenX;    // center X on screen
    int   screenY;    // base Y on screen (foot position)
    float scale;      // size scale factor
    float fog;        // fog amount
    int   idx;        // index into sprite array
    int   subIdx;     // -1 = main sprite, 0-3 = sub-sprite index
    int   drawOrder;  // 0 = behind parent, 1 = parent, 2 = in front
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

// ---------------------------------------------------------------------------
// Near-plane clipping. ProjectPoint clamps fovLambda to 0.1 to avoid /0,
// but for triangles with a vertex BEHIND the camera the clamp pulls that
// vertex onto the near plane at an arbitrary screen position — the triangle
// then stretches across the viewport ("geometry bends" at certain angles).
// Proper fix: clip in view space against z = NEAR before projection.
// ---------------------------------------------------------------------------
struct ClipVtx {
    float vx, vy, vz;  // view-space (cam-relative) position
    float u,  v;       // texture coords (post-V-flip)
};

static constexpr float kNearPlane = 0.1f;

static ClipVtx clip_lerp(const ClipVtx& a, const ClipVtx& b, float t)
{
    ClipVtx o;
    o.vx = a.vx + (b.vx - a.vx) * t;
    o.vy = a.vy + (b.vy - a.vy) * t;
    o.vz = a.vz + (b.vz - a.vz) * t;
    o.u  = a.u  + (b.u  - a.u ) * t;
    o.v  = a.v  + (b.v  - a.v ) * t;
    return o;
}

// Returns the number of output triangles (0, 1, or 2). out[] is filled with
// 3*N vertices in the same winding as the input.
static int clip_tri_near(const ClipVtx& a, const ClipVtx& b, const ClipVtx& c,
                         ClipVtx out[6])
{
    bool ia = a.vz >= kNearPlane;
    bool ib = b.vz >= kNearPlane;
    bool ic = c.vz >= kNearPlane;
    int n = (int)ia + (int)ib + (int)ic;
    if (n == 0) return 0;
    if (n == 3) { out[0]=a; out[1]=b; out[2]=c; return 1; }

    if (n == 1) {
        // One vertex in front; emit one tri with the two clipped edges.
        const ClipVtx *p, *q, *r;   // p in front, q and r behind
        if (ia)      { p=&a; q=&b; r=&c; }
        else if (ib) { p=&b; q=&c; r=&a; }
        else         { p=&c; q=&a; r=&b; }
        float tq = (p->vz - kNearPlane) / (p->vz - q->vz);
        float tr = (p->vz - kNearPlane) / (p->vz - r->vz);
        out[0] = *p;
        out[1] = clip_lerp(*p, *q, tq);
        out[2] = clip_lerp(*p, *r, tr);
        return 1;
    }
    // n == 2: two in front, one behind → quad → two tris.
    const ClipVtx *p, *q, *r;   // p behind, q and r in front
    if (!ia)      { p=&a; q=&b; r=&c; }
    else if (!ib) { p=&b; q=&c; r=&a; }
    else          { p=&c; q=&a; r=&b; }
    float tq = (q->vz - kNearPlane) / (q->vz - p->vz);   // along q→p
    float tr = (r->vz - kNearPlane) / (r->vz - p->vz);   // along r→p
    ClipVtx clipQ = clip_lerp(*q, *p, tq);
    ClipVtx clipR = clip_lerp(*r, *p, tr);
    out[0] = *q;      out[1] = *r;    out[2] = clipR;
    out[3] = *q;      out[4] = clipR; out[5] = clipQ;
    return 2;
}

// Project a view-space ClipVtx to screen coords using current camera params.
static inline void project_vs(const ClipVtx& vtx, const Mode7Camera& cam,
                              float& sx, float& sy)
{
    float lambda = vtx.vz / cam.fov;
    if (lambda < 0.01f) lambda = 0.01f;
    sx = 120.0f + vtx.vx / lambda;
    sy = cam.horizon - vtx.vy / lambda;
}

// Draw a textured triangle with perspective-correct UV interpolation.
// d0/d1/d2 are per-vertex view-space depths (must be positive — caller is
// responsible for near-clipping). If any depth is tiny we fall back to
// affine to avoid divide-by-zero; for typical scene meshes that's rare.
//
// Why perspective-correct: at oblique angles, plain affine UV interp warps
// the texture in screen space because a linear u,v across screen-x is NOT
// the same as a linear u,v across world-space-x once a perspective camera
// is involved. The fix is to interpolate u/z, v/z, and 1/z linearly across
// screen, then divide u/z by 1/z at each pixel to recover the world-space
// u. Cost: 2 extra divides per pixel and 3 reciprocals per vertex.
static void DrawTriangleTex(float x0, float y0, float u0, float v0, float d0,
                            float x1, float y1, float u1, float v1, float d1,
                            float x2, float y2, float u2, float v2, float d2,
                            const uint8_t* texPixels, const uint32_t* texPal,
                            int texW, int texH, float fogAlpha)
{
    // Barycentric rasterizer — compute bounding box, test each pixel
    int minX = std::max(0, (int)floorf(std::min({x0, x1, x2})));
    int maxX = std::min(kGBAWidth - 1, (int)ceilf(std::max({x0, x1, x2})));
    int minY = std::max(0, (int)floorf(std::min({y0, y1, y2})));
    int maxY = std::min(kGBAHeight - 1, (int)ceilf(std::max({y0, y1, y2})));

    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(denom) < 0.001f) return;
    float invD = 1.0f / denom;

    // Guard against near-zero / negative depth (vertex behind/at camera).
    bool perspOk = d0 > 0.01f && d1 > 0.01f && d2 > 0.01f;
    float iz0 = perspOk ? 1.0f / d0 : 1.0f;
    float iz1 = perspOk ? 1.0f / d1 : 1.0f;
    float iz2 = perspOk ? 1.0f / d2 : 1.0f;
    float uz0 = u0 * iz0, uz1 = u1 * iz1, uz2 = u2 * iz2;
    float vz0 = v0 * iz0, vz1 = v1 * iz1, vz2 = v2 * iz2;

    for (int y = minY; y <= maxY; y++)
    {
        uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
        for (int x = minX; x <= maxX; x++)
        {
            float w0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) * invD;
            float w1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) * invD;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float su, sv;
            float pixInvZ;
            if (perspOk) {
                pixInvZ = w0 * iz0 + w1 * iz1 + w2 * iz2;
                float invInvZ = 1.0f / pixInvZ;
                su = (w0 * uz0 + w1 * uz1 + w2 * uz2) * invInvZ;
                sv = (w0 * vz0 + w1 * vz1 + w2 * vz2) * invInvZ;
            } else {
                pixInvZ = w0 * iz0 + w1 * iz1 + w2 * iz2; // iz* are 1.0 here
                su = u0 * w0 + u1 * w1 + u2 * w2;
                sv = v0 * w0 + v1 * w1 + v2 * w2;
            }
            // Z-test with a small relative bias: skip only if this pixel is
            // measurably farther than what's stored. Float precision on
            // coplanar surfaces makes each pixel's interpolated 1/z jitter
            // around an ideal value, so a strict `<` was giving a speckled
            // half-and-half result for coplanar meshes. The `* 1.001f`
            // tolerance means "within 0.1% counts as equal," so the later-
            // drawn mesh (smaller-volume, by our sort) reliably wins
            // coplanar ties. Non-coplanar cases differ by far more than
            // this and still resolve normally.
            float* zSlot = &sZBuf[y * kGBAWidth + x];
            if (pixInvZ * 1.01f < *zSlot) continue;
            *zSlot = pixInvZ;
            int tu = (int)floorf(su * texW) % texW; if (tu < 0) tu += texW;
            int tv = (int)floorf(sv * texH) % texH; if (tv < 0) tv += texH;
            uint8_t idx = texPixels[tv * texW + tu];
            uint32_t c = texPal[idx];
            uint8_t tr = c & 0xFF, tg = (c >> 8) & 0xFF, tb = (c >> 16) & 0xFF;
            row[x * 3 + 0] = (uint8_t)(tr * (1 - fogAlpha) + kSkyCol[0] * fogAlpha);
            row[x * 3 + 1] = (uint8_t)(tg * (1 - fogAlpha) + kSkyCol[1] * fogAlpha);
            row[x * 3 + 2] = (uint8_t)(tb * (1 - fogAlpha) + kSkyCol[2] * fogAlpha);
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

// Z-tested line. Interpolates 1/z along the line and skips pixels that are
// farther than what's already in the depth buffer. Test is `>=` so an edge
// co-planar with its own filled triangle still draws — no constant bias,
// because in 1/z space a fixed bias is huge for far surfaces (1% of value
// when z=100) and that was letting far wireframe edges leak through closer
// surfaces.
static void DrawLineZ(int ax, int ay, float az,
                      int bx, int by, float bz,
                      uint8_t cr, uint8_t cg, uint8_t cb)
{
    int dx = abs(bx - ax), dy = abs(by - ay);
    int sx = (ax < bx) ? 1 : -1;
    int sy = (ay < by) ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);
    if (steps < 1) steps = 1;
    float invZa = (az > 0.01f) ? 1.0f / az : 1.0f;
    float invZb = (bz > 0.01f) ? 1.0f / bz : 1.0f;
    float invZ = invZa;
    float invZStep = (invZb - invZa) / (float)steps;
    for (int s = 0; s <= steps; s++)
    {
        if (ax >= 0 && ax < kGBAWidth && ay >= 0 && ay < kGBAHeight)
        {
            float* zSlot = &sZBuf[ay * kGBAWidth + ax];
            if (invZ >= *zSlot)
            {
                uint8_t* p = sFrameBuf + (ay * kGBAWidth + ax) * 3;
                p[0] = cr; p[1] = cg; p[2] = cb;
            }
        }
        if (ax == bx && ay == by) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; ax += sx; }
        if (e2 <  dx) { err += dx; ay += sy; }
        invZ += invZStep;
    }
}

// Project a 3D world-space point to Mode 4 screen space
// Returns false if behind camera
static bool ProjectPoint(float wx, float wy, float wz,
                         const Mode7Camera& cam, float cosA, float sinA,
                         float& sx, float& sy)
{
    float dx = wx - cam.x;
    float dz = wz - cam.z;

    float fovLambda = dx * sinA - dz * cosA;
    if (fovLambda < 0.1f) fovLambda = 0.1f;

    float lambda = fovLambda / cam.fov;
    if (lambda < 0.01f) lambda = 0.01f;

    sy = cam.horizon + (cam.height - wy) / lambda;
    float sideComponent = dx * cosA + dz * sinA;
    sx = 120.0f + sideComponent / lambda;

    // Clamp to prevent extreme values from causing rendering issues
    if (sx < -8192.0f) sx = -8192.0f;
    if (sx > 8432.0f) sx = 8432.0f;
    if (sy < -8192.0f) sy = -8192.0f;
    if (sy > 8352.0f) sy = 8352.0f;
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
    int fW = fSize, fH = fSize;
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

        int fy = dy * fH / drawH;
        if (fy >= fH) fy = fH - 1;

        for (int dx = 0; dx < drawW; dx++)
        {
            int sx = sx0 + dx;
            if (sx < 0 || sx >= kGBAWidth) continue;

            int fx = dx * fW / drawW;
            if (fx >= fW) fx = fW - 1;

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
            const MeshAsset* meshAssets, int meshAssetCount,
            bool mode7Floor,
            const unsigned char* skyPixels, int skyW, int skyH,
            const unsigned char* floorPixels, int floorW, int floorH,
            const RiggedMeshAsset* riggedAssets, int riggedAssetCount)
{
    float cosA = cosf(-cam.angle);
    float sinA = sinf(-cam.angle);

    // Clear the depth buffer. 0.0 represents "infinitely far" since we store
    // 1/z and test "this pixel's 1/z >= stored." First mesh pixel always wins.
    memset(sZBuf, 0, sizeof(sZBuf));

    // Horizon line — controlled by camera pitch (I/K keys)
    int horizon = (int)cam.horizon;

    if (mode7Floor)
    {
        // --- Mode 4 affine floor rendering ---
        for (int y = 0; y < kGBAHeight; y++)
        {
            uint8_t* row = sFrameBuf + y * kGBAWidth * 3;

            if (y <= horizon)
            {
                // Sky — sample from panorama texture if available
                if (skyPixels && skyW > 0 && skyH > 0)
                {
                    float angNorm = cam.angle / (2.0f * 3.14159265f);
                    angNorm = angNorm - floorf(angNorm);
                    int skyBaseX = (int)(angNorm * skyW);
                    // Map y [0..horizon] to texture row: bottom of sky texture at horizon
                    int skyRow = (horizon > 0) ? (y * skyH / (horizon + 1)) : 0;
                    if (skyRow >= skyH) skyRow = skyH - 1;
                    for (int x = 0; x < kGBAWidth; x++)
                    {
                        // Map screen x [0..240) to panorama with wrapping
                        int skyX = (skyBaseX + x * skyW / kGBAWidth) % skyW;
                        int idx = (skyRow * skyW + skyX) * 4;
                        row[x * 3 + 0] = skyPixels[idx + 0];
                        row[x * 3 + 1] = skyPixels[idx + 1];
                        row[x * 3 + 2] = skyPixels[idx + 2];
                    }
                }
                else
                {
                    for (int x = 0; x < kGBAWidth; x++)
                    {
                        row[x * 3 + 0] = kSkyCol[0];
                        row[x * 3 + 1] = kSkyCol[1];
                        row[x * 3 + 2] = kSkyCol[2];
                    }
                }
                continue;
            }

            // Mode 4 per-scanline math (matches Tonc HBlank ISR)
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
                if (floorPixels && floorW > 0 && floorH > 0)
                {
                    // Sample from floor image — world ±512 maps to image
                    static constexpr float kHalf = 512.0f;
                    if (wx < -kHalf || wx > kHalf || wz < -kHalf || wz > kHalf) {
                        r = kSkyCol[0]; g = kSkyCol[1]; b = kSkyCol[2];
                    } else {
                        int px = (int)(((wx + kHalf) / (2.0f * kHalf)) * floorW);
                        int py = (int)(((wz + kHalf) / (2.0f * kHalf)) * floorH);
                        if (px < 0) px = 0; if (px >= floorW) px = floorW - 1;
                        if (py < 0) py = 0; if (py >= floorH) py = floorH - 1;
                        int idx = (py * floorW + px) * 4;
                        r = floorPixels[idx + 0];
                        g = floorPixels[idx + 1];
                        b = floorPixels[idx + 2];
                    }
                }
                else
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
    }
    else
    {
        // --- Clear to sky ---
        for (int y = 0; y < kGBAHeight; y++)
        {
            uint8_t* row = sFrameBuf + y * kGBAWidth * 3;
            if (skyPixels && skyW > 0 && skyH > 0 && y <= horizon)
            {
                float angNorm = cam.angle / (2.0f * 3.14159265f);
                angNorm = angNorm - floorf(angNorm);
                int skyBaseX = (int)(angNorm * skyW);
                int skyRow = (horizon > 0) ? (y * skyH / (horizon + 1)) : 0;
                if (skyRow >= skyH) skyRow = skyH - 1;
                for (int x = 0; x < kGBAWidth; x++)
                {
                    int skyX = (skyBaseX + x * skyW / kGBAWidth) % skyW;
                    int idx = (skyRow * skyW + skyX) * 4;
                    row[x * 3 + 0] = skyPixels[idx + 0];
                    row[x * 3 + 1] = skyPixels[idx + 1];
                    row[x * 3 + 2] = skyPixels[idx + 2];
                }
            }
            else
            {
                for (int x = 0; x < kGBAWidth; x++)
                {
                    row[x * 3 + 0] = kSkyCol[0];
                    row[x * 3 + 1] = kSkyCol[1];
                    row[x * 3 + 2] = kSkyCol[2];
                }
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
    }

    // --- Render sprites as scaled billboards ---
    if ((!sprites || spriteCount <= 0) && !camObj) return;
    if (!sprites) spriteCount = 0;

    // Project each sprite (+ sub-sprites) to screen space
    // Max entries: kMaxFloorSprites * (1 + kMaxSubSprites)
    SpriteProj projected[kMaxFloorSprites * 5];
    int projCount = 0;

    for (int i = 0; i < spriteCount && i < kMaxFloorSprites; i++)
    {
        // Project parent + sub-sprites: subPass -1 = parent, 0..N = sub-sprites
        int totalPasses = 1 + sprites[i].subSpriteCount;
        for (int subPass = -1; subPass < sprites[i].subSpriteCount; subPass++)
        {
            float wx = sprites[i].x;
            float wy = sprites[i].y;
            float wz = sprites[i].z;
            if (subPass >= 0) {
                const auto& sub = sprites[i].subSprites[subPass];
                if (sub.hidden) continue;   // effect sprite: hidden in editor (declutter)
                // Bone attach: snap the sub-sprite to the parent rig's animated bone
                // (same as the runtime) so positioning in the editor is WYSIWYG —
                // X/Y/Z offset FROM the bone, in the rig's LOCAL frame (so a +Z
                // nudge is "in front of the character"). Falls back to origin.
                bool boneSnapped = false;
                if (sub.boneIdx >= 0 && sprites[i].riggedMeshIdx >= 0
                    && sprites[i].riggedMeshIdx < riggedAssetCount && riggedAssets) {
                    const RiggedMeshAsset& prm = riggedAssets[sprites[i].riggedMeshIdx];
                    int nb = prm.boneCount;
                    if (sub.boneIdx < nb && (int)prm.bindPose.size() == nb) {
                        BonePose P = prm.bindPose[sub.boneIdx];   // rest pose default
                        if (sprites[i].rigAnimIdx >= 0 && sprites[i].rigAnimIdx < (int)prm.clips.size()
                            && prm.clips[sprites[i].rigAnimIdx].frameCount > 0) {
                            const RigAnimClip& clip = prm.clips[sprites[i].rigAnimIdx];
                            int fc = clip.frameCount;
                            float ff = sprites[i].rigAnimClock < 0 ? 0 : sprites[i].rigAnimClock;
                            int f0 = (int)floorf(ff) % fc, f1 = (f0 + 1) % fc;
                            float u = ff - floorf(ff);
                            const BonePose& A = clip.frames[f0 * nb + sub.boneIdx];
                            const BonePose& B = clip.frames[f1 * nb + sub.boneIdx];
                            P.px = A.px*(1-u)+B.px*u; P.py = A.py*(1-u)+B.py*u; P.pz = A.pz*(1-u)+B.pz*u;
                        }
                        // Same world transform as the rig draw below (scale, Y/X/Z rot, translate).
                        float ms = sprites[i].scale;
                        float rY = sprites[i].rotation  * 3.14159265f/180.0f;
                        float rX = sprites[i].rotationX * 3.14159265f/180.0f;
                        float rZ = sprites[i].rotationZ * 3.14159265f/180.0f;
                        float cY=cosf(rY),sY=sinf(rY),cX=cosf(rX),sX=sinf(rX),cZ=cosf(rZ),sZ=sinf(rZ);
                        // Apply the rig rotation to BOTH the bone position and the
                        // offset (rig-local), so the nudge rotates with the character.
                        auto rigRot = [&](float lx, float ly, float lz, float& ox, float& oy, float& oz){
                            float rxx=lx*cY+lz*sY, rzz=-lx*sY+lz*cY, ryy=ly;
                            float ry2=ryy*cX-rzz*sX, rz2=ryy*sX+rzz*cX;
                            ox=rxx*cZ-ry2*sZ; oy=rxx*sZ+ry2*cZ; oz=rz2;
                        };
                        float bxo,byo,bzo; rigRot(P.px*ms, P.py*ms, P.pz*ms, bxo,byo,bzo);
                        float oxo,oyo,ozo; rigRot(sub.offsetX, sub.offsetY, sub.offsetZ, oxo,oyo,ozo);
                        wx = sprites[i].x + bxo + oxo;
                        wy = sprites[i].y + byo + oyo;
                        wz = sprites[i].z + bzo + ozo;
                        boneSnapped = true;
                    }
                }
                if (!boneSnapped) {
                    wx += sub.offsetX;
                    wy += sub.offsetY;
                    wz += sub.offsetZ;
                }
            }

            float dx = wx - cam.x;
            float dz = wz - cam.z;
            float fovLambda = dx * sinA - dz * cosA;
            if (fovLambda < 0.1f) fovLambda = 0.1f;
            float lambda = fovLambda / cam.fov;
            if (lambda < 0.01f) lambda = 0.01f;

            int screenY = horizon + (int)((cam.height - wy) / lambda);
            float sideComponent = dx * cosA + dz * sinA;
            int screenX = 120 + (int)(sideComponent / lambda);

            bool isMesh = (subPass < 0 && sprites[i].type == SpriteType::Mesh
                          && sprites[i].meshIdx >= 0);
            // Skip screen-bounds culling for mesh sprites — their vertices are projected independently
            if (!isMesh) {
                if (screenY < 0 || screenY >= kGBAHeight) continue;
                if (screenX < -32 || screenX >= kGBAWidth + 32) continue;
            }

            float scale = cam.height / lambda;
            float fog = lambda / 300.0f;
            if (fog > 1.0f) fog = 1.0f;
            if (!isMesh && fog > 0.95f) continue;

            // Draw order: 0 = behind, 1 = parent, 2 = in front
            int drawOrd = 1; // parent
            if (subPass >= 0) {
                int order = sprites[i].subSprites[subPass].drawOrder;
                drawOrd = (order == 0) ? 0 : 2; // behind or in front
            }

            projected[projCount].depth   = lambda;
            projected[projCount].screenX = screenX;
            projected[projCount].screenY = screenY;
            projected[projCount].scale   = scale;
            projected[projCount].fog     = fog;
            projected[projCount].idx     = i;
            projected[projCount].subIdx  = subPass;
            projected[projCount].drawOrder = drawOrd;
            projCount++;
            if (projCount >= (int)(sizeof(projected) / sizeof(projected[0]))) break;
        }

        if (projCount >= (int)(sizeof(projected) / sizeof(projected[0]))) break;
    }

    // Sort back-to-front (farthest first), with drawOrder as tiebreaker for same-parent sprites
    // Meshes draw before (behind) non-mesh sprites so sprites appear on top like GBA OBJ layer
    for (int i = 0; i < projCount - 1; i++)
        for (int j = i + 1; j < projCount; j++) {
            bool swap = false;
            if (projected[i].idx == projected[j].idx) {
                // Same parent: sort by drawOrder (lower = behind = drawn first)
                swap = (projected[i].drawOrder > projected[j].drawOrder);
            } else {
                // Different parents: meshes draw first (behind), then sort by depth
                bool iMesh = (projected[i].subIdx < 0 && sprites[projected[i].idx].type == SpriteType::Mesh);
                bool jMesh = (projected[j].subIdx < 0 && sprites[projected[j].idx].type == SpriteType::Mesh);
                if (iMesh != jMesh)
                    swap = !iMesh && jMesh; // mesh should come first (drawn behind)
                else if (iMesh && jMesh) {
                    // Both meshes: sort by drawPriority (higher = drawn first/behind),
                    // then by AABB volume (larger first → smaller "props" draw
                    // last so they win z-buffer ties when coplanar with bigger
                    // surfaces — e.g. boost pad sitting on grass). Falling
                    // back to depth would flip the winner whenever you orbit
                    // 180°, which is exactly the bug. Z-buffer still resolves
                    // non-coplanar occlusion correctly within each tier.
                    const auto& iMa = meshAssets[sprites[projected[i].idx].meshIdx];
                    const auto& jMa = meshAssets[sprites[projected[j].idx].meshIdx];
                    int iPri = iMa.drawPriority;
                    int jPri = jMa.drawPriority;
                    if (iPri != jPri)
                        swap = (iPri < jPri); // higher priority draws first
                    else {
                        // XZ footprint, not full volume — flat surfaces have
                        // ~0 Y extent so volume collapses to 0 and can't
                        // distinguish a large grass plane from a small pad
                        // mesh sitting on it. Footprint area DOES separate
                        // them (grass huge, pad small) and gives a stable
                        // sort that doesn't flip with camera orbit angle.
                        auto areaOf = [](const MeshAsset& m) {
                            float dx = m.boundsMax[0] - m.boundsMin[0];
                            float dz = m.boundsMax[2] - m.boundsMin[2];
                            return dx * dz;
                        };
                        float iA = areaOf(iMa), jA = areaOf(jMa);
                        if (iA != jA)
                            swap = (iA < jA); // larger footprint first
                        else
                            swap = (projected[i].depth < projected[j].depth);
                    }
                } else
                    swap = (projected[i].depth < projected[j].depth);
            }
            if (swap) std::swap(projected[i], projected[j]);
        }

    // Draw each sprite as a diamond billboard (or asset frame) and store projection for click-select
    sLastProjCount = 0;
    for (int i = 0; i < projCount; i++)
    {
        const SpriteProj& sp = projected[i];
        const FloorSprite& fs = sprites[sp.idx];

        // For sub-sprites, override asset/anim from sub-sprite data
        int effectiveAssetIdx = fs.assetIdx;
        int effectiveAnimIdx = fs.animIdx;
        bool effectiveAnimEnabled = fs.animEnabled;
        if (sp.subIdx >= 0 && sp.subIdx < fs.subSpriteCount) {
            const auto& sub = fs.subSprites[sp.subIdx];
            effectiveAssetIdx = sub.assetIdx;
            effectiveAnimIdx = sub.animIdx;
            effectiveAnimEnabled = sub.animEnabled;
        }

        // Extract color (ABGR format)
        uint8_t cr = (fs.color >>  0) & 0xFF;
        uint8_t cg = (fs.color >>  8) & 0xFF;
        uint8_t cb = (fs.color >> 16) & 0xFF;

        float effectiveScale = fs.scale;
        if (sp.subIdx >= 0 && sp.subIdx < fs.subSpriteCount)
            effectiveScale *= fs.subSprites[sp.subIdx].scale;
        // Use asset baseSize for proportional sprite rendering in editor
        float baseSzF = 16.0f; // default fallback
        if (effectiveAssetIdx >= 0 && effectiveAssetIdx < assetCount && assets)
            baseSzF = (float)assets[effectiveAssetIdx].baseSize;
        float szFactor = sp.scale / cam.height * 1.6f * effectiveScale;
        int halfW = std::clamp((int)(baseSzF * 0.25f * szFactor), 2, 200);
        int halfH = std::clamp((int)(baseSzF * 0.25f * szFactor), 2, 200);
        int meshSelCX = sp.screenX, meshSelCY = 0;
        bool hasMeshBounds = false;

        // Sprite draws upward from its foot position
        int drawCenterY = sp.screenY - halfH;

        // Check if this sprite has a linked asset with directional images
        bool drewSprite = false;
        // A rigged (skinned) mesh on a parent sprite replaces its billboard
        // entirely — suppress the sprite/dir/mesh draws so the rig block runs.
        bool rigParent = (sp.subIdx < 0 && fs.riggedMeshIdx >= 0
                          && riggedAssets && fs.riggedMeshIdx < riggedAssetCount);
        bool isForceStatic = (sp.subIdx < 0) ? fs.forceStatic
            : (sp.subIdx < fs.subSpriteCount && fs.subSprites[sp.subIdx].forceStatic);
        if (!rigParent && effectiveAssetIdx >= 0 && effectiveAssetIdx < assetCount && assets
            && assetDirImages && effectiveAssetIdx < assetDirCount
            && assets[effectiveAssetIdx].hasDirections)
        {
            int dirIdx;
            if (isForceStatic)
            {
                dirIdx = 0; // static: always show facing 0
            }
            else if (fs.type == SpriteType::Player && sp.subIdx < 0)
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
                // Non-player / sub-sprite: compute angle from camera to sprite
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

            // Use per-sprite dir images if available (only for parent), else per-asset
            const AssetDirImages& dirImgs = (sp.subIdx < 0 && spriteDirImages && sp.idx < spriteDirCount)
                ? spriteDirImages[sp.idx]
                : assetDirImages[effectiveAssetIdx];
            // If requested direction has no pixels, find nearest available
            if (!(dirImgs.dirs[dirIdx].pixels && dirImgs.dirs[dirIdx].width > 0))
            {
                for (int d = 1; d <= 4; d++) {
                    int fwd = (dirIdx + d) & 7;
                    int bwd = (dirIdx - d + 8) & 7;
                    if (dirImgs.dirs[fwd].pixels && dirImgs.dirs[fwd].width > 0) { dirIdx = fwd; break; }
                    if (dirImgs.dirs[bwd].pixels && dirImgs.dirs[bwd].width > 0) { dirIdx = bwd; break; }
                }
            }
            const PlayerDirImage& adi = dirImgs.dirs[dirIdx];
            if (adi.pixels && adi.width > 0 && adi.height > 0)
            {
                int halfS = std::max(halfW, halfH);
                DrawRGBASprite(sp.screenX, drawCenterY, halfS, halfS,
                               adi.pixels, adi.width, adi.height, sp.fog);
                drewSprite = true;
            }
        }

        // Check if this sprite has a linked asset with frames
        if (!drewSprite && !rigParent && effectiveAssetIdx >= 0 && effectiveAssetIdx < assetCount && assets)
        {
            const SpriteAsset& asset = assets[effectiveAssetIdx];
            if (!asset.frames.empty())
            {
                // Determine which frame to show (animate if anim linked)
                int frameIdx = 0;
                if (effectiveAnimIdx >= 0 && effectiveAnimIdx < (int)asset.anims.size())
                {
                    const SpriteAnim& anim = asset.anims[effectiveAnimIdx];
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

                const SpriteFrame& sf = asset.frames[frameIdx];
                int halfS = std::max(halfW, halfH);
                DrawSpriteFrame(sp.screenX, drawCenterY, halfS, halfS,
                                sf, asset.palette, sp.fog);
                drewSprite = true;
            }
        }
        // Render mesh geometry for Mesh-type sprites (skip for sub-sprites)
        if (!drewSprite && !rigParent && sp.subIdx < 0 && fs.type == SpriteType::Mesh
            && fs.meshIdx >= 0 && fs.meshIdx < meshAssetCount && meshAssets)
        {
            const MeshAsset& ma = meshAssets[fs.meshIdx];
            if (!ma.visible) continue;
            if (!ma.vertices.empty() && (!ma.indices.empty() || !ma.quadIndices.empty()))
            {
                // Subdivide mesh data if requested (fixes texture distortion on large faces)
                const std::vector<MeshVertex>* useVerts = &ma.vertices;
                const std::vector<uint32_t>* useIndices = &ma.indices;
                const std::vector<uint32_t>* useQuadIndices = &ma.quadIndices;
                std::vector<MeshVertex> subVerts;
                std::vector<uint32_t> subIndices, subQuadIndices;
                if (ma.subdivide >= 2 && ma.subdivide <= 4)
                {
                    int N = ma.subdivide;
                    subVerts = ma.vertices; // start with original verts
                    // Subdivide quads: each quad -> NxN sub-quads
                    for (size_t qi = 0; qi + 4 <= ma.quadIndices.size(); qi += 4)
                    {
                        int q0 = ma.quadIndices[qi+0], q1 = ma.quadIndices[qi+1];
                        int q2 = ma.quadIndices[qi+2], q3 = ma.quadIndices[qi+3];
                        const MeshVertex& v0 = ma.vertices[q0];
                        const MeshVertex& v1 = ma.vertices[q1];
                        const MeshVertex& v2 = ma.vertices[q2];
                        const MeshVertex& v3 = ma.vertices[q3];
                        uint32_t grid[5][5];
                        for (int gy = 0; gy <= N; gy++)
                            for (int gx = 0; gx <= N; gx++) {
                                float fu = (float)gx/N, fv = (float)gy/N;
                                float su = 1.0f-fu, sv = 1.0f-fv;
                                float w0 = su*sv, w1 = fu*sv, w2 = fu*fv, w3 = su*fv;
                                MeshVertex mv;
                                mv.px = v0.px*w0 + v1.px*w1 + v2.px*w2 + v3.px*w3;
                                mv.py = v0.py*w0 + v1.py*w1 + v2.py*w2 + v3.py*w3;
                                mv.pz = v0.pz*w0 + v1.pz*w1 + v2.pz*w2 + v3.pz*w3;
                                mv.nx = v0.nx*w0 + v1.nx*w1 + v2.nx*w2 + v3.nx*w3;
                                mv.ny = v0.ny*w0 + v1.ny*w1 + v2.ny*w2 + v3.ny*w3;
                                mv.nz = v0.nz*w0 + v1.nz*w1 + v2.nz*w2 + v3.nz*w3;
                                mv.r = v0.r*w0 + v1.r*w1 + v2.r*w2 + v3.r*w3;
                                mv.g = v0.g*w0 + v1.g*w1 + v2.g*w2 + v3.g*w3;
                                mv.b = v0.b*w0 + v1.b*w1 + v2.b*w2 + v3.b*w3;
                                mv.u = v0.u*w0 + v1.u*w1 + v2.u*w2 + v3.u*w3;
                                mv.v = v0.v*w0 + v1.v*w1 + v2.v*w2 + v3.v*w3;
                                mv.objPosIdx = v0.objPosIdx;
                                grid[gy][gx] = (uint32_t)subVerts.size();
                                subVerts.push_back(mv);
                            }
                        for (int gy = 0; gy < N; gy++)
                            for (int gx = 0; gx < N; gx++) {
                                subQuadIndices.push_back(grid[gy][gx]);
                                subQuadIndices.push_back(grid[gy][gx+1]);
                                subQuadIndices.push_back(grid[gy+1][gx+1]);
                                subQuadIndices.push_back(grid[gy+1][gx]);
                            }
                    }
                    // Subdivide triangles: each tri -> N*N sub-tris
                    for (size_t ti = 0; ti + 3 <= ma.indices.size(); ti += 3)
                    {
                        int t0 = ma.indices[ti+0], t1 = ma.indices[ti+1], t2 = ma.indices[ti+2];
                        const MeshVertex& vt0 = ma.vertices[t0];
                        const MeshVertex& vt1 = ma.vertices[t1];
                        const MeshVertex& vt2 = ma.vertices[t2];
                        std::vector<std::vector<uint32_t>> rows(N+1);
                        for (int r = 0; r <= N; r++) {
                            rows[r].resize(r+1);
                            for (int c = 0; c <= r; c++) {
                                float b0 = 1.0f - (float)r/N;
                                float brem = (float)r/N;
                                float b1 = (r > 0) ? brem * (float)c / r : 0.0f;
                                float b2 = brem - b1;
                                MeshVertex mv;
                                mv.px = vt0.px*b0 + vt1.px*b1 + vt2.px*b2;
                                mv.py = vt0.py*b0 + vt1.py*b1 + vt2.py*b2;
                                mv.pz = vt0.pz*b0 + vt1.pz*b1 + vt2.pz*b2;
                                mv.nx = vt0.nx*b0 + vt1.nx*b1 + vt2.nx*b2;
                                mv.ny = vt0.ny*b0 + vt1.ny*b1 + vt2.ny*b2;
                                mv.nz = vt0.nz*b0 + vt1.nz*b1 + vt2.nz*b2;
                                mv.r = vt0.r*b0 + vt1.r*b1 + vt2.r*b2;
                                mv.g = vt0.g*b0 + vt1.g*b1 + vt2.g*b2;
                                mv.b = vt0.b*b0 + vt1.b*b1 + vt2.b*b2;
                                mv.u = vt0.u*b0 + vt1.u*b1 + vt2.u*b2;
                                mv.v = vt0.v*b0 + vt1.v*b1 + vt2.v*b2;
                                mv.objPosIdx = vt0.objPosIdx;
                                rows[r][c] = (uint32_t)subVerts.size();
                                subVerts.push_back(mv);
                            }
                        }
                        for (int r = 0; r < N; r++)
                            for (int c = 0; c <= r; c++) {
                                subIndices.push_back(rows[r][c]);
                                subIndices.push_back(rows[r+1][c+1]);
                                subIndices.push_back(rows[r+1][c]);
                                if (c < r) {
                                    subIndices.push_back(rows[r][c]);
                                    subIndices.push_back(rows[r][c+1]);
                                    subIndices.push_back(rows[r+1][c+1]);
                                }
                            }
                    }
                    useVerts = &subVerts;
                    useIndices = &subIndices;
                    useQuadIndices = &subQuadIndices;
                }
                const std::vector<MeshVertex>& verts = *useVerts;
                const std::vector<uint32_t>& triIdx = *useIndices;
                const std::vector<uint32_t>& quadIdx = *useQuadIndices;

                float meshScale = fs.scale;
                float rY = fs.rotation * 3.14159265f / 180.0f;
                float rX = fs.rotationX * 3.14159265f / 180.0f;
                float rZ = fs.rotationZ * 3.14159265f / 180.0f;
                float cY = cosf(rY), sY = sinf(rY);
                float cX = cosf(rX), sX = sinf(rX);
                float cZ = cosf(rZ), sZ = sinf(rZ);

                // Project all vertices to screen space
                int nv = (int)verts.size();
                // Use dynamic alloc only for large meshes, stack for small
                float scrX[256], scrY[256], depth[256];
                float vsX[256], vsY[256];   // view-space side (X) and vertical
                bool  vis[256];
                float* pSX = (nv <= 256) ? scrX : new float[nv];
                float* pSY = (nv <= 256) ? scrY : new float[nv];
                float* pDepth = (nv <= 256) ? depth : new float[nv];
                float* pVX = (nv <= 256) ? vsX : new float[nv];
                float* pVY = (nv <= 256) ? vsY : new float[nv];
                bool*  pVis = (nv <= 256) ? vis : new bool[nv];

                for (int v = 0; v < nv; v++)
                {
                    const MeshVertex& mv = verts[v];
                    float lx = mv.px * meshScale;
                    float ly = mv.py * meshScale;
                    float lz = mv.pz * meshScale;
                    // Y rotation
                    float rx = lx * cY + lz * sY;
                    float rz = -lx * sY + lz * cY;
                    float ry = ly;
                    // X rotation
                    float ry2 = ry * cX - rz * sX;
                    float rz2 = ry * sX + rz * cX;
                    // Z rotation
                    float rx2 = rx * cZ - ry2 * sZ;
                    float ry3 = rx * sZ + ry2 * cZ;
                    float wx = fs.x + rx2;
                    float wy = fs.y + ry3;
                    float wz = fs.z + rz2;
                    pVis[v] = ProjectPoint(wx, wy, wz, cam, cosA, sinA, pSX[v], pSY[v]);
                    // View-space coords (cam-relative, rotated by view angle).
                    // Z = forward depth; X = side; Y = vertical offset.
                    // Used by the near-plane clipper to handle triangles that
                    // straddle the camera plane — without this, ProjectPoint's
                    // fovLambda clamp produces hugely-stretched screen positions
                    // that show up as bent/distorted geometry near the camera.
                    float dx = wx - cam.x;
                    float dz = wz - cam.z;
                    pDepth[v] = dx * sinA - dz * cosA;
                    pVX[v]    = dx * cosA + dz * sinA;
                    pVY[v]    = wy - cam.height;
                }

                // Simple flat shading: use a directional light
                float lightDirX = 0.3f, lightDirY = -0.8f, lightDirZ = 0.5f;
                float ll = sqrtf(lightDirX*lightDirX + lightDirY*lightDirY + lightDirZ*lightDirZ);
                lightDirX /= ll; lightDirY /= ll; lightDirZ /= ll;

                // Build sort list: each entry is a face (tri or quad) with depth
                // Negative index = quad, positive = tri
                int nTriFaces = (int)triIdx.size() / 3;
                int nQuadFaces = (int)quadIdx.size() / 4;
                int nFaces = nTriFaces + nQuadFaces;
                int sortIdx[512];
                float faceDepth[512];
                int* pSort = (nFaces <= 512) ? sortIdx : new int[nFaces];
                float* pFaceDepth = (nFaces <= 512) ? faceDepth : new float[nFaces];

                // Face-depth = average of vertex depths, but CLAMP each vertex
                // depth to the near plane first. A vertex behind the camera has
                // negative pDepth; summed unclamped, it pulls a face's sort key
                // toward 0 and the face ends up drawn LAST (on top of every
                // other face) — that's what was making far geometry appear to
                // poke through nearer surfaces.
                auto depthFor = [&](int idx) {
                    float d = pDepth[idx];
                    return d > kNearPlane ? d : kNearPlane;
                };
                for (int t = 0; t < nTriFaces; t++)
                {
                    int i0 = triIdx[t*3], i1 = triIdx[t*3+1], i2 = triIdx[t*3+2];
                    pFaceDepth[t] = depthFor(i0) + depthFor(i1) + depthFor(i2);
                    pSort[t] = t;
                }
                for (int q = 0; q < nQuadFaces; q++)
                {
                    int i0 = quadIdx[q*4], i1 = quadIdx[q*4+1];
                    int i2 = quadIdx[q*4+2], i3 = quadIdx[q*4+3];
                    pFaceDepth[nTriFaces + q] = depthFor(i0) + depthFor(i1) + depthFor(i2) + depthFor(i3);
                    pSort[nTriFaces + q] = nTriFaces + q;
                }
                std::sort(pSort, pSort + nFaces, [&](int a, int b) {
                    return pFaceDepth[a] > pFaceDepth[b];
                });

                uint8_t wr = (uint8_t)(cr * 0.3f);
                uint8_t wg = (uint8_t)(cg * 0.3f);
                uint8_t wb = (uint8_t)(cb * 0.3f);

                for (int fi = 0; fi < nFaces; fi++)
                {
                    int f = pSort[fi];
                    bool isQuad = (f >= nTriFaces);

                    int i0, i1, i2, i3 = 0;
                    if (isQuad)
                    {
                        int q = f - nTriFaces;
                        i0 = quadIdx[q*4]; i1 = quadIdx[q*4+1];
                        i2 = quadIdx[q*4+2]; i3 = quadIdx[q*4+3];
                    }
                    else
                    {
                        i0 = triIdx[f*3]; i1 = triIdx[f*3+1]; i2 = triIdx[f*3+2];
                    }

                    if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
                    if (isQuad && i3 >= nv) continue;
                    if (!pVis[i0] && !pVis[i1] && !pVis[i2] && !(isQuad && pVis[i3])) continue;

                    // Backface culling deferred: pSX values for vertices
                    // behind the near plane are clamped extreme positions, so
                    // the screen-space cross product flips sign unreliably —
                    // legitimately front-facing triangles would get culled
                    // (mesh "disappears at certain angles") and back faces
                    // could leak through. The rasterizeTri lambda below now
                    // runs the cull on the POST-CLIP projected coords, which
                    // are guaranteed valid.

                    // Face normal for shading
                    float fnx, fny, fnz;
                    if (isQuad)
                    {
                        fnx = (verts[i0].nx + verts[i1].nx + verts[i2].nx + verts[i3].nx) * 0.25f;
                        fny = (verts[i0].ny + verts[i1].ny + verts[i2].ny + verts[i3].ny) * 0.25f;
                        fnz = (verts[i0].nz + verts[i1].nz + verts[i2].nz + verts[i3].nz) * 0.25f;
                    }
                    else
                    {
                        fnx = (verts[i0].nx + verts[i1].nx + verts[i2].nx) / 3.0f;
                        fny = (verts[i0].ny + verts[i1].ny + verts[i2].ny) / 3.0f;
                        fnz = (verts[i0].nz + verts[i1].nz + verts[i2].nz) / 3.0f;
                    }
                    // Rotate normal: Y, then X, then Z
                    float nx1 = fnx * cY + fnz * sY;
                    float nz1 = -fnx * sY + fnz * cY;
                    float ny1 = fny;
                    float ny2 = ny1 * cX - nz1 * sX;
                    float nz2 = ny1 * sX + nz1 * cX;
                    float rnx = nx1 * cZ - ny2 * sZ;
                    float rny = nx1 * sZ + ny2 * cZ;
                    float rnz = nz2;
                    float dot = -(rnx * lightDirX + rny * lightDirY + rnz * lightDirZ);
                    float shade = 0.3f + 0.7f * std::max(0.0f, dot);

                    if (ma.textured && !ma.texturePixels.empty() && ma.texW > 0 && ma.texH > 0)
                    {
                        // Flip V to match OpenGL convention (3D tab uses 1-v).
                        // Build view-space ClipVtx, clip against near plane,
                        // re-project the (possibly new) vertices, and rasterize.
                        // This is what stops geometry from bending when a triangle
                        // straddles the camera plane — the old path passed clamped
                        // screen coords directly to DrawTriangleTex.
                        auto rasterizeTri = [&](int a, int b, int c) {
                            ClipVtx ca{ pVX[a], pVY[a], pDepth[a], verts[a].u, 1.0f - verts[a].v };
                            ClipVtx cb{ pVX[b], pVY[b], pDepth[b], verts[b].u, 1.0f - verts[b].v };
                            ClipVtx cc{ pVX[c], pVY[c], pDepth[c], verts[c].u, 1.0f - verts[c].v };
                            ClipVtx out[6];
                            int n = clip_tri_near(ca, cb, cc, out);
                            for (int k = 0; k < n; k++) {
                                float sx0, sy0, sx1, sy1, sx2, sy2;
                                project_vs(out[k*3+0], cam, sx0, sy0);
                                project_vs(out[k*3+1], cam, sx1, sy1);
                                project_vs(out[k*3+2], cam, sx2, sy2);
                                // Back-face cull on the clipped/projected
                                // coords (all post-near-plane, so reliable).
                                if (ma.cullMode != CullMode::None) {
                                    float cr2 = (sx1 - sx0) * (sy2 - sy0)
                                              - (sy1 - sy0) * (sx2 - sx0);
                                    if (ma.cullMode == CullMode::Back  && cr2 >= 0.0f) continue;
                                    if (ma.cullMode == CullMode::Front && cr2 <= 0.0f) continue;
                                }
                                DrawTriangleTex(
                                    sx0, sy0, out[k*3+0].u, out[k*3+0].v, out[k*3+0].vz,
                                    sx1, sy1, out[k*3+1].u, out[k*3+1].v, out[k*3+1].vz,
                                    sx2, sy2, out[k*3+2].u, out[k*3+2].v, out[k*3+2].vz,
                                    ma.texturePixels.data(), ma.texturePalette,
                                    ma.texW, ma.texH, sp.fog);
                            }
                        };
                        rasterizeTri(i0, i1, i2);
                        if (isQuad)
                            rasterizeTri(i0, i2, i3);
                    }
                    else
                    {
                        uint8_t tr = (uint8_t)(cr * shade);
                        uint8_t tg = (uint8_t)(cg * shade);
                        uint8_t tb = (uint8_t)(cb * shade);
                        DrawTriangle(pSX[i0], pSY[i0], pSX[i1], pSY[i1], pSX[i2], pSY[i2],
                                     tr, tg, tb, sp.fog);
                        if (isQuad)
                            DrawTriangle(pSX[i0], pSY[i0], pSX[i2], pSY[i2], pSX[i3], pSY[i3],
                                         tr, tg, tb, sp.fog);
                    }

                    // Wireframe: draw actual face edges (z-tested so edges
                    // behind another mesh don't punch through it). Skip an
                    // edge if either endpoint is behind the near plane —
                    // that endpoint's pSX is the clamped ±8192 value and
                    // would draw a line across the whole screen.
                    auto edgeOk = [&](int va, int vb) {
                        return pDepth[va] >= kNearPlane && pDepth[vb] >= kNearPlane;
                    };
                    if (isQuad)
                    {
                        if (edgeOk(i0,i1)) DrawLineZ((int)pSX[i0],(int)pSY[i0],pDepth[i0], (int)pSX[i1],(int)pSY[i1],pDepth[i1], wr,wg,wb);
                        if (edgeOk(i1,i2)) DrawLineZ((int)pSX[i1],(int)pSY[i1],pDepth[i1], (int)pSX[i2],(int)pSY[i2],pDepth[i2], wr,wg,wb);
                        if (edgeOk(i2,i3)) DrawLineZ((int)pSX[i2],(int)pSY[i2],pDepth[i2], (int)pSX[i3],(int)pSY[i3],pDepth[i3], wr,wg,wb);
                        if (edgeOk(i3,i0)) DrawLineZ((int)pSX[i3],(int)pSY[i3],pDepth[i3], (int)pSX[i0],(int)pSY[i0],pDepth[i0], wr,wg,wb);
                    }
                    else
                    {
                        if (edgeOk(i0,i1)) DrawLineZ((int)pSX[i0],(int)pSY[i0],pDepth[i0], (int)pSX[i1],(int)pSY[i1],pDepth[i1], wr,wg,wb);
                        if (edgeOk(i1,i2)) DrawLineZ((int)pSX[i1],(int)pSY[i1],pDepth[i1], (int)pSX[i2],(int)pSY[i2],pDepth[i2], wr,wg,wb);
                        if (edgeOk(i2,i0)) DrawLineZ((int)pSX[i2],(int)pSY[i2],pDepth[i2], (int)pSX[i0],(int)pSY[i0],pDepth[i0], wr,wg,wb);
                    }
                }
                int ntri = nTriFaces + nQuadFaces * 2; // for cleanup check

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

                if (nFaces > 512) { delete[] pSort; delete[] pFaceDepth; }
                if (nv > 256) { delete[] pSX; delete[] pSY; delete[] pDepth; delete[] pVis; }
                drewSprite = true;
            }
        }

        // --- Rigged (skinned glTF) mesh — DSMA preview ---
        // Pose the rig for the current frame (CPU skinning: each vertex is in its
        // bone's local space, transformed by that bone's absolute pose), then run
        // the same sprite transform + projection + depth sort + flat fill as the
        // static mesh path. Untextured for now (flat shaded by glTF base color).
        if (!drewSprite && sp.subIdx < 0
            && fs.riggedMeshIdx >= 0 && fs.riggedMeshIdx < riggedAssetCount && riggedAssets)
        {
            const RiggedMeshAsset& rm = riggedAssets[fs.riggedMeshIdx];
            int nb = rm.boneCount;
            if (!rm.baseVerts.empty() && !rm.indices.empty() && nb > 0)
            {
                // Rotate a vector by a unit quaternion: v + 2*w*(q x v) + 2*(q x (q x v)).
                auto rotq = [](float qw, float qx, float qy, float qz,
                               float vx, float vy, float vz,
                               float& ox, float& oy, float& oz) {
                    float tx = 2.0f * (qy * vz - qz * vy);
                    float ty = 2.0f * (qz * vx - qx * vz);
                    float tz = 2.0f * (qx * vy - qy * vx);
                    ox = vx + qw * tx + (qy * tz - qz * ty);
                    oy = vy + qw * ty + (qz * tx - qx * tz);
                    oz = vz + qw * tz + (qx * ty - qy * tx);
                };

                // Resolve absolute bone poses for the current playback frame.
                std::vector<BonePose> pose(nb);
                bool hasClip = (fs.rigAnimIdx >= 0 && fs.rigAnimIdx < (int)rm.clips.size()
                                && rm.clips[fs.rigAnimIdx].frameCount > 0);
                if (hasClip) {
                    const RigAnimClip& clip = rm.clips[fs.rigAnimIdx];
                    int fc = clip.frameCount;
                    float ff = fs.rigAnimClock < 0 ? 0 : fs.rigAnimClock;
                    int f0 = (int)floorf(ff) % fc;
                    int f1 = (f0 + 1) % fc;
                    float u = ff - floorf(ff);
                    for (int b = 0; b < nb; b++) {
                        const BonePose& A = clip.frames[f0 * nb + b];
                        const BonePose& B = clip.frames[f1 * nb + b];
                        BonePose& P = pose[b];
                        P.px = A.px * (1 - u) + B.px * u;
                        P.py = A.py * (1 - u) + B.py * u;
                        P.pz = A.pz * (1 - u) + B.pz * u;
                        float dot = A.qw*B.qw + A.qx*B.qx + A.qy*B.qy + A.qz*B.qz;
                        float s = (dot < 0) ? -1.0f : 1.0f;
                        float qw = A.qw*(1-u) + s*B.qw*u, qx = A.qx*(1-u) + s*B.qx*u;
                        float qy = A.qy*(1-u) + s*B.qy*u, qz = A.qz*(1-u) + s*B.qz*u;
                        float m = sqrtf(qw*qw + qx*qx + qy*qy + qz*qz); if (m == 0) m = 1;
                        P.qw = qw/m; P.qx = qx/m; P.qy = qy/m; P.qz = qz/m;
                    }
                } else {
                    for (int b = 0; b < nb; b++) pose[b] = rm.bindPose[b];
                }

                // Skin vertices + normals into rig-local space.
                int nv = (int)rm.baseVerts.size();
                std::vector<float> skX(nv), skY(nv), skZ(nv), snX(nv), snY(nv), snZ(nv);
                for (int v = 0; v < nv; v++) {
                    const MeshVertex& bv = rm.baseVerts[v];
                    const BonePose& P = pose[rm.vertBone[v]];
                    float px, py, pz; rotq(P.qw,P.qx,P.qy,P.qz, bv.px,bv.py,bv.pz, px,py,pz);
                    skX[v] = px + P.px; skY[v] = py + P.py; skZ[v] = pz + P.pz;
                    float nx, ny, nz; rotq(P.qw,P.qx,P.qy,P.qz, bv.nx,bv.ny,bv.nz, nx,ny,nz);
                    snX[v] = nx; snY[v] = ny; snZ[v] = nz;
                }

                // Sprite transform (scale + Y/X/Z rotation + translate) and project.
                float meshScale = fs.scale;
                float rY = fs.rotation * 3.14159265f/180.0f, rX = fs.rotationX * 3.14159265f/180.0f, rZ = fs.rotationZ * 3.14159265f/180.0f;
                float cY = cosf(rY), sY = sinf(rY), cX = cosf(rX), sX = sinf(rX), cZ = cosf(rZ), sZ = sinf(rZ);
                std::vector<float> pSX(nv), pSY(nv), pDepth(nv);
                std::vector<float> wX(nv), wY(nv), wZ(nv);   // world positions (for face normals)
                std::vector<char> pVis(nv);
                for (int v = 0; v < nv; v++) {
                    float lx = skX[v]*meshScale, ly = skY[v]*meshScale, lz = skZ[v]*meshScale;
                    float rx = lx*cY + lz*sY, rz = -lx*sY + lz*cY, ry = ly;
                    float ry2 = ry*cX - rz*sX, rz2 = ry*sX + rz*cX;
                    float rx2 = rx*cZ - ry2*sZ, ry3 = rx*sZ + ry2*cZ;
                    float wx = fs.x + rx2, wy = fs.y + ry3, wz = fs.z + rz2;
                    wX[v] = wx; wY[v] = wy; wZ[v] = wz;
                    float sx, sy; pVis[v] = ProjectPoint(wx, wy, wz, cam, cosA, sinA, sx, sy) ? 1 : 0;
                    pSX[v] = sx; pSY[v] = sy;
                    pDepth[v] = (wx - cam.x)*sinA - (wz - cam.z)*cosA;
                }

                // Depth-sort triangles (far first) for painter's-order fill.
                int nTri = (int)rm.indices.size() / 3;
                std::vector<int> order(nTri);
                std::vector<float> fdep(nTri);
                for (int t = 0; t < nTri; t++) {
                    int i0 = rm.indices[t*3], i1 = rm.indices[t*3+1], i2 = rm.indices[t*3+2];
                    auto dF = [&](int i){ float d = pDepth[i]; return d > kNearPlane ? d : kNearPlane; };
                    fdep[t] = dF(i0) + dF(i1) + dF(i2); order[t] = t;
                }
                std::sort(order.begin(), order.end(), [&](int a, int b){ return fdep[a] > fdep[b]; });

                float lDx = 0.3f, lDy = -0.8f, lDz = 0.5f;
                float lL = sqrtf(lDx*lDx + lDy*lDy + lDz*lDz); lDx/=lL; lDy/=lL; lDz/=lL;

                float camWX = cam.x, camWY = cam.height, camWZ = cam.z;
                float minSX = 9999, maxSX = -9999, minSY = 9999, maxSY = -9999;
                for (int oi = 0; oi < nTri; oi++) {
                    int t = order[oi];
                    int i0 = rm.indices[t*3], i1 = rm.indices[t*3+1], i2 = rm.indices[t*3+2];
                    if (!pVis[i0] && !pVis[i1] && !pVis[i2]) continue;
                    // Geometric face normal (world space) from the skinned positions —
                    // independent of the loaded vertex normals, so shading always works.
                    float ax = wX[i1]-wX[i0], ay = wY[i1]-wY[i0], az = wZ[i1]-wZ[i0];
                    float bx = wX[i2]-wX[i0], by = wY[i2]-wY[i0], bz = wZ[i2]-wZ[i0];
                    float rnx = ay*bz - az*by, rny = az*bx - ax*bz, rnz = ax*by - ay*bx;
                    float nl = sqrtf(rnx*rnx+rny*rny+rnz*rnz); if (nl>0){rnx/=nl;rny/=nl;rnz/=nl;}
                    // Orient toward camera (two-sided shading) so every visible face is lit.
                    float fcx=(wX[i0]+wX[i1]+wX[i2])/3.0f, fcy=(wY[i0]+wY[i1]+wY[i2])/3.0f, fcz=(wZ[i0]+wZ[i1]+wZ[i2])/3.0f;
                    if (rnx*(camWX-fcx) + rny*(camWY-fcy) + rnz*(camWZ-fcz) < 0) { rnx=-rnx; rny=-rny; rnz=-rnz; }
                    float shade = 0.35f + 0.65f * std::max(0.0f, -(rnx*lDx + rny*lDy + rnz*lDz));
                    const MeshVertex& bv0 = rm.baseVerts[i0];
                    uint8_t tr = (uint8_t)(255.0f * bv0.r * shade);
                    uint8_t tg = (uint8_t)(255.0f * bv0.g * shade);
                    uint8_t tb = (uint8_t)(255.0f * bv0.b * shade);
                    DrawTriangle(pSX[i0], pSY[i0], pSX[i1], pSY[i1], pSX[i2], pSY[i2], tr, tg, tb, sp.fog);
                    for (int k : { i0, i1, i2 }) {
                        if (!pVis[k]) continue;
                        if (pSX[k] < minSX) minSX = pSX[k]; if (pSX[k] > maxSX) maxSX = pSX[k];
                        if (pSY[k] < minSY) minSY = pSY[k]; if (pSY[k] > maxSY) maxSY = pSY[k];
                    }
                }
                if (maxSX > minSX && maxSY > minSY) {
                    meshSelCX = (int)((minSX + maxSX) * 0.5f);
                    meshSelCY = (int)((minSY + maxSY) * 0.5f);
                    halfW = (int)((maxSX - minSX) * 0.5f) + 2;
                    halfH = (int)((maxSY - minSY) * 0.5f) + 2;
                    drawCenterY = meshSelCY;
                    hasMeshBounds = true;
                }
                drewSprite = true;
            }
        }

        if (!drewSprite)
            DrawDiamond(sp.screenX, drawCenterY, halfW, halfH, cr, cg, cb, sp.fog);

        // Selection highlight and click-select only for parent sprites
        if (sp.subIdx < 0)
        {
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

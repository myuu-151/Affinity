#include "gba_package.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <iomanip>
#include <set>
#include <unordered_map>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#undef PlaySound
#endif

namespace fs = std::filesystem;

namespace Affinity
{

// Run a command via devkitPro's MSYS2 bash using CreateProcess
// so we bypass cmd.exe entirely.
static int RunDevkitBash(const std::string& cmd, std::string& output)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Create pipe for stdout/stderr
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    // Build command line: bash.exe -lc '<cmd>'
    std::string cmdLine = "C:\\devkitPro\\msys2\\usr\\bin\\bash.exe -lc '" + cmd + "'";

    BOOL ok = CreateProcessA(
        nullptr,
        (LPSTR)cmdLine.c_str(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    CloseHandle(hWritePipe);

    if (!ok)
    {
        CloseHandle(hReadPipe);
        output = "Failed to launch devkitPro bash";
        return -1;
    }

    // Read output
    output.clear();
    char buf[512];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
    {
        buf[bytesRead] = '\0';
        output += buf;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return (int)exitCode;
}

// Convert Windows path to MSYS2 path: C:\foo\bar -> /c/foo/bar
static std::string ToMsysPath(const std::string& winPath)
{
    std::string p = winPath;
    for (auto& c : p) if (c == '\\') c = '/';
    if (p.size() >= 2 && p[1] == ':')
    {
        char drive = (char)tolower(p[0]);
        p = "/" + std::string(1, drive) + p.substr(2);
    }
    return p;
}

// Convert editor world coords to GBA 16.8 fixed-point.
// Editor: ±512 range, GBA: 0-255 pixel map (32 tiles * 8px).
// Mapping: gba_pixel = (editor + 512) * 256 / 1024 = (editor + 512) / 4
// Fixed 16.8: gba_fixed = gba_pixel * 256
static int EditorToGBAFixed(float editorCoord)
{
    float gbaPx = (editorCoord + 512.0f) / 4.0f;
    return (int)(gbaPx * 256.0f);
}

// Convert editor height to GBA 16.8 fixed-point.
// Divide by 4 to match XZ coordinate scaling (editor world / 4 = GBA world).
static int EditorHeightToGBAFixed(float editorH)
{
    return (int)((editorH / 4.0f) * 256.0f);
}

// Convert editor sprite Y to GBA 16.8 fixed-point
static int EditorSpriteYToGBAFixed(float editorY)
{
    return (int)((editorY / 4.0f) * 256.0f);
}

// Convert editor angle (radians) to GBA brad (0-65535)
static unsigned short EditorAngleToBrad(float radians)
{
    // Normalize to 0..2pi
    float a = fmodf(radians, 6.2831853f);
    if (a < 0) a += 6.2831853f;
    return (unsigned short)(a / 6.2831853f * 65536.0f);
}

// Convert editor RGBA8 palette color to GBA RGB15
static unsigned short EditorColorToRGB15(uint32_t rgba)
{
    unsigned r = ((rgba >> 0) & 0xFF) >> 3;
    unsigned g = ((rgba >> 8) & 0xFF) >> 3;
    unsigned b = ((rgba >> 16) & 0xFF) >> 3;
    return (unsigned short)(r | (g << 5) | (b << 10));
}

// Snap to nearest valid GBA OBJ size (8, 16, 32, 64)
static int SnapToOBJSize(int sz)
{
    if (sz <= 8)  return 8;
    if (sz <= 16) return 16;
    if (sz <= 32) return 32;
    return 64;
}

// Convert a sprite frame to 4bpp GBA tile u32 data.
// Emits tiles at the frame's native size (8/16/32/64), upscaling smaller
// source frames as needed. Returns u32s in row-major tile order for 1D OBJ mapping.
//
// DEBUG: set AFN_DEBUG_TEST_PATTERN to 1 to replace all sprite frames with a
// colored-quadrant test pattern.
#define AFN_DEBUG_TEST_PATTERN 0

static std::vector<uint32_t> FrameToGBATiles(const GBASpriteFrameExport& frame, int overrideSize = 0)
{
    int fSize = overrideSize > 0 ? SnapToOBJSize(overrideSize) : SnapToOBJSize(frame.width);
    int outW = fSize, outH = fSize;
    const int outTilesX = outW / 8;
    const int outTilesY = outH / 8;
    const int outTotalTiles = outTilesX * outTilesY;
    std::vector<uint32_t> data(outTotalTiles * 8, 0);

#if AFN_DEBUG_TEST_PATTERN
    for (int oy = 0; oy < outH; oy++)
    {
        for (int ox = 0; ox < outW; ox++)
        {
            uint8_t palIdx;
            if (ox == 0 || ox == outW - 1 || oy == 0 || oy == outH - 1)
                palIdx = 5;
            else if (oy < outH/2 && ox < outW/2) palIdx = 1;
            else if (oy < outH/2)                 palIdx = 2;
            else if (ox < outW/2)                 palIdx = 3;
            else                                   palIdx = 4;

            int tileIdx = (oy / 8) * outTilesX + (ox / 8);
            int lx = ox & 7;
            int ly = oy & 7;
            int rowIdx = tileIdx * 8 + ly;
            int bit = lx * 4;
            data[rowIdx] |= ((uint32_t)palIdx << bit);
        }
    }
    return data;
#endif

    int fW = frame.width, fH = frame.height;
    if (fW <= 0 || fH <= 0) return data;

    // Map each output pixel back to source frame pixel (scale to fit)
    for (int oy = 0; oy < outH; oy++)
    {
        int sy = oy * fH / outH;
        if (sy >= fH) sy = fH - 1;
        for (int ox = 0; ox < outW; ox++)
        {
            int sx = ox * fW / outW;
            if (sx >= fW) sx = fW - 1;
            uint8_t palIdx = frame.pixels[sy * kExportMaxFrameSize + sx] & 0xF;
            if (palIdx == 0) continue;

            int tileIdx = (oy / 8) * outTilesX + (ox / 8);
            int lx = ox & 7;
            int ly = oy & 7;
            int rowIdx = tileIdx * 8 + ly;
            int bit = lx * 4;
            data[rowIdx] |= ((uint32_t)palIdx << bit);
        }
    }
    return data;
}

// Quantize an RGBA8 image to 4bpp palette indices (32x32 output).
// Builds a shared 15-color palette from all 8 direction images.
struct QuantizedDirFrame
{
    uint8_t pixels[64 * 64]; // palette indices 0-15 (0=transparent)
};

// Convert a raw NxN palette-indexed buffer to 4bpp GBA tiles
static std::vector<uint32_t> RawPixelsToGBATiles(const uint8_t* pixels, int size)
{
    const int tilesPerRow = size / 8;
    const int totalTiles = tilesPerRow * tilesPerRow;
    std::vector<uint32_t> data(totalTiles * 8, 0);

    for (int oy = 0; oy < size; oy++)
    {
        for (int ox = 0; ox < size; ox++)
        {
            uint8_t palIdx = pixels[oy * size + ox] & 0xF;
            if (palIdx == 0) continue;
            int tileIdx = (oy / 8) * tilesPerRow + (ox / 8);
            int lx = ox & 7;
            int ly = oy & 7;
            int rowIdx = tileIdx * 8 + ly;
            int bit = lx * 4;
            data[rowIdx] |= ((uint32_t)palIdx << bit);
        }
    }
    return data;
}

// Generate mapdata.h with sprite asset tile data, palettes, camera, and player data
static bool GenerateMapData(const std::string& runtimeDir,
                            const std::vector<GBASpriteExport>& sprites,
                            const std::vector<GBASpriteAssetExport>& assets,
                            const GBACameraExport& camera,
                            const std::vector<GBAMeshExport>& meshes,
                            const unsigned char* m7FloorPixels, int m7FloorW, int m7FloorH,
                            float orbitDist,
                            const GBAScriptExport& script,
                            const std::vector<GBABlueprintExport>& blueprints,
                            const std::vector<GBABlueprintInstanceExport>& bpInstances,
                            const std::vector<GBATmSceneExport>& tmScenes,
                            const std::vector<GBAHudElementExport>& hudElements,
                            const std::vector<GBASoundSampleExport>& soundSamples,
                            const std::vector<GBASoundInstanceExport>& soundInstances,
                            int startMode)
{
    fs::path outPath = fs::path(runtimeDir) / "include" / "mapdata.h";
    std::ofstream f(outPath);
    if (!f.is_open()) return false;

    f << "// Generated by Affinity editor\n";
    f << "#ifndef MAPDATA_H\n";
    f << "#define MAPDATA_H\n\n";
    f << "#define AFFINITY_HAS_SPRITES\n\n";

    // Scene mode tracking (must be before blueprint dispatch)
    f << "static int afn_current_mode;\n";
    f << "static int tm_scene_idx;\n\n";

    // Camera start
    f << "// Camera start position\n";
    f << "#define AFN_CAM_X     " << EditorToGBAFixed(camera.x) << "\n";
    f << "#define AFN_CAM_Z     " << EditorToGBAFixed(camera.z) << "\n";
    f << "#define AFN_CAM_H     " << EditorHeightToGBAFixed(camera.height) << "\n";
    f << "#define AFN_CAM_ANGLE " << EditorAngleToBrad(camera.angle) << "\n";
    f << "#define AFN_CAM_HORIZON " << (int)camera.horizon << "\n";
    // Movement speeds (GBA fixed-point, scaled from editor units)
    // Editor 35 -> GBA 37, so scale factor is ~37/35 = 1.057
    f << "#define AFN_WALK_SPEED "   << (int)(camera.walkSpeed * 37.0f / 35.0f) << "\n";
    f << "#define AFN_SPRINT_SPEED " << (int)(camera.sprintSpeed * 37.0f / 35.0f) << "\n";
    // Camera follow ease rates (stored as fixed-point: pct * 256 / 100)
    f << "#define AFN_WALK_EASE_IN "    << (int)(camera.walkEaseIn * 256.0f / 100.0f) << "\n";
    f << "#define AFN_WALK_EASE_OUT "   << (int)(camera.walkEaseOut * 256.0f / 100.0f) << "\n";
    f << "#define AFN_SPRINT_EASE_IN "  << (int)(camera.sprintEaseIn * 256.0f / 100.0f) << "\n";
    f << "#define AFN_SPRINT_EASE_OUT " << (int)(camera.sprintEaseOut * 256.0f / 100.0f) << "\n";
    // Jump physics as 16.8 fixed-point (editor pixels * 256)
    f << "#define AFN_JUMP_VEL "      << (int)(camera.jumpForce * 256.0f) << "\n";
    f << "#define AFN_GRAVITY "       << (int)(camera.gravity * 256.0f) << "\n";
    f << "#define AFN_TERMINAL_VEL "  << (int)(camera.maxFallSpeed * 256.0f) << "\n";
    f << "#define AFN_JUMP_CAM_LAND " << (int)(camera.jumpCamLand * 256.0f / 100.0f) << "\n";
    f << "#define AFN_JUMP_CAM_AIR "  << (int)(camera.jumpCamAir * 256.0f / 100.0f) << "\n";
    // Auto-orbit speed (brads per frame when strafing)
    if (camera.autoOrbitSpeed > 0.0f)
        f << "#define AFN_AUTO_ORBIT_SPEED " << (int)camera.autoOrbitSpeed << "\n";
    // Jump dampen factor (0-1 as 8.8 fixed: 0.75 * 256 = 192)
    f << "#define AFN_JUMP_DAMPEN " << (int)(camera.jumpDampen * 256.0f) << "\n";
    // Draw distance as 16.8 fixed-point (editor units / 4 * 256), 0 = unlimited
    if (camera.drawDistance > 0.0f)
        f << "#define AFN_DRAW_DISTANCE " << (int)(camera.drawDistance / 4.0f * 256.0f) << "\n";
    if (camera.smallTriCull > 0)
        f << "#define AFN_SMALL_TRI_CULL " << camera.smallTriCull << "\n";
    if (camera.coverageBuf)
        f << "#define AFN_COVERAGE_BUF 1\n";
    f << "\n";

    // Find player sprite index
    int playerIdx = -1;
    for (size_t i = 0; i < sprites.size(); i++)
    {
        if (sprites[i].spriteType == 1) // SpriteType::Player
        { playerIdx = (int)i; break; }
    }
    f << "// Player sprite index (-1 = none)\n";
    f << "#define AFN_PLAYER_IDX " << playerIdx << "\n";

    // Orbit distance (editor units -> GBA 16.8 fixed)
    // Editor orbit dist is in world units; convert same as position offset
    // orbitDist in editor = pixels in editor space. GBA: / 4 * 256
    int orbitFixed = (int)(orbitDist / 4.0f * 256.0f);
    f << "#define AFN_ORBIT_DIST " << orbitFixed << "\n\n";

    // Sprite assets
    f << "#define AFN_ASSET_COUNT " << (int)assets.size() << "\n\n";

    // Determine which assets are actually referenced by sprites in the scene.
    // Only referenced assets get VRAM tiles — unreferenced ones are skipped to prevent
    // OBJ VRAM overflow in Mode 4 where only 512 tiles (16KB) are usable.
    std::vector<bool> assetReferencedBySprite(assets.size(), false);
    std::vector<bool> assetReferencedByTilemap(assets.size(), false);
    for (size_t si = 0; si < sprites.size(); si++)
        if (sprites[si].assetIdx >= 0 && sprites[si].assetIdx < (int)assets.size())
            assetReferencedBySprite[sprites[si].assetIdx] = true;
    // Also mark assets referenced by tilemap objects that need OAM tiles.
    // Tile-type objects (type 6) generate BG tiles — they don't need OBJ VRAM.
    for (const auto& sc : tmScenes)
        for (const auto& obj : sc.objects)
            if (obj.spriteAssetIdx >= 0 && obj.spriteAssetIdx < (int)assets.size() && obj.type != 6) {
                assetReferencedBySprite[obj.spriteAssetIdx] = true;
                assetReferencedByTilemap[obj.spriteAssetIdx] = true;
            }
    // Mark assets referenced by HUD element pieces and cursors
    for (const auto& el : hudElements) {
        for (const auto& pc : el.pieces)
            if (pc.spriteAssetIdx >= 0 && pc.spriteAssetIdx < (int)assets.size())
                assetReferencedBySprite[pc.spriteAssetIdx] = true;
        if (el.cursorAssetIdx >= 0 && el.cursorAssetIdx < (int)assets.size())
            assetReferencedBySprite[el.cursorAssetIdx] = true;
    }

    // Build one combined tile data array: all assets, all frames, packed contiguously
    std::vector<uint32_t> allTiles;
    std::vector<int> assetTileStart;
    std::vector<int> assetTilesPerFrame;

    // Compute max piece size per asset from HUD elements (pieces may request larger OAM than asset's native size)
    std::vector<int> hudMaxSize(assets.size(), 0);
    for (const auto& el : hudElements) {
        for (const auto& pc : el.pieces)
            if (pc.spriteAssetIdx >= 0 && pc.spriteAssetIdx < (int)assets.size())
                if (pc.size > hudMaxSize[pc.spriteAssetIdx])
                    hudMaxSize[pc.spriteAssetIdx] = pc.size;
        for (const auto& sp : el.sprites)
            if (sp.spriteAssetIdx >= 0 && sp.spriteAssetIdx < (int)assets.size())
                if (sp.size > hudMaxSize[sp.spriteAssetIdx])
                    hudMaxSize[sp.spriteAssetIdx] = sp.size;
    }

    std::vector<int> assetObjSize; // snapped OBJ size per asset
    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        const auto& asset = assets[ai];
        int objSize = SnapToOBJSize(asset.baseSize);
        // If a HUD piece uses this asset at a larger size, expand to fit
        if (hudMaxSize[ai] > objSize)
            objSize = SnapToOBJSize(hudMaxSize[ai]);
        assetObjSize.push_back(objSize);
        int tilesPerFrame = (objSize / 8) * (objSize / 8);
        assetTileStart.push_back((int)allTiles.size() / 8);
        assetTilesPerFrame.push_back(tilesPerFrame);

        // Skip static frames for direction-based assets — they use DMA direction tiles
        // (but tilemap-referenced direction assets need static OBJ tiles for Mode 0)
        if (asset.hasDirections && !asset.dirAnimSets.empty() && !assetReferencedByTilemap[ai]) continue;
        // Skip unreferenced assets — no sprite uses them, saves OBJ VRAM
        if (!assetReferencedBySprite[ai]) continue;

        for (size_t fi = 0; fi < asset.frames.size(); fi++)
        {
            auto td = FrameToGBATiles(asset.frames[fi], objSize);
            allTiles.insert(allTiles.end(), td.begin(), td.end());
        }
    }

    // Quantize and append per-asset directional sprites (multi-set support)
    struct AssetDirInfo {
        bool has = false;
        int setCount = 0;
        int dirSize = 64;
        int palBank = 0;
        int vramTile0 = 0;
        uint32_t palette[16] = {};
        std::vector<int> romSetU32Offset; // u32 index into dirAnimAllTiles per set
    };
    std::vector<AssetDirInfo> assetDirInfos(assets.size());
    std::vector<uint32_t> dirAnimAllTiles; // ROM data for DMA streaming
    int dirVramNextTile = 0; // running VRAM tile offset for direction assets

    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        if (!assets[ai].hasDirections || assets[ai].dirAnimSets.empty()) continue;

        // Check if any direction image exists across all sets
        bool anyDir = false;
        for (size_t si = 0; si < assets[ai].dirAnimSets.size() && !anyDir; si++)
            for (int d = 0; d < 8; d++)
                if (assets[ai].dirAnimSets[si].dirImages[d].pixels &&
                    assets[ai].dirAnimSets[si].dirImages[d].width > 0)
                { anyDir = true; break; }
        if (!anyDir) continue;

        int setCount = (int)assets[ai].dirAnimSets.size();
        assetDirInfos[ai].has = true;
        assetDirInfos[ai].setCount = setCount;
        assetDirInfos[ai].dirSize = 64;
        // Use source asset's palBank if sharing palette
        int srcPalAsset = assets[ai].paletteSrc;
        if (srcPalAsset >= 0 && srcPalAsset < (int)assets.size() && srcPalAsset != (int)ai)
            assetDirInfos[ai].palBank = assets[srcPalAsset].palBank;
        else
            assetDirInfos[ai].palBank = assets[ai].palBank;

        // Compute which direction sets are actually used by animations
        // to avoid wasting palette slots on unused sets' colors
        std::vector<bool> usedSets(assets[ai].dirAnimSets.size(), false);
        {
            int base = 0;
            for (size_t an = 0; an < assets[ai].anims.size(); an++) {
                int fc = assets[ai].anims[an].endFrame;
                for (int f2 = 0; f2 < fc && (base + f2) < (int)usedSets.size(); f2++)
                    usedSets[base + f2] = true;
                base += fc;
            }
        }

        // Collect unique colors only from animation-referenced direction sets
        struct ColorFreq { unsigned short rgb15; int count; };
        std::vector<ColorFreq> colorFreqs;
        auto findOrAdd = [&](unsigned short c15) -> int {
            for (size_t i = 0; i < colorFreqs.size(); i++)
                if (colorFreqs[i].rgb15 == c15) { colorFreqs[i].count++; return (int)i; }
            colorFreqs.push_back({c15, 1});
            return (int)colorFreqs.size() - 1;
        };

        // Collect colors from a specific asset, restricted to used sets
        auto collectColorsFromAsset = [&](int assetIdx) {
            for (size_t si2 = 0; si2 < assets[assetIdx].dirAnimSets.size(); si2++)
            {
                if (si2 < usedSets.size() && !usedSets[si2]) continue;
                for (int d = 0; d < 8; d++)
                {
                    const auto& img = assets[assetIdx].dirAnimSets[si2].dirImages[d];
                    if (!img.pixels || img.width <= 0 || img.height <= 0) continue;
                    for (int y = 0; y < img.height; y++)
                        for (int x = 0; x < img.width; x++)
                        {
                            int idx2 = (y * img.width + x) * 4;
                            if (img.pixels[idx2 + 3] < 128) continue;
                            unsigned r2 = img.pixels[idx2 + 0] >> 3;
                            unsigned g2 = img.pixels[idx2 + 1] >> 3;
                            unsigned b2 = img.pixels[idx2 + 2] >> 3;
                            findOrAdd((unsigned short)(r2 | (g2 << 5) | (b2 << 10)));
                        }
                }
            }
        };

        // If sharing palette, use source's colors; otherwise use own
        if (srcPalAsset >= 0 && srcPalAsset < (int)assets.size() && srcPalAsset != (int)ai
            && assetDirInfos[srcPalAsset].has)
        {
            // Reuse the source asset's palette directly
            memcpy(assetDirInfos[ai].palette, assetDirInfos[srcPalAsset].palette, sizeof(assetDirInfos[ai].palette));
            // Rebuild colorFreqs from source palette for nearestPal
            for (int i = 1; i < 16; i++)
            {
                uint32_t c = assetDirInfos[srcPalAsset].palette[i];
                if (c == 0) continue;
                unsigned r2 = ((c >> 0) & 0xFF) >> 3;
                unsigned g2 = ((c >> 8) & 0xFF) >> 3;
                unsigned b2 = ((c >> 16) & 0xFF) >> 3;
                colorFreqs.push_back({(unsigned short)(r2 | (g2 << 5) | (b2 << 10)), 1});
            }
        }
        else
        {
            collectColorsFromAsset((int)ai);

            // Merge to 15 colors (preserve visually distinct colors)
            while ((int)colorFreqs.size() > 15)
            {
                int bestI = 0, bestJ = 1, bestDist = 999999;
                for (size_t i = 0; i < colorFreqs.size(); i++)
                    for (size_t j = i + 1; j < colorFreqs.size(); j++)
                    {
                        int dr = (int)(colorFreqs[i].rgb15 & 0x1F) - (int)(colorFreqs[j].rgb15 & 0x1F);
                        int dg = (int)((colorFreqs[i].rgb15 >> 5) & 0x1F) - (int)((colorFreqs[j].rgb15 >> 5) & 0x1F);
                        int db = (int)((colorFreqs[i].rgb15 >> 10) & 0x1F) - (int)((colorFreqs[j].rgb15 >> 10) & 0x1F);
                        int dist = dr*dr + dg*dg + db*db;
                        if (dist < bestDist) { bestDist = dist; bestI = (int)i; bestJ = (int)j; }
                    }
                if (colorFreqs[bestI].count < colorFreqs[bestJ].count)
                    colorFreqs[bestI].rgb15 = colorFreqs[bestJ].rgb15;
                colorFreqs[bestI].count += colorFreqs[bestJ].count;
                colorFreqs.erase(colorFreqs.begin() + bestJ);
            }

            int palCount2 = (int)colorFreqs.size();
            memset(assetDirInfos[ai].palette, 0, sizeof(assetDirInfos[ai].palette));
            for (int i = 0; i < palCount2; i++)
            {
                unsigned short c = colorFreqs[i].rgb15;
                unsigned r2 = (c & 0x1F) << 3;
                unsigned g2 = ((c >> 5) & 0x1F) << 3;
                unsigned b2 = ((c >> 10) & 0x1F) << 3;
                assetDirInfos[ai].palette[i + 1] = r2 | (g2 << 8) | (b2 << 16) | 0xFF000000;
            }
        }

        int palCount = (int)colorFreqs.size();
        auto nearestPal = [&](unsigned short c15) -> uint8_t {
            int bestDist = 999999;
            uint8_t bestIdx = 1;
            for (int i = 0; i < palCount; i++)
            {
                int dr = (int)(c15 & 0x1F) - (int)(colorFreqs[i].rgb15 & 0x1F);
                int dg = (int)((c15 >> 5) & 0x1F) - (int)((colorFreqs[i].rgb15 >> 5) & 0x1F);
                int db = (int)((c15 >> 10) & 0x1F) - (int)((colorFreqs[i].rgb15 >> 10) & 0x1F);
                int dist = dr*dr + dg*dg + db*db;
                if (dist < bestDist) { bestDist = dist; bestIdx = (uint8_t)(i + 1); }
            }
            return bestIdx;
        };

        // Quantize and tile each set
        int dirSize = 64;
        // Only allocate VRAM for assets referenced by sprites — unreferenced assets
        // get ROM data but vramTile0 stays 0 (DMA skipped at runtime since hasDirs=0
        // unless we mark them). We still emit ROM tiles for all direction assets.
        // Direction tiles go first in VRAM (before static tiles) so they get
        // priority for the limited Mode 4 OBJ VRAM (512 tiles at indices 512-1023).
        if (assetReferencedBySprite[ai])
            assetDirInfos[ai].vramTile0 = dirVramNextTile;

        for (int si = 0; si < setCount; si++)
        {
            assetDirInfos[ai].romSetU32Offset.push_back((int)dirAnimAllTiles.size());

            for (int d = 0; d < 8; d++)
            {
                QuantizedDirFrame qf;
                memset(qf.pixels, 0, sizeof(qf.pixels));

                const auto& img = assets[ai].dirAnimSets[si].dirImages[d];
                if (img.pixels && img.width > 0 && img.height > 0)
                {
                    for (int oy = 0; oy < dirSize; oy++)
                    {
                        int sy = oy * img.height / dirSize;
                        for (int ox = 0; ox < dirSize; ox++)
                        {
                            int sx = ox * img.width / dirSize;
                            int idx = (sy * img.width + sx) * 4;
                            if (img.pixels[idx + 3] < 128) continue;
                            // Skip edge pixels to prevent stray anti-aliased artifacts
                            if (ox == 0 || oy == 0 || ox == dirSize - 1 || oy == dirSize - 1)
                            {
                                if (img.pixels[idx + 3] < 255) continue;
                            }
                            unsigned r = img.pixels[idx + 0] >> 3;
                            unsigned g = img.pixels[idx + 1] >> 3;
                            unsigned b = img.pixels[idx + 2] >> 3;
                            qf.pixels[oy * dirSize + ox] = nearestPal(
                                (unsigned short)(r | (g << 5) | (b << 10)));
                        }
                    }
                }

                auto td = RawPixelsToGBATiles(qf.pixels, dirSize);
                dirAnimAllTiles.insert(dirAnimAllTiles.end(), td.begin(), td.end());
            }
        }

        // Advance VRAM offset only for referenced assets
        int tpf = (dirSize / 8) * (dirSize / 8); // tiles per direction frame
        if (assetReferencedBySprite[ai])
            dirVramNextTile += 8 * tpf;
    }

    // Auto-assign unique palBanks for ALL assets to prevent palette overwrites.
    // Assets sharing a palette (paletteSrc) share the same bank. Bank 0 is reserved
    // (transparent), banks 1-15 available. Bank 6 reserved for minimap.
    // Stores resolved bank per asset in resolvedPalBank[].
    std::vector<int> resolvedPalBank(assets.size(), -1);
    {
        bool usedBanks[16] = {};
        usedBanks[0] = true;  // bank 0 = transparent, never use
        usedBanks[6] = true;  // bank 6 = minimap dots
        usedBanks[15] = true; // bank 15 = HUD font text

        // Pass 1: assign banks to independent assets (paletteSrc < 0 or self)
        // Also detect identical palettes and merge them to the same bank
        for (size_t ai = 0; ai < assets.size(); ai++) {
            int src = assets[ai].paletteSrc;
            if (src >= 0 && src < (int)assets.size() && src != (int)ai) continue; // shared — handle later

            // Check if any earlier independent asset has an identical palette
            // Skip assets with direction sprites — their palBank gets overwritten by direction palette
            // Direction assets must NEVER share a bank with non-direction assets, because
            // runtime loads direction palettes on top of the shared bank, corrupting the other asset.
            int matchBank = -1;
            if (assets[ai].hasDirections && !assets[ai].dirAnimSets.empty()) {
                // Force direction assets to get their own unique bank — skip merging entirely
            } else
            for (size_t bi = 0; bi < ai; bi++) {
                if (resolvedPalBank[bi] < 0) continue;
                int bsrc = assets[bi].paletteSrc;
                if (bsrc >= 0 && bsrc < (int)assets.size() && bsrc != (int)bi) continue;
                // Don't merge with assets that have direction palettes (they overwrite the bank)
                if (assets[bi].hasDirections && !assets[bi].dirAnimSets.empty()) continue;
                bool same = true;
                for (int c = 0; c < 16; c++)
                    if (assets[ai].palette[c] != assets[bi].palette[c]) { same = false; break; }
                if (same) { matchBank = resolvedPalBank[bi]; break; }
            }
            if (matchBank >= 0) {
                resolvedPalBank[ai] = matchBank;
                continue;
            }

            int bank = assets[ai].palBank & 15;
            if (bank == 0 || usedBanks[bank]) {
                // Find next free bank
                bank = -1;
                for (int b = 1; b < 15; b++)
                    if (!usedBanks[b]) { bank = b; break; }
                if (bank < 0) {
                    // All banks exhausted — merge with most similar palette
                    // Skip entry 0 (transparent) and weight by non-zero entries only
                    int bestBank = 1; int bestDiff = 0x7FFFFFFF;
                    for (size_t bi = 0; bi < ai; bi++) {
                        if (resolvedPalBank[bi] < 1) continue;
                        if (assets[bi].hasDirections && !assets[bi].dirAnimSets.empty()) continue;
                        int diff = 0; int usedCount = 0;
                        for (int c = 1; c < 16; c++) { // skip entry 0
                            uint32_t ca = assets[ai].palette[c];
                            uint32_t cb = assets[bi].palette[c];
                            if (ca == 0 && cb == 0) continue; // both unused
                            usedCount++;
                            int dr = (int)(ca & 0xFF) - (int)(cb & 0xFF);
                            int dg = (int)((ca >> 8) & 0xFF) - (int)((cb >> 8) & 0xFF);
                            int db = (int)((ca >> 16) & 0xFF) - (int)((cb >> 16) & 0xFF);
                            diff += dr*dr + dg*dg + db*db;
                            // Penalize when one is zero and other isn't (mismatched usage)
                            if ((ca == 0) != (cb == 0)) diff += 10000;
                        }
                        if (usedCount == 0) diff = 0x7FFFFFFE; // no overlap at all
                        if (diff < bestDiff) { bestDiff = diff; bestBank = resolvedPalBank[bi]; }
                    }
                    bank = bestBank;
                }
            }
            usedBanks[bank] = true;
            resolvedPalBank[ai] = bank;
        }
        // Pass 2: shared palette assets inherit their source's resolved bank
        for (size_t ai = 0; ai < assets.size(); ai++) {
            int src = assets[ai].paletteSrc;
            if (src >= 0 && src < (int)assets.size() && src != (int)ai)
                resolvedPalBank[ai] = resolvedPalBank[src];
            // Fallback if somehow unassigned
            if (resolvedPalBank[ai] < 0) resolvedPalBank[ai] = 1;
        }
        // Apply resolved banks to direction infos too
        for (size_t ai = 0; ai < assets.size(); ai++)
            if (assetDirInfos[ai].has)
                assetDirInfos[ai].palBank = resolvedPalBank[ai];
    }

    int totalTileCount = (int)allTiles.size() / 8;
    // In Mode 4 (meshes present), tiles 0-511 overlap bitmap — offset by 512
    int tileOffset = meshes.empty() ? 0 : 512;
    int minimapTile = totalTileCount + dirVramNextTile + tileOffset;

    // Emit combined tile data (always emit even if empty — runtime expects the symbol)
    if (!allTiles.empty())
    {
        f << "// Combined OBJ tile data (" << totalTileCount << " tiles, "
          << (int)allTiles.size() * 4 << " bytes)\n";
        f << "static const u32 afn_all_tiles[" << (int)allTiles.size() << "] = {";
        for (size_t i = 0; i < allTiles.size(); i++)
        {
            if (i % 8 == 0) f << "\n    ";
            char hex[12];
            snprintf(hex, sizeof(hex), "0x%08X", allTiles[i]);
            f << hex;
            if (i + 1 < allTiles.size()) f << ", ";
        }
        f << "\n};\n";
    }
    else
    {
        f << "// No static OBJ tiles (all assets use direction DMA)\n";
        f << "static const u32 afn_all_tiles[1] = { 0 };\n";
    }
    f << "#define AFN_ALL_TILES_LEN " << (int)allTiles.size() * 4 << "\n";
    f << "#define AFN_DIR_VRAM_TILES " << dirVramNextTile << "\n\n";

    // Emit direction animation ROM tile data (for DMA streaming)
    if (!dirAnimAllTiles.empty())
    {
        f << "// Direction animation tile data — ROM only, DMA'd to VRAM on set change\n";
        f << "static const u32 afn_dir_anim_tiles[" << (int)dirAnimAllTiles.size() << "] = {";
        for (size_t i = 0; i < dirAnimAllTiles.size(); i++)
        {
            if (i % 8 == 0) f << "\n    ";
            char hex[12];
            snprintf(hex, sizeof(hex), "0x%08X", dirAnimAllTiles[i]);
            f << hex;
            if (i + 1 < dirAnimAllTiles.size()) f << ", ";
        }
        f << "\n};\n";
        f << "#define AFN_DIR_ANIM_TILES_LEN " << (int)dirAnimAllTiles.size() * 4 << "\n\n";
    }

    // Emit per-asset palette table (2D array indexed by asset)
    f << "static const u16 afn_pal[" << assets.size() << "][16] = {\n";
    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        f << "    { ";
        for (int c = 0; c < 16; c++)
        {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", EditorColorToRGB15(assets[ai].palette[c]));
            f << hex;
            if (c < 15) f << ", ";
        }
        f << " },\n";
    }
    f << "};\n";

    // Per-asset direction palette table (2D array indexed by asset)
    if (!assets.empty())
    {
        f << "static const u16 afn_pal_assetdir[" << assets.size() << "][16] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { ";
            for (int c = 0; c < 16; c++)
            {
                char hex[8];
                snprintf(hex, sizeof(hex), "0x%04X", EditorColorToRGB15(assetDirInfos[ai].palette[c]));
                f << hex;
                if (c < 15) f << ", ";
            }
            f << " },\n";
        }
        f << "};\n";
    }
    f << "\n";

    // tileOffset already computed above (before minimapTile)

    // Asset direction descriptor table: { setCount, tpf, dirSize, palBank, hasDirs, vramTile0 }
    if (!assets.empty())
    {
        int maxDirSets = 0;
        for (size_t ai = 0; ai < assets.size(); ai++)
            if (assetDirInfos[ai].has && assetDirInfos[ai].setCount > maxDirSets)
                maxDirSets = assetDirInfos[ai].setCount;
        if (maxDirSets < 1) maxDirSets = 1;
        f << "#define AFN_HAS_ASSET_DIRS 1\n";
        f << "#define AFN_MAX_DIR_SETS " << maxDirSets << "\n";
        f << "static const int afn_asset_dir_desc[][6] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            if (assetDirInfos[ai].has && assetReferencedBySprite[ai])
            {
                int tpf = (assetDirInfos[ai].dirSize / 8) * (assetDirInfos[ai].dirSize / 8);
                f << "    { " << assetDirInfos[ai].setCount << ", " << tpf
                  << ", " << assetDirInfos[ai].dirSize
                  << ", " << assetDirInfos[ai].palBank
                  << ", 1, " << (assetDirInfos[ai].vramTile0 + tileOffset) << " },\n";
            }
            else
            {
                f << "    { 0, 0, 0, 0, 0, 0 },\n";
            }
        }
        f << "};\n\n";

        // Per-set ROM u32 offsets into afn_dir_anim_tiles
        f << "static const int afn_dir_set_offsets[][AFN_MAX_DIR_SETS] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { ";
            for (int si = 0; si < maxDirSets; si++)
            {
                if (assetDirInfos[ai].has && assetReferencedBySprite[ai] && si < (int)assetDirInfos[ai].romSetU32Offset.size())
                    f << assetDirInfos[ai].romSetU32Offset[si];
                else
                    f << -1;
                if (si < maxDirSets - 1) f << ", ";
            }
            f << " },\n";
        }
        f << "};\n\n";
    }

    // Per-asset animation descriptors: { baseSetIdx, frameCount, fps }
    // Allows runtime to cycle through direction frames within an animation
    if (!assets.empty())
    {
        int maxAnims = 0;
        for (size_t ai = 0; ai < assets.size(); ai++)
            if ((int)assets[ai].anims.size() > maxAnims)
                maxAnims = (int)assets[ai].anims.size();
        if (maxAnims < 1) maxAnims = 1;
        f << "#define AFN_MAX_ANIMS " << maxAnims << "\n";
        f << "static const int afn_anim_desc[][AFN_MAX_ANIMS][3] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { ";
            int base = 0;
            for (int an = 0; an < maxAnims; an++)
            {
                if (an < (int)assets[ai].anims.size())
                {
                    int fc = assets[ai].anims[an].endFrame;
                    int fps = assets[ai].anims[an].fps;
                    if (fps <= 0) fps = 8;
                    float spd = assets[ai].anims[an].speed;
                    if (spd <= 0.0f) spd = 1.0f;
                    int effectiveFps = (int)(fps * spd);
                    if (effectiveFps < 1) effectiveFps = 1;
                    if (fc < 1) fc = 1;
                    f << "{ " << base << ", " << fc << ", " << effectiveFps << " }";
                    base += fc;
                }
                else
                {
                    f << "{ 0, 0, 8 }";
                }
                if (an < maxAnims - 1) f << ", ";
            }
            f << " },\n";
        }
        f << "};\n\n";

        // State-to-slot mapping: for each asset, map game state -> anim slot index
        // States: 0=None, 1=Idle, 2=Walk, 3=Run, 4=Sprint
        f << "#define AFN_STATE_COUNT 5\n";
        f << "static const int afn_state_to_anim[][AFN_STATE_COUNT] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { ";
            for (int st = 0; st < 5; st++)
            {
                int slot = -1;
                for (int an = 0; an < (int)assets[ai].anims.size(); an++)
                {
                    if (assets[ai].anims[an].gameState == st)
                    { slot = an; break; }
                    // Walk(2) and Run(3) are interchangeable
                    if (st == 2 && assets[ai].anims[an].gameState == 3)
                    { slot = an; break; }
                    if (st == 3 && assets[ai].anims[an].gameState == 2)
                    { slot = an; break; }
                }
                f << slot;
                if (st < 4) f << ", ";
            }
            f << " },\n";
        }
        f << "};\n\n";
    }

    // Minimap tile index (only emit if it fits within 1024-tile OBJ VRAM limit)
    if (minimapTile < 1024)
        f << "#define AFN_MINIMAP_TILE " << minimapTile << "\n\n";

    // Asset descriptor table
    if (!assets.empty())
    {
        f << "static const int afn_asset_desc[][5] = {\n";
        f << "    // { tileStart, tilesPerFrame, frameCount, objSize, palBank }\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { " << (assetTileStart[ai] + tileOffset + dirVramNextTile) << ", " << assetTilesPerFrame[ai]
              << ", " << (int)assets[ai].frames.size()
              << ", " << assetObjSize[ai]
              << ", " << resolvedPalBank[ai] << " },\n";
        }
        f << "};\n\n";
    }

    // Sprites
    f << "#define AFN_SPRITE_COUNT " << (int)sprites.size() << "\n\n";

    if (!sprites.empty())
    {
        f << "static const int afn_sprite_data[][10] = {\n";
        f << "    // { x_fixed, y_fixed, z_fixed, palIdx, assetIdx, scale_8_8, spriteType, rotation_brad, animEnabled, meshIdx }\n";
        for (size_t i = 0; i < sprites.size(); i++)
        {
            int gx = EditorToGBAFixed(sprites[i].x);
            int gy = EditorSpriteYToGBAFixed(sprites[i].y);
            int gz = EditorToGBAFixed(sprites[i].z);
            int pal = sprites[i].palIdx;
            int aIdx = sprites[i].assetIdx;
            int scaleFixed = (int)(sprites[i].scale * 256.0f);
            int sType = sprites[i].spriteType;
            // Convert degrees to brad (0-65535): degrees * 65536 / 360
            int rotBrad = (int)(sprites[i].rotation * 65536.0f / 360.0f) & 0xFFFF;
            int animEn = sprites[i].animEnabled ? 1 : 0;
            int meshIdx2 = sprites[i].meshIdx;
            f << "    { " << gx << ", " << gy << ", " << gz << ", "
              << pal << ", " << aIdx << ", " << scaleFixed << ", " << sType << ", " << rotBrad << ", " << animEn << ", " << meshIdx2 << " },\n";
        }
        f << "};\n";
    }

    // ---- Mesh assets ----
    f << "#define AFN_MESH_COUNT " << (int)meshes.size() << "\n\n";

    // Track final vertex counts per mesh for descriptor table
    std::vector<int> finalVertCounts(meshes.size(), 0);

    if (!meshes.empty())
    {
        // Mesh shading palette base
        f << "#define AFN_MESH_PAL_BASE 224\n\n";

        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            const auto& mesh = meshes[mi];
            int origVc = (int)mesh.positions.size() / 3;
            int ic = (int)mesh.indices.size();

            // Pointers to the vertex/normal/index data we'll actually emit
            const float* outPos = mesh.positions.data();
            const float* outNorm = mesh.normals.data();
            int vc = origVc;

            // Performance/Barebones: weld vertices sharing the same OBJ position index
            std::vector<float> weldedPos, weldedNorm, weldedUVs;
            std::vector<uint32_t> remappedIdx(ic);
            std::vector<int> vertRemap;
            bool didWeld = false;
            bool preSorted = false;

            if (mesh.exportMode >= 1) // Performance or Barebones
            {
                std::vector<int> weldedNormCount;
                vertRemap.assign(origVc, -1);
                // Store objIdx + UV hash for weld matching
                struct WeldKey { int objIdx; float u, v; int weldedIdx; };
                std::vector<WeldKey> weldKeys;
                bool hasUVs = mesh.textured && !mesh.uvs.empty();

                for (int v = 0; v < origVc; v++)
                {
                    int objIdx = (v < (int)mesh.objPosIdx.size()) ? mesh.objPosIdx[v] : -1;
                    float vu = 0.0f, vv = 0.0f;
                    if (hasUVs && v * 2 + 1 < (int)mesh.uvs.size())
                    { vu = mesh.uvs[v * 2 + 0]; vv = mesh.uvs[v * 2 + 1]; }

                    int found = -1;
                    if (objIdx >= 0)
                    {
                        for (size_t w = 0; w < weldKeys.size(); w++)
                        {
                            if (weldKeys[w].objIdx == objIdx)
                            {
                                // For textured meshes, only weld if UVs also match
                                if (hasUVs)
                                {
                                    float du = weldKeys[w].u - vu;
                                    float dv = weldKeys[w].v - vv;
                                    if (du * du + dv * dv > 0.0001f) continue; // UV mismatch — texture seam
                                }
                                found = weldKeys[w].weldedIdx;
                                break;
                            }
                        }
                    }

                    if (found >= 0)
                    {
                        weldedNorm[found * 3 + 0] += mesh.normals[v * 3 + 0];
                        weldedNorm[found * 3 + 1] += mesh.normals[v * 3 + 1];
                        weldedNorm[found * 3 + 2] += mesh.normals[v * 3 + 2];
                        weldedNormCount[found]++;
                        vertRemap[v] = found;
                    }
                    else
                    {
                        int weldedVc = (int)weldedPos.size() / 3;
                        vertRemap[v] = weldedVc;
                        if (objIdx >= 0)
                            weldKeys.push_back({objIdx, vu, vv, weldedVc});
                        weldedPos.push_back(mesh.positions[v * 3 + 0]);
                        weldedPos.push_back(mesh.positions[v * 3 + 1]);
                        weldedPos.push_back(mesh.positions[v * 3 + 2]);
                        weldedNorm.push_back(mesh.normals[v * 3 + 0]);
                        weldedNorm.push_back(mesh.normals[v * 3 + 1]);
                        weldedNorm.push_back(mesh.normals[v * 3 + 2]);
                        weldedNormCount.push_back(1);
                        weldedUVs.push_back(vu);
                        weldedUVs.push_back(vv);
                    }
                }

                // Average and normalize
                vc = (int)weldedPos.size() / 3;
                for (int w = 0; w < vc; w++)
                {
                    float nx = weldedNorm[w * 3 + 0] / weldedNormCount[w];
                    float ny = weldedNorm[w * 3 + 1] / weldedNormCount[w];
                    float nz = weldedNorm[w * 3 + 2] / weldedNormCount[w];
                    float len = sqrtf(nx * nx + ny * ny + nz * nz);
                    if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
                    weldedNorm[w * 3 + 0] = nx;
                    weldedNorm[w * 3 + 1] = ny;
                    weldedNorm[w * 3 + 2] = nz;
                }

                // Remap indices
                for (int i = 0; i < ic; i++)
                {
                    int oldIdx = (int)mesh.indices[i];
                    remappedIdx[i] = (oldIdx < origVc) ? vertRemap[oldIdx] : 0;
                }

                outPos = weldedPos.data();
                outNorm = weldedNorm.data();
                didWeld = true;

            }

            // Barebones: pre-sort triangles by centroid distance from mesh center (farthest first)
            // This lets the runtime skip the insertion sort entirely
            if (mesh.exportMode == 2)
            {
                int triCount = ic / 3;
                struct TriDist { int triIdx; float dist; };
                std::vector<TriDist> triDists(triCount);

                // Compute mesh center
                float cx = 0, cy = 0, cz = 0;
                for (int v = 0; v < vc; v++)
                {
                    cx += outPos[v * 3 + 0];
                    cy += outPos[v * 3 + 1];
                    cz += outPos[v * 3 + 2];
                }
                cx /= vc; cy /= vc; cz /= vc;

                // Compute centroid distance per triangle
                for (int t = 0; t < triCount; t++)
                {
                    int i0 = remappedIdx[t * 3 + 0];
                    int i1 = remappedIdx[t * 3 + 1];
                    int i2 = remappedIdx[t * 3 + 2];
                    float tx = (outPos[i0*3+0] + outPos[i1*3+0] + outPos[i2*3+0]) / 3.0f - cx;
                    float ty = (outPos[i0*3+1] + outPos[i1*3+1] + outPos[i2*3+1]) / 3.0f - cy;
                    float tz = (outPos[i0*3+2] + outPos[i1*3+2] + outPos[i2*3+2]) / 3.0f - cz;
                    triDists[t].triIdx = t;
                    triDists[t].dist = tx*tx + ty*ty + tz*tz;
                }

                // Sort farthest first
                std::sort(triDists.begin(), triDists.end(),
                    [](const TriDist& a, const TriDist& b) { return a.dist > b.dist; });

                // Rebuild index buffer in sorted order
                std::vector<uint32_t> sortedIdx(ic);
                for (int t = 0; t < triCount; t++)
                {
                    int src = triDists[t].triIdx;
                    sortedIdx[t * 3 + 0] = remappedIdx[src * 3 + 0];
                    sortedIdx[t * 3 + 1] = remappedIdx[src * 3 + 1];
                    sortedIdx[t * 3 + 2] = remappedIdx[src * 3 + 2];
                }
                remappedIdx = std::move(sortedIdx);
                preSorted = true;
            }

            finalVertCounts[mi] = vc;

            // Vertex positions as 8.8 fixed-point (editor scale / 4 for GBA)
            if (didWeld)
                f << "// Mesh " << mi << ": " << origVc << " verts welded to " << vc << "\n";
            f << "static const s16 afn_mesh" << mi << "_verts[" << vc * 3 << "] = {";
            for (int v = 0; v < vc; v++)
            {
                if (v % 4 == 0) f << "\n    ";
                for (int c = 0; c < 3; c++)
                {
                    int fixed = (int)(outPos[v * 3 + c] / 4.0f * 256.0f);
                    if (fixed > 32767) fixed = 32767;
                    else if (fixed < -32768) fixed = -32768;
                    f << fixed;
                    if (v * 3 + c + 1 < vc * 3) f << ", ";
                }
            }
            f << "\n};\n";

            // Vertex normals as 0.7 signed fixed-point (-128 to 127)
            f << "static const s8 afn_mesh" << mi << "_norms[" << vc * 3 << "] = {";
            for (int v = 0; v < vc; v++)
            {
                if (v % 4 == 0) f << "\n    ";
                for (int c = 0; c < 3; c++)
                {
                    int fixed = (int)(outNorm[v * 3 + c] * 127.0f);
                    if (fixed > 127) fixed = 127;
                    if (fixed < -128) fixed = -128;
                    f << fixed;
                    if (v * 3 + c + 1 < vc * 3) f << ", ";
                }
            }
            f << "\n};\n";

            // Triangle indices
            f << "static const u16 afn_mesh" << mi << "_idx[" << ic << "] = {";
            for (int i = 0; i < ic; i++)
            {
                if (i % 12 == 0) f << "\n    ";
                f << (didWeld ? remappedIdx[i] : mesh.indices[i]);
                if (i + 1 < ic) f << ", ";
            }
            f << "\n};\n";

            // Quad indices
            int qic = (int)mesh.quadIndices.size();
            if (qic > 0)
            {
                f << "static const u16 afn_mesh" << mi << "_qidx[" << qic << "] = {";
                for (int i = 0; i < qic; i++)
                {
                    if (i % 12 == 0) f << "\n    ";
                    int qi = (int)mesh.quadIndices[i];
                    f << (didWeld && qi < origVc ? vertRemap[qi] : qi);
                    if (i + 1 < qic) f << ", ";
                }
                f << "\n};\n";
            }

            // UV coordinates as 8.8 fixed-point (textured meshes only)
            if (mesh.textured && !mesh.uvs.empty())
            {
                // Use welded UVs if available, otherwise original
                const std::vector<float>& uvSrc = (didWeld && !weldedUVs.empty()) ? weldedUVs : mesh.uvs;
                f << "static const s16 afn_mesh" << mi << "_uvs[" << vc * 2 << "] = {";
                for (int v = 0; v < vc; v++)
                {
                    if (v % 4 == 0) f << "\n    ";
                    float u = 0.0f, v2 = 0.0f;
                    if (v * 2 + 1 < (int)uvSrc.size())
                    {
                        u = uvSrc[v * 2 + 0];
                        v2 = uvSrc[v * 2 + 1];
                    }
                    // Convert to 8.8 fixed, multiply by texture size
                    int fu = (int)(u * mesh.texW * 256.0f);
                    int fv = (int)((1.0f - v2) * mesh.texH * 256.0f); // flip V (OBJ convention)
                    f << fu << ", " << fv;
                    if (v + 1 < vc) f << ", ";
                }
                f << "\n};\n";

                // Texture pixel data
                f << "static const u8 afn_mesh" << mi << "_tex[" << mesh.texW * mesh.texH << "] = {";
                for (int p = 0; p < mesh.texW * mesh.texH; p++)
                {
                    if (p % 16 == 0) f << "\n    ";
                    f << (int)mesh.texPixels[p];
                    if (p + 1 < mesh.texW * mesh.texH) f << ", ";
                }
                f << "\n};\n";
            }
            f << "\n";
        }

        // Allocate texture palette slots (16 colors each, starting at BG palette index 32)
        int nextTexPalBase = 32;
        std::vector<int> texPalBases(meshes.size(), 0);
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            if (meshes[mi].textured)
            {
                texPalBases[mi] = nextTexPalBase;
                nextTexPalBase += 16;
            }
        }

        // Mesh descriptor table: { vertCount, indexCount, quadIndexCount, colorRGB15, cullMode, lit, sorted, halfRes, textured, texW, texShift, texPalBase, wireframe, grayscale, drawDist, drawPriority, visible }
        f << "static const int afn_mesh_desc[][17] = {\n";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            int vc = finalVertCounts[mi];
            int ic = (int)meshes[mi].indices.size();
            int qic = (int)meshes[mi].quadIndices.size();
            int lit = meshes[mi].lit;
            int sorted = 0;
            int halfRes = meshes[mi].halfRes;
            int textured = meshes[mi].textured;
            int wireframe = meshes[mi].wireframe;
            int grayscale = meshes[mi].grayscale;
            int texW = meshes[mi].texW;
            int texShift = 0;
            { int tw = texW; while (tw > 1) { texShift++; tw >>= 1; } }
            if (meshes[mi].exportMode == 2) { lit = 0; if (!meshes[mi].textured) sorted = 1; }
            // Convert draw distance from editor units to GBA 16.8 fixed (0 = unlimited)
            int drawDist = 0;
            if (meshes[mi].drawDistance > 0.0f)
                drawDist = (int)(meshes[mi].drawDistance / 4.0f * 256.0f);
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", meshes[mi].colorRGB15);
            f << "    { " << vc << ", " << ic << ", " << qic << ", " << hex << ", " << meshes[mi].cullMode << ", " << lit << ", " << sorted << ", " << halfRes << ", " << textured << ", " << texW << ", " << texShift << ", " << texPalBases[mi] << ", " << wireframe << ", " << grayscale << ", " << drawDist << ", " << meshes[mi].drawPriority << ", " << meshes[mi].visible << " },\n";
        }
        f << "};\n\n";

        // Pointer arrays for runtime access
        f << "static const s16* const afn_mesh_vert_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            f << "afn_mesh" << mi << "_verts";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n";

        f << "static const s8* const afn_mesh_norm_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            f << "afn_mesh" << mi << "_norms";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n";

        f << "static const u16* const afn_mesh_idx_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            f << "afn_mesh" << mi << "_idx";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n";

        f << "static const u16* const afn_mesh_qidx_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            if (!meshes[mi].quadIndices.empty())
                f << "afn_mesh" << mi << "_qidx";
            else
                f << "0";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n";

        // UV pointer array (NULL for non-textured meshes)
        f << "static const s16* const afn_mesh_uv_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            if (meshes[mi].textured)
                f << "afn_mesh" << mi << "_uvs";
            else
                f << "0";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n";

        // Texture pixel pointer array (NULL for non-textured meshes)
        f << "static const u8* const afn_mesh_tex_ptrs[] = { ";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            if (meshes[mi].textured)
                f << "afn_mesh" << mi << "_tex";
            else
                f << "0";
            if (mi + 1 < meshes.size()) f << ", ";
        }
        f << " };\n\n";

        // Texture palettes — emit into a flat array for loading into BG palette
        {
            int totalTexPalEntries = 0;
            for (size_t mi = 0; mi < meshes.size(); mi++)
                if (meshes[mi].textured) totalTexPalEntries += 16;
            if (totalTexPalEntries > 0)
            {
                f << "#define AFN_TEX_PAL_BASE 32\n";
                f << "#define AFN_TEX_PAL_COUNT " << totalTexPalEntries << "\n";
                f << "static const u16 afn_tex_palette[" << totalTexPalEntries << "] = {\n";
                for (size_t mi = 0; mi < meshes.size(); mi++)
                {
                    if (!meshes[mi].textured) continue;
                    f << "    // Mesh " << mi << " texture palette\n    ";
                    for (int c = 0; c < 16; c++)
                    {
                        char hex[8];
                        snprintf(hex, sizeof(hex), "0x%04X", meshes[mi].texPalette[c]);
                        f << hex;
                        if (c < 15) f << ", ";
                    }
                    f << ",\n";
                }
                f << "};\n\n";
            }
        }

        // Mesh shading palette: 8 shades per mesh from dark to base color
        f << "static const u16 afn_mesh_palette[" << (int)meshes.size() * 8 << "] = {\n";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            unsigned short c = meshes[mi].colorRGB15;
            unsigned r = c & 0x1F;
            unsigned g = (c >> 5) & 0x1F;
            unsigned b = (c >> 10) & 0x1F;
            f << "    // Mesh " << mi << "\n    ";
            for (int s = 0; s < 8; s++)
            {
                float t = (float)(s + 1) / 8.0f;
                unsigned sr = (unsigned)(r * t);
                unsigned sg = (unsigned)(g * t);
                unsigned sb = (unsigned)(b * t);
                char hex[8];
                snprintf(hex, sizeof(hex), "0x%04X", (unsigned short)(sr | (sg << 5) | (sb << 10)));
                f << hex;
                if (s < 7 || mi + 1 < meshes.size()) f << ", ";
            }
            f << "\n";
        }
        f << "};\n\n";
    }

    // ---- Collision data: pre-baked world-space faces + spatial grid ----
    {
        struct CollFaceExp {
            float v0x, v0z, v1x, v1z, v2x, v2z;
            float v0y, v1y, v2y;
            float nx, nz;
            int flags;     // 1=floor, 2=ceiling, 4=wall
            int sprIdx;    // sprite index that owns this face (-1 = none)
        };

        std::vector<CollFaceExp> collFaces;

        for (size_t si = 0; si < sprites.size(); si++)
        {
            if (sprites[si].meshIdx < 0) continue;
            int mi = sprites[si].meshIdx;
            if (mi >= (int)meshes.size()) continue;
            if (!meshes[mi].collision) continue;

            const auto& mesh = meshes[mi];
            const auto& spr = sprites[si];

            float sprScale = spr.scale;
            float rotRad = spr.rotation * 3.14159265f / 180.0f;
            float cosR = cosf(rotRad);
            float sinR = sinf(rotRad);

            int vc = (int)mesh.positions.size() / 3;
            std::vector<float> wp(vc * 3);
            for (int v = 0; v < vc; v++)
            {
                float lx = mesh.positions[v * 3 + 0] * sprScale;
                float ly = mesh.positions[v * 3 + 1] * sprScale;
                float lz = mesh.positions[v * 3 + 2] * sprScale;
                wp[v * 3 + 0] = lx * cosR - lz * sinR + spr.x;
                wp[v * 3 + 1] = ly + spr.y;
                wp[v * 3 + 2] = lx * sinR + lz * cosR + spr.z;
            }

            auto emitTri = [&](int i0, int i1, int i2) {
                float ax = wp[i0*3], ay = wp[i0*3+1], az = wp[i0*3+2];
                float bx = wp[i1*3], by = wp[i1*3+1], bz = wp[i1*3+2];
                float cx = wp[i2*3], cy = wp[i2*3+1], cz = wp[i2*3+2];

                float e1x = bx-ax, e1y = by-ay, e1z = bz-az;
                float e2x = cx-ax, e2y = cy-ay, e2z = cz-az;
                float fnx = e1y*e2z - e1z*e2y;
                float fny = e1z*e2x - e1x*e2z;
                float fnz = e1x*e2y - e1y*e2x;
                float len = sqrtf(fnx*fnx + fny*fny + fnz*fnz);
                if (len < 0.0001f) return;
                fnx /= len; fny /= len; fnz /= len;

                int flags;
                if (fny > 0.7f)       flags = 1; // floor
                else if (fny < -0.7f) flags = 2; // ceiling
                else                   flags = 4; // wall

                float nxzLen = sqrtf(fnx*fnx + fnz*fnz);
                float nnx = 0, nnz = 0;
                if (nxzLen > 0.001f) { nnx = fnx / nxzLen; nnz = fnz / nxzLen; }

                CollFaceExp cf;
                cf.v0x = ax; cf.v0z = az; cf.v1x = bx; cf.v1z = bz; cf.v2x = cx; cf.v2z = cz;
                cf.v0y = ay; cf.v1y = by; cf.v2y = cy;
                cf.nx = nnx; cf.nz = nnz;
                cf.flags = flags;
                cf.sprIdx = (int)si;
                collFaces.push_back(cf);
            };

            for (int t = 0; t < (int)mesh.indices.size() / 3; t++)
                emitTri(mesh.indices[t*3], mesh.indices[t*3+1], mesh.indices[t*3+2]);

            for (int q = 0; q < (int)mesh.quadIndices.size() / 4; q++)
            {
                int q0 = mesh.quadIndices[q*4], q1 = mesh.quadIndices[q*4+1];
                int q2 = mesh.quadIndices[q*4+2], q3 = mesh.quadIndices[q*4+3];
                emitTri(q0, q1, q2);
                emitTri(q0, q2, q3);
            }
        }

        if (!collFaces.empty())
        {
            int totalFaces = (int)collFaces.size();
            const int GRID_SIZE = 8;

            std::vector<std::vector<int>> gridCells(GRID_SIZE * GRID_SIZE);
            for (int fi = 0; fi < totalFaces; fi++)
            {
                const auto& cf = collFaces[fi];
                auto toGBAPx = [](float ec) { return (ec + 512.0f) / 4.0f; };

                float minX = std::min({toGBAPx(cf.v0x), toGBAPx(cf.v1x), toGBAPx(cf.v2x)});
                float maxX = std::max({toGBAPx(cf.v0x), toGBAPx(cf.v1x), toGBAPx(cf.v2x)});
                float minZ = std::min({toGBAPx(cf.v0z), toGBAPx(cf.v1z), toGBAPx(cf.v2z)});
                float maxZ = std::max({toGBAPx(cf.v0z), toGBAPx(cf.v1z), toGBAPx(cf.v2z)});

                int cMinX = std::max(0, (int)(minX / 32.0f));
                int cMaxX = std::min(GRID_SIZE - 1, (int)(maxX / 32.0f));
                int cMinZ = std::max(0, (int)(minZ / 32.0f));
                int cMaxZ = std::min(GRID_SIZE - 1, (int)(maxZ / 32.0f));

                for (int gz = cMinZ; gz <= cMaxZ; gz++)
                    for (int gx = cMinX; gx <= cMaxX; gx++)
                        gridCells[gz * GRID_SIZE + gx].push_back(fi);
            }

            std::vector<int> gridStart(GRID_SIZE * GRID_SIZE);
            std::vector<int> gridCount(GRID_SIZE * GRID_SIZE);
            std::vector<int> gridFaceList;
            for (int c = 0; c < GRID_SIZE * GRID_SIZE; c++)
            {
                gridStart[c] = (int)gridFaceList.size();
                gridCount[c] = (int)gridCells[c].size();
                for (int fi : gridCells[c]) gridFaceList.push_back(fi);
            }

            f << "// ---- Collision data (" << totalFaces << " faces, "
              << gridFaceList.size() << " grid refs) ----\n";
            f << "#define AFN_COL_FACE_COUNT " << totalFaces << "\n";
            f << "#define AFN_COL_GRID_SIZE 8\n";
            f << "#define AFN_COL_GRID_SHIFT 13\n\n";

            f << "typedef struct {\n";
            f << "    int v0x, v0z, v1x, v1z, v2x, v2z;\n";
            f << "    int v0y, v1y, v2y;\n";
            f << "    int nx, nz;\n";
            f << "    int flags;\n";
            f << "    int sprIdx;\n";
            f << "} CollFace;\n\n";

            f << "static const CollFace afn_col_faces[" << totalFaces << "] = {\n";
            for (int fi = 0; fi < totalFaces; fi++)
            {
                const auto& cf = collFaces[fi];
                auto toFx = [](float ec) { return (int)((ec + 512.0f) / 4.0f * 256.0f); };
                auto toFy = [](float ey) { return (int)(ey / 4.0f * 256.0f); };

                f << "    { "
                  << toFx(cf.v0x) << "," << toFx(cf.v0z) << ", "
                  << toFx(cf.v1x) << "," << toFx(cf.v1z) << ", "
                  << toFx(cf.v2x) << "," << toFx(cf.v2z) << ", "
                  << toFy(cf.v0y) << "," << toFy(cf.v1y) << "," << toFy(cf.v2y) << ", "
                  << (int)(cf.nx * 256.0f) << "," << (int)(cf.nz * 256.0f) << ", "
                  << cf.flags << ", " << cf.sprIdx << " },\n";
            }
            f << "};\n\n";

            f << "static const u16 afn_col_grid_start[" << GRID_SIZE * GRID_SIZE << "] = {\n    ";
            for (int c = 0; c < GRID_SIZE * GRID_SIZE; c++)
            {
                f << gridStart[c];
                if (c + 1 < GRID_SIZE * GRID_SIZE) f << ",";
                if ((c + 1) % 8 == 0 && c + 1 < GRID_SIZE * GRID_SIZE) f << "\n    ";
            }
            f << "\n};\n";

            f << "static const u16 afn_col_grid_count[" << GRID_SIZE * GRID_SIZE << "] = {\n    ";
            for (int c = 0; c < GRID_SIZE * GRID_SIZE; c++)
            {
                f << gridCount[c];
                if (c + 1 < GRID_SIZE * GRID_SIZE) f << ",";
                if ((c + 1) % 8 == 0 && c + 1 < GRID_SIZE * GRID_SIZE) f << "\n    ";
            }
            f << "\n};\n";

            if (!gridFaceList.empty())
            {
                f << "static const u16 afn_col_grid_faces[" << gridFaceList.size() << "] = {\n    ";
                for (size_t i = 0; i < gridFaceList.size(); i++)
                {
                    f << gridFaceList[i];
                    if (i + 1 < gridFaceList.size()) f << ",";
                    if ((i + 1) % 16 == 0 && i + 1 < gridFaceList.size()) f << "\n    ";
                }
                f << "\n};\n";
            }
            f << "\n";
        }
    }

    // ---- HUD Element Data (emitted early so script/blueprint functions can reference arrays) ----
    if (!hudElements.empty())
    {
        f << "\n// ---- HUD Elements ----\n";
        f << "#define AFN_HUD_ELEM_COUNT " << (int)hudElements.size() << "\n";

        // Emit element descriptors
        int totalPieces = 0, totalSprites = 0, totalStops = 0, totalText = 0, totalKf = 0;
        for (auto& el : hudElements) { totalPieces += (int)el.pieces.size(); totalSprites += (int)el.sprites.size(); totalStops += (int)el.stops.size(); totalText += (int)el.textRows.size(); totalKf += (int)el.keyframes.size(); }

        f << "static const struct { s16 x,y; u16 pieceStart,pieceCount,spriteStart,spriteCount,stopStart,stopCount,textStart,textCount; s8 curAsset,curFrame,curOffX,curOffY; u8 layerPieces,layerSprites,layerText,layerCursor; u16 kfStart,kfCount; u8 kfLoop; } afn_hud_elems[" << (int)hudElements.size() << "] = {\n";
        int pOff = 0, spOff = 0, sOff = 0, tOff = 0, kfOff = 0;
        for (auto& el : hudElements) {
            f << "    {" << el.screenX << "," << el.screenY << ","
              << pOff << "," << (int)el.pieces.size() << ","
              << spOff << "," << (int)el.sprites.size() << ","
              << sOff << "," << (int)el.stops.size() << ","
              << tOff << "," << (int)el.textRows.size() << ","
              << el.cursorAssetIdx << "," << el.cursorFrame << "," << el.cursorOffX << "," << el.cursorOffY << ","
              << el.layerPieces << "," << el.layerSprites << "," << el.layerText << "," << el.layerCursor << ","
              << kfOff << "," << (int)el.keyframes.size() << "," << (el.animLoop ? 1 : 0) << "},\n";
            pOff += (int)el.pieces.size();
            spOff += (int)el.sprites.size();
            sOff += (int)el.stops.size();
            tOff += (int)el.textRows.size();
            kfOff += (int)el.keyframes.size();
        }
        f << "};\n";

        // Pieces: {assetIdx, frame, localX, localY, size}
        if (totalPieces > 0) {
            f << "static const struct { s8 asset; u8 frame; s16 x,y; u8 size; u8 blackTint; u8 opacity; } afn_hud_pieces[" << totalPieces << "] = {\n";
            for (auto& el : hudElements)
                for (auto& pc : el.pieces)
                    f << "    {" << pc.spriteAssetIdx << "," << pc.frame << "," << pc.localX << "," << pc.localY << "," << pc.size << "," << (pc.blackTint ? 1 : 0) << "," << pc.opacity << "},\n";
            f << "};\n";
        } else {
            f << "static const int afn_hud_pieces[1] = {0}; // no pieces\n";
        }

        // Sprites: {assetIdx, frame, localX, localY, size}
        if (totalSprites > 0) {
            f << "static const struct { s8 asset; u8 frame; s16 x,y; u8 size; } afn_hud_sprites[" << totalSprites << "] = {\n";
            for (auto& el : hudElements)
                for (auto& sp : el.sprites)
                    f << "    {" << sp.spriteAssetIdx << "," << sp.frame << "," << sp.localX << "," << sp.localY << "," << sp.size << "},\n";
            f << "};\n";
        } else {
            f << "static const struct { s8 asset; u8 frame; s16 x,y; u8 size; } afn_hud_sprites[1] = {{0}};\n";
        }

        // Stops: {localX, localY, linkedElement}
        if (totalStops > 0) {
            f << "static const struct { s16 x,y; s8 link; } afn_hud_stops[" << totalStops << "] = {\n";
            for (auto& el : hudElements)
                for (auto& st : el.stops)
                    f << "    {" << st.localX << "," << st.localY << "," << st.linkedElement << "},\n";
            f << "};\n";
        } else {
            f << "static const int afn_hud_stops[1] = {0}; // no stops\n";
        }

        // Text rows: {localX, localY, colorRGB15, text}
        if (totalText > 0) {
            f << "static const struct { s16 x,y; u16 color; char text[32]; } afn_hud_texts[" << totalText << "] = {\n";
            for (auto& el : hudElements)
                for (auto& tr : el.textRows) {
                    f << "    {" << tr.localX << "," << tr.localY << ",0x" << std::hex << tr.colorRGB15 << std::dec << ",\"";
                    // Escape the text
                    for (int ci = 0; tr.text[ci] && ci < 31; ci++) {
                        char c = tr.text[ci];
                        if (c == '"' || c == '\\') f << '\\';
                        f << c;
                    }
                    f << "\"},\n";
                }
            f << "};\n";
        } else {
            f << "static const int afn_hud_texts[1] = {0}; // no text\n";
        }

        // Keyframes: {frame, offX, offY, rot(brad8), scaleX(8.8), scaleY(8.8)}
        if (totalKf > 0) {
            f << "static const struct { u16 frame; s16 offX,offY; s16 rot; u16 scaleX,scaleY; } afn_hud_kf[" << totalKf << "] = {\n";
            for (auto& el : hudElements)
                for (auto& kf : el.keyframes) {
                    // Convert degrees to brads (0-255 = 0-360)
                    int brad = (kf.rot * 256 + 180) / 360;
                    brad &= 0xFF;
                    f << "    {" << kf.frame << "," << kf.offX << "," << kf.offY << "," << brad << "," << kf.scaleX << "," << kf.scaleY << "},\n";
                }
            f << "};\n";
            f << "#define AFN_HUD_HAS_KF 1\n";
        } else {
            f << "static const int afn_hud_kf[1] = {0};\n";
        }

        // Animation layers
        int totalAnimLayers = 0, totalLayerKf = 0, totalLayerItems = 0;
        for (auto& el : hudElements) {
            totalAnimLayers += (int)el.animLayers.size();
            for (auto& lay : el.animLayers) {
                totalLayerKf += (int)lay.keyframes.size();
                totalLayerItems += (int)lay.items.size();
            }
        }
        if (totalAnimLayers > 0) {
            // Layer keyframes (same format as legacy kf)
            f << "static const struct { u16 frame; s16 offX,offY; s16 rot; u16 scaleX,scaleY; } afn_hud_layer_kf[" << totalLayerKf << "] = {\n";
            for (auto& el : hudElements)
                for (auto& lay : el.animLayers)
                    for (auto& kf : lay.keyframes) {
                        int brad = (kf.rot * 256 + 180) / 360; brad &= 0xFF;
                        f << "    {" << kf.frame << "," << kf.offX << "," << kf.offY << "," << brad << "," << kf.scaleX << "," << kf.scaleY << "},\n";
                    }
            f << "};\n";

            // Layer items: {type, index}
            f << "static const struct { u8 type; u8 index; } afn_hud_layer_items[" << totalLayerItems << "] = {\n";
            for (auto& el : hudElements)
                for (auto& lay : el.animLayers)
                    for (auto& it : lay.items)
                        f << "    {" << it.type << "," << it.index << "},\n";
            f << "};\n";

            // Layer metadata: {elemIdx, kfStart, kfCount, itemStart, itemCount, interp, loop, length}
            f << "static const struct { u8 elemIdx; u16 kfStart,kfCount; u16 itemStart,itemCount; u8 interp; u8 loop; u16 length; } afn_hud_layers[" << totalAnimLayers << "] = {\n";
            int lkfOff = 0, liOff = 0;
            for (int ei = 0; ei < (int)hudElements.size(); ei++)
                for (auto& lay : hudElements[ei].animLayers) {
                    f << "    {" << ei << "," << lkfOff << "," << (int)lay.keyframes.size() << ","
                      << liOff << "," << (int)lay.items.size() << "," << lay.interp << "," << (lay.loop ? 1 : 0) << "," << lay.length << "},\n";
                    lkfOff += (int)lay.keyframes.size();
                    liOff += (int)lay.items.size();
                }
            f << "};\n";
            f << "#define AFN_HUD_LAYER_COUNT " << totalAnimLayers << "\n";

            // Runtime state: per-layer animation frame counter + active flag + speed
            f << "static int afn_hud_layer_frame[" << totalAnimLayers << "];\n";
            f << "static u8  afn_hud_layer_active[" << totalAnimLayers << "];\n";
            f << "static u8  afn_hud_layer_speed[" << totalAnimLayers << "] = {";
            { int li2 = 0;
            for (auto& el : hudElements)
                for (auto& lay : el.animLayers) {
                    f << (li2 > 0 ? "," : "") << lay.speed;
                    li2++;
                }
            }
            f << "};\n";
            f << "static u8  afn_hud_layer_tick[" << totalAnimLayers << "];\n";
            f << "#define AFN_HUD_HAS_LAYERS 1\n";
        }
    }

    // ---- Generate script code from visual node graph ----
    // Define AFN_HAS_SCRIPT and emit variable declarations if ANY scripts or blueprints exist
    bool hasAnyScript = !script.nodes.empty() || !blueprints.empty();
    if (hasAnyScript)
    {
        f << "#define AFN_HAS_SCRIPT 1\n\n";

        // Forward declarations for script state variables (defined later in main.c)
        f << "// Script state variables (defined in main.c)\n";
        f << "static FIXED afn_input_fwd;\n";
        f << "static FIXED afn_input_right;\n";
        f << "static FIXED afn_move_speed;\n";
        f << "static int   afn_auto_orbit_speed;\n";
        f << "static int   afn_play_anim;\n";
        f << "static int   afn_pending_scene = -1;\n";
        f << "static int   afn_pending_scene_mode = -1;\n";
        f << "static int   afn_collided_sprite;\n";
        f << "static int   afn_collided_tm_obj = -1;\n";
        f << "static int   afn_bp_cur_tm_obj = -1;\n";
        f << "static int   afn_bp_cur_spr_idx = -1;\n";
        f << "static FIXED afn_gravity;\n";
        f << "static FIXED afn_terminal_vel;\n";
        f << "static FIXED player_vy;\n";
        f << "static int   player_on_ground;\n";
        f << "static u16   orbit_angle;\n";
        f << "extern int player_moving;\n";
        f << "static u32   afn_flags;\n";
        f << "static int   afn_player_frozen;\n";
        f << "static int   tm_player_facing = 4;\n";
        f << "extern int   tm_move_timer;\n";
        f << "static int   afn_anim_speed = 1;\n";
        f << "static u32   afn_rng = 12345;\n";
        f << "static u8    afn_sprite_visible[16];\n";
        f << "static int   afn_shake_intensity;\n";
        f << "static int   afn_shake_frames;\n";
        f << "static int   afn_fade_target;\n";
        f << "static int   afn_fade_frames;\n";
        f << "static int   afn_fade_counter;\n";
        f << "static int   afn_fade_level;\n";
        f << "static int   afn_hp[16];\n";
        f << "static int   afn_score;\n";
        f << "static FIXED afn_start_x, afn_start_y, afn_start_z;\n";
        f << "static int   afn_frame_count;\n";
        f << "static u8    afn_sprite_flip[16];\n";
        f << "static int   afn_draw_distance;\n";
        f << "static u8    afn_collision_enabled[16];\n";
        f << "static int   afn_cam_locked;\n";
        f << "static int   afn_cam_speed = 256;\n";
        f << "static FIXED afn_force_x, afn_force_z;\n";
        f << "static int   afn_friction = 256;\n";
        f << "static int   afn_vars[16];\n";
        f << "static int   afn_scripts_stopped;\n";
        f << "static u8    afn_sprite_layer[16];\n";
        f << "static u8    afn_sprite_alpha[16];\n";
        f << "static u8    afn_flash_obj[16];\n";
        f << "static u16   afn_sprite_rot[16];\n";
        f << "static int   afn_max_hp[16];\n";
        f << "static u8    afn_ai_mode[16];\n";
        f << "static u16   afn_sprite_tint[16];\n";
        f << "static u8    afn_sprite_shake[16];\n";
        f << "static int   afn_hud_value[4];\n";
        f << "static u8    afn_hud_visible[4];\n";
        f << "static int   afn_cursor_stop;\n";
        f << "static int   afn_stop_count;\n";
        f << "static int   afn_stop_links[8];\n";
        f << "static int   afn_elem_idx;\n";
        f << "static int   afn_active_element;\n";
        f << "static FIXED afn_patrol_home_x[16];\n";
        f << "static FIXED afn_patrol_home_z[16];\n";
        f << "static u16   afn_bg_color;\n";
        // Inventory
        f << "static int   afn_inventory[16];\n";
        // Dialogue
        f << "static int   afn_dlg_open;\n";
        f << "static int   afn_dlg_text;\n";
        f << "static int   afn_dlg_line;\n";
        f << "static int   afn_dlg_speaker;\n";
        f << "static int   afn_dlg_choice_a, afn_dlg_choice_b;\n";
        f << "static int   afn_dlg_choosing;\n";
        // State machine
        f << "static int   afn_state[16];\n";
        f << "static int   afn_prev_state[16];\n";
        f << "static int   afn_state_timer[16];\n";
        // Text rendering
        f << "static u16   afn_text_color = 0x7FFF;\n";
        // Collision
        f << "static int   afn_collision_size[16];\n";
        f << "static int   afn_collision_ignore[16];\n";
        // Lifetime / spawning
        f << "static int   afn_lifetime[16];\n";
        // HUD bars
        f << "static u16   afn_bar_color[4];\n";
        f << "static int   afn_bar_max[4];\n";
        f << "static int   afn_timer_visible;\n";
        // Checkpoint
        f << "static FIXED afn_checkpoint_x, afn_checkpoint_z;\n";
        f << "static int   afn_checkpoint_set;\n";
        // Input
        f << "static int   afn_last_key;\n";
        f << "static int   afn_current_scene;\n\n";
        // Clone sprite stub
        f << "static inline void afn_clone_sprite(int src) { (void)src; }\n";
        // Emit particle stub
        f << "static inline void afn_emit_particle(int type, FIXED x, FIXED z) { (void)type; (void)x; (void)z; }\n";
        // Text rendering stubs
        f << "static inline void afn_draw_number(int val, int x, int y) { (void)val; (void)x; (void)y; }\n";
        f << "static inline void afn_draw_text(int id, int x, int y) { (void)id; (void)x; (void)y; }\n";
        f << "static inline void afn_clear_text(void) {}\n";
        // mGBA debug log stub
        f << "#ifndef mgba_printf\n";
        f << "#define mgba_printf(...) ((void)0)\n";
        f << "#endif\n\n";
        // SRAM helpers
        f << "static inline void afn_sram_save(void) {\n";
        f << "    volatile u8* sram = (volatile u8*)0x0E000000;\n";
        f << "    sram[0] = 'A'; sram[1] = 'F'; // magic\n";
        f << "    int i; for (i = 0; i < 4; i++) sram[2+i] = (afn_flags >> (i*8)) & 0xFF;\n";
        f << "    for (i = 0; i < 4; i++) sram[6+i] = (afn_score >> (i*8)) & 0xFF;\n";
        f << "}\n";
        f << "static inline void afn_sram_load(void) {\n";
        f << "    volatile u8* sram = (volatile u8*)0x0E000000;\n";
        f << "    if (sram[0] != 'A' || sram[1] != 'F') return;\n";
        f << "    afn_flags = sram[2] | (sram[3]<<8) | (sram[4]<<16) | (sram[5]<<24);\n";
        f << "    afn_score = sram[6] | (sram[7]<<8) | (sram[8]<<16) | (sram[9]<<24);\n";
        f << "}\n";
        f << "static inline void afn_spawn_effect(int id, FIXED x, FIXED z) { (void)id; (void)x; (void)z; }\n";
        f << "static inline void afn_spawn_sprite(int asset, FIXED x, FIXED z) { (void)asset; (void)x; (void)z; }\n";
        f << "static inline void afn_spawn_projectile(int obj, int asset, int speed) { (void)obj; (void)asset; (void)speed; }\n";
        f << "static inline void afn_draw_bar(int x, int y, int w, int fill) { (void)x; (void)y; (void)w; (void)fill; }\n";
        f << "static inline void afn_draw_sprite_icon(int asset, int x, int y) { (void)asset; (void)x; (void)y; }\n\n";
    }
    // Helper: get suffix string for an action node type
    auto actionSuffix = [](GBAScriptNodeType t) -> const char* {
        switch (t) {
        case GBAScriptNodeType::MovePlayer:    return "_move";
        case GBAScriptNodeType::Jump:          return "_jump";
        case GBAScriptNodeType::Walk:          return "_walk";
        case GBAScriptNodeType::Sprint:        return "_sprint";
        case GBAScriptNodeType::OrbitCamera:   return "_orbit";
        case GBAScriptNodeType::PlayAnim:      return "_play_anim";
        case GBAScriptNodeType::SetGravity:    return "_set_gravity";
        case GBAScriptNodeType::SetMaxFall:    return "_set_max_fall";
        case GBAScriptNodeType::DestroyObject: return "_destroy";
        case GBAScriptNodeType::AutoOrbit:     return "_auto_orbit";
        case GBAScriptNodeType::DampenJump:    return "_dampen_jump";
        case GBAScriptNodeType::IsMoving:      return "_is_moving";
        case GBAScriptNodeType::IsOnGround:    return "_is_grounded";
        case GBAScriptNodeType::IsJumping:     return "_is_jumping";
        case GBAScriptNodeType::CheckFlag:     return "_check_flag";
        case GBAScriptNodeType::SetFlag:       return "_set_flag";
        case GBAScriptNodeType::ToggleFlag:    return "_toggle_flag";
        case GBAScriptNodeType::FreezePlayer:  return "_freeze";
        case GBAScriptNodeType::UnfreezePlayer:return "_unfreeze";
        case GBAScriptNodeType::SetCameraHeight:return "_set_cam_h";
        case GBAScriptNodeType::SetHorizon:    return "_set_horizon";
        case GBAScriptNodeType::Teleport:      return "_teleport";
        case GBAScriptNodeType::SetVisible:    return "_set_visible";
        case GBAScriptNodeType::SetPosition:   return "_set_pos";
        case GBAScriptNodeType::StopAnim:      return "_stop_anim";
        case GBAScriptNodeType::SetAnimSpeed:  return "_set_anim_speed";
        case GBAScriptNodeType::SetVelocityY:  return "_set_vel_y";
        case GBAScriptNodeType::StopSound:     return "_stop_sound";
        case GBAScriptNodeType::SetScale:      return "_set_scale";
        case GBAScriptNodeType::ScreenShake:   return "_shake";
        case GBAScriptNodeType::FadeOut:       return "_fade_out";
        case GBAScriptNodeType::FadeIn:        return "_fade_in";
        case GBAScriptNodeType::MoveToward:    return "_move_toward";
        case GBAScriptNodeType::LookAt:        return "_look_at";
        case GBAScriptNodeType::SetSpriteAnim: return "_set_sprite_anim";
        case GBAScriptNodeType::SpawnEffect:   return "_spawn_effect";
        case GBAScriptNodeType::DoOnce:        return "_do_once";
        case GBAScriptNodeType::FlipFlop:      return "_flip_flop";
        case GBAScriptNodeType::Gate:          return "_gate";
        case GBAScriptNodeType::ForLoop:       return "_for_loop";
        case GBAScriptNodeType::Sequence:      return "_sequence";
        case GBAScriptNodeType::SetHP:         return "_set_hp";
        case GBAScriptNodeType::DamageHP:      return "_damage_hp";
        case GBAScriptNodeType::AddScore:      return "_add_score";
        case GBAScriptNodeType::Respawn:       return "_respawn";
        case GBAScriptNodeType::SaveData:      return "_save_data";
        case GBAScriptNodeType::LoadData:      return "_load_data";
        case GBAScriptNodeType::FlipSprite:    return "_flip_sprite";
        case GBAScriptNodeType::SetDrawDist:   return "_set_draw_dist";
        case GBAScriptNodeType::EnableCollision:return "_enable_collision";
        case GBAScriptNodeType::ChangeScene:   return "_change_scene";
        case GBAScriptNodeType::CustomCode:    return "_custom";
        case GBAScriptNodeType::Countdown:     return "_countdown";
        case GBAScriptNodeType::ResetTimer:    return "_reset_timer";
        case GBAScriptNodeType::Increment:     return "_increment";
        case GBAScriptNodeType::Decrement:     return "_decrement";
        case GBAScriptNodeType::SetFOV:        return "_set_fov";
        case GBAScriptNodeType::ShakeStop:     return "_shake_stop";
        case GBAScriptNodeType::LockCamera:    return "_lock_cam";
        case GBAScriptNodeType::UnlockCamera:  return "_unlock_cam";
        case GBAScriptNodeType::SetCamSpeed:   return "_set_cam_speed";
        case GBAScriptNodeType::ApplyForce:    return "_apply_force";
        case GBAScriptNodeType::Bounce:        return "_bounce";
        case GBAScriptNodeType::SetFriction:   return "_set_friction";
        case GBAScriptNodeType::CloneSprite:   return "_clone";
        case GBAScriptNodeType::HideAll:       return "_hide_all";
        case GBAScriptNodeType::ShowAll:       return "_show_all";
        case GBAScriptNodeType::IsFlagSet:     return "_is_flag_set";
        case GBAScriptNodeType::IsHPZero:      return "_is_hp_zero";
        case GBAScriptNodeType::IsNear:        return "_is_near";
        case GBAScriptNodeType::Print:         return "_print";
        case GBAScriptNodeType::SetColor:      return "_set_color";
        case GBAScriptNodeType::SwapSprite:    return "_swap_sprite";
        case GBAScriptNodeType::SetSpriteY:    return "_set_sprite_y";
        case GBAScriptNodeType::WaitUntil:     return "_wait_until";
        case GBAScriptNodeType::RepeatWhile:   return "_repeat_while";
        case GBAScriptNodeType::StopAll:       return "_stop_all";
        case GBAScriptNodeType::SetLayer:      return "_set_layer";
        case GBAScriptNodeType::SetAlpha:      return "_set_alpha";
        case GBAScriptNodeType::Flash:         return "_flash";
        case GBAScriptNodeType::Delay:         return "_delay";
        case GBAScriptNodeType::SetSpriteScale:return "_set_spr_scale";
        case GBAScriptNodeType::RotateSprite:  return "_rotate_sprite";
        case GBAScriptNodeType::SetHP2:        return "_set_hp_clamped";
        case GBAScriptNodeType::HealHP:        return "_heal_hp";
        case GBAScriptNodeType::SetMaxHP:      return "_set_max_hp";
        case GBAScriptNodeType::IsAlive:       return "_is_alive_gate";
        case GBAScriptNodeType::SetBGColor:    return "_set_bg_color";
        case GBAScriptNodeType::FacePlayer:    return "_face_player";
        case GBAScriptNodeType::MoveForward:   return "_move_fwd";
        case GBAScriptNodeType::Patrol:        return "_patrol";
        case GBAScriptNodeType::ChasePlayer:   return "_chase_player";
        case GBAScriptNodeType::FollowPlayer:  return "_follow_player";
        case GBAScriptNodeType::IsNear2D:      return "_is_near_2d";
        case GBAScriptNodeType::IsFollowMoving: return "_is_fol_moving";
        case GBAScriptNodeType::SetFollowFacing: return "_set_fol_facing";
        case GBAScriptNodeType::FleePlayer:    return "_flee_player";
        case GBAScriptNodeType::SetAI:         return "_set_ai";
        case GBAScriptNodeType::EmitParticle:  return "_emit_particle";
        case GBAScriptNodeType::SetTint:       return "_set_tint";
        case GBAScriptNodeType::Shake:         return "_shake_sprite";
        case GBAScriptNodeType::SetText:       return "_set_text";
        case GBAScriptNodeType::ShowHUD:       return "_show_hud";
        case GBAScriptNodeType::HideHUD:       return "_hide_hud";
        case GBAScriptNodeType::ArraySet:      return "_array_set";
        case GBAScriptNodeType::DrawNumber:    return "_draw_number";
        case GBAScriptNodeType::DrawTextID:    return "_draw_text";
        case GBAScriptNodeType::ClearText:     return "_clear_text";
        case GBAScriptNodeType::SetTextColor:  return "_set_text_color";
        case GBAScriptNodeType::AddItem:       return "_add_item";
        case GBAScriptNodeType::RemoveItem:    return "_remove_item";
        case GBAScriptNodeType::SetItemCount:  return "_set_item_count";
        case GBAScriptNodeType::UseItem:       return "_use_item";
        case GBAScriptNodeType::ShowDialogue:  return "_show_dialogue";
        case GBAScriptNodeType::HideDialogue:  return "_hide_dialogue";
        case GBAScriptNodeType::NextLine:      return "_next_line";
        case GBAScriptNodeType::SetSpeaker:    return "_set_speaker";
        case GBAScriptNodeType::SetState:      return "_set_state";
        case GBAScriptNodeType::TransitionState:return "_transition";
        case GBAScriptNodeType::IsInState:     return "_is_in_state";
        case GBAScriptNodeType::HasItem:       return "_has_item";
        case GBAScriptNodeType::IsDialogueOpen:return "_is_dlg_open";
        case GBAScriptNodeType::SetCollisionSize:return "_set_col_size";
        case GBAScriptNodeType::IgnoreCollision:return "_ignore_col";
        case GBAScriptNodeType::IsColliding:   return "_is_colliding";
        case GBAScriptNodeType::SpawnAt:       return "_spawn_at";
        case GBAScriptNodeType::DestroyAfter:  return "_destroy_after";
        case GBAScriptNodeType::SpawnProjectile:return "_spawn_proj";
        case GBAScriptNodeType::SetLifetime:   return "_set_lifetime";
        case GBAScriptNodeType::DrawBar:       return "_draw_bar";
        case GBAScriptNodeType::DrawSpriteIcon:return "_draw_icon";
        case GBAScriptNodeType::ShowTimer:     return "_show_timer";
        case GBAScriptNodeType::HideTimer:     return "_hide_timer";
        case GBAScriptNodeType::SetBarColor:   return "_set_bar_color";
        case GBAScriptNodeType::SetBarMax:     return "_set_bar_max";
        case GBAScriptNodeType::ReloadScene:   return "_reload_scene";
        case GBAScriptNodeType::SetCheckpoint: return "_set_checkpoint";
        case GBAScriptNodeType::LoadCheckpoint:return "_load_checkpoint";
        case GBAScriptNodeType::CursorUp:      return "_cursor_up";
        case GBAScriptNodeType::CursorDown:    return "_cursor_down";
        case GBAScriptNodeType::FollowLink:    return "_follow_link";
        case GBAScriptNodeType::PlayHudAnim:   return "_play_hud_anim";
        case GBAScriptNodeType::StopHudAnim:   return "_stop_hud_anim";
        case GBAScriptNodeType::SetHudAnimSpeed: return "_set_hud_anim_speed";
        default: return "";
        }
    };

    // Per-blueprint-definition frozen flags (must precede script/blueprint functions)
    if (!blueprints.empty())
        f << "static int afn_bp_def_frozen[" << (int)blueprints.size() << "];\n\n";
    else
        f << "static int afn_bp_def_frozen[1];\n\n";

    if (!script.nodes.empty())
    {
        // Helper: find node by id
        auto findNode = [&](int id) -> const GBAScriptNodeExport* {
            for (auto& n : script.nodes)
                if (n.id == id) return &n;
            return nullptr;
        };

        // Helper: find ALL exec links from a node's exec out pin (pinIdx)
        auto findExecOuts = [&](int nodeId, int pinIdx) -> std::vector<int> {
            std::vector<int> targets;
            for (auto& l : script.links)
                if (l.fromNodeId == nodeId && l.fromPinType == 0 && l.fromPinIdx == pinIdx)
                    targets.push_back(l.toNodeId);
            return targets;
        };

        // Helper: find data node connected to a node's data input pin
        auto findDataIn = [&](int nodeId, int pinIdx) -> const GBAScriptNodeExport* {
            for (auto& l : script.links)
                if (l.toNodeId == nodeId && l.toPinType == 3 && l.toPinIdx == pinIdx)
                    return findNode(l.fromNodeId);
            return nullptr;
        };

        // Helper: resolve integer value from a data node
        auto resolveInt = [&](const GBAScriptNodeExport* dn) -> int {
            if (!dn) return 0;
            if (dn->type == GBAScriptNodeType::Animation) return dn->paramInt[1];
            return dn->paramInt[0];
        };

        // Helper: resolve integer as C expression string (handles CustomCode data-out)
        auto resolveIntExpr = [&](const GBAScriptNodeExport* dn) -> std::string {
            if (!dn) return "0";
            if (dn->type == GBAScriptNodeType::CustomCode) {
                char buf[64];
                if (dn->funcName[0])
                    snprintf(buf, sizeof(buf), "%s()", dn->funcName);
                else
                    snprintf(buf, sizeof(buf), "afn_script_custom_%d()", dn->id);
                return buf;
            }
            if (dn->type == GBAScriptNodeType::Animation) return std::to_string(dn->paramInt[1]);
            return std::to_string(dn->paramInt[0]);
        };

        // Helper: resolve float from a data node (stored as IEEE754 bits in paramInt[0])
        auto resolveFloat = [&](const GBAScriptNodeExport* dn) -> float {
            if (!dn) return 0.0f;
            float fv;
            memcpy(&fv, &dn->paramInt[0], sizeof(float));
            return fv;
        };

        // Helper: resolve key name from a data node or event node's key param
        auto resolveKeyName = [&](int keyIdx) -> const char* {
            static const char* keys[] = { "KEY_A", "KEY_B", "KEY_L", "KEY_R",
                                           "KEY_START", "KEY_SELECT",
                                           "KEY_UP", "KEY_DOWN", "KEY_LEFT", "KEY_RIGHT" };
            if (keyIdx >= 0 && keyIdx < 10) return keys[keyIdx];
            return "KEY_A";
        };

        // Helper: resolve key index from event node (check data input first, then paramInt)
        // Returns -1 if ambiguous (multiple keys connected) — caller should not wrap in key check
        auto resolveEventKey = [&](const GBAScriptNodeExport& evNode) -> int {
            int count = 0;
            int keyVal = -1;
            for (auto& l : script.links)
                if (l.toNodeId == evNode.id && l.toPinType == 3 && l.toPinIdx == 0)
                {
                    auto* dn = findNode(l.fromNodeId);
                    if (dn) { keyVal = dn->paramInt[0]; count++; }
                }
            if (count == 1) return keyVal;
            if (count == 0) return evNode.paramInt[0];
            return -1; // ambiguous — multiple keys connected
        };

        // Collect event chains: for each event node, walk exec links (BFS) to gather all actions
        struct EventChain {
            const GBAScriptNodeExport* event;
            std::vector<const GBAScriptNodeExport*> actions;
        };
        std::vector<EventChain> chains;

        for (auto& n : script.nodes)
        {
            if (n.type != GBAScriptNodeType::OnKeyPressed &&
                n.type != GBAScriptNodeType::OnKeyReleased &&
                n.type != GBAScriptNodeType::OnKeyHeld &&
                n.type != GBAScriptNodeType::OnStart &&
                n.type != GBAScriptNodeType::OnUpdate &&
                n.type != GBAScriptNodeType::OnCollision &&
                n.type != GBAScriptNodeType::OnCollision2D)
                continue;

            EventChain chain;
            chain.event = &n;

            // BFS from this event node, following all exec links
            std::vector<int> frontier = findExecOuts(n.id, 0);
            std::vector<bool> visited(10000, false);
            int safety = 0;
            while (!frontier.empty() && safety < 256)
            {
                int nid = frontier.front();
                frontier.erase(frontier.begin());
                if (nid < 0 || nid >= (int)visited.size() || visited[nid]) continue;
                visited[nid] = true;
                safety++;

                auto* an = findNode(nid);
                if (!an) continue;
                chain.actions.push_back(an);

                // Follow this node's exec outputs (all pins for CustomCode, pin 0 for others)
                if (an->type == GBAScriptNodeType::CustomCode) {
                    for (int ep = 0; ep < 8; ep++) {
                        auto next = findExecOuts(an->id, ep);
                        for (int t : next) frontier.push_back(t);
                    }
                } else {
                    auto next = findExecOuts(an->id, 0);
                    for (int t : next) frontier.push_back(t);
                }
            }
            if (!chain.actions.empty())
                chains.push_back(chain);
        }

        if (!chains.empty())
        {
            f << "// ---- Generated script code from visual node graph ----\n\n";

            // Emit action body lines for a single action node
            // Helper: replace $0..$7 in a string with resolved data-in values
            auto resolveCustomPlaceholders = [&](const std::string& src, int nodeId) -> std::string {
                std::string code = src;
                for (int pi = 7; pi >= 0; pi--) {
                    char placeholder[4]; snprintf(placeholder, sizeof(placeholder), "$%d", pi);
                    size_t pos = 0;
                    while ((pos = code.find(placeholder, pos)) != std::string::npos) {
                        auto* din = findDataIn(nodeId, pi);
                        std::string valStr = din ? resolveIntExpr(din) : "0";
                        code.replace(pos, strlen(placeholder), valStr);
                        pos += valStr.size();
                    }
                }
                return code;
            };

            auto emitActionBody = [&](const GBAScriptNodeExport* action) {
                // Use custom code override if set
                if (action->customCode[0]) {
                    // Emit per-pin code blocks first
                    for (int pi = 0; pi < action->ccPinCount && pi < 8; pi++) {
                        if (action->ccPinCode[pi][0]) {
                            std::string pinCode = resolveCustomPlaceholders(action->ccPinCode[pi], action->id);
                            f << "    " << pinCode << "\n";
                        }
                    }
                    // Emit main custom code with placeholders resolved
                    std::string code = resolveCustomPlaceholders(action->customCode, action->id);
                    f << "    " << code << "\n";
                    return;
                }
                switch (action->type)
                {
                case GBAScriptNodeType::MovePlayer: {
                    auto* dirData = findDataIn(action->id, 0);
                    int dir = dirData ? dirData->paramInt[0] : 0;
                    // 0=Left, 1=Right, 2=Up, 3=Down
                    const char* dirKeys[] = { "KEY_LEFT", "KEY_RIGHT", "KEY_UP", "KEY_DOWN" };
                    const char* dirVars[] = { "afn_input_right -= 256", "afn_input_right += 256",
                                              "afn_input_fwd += 256", "afn_input_fwd -= 256" };
                    // facing: Left=6(W), Right=2(E), Up=0(N), Down=4(S)
                    const int dirFacing[] = { 6, 2, 0, 4 };
                    if (dir >= 0 && dir < 4)
                        f << "    if (!afn_player_frozen && key_is_down(" << dirKeys[dir] << ")) { " << dirVars[dir] << "; if (tm_move_timer == 0) tm_player_facing = " << dirFacing[dir] << "; }\n";
                    break;
                }
                case GBAScriptNodeType::Jump: {
                    auto* forceData = findDataIn(action->id, 0);
                    float force = forceData ? resolveFloat(forceData) : 2.0f;
                    int forceFixed = (int)(force * 256.0f);
                    f << "    if (player_on_ground) player_vy = " << forceFixed << ";\n";
                    break;
                }
                case GBAScriptNodeType::Walk: {
                    auto* speedData = findDataIn(action->id, 0);
                    if (speedData) {
                        int speed = resolveInt(speedData);
                        int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                        f << "    afn_move_speed = " << gbaSpeed << ";\n";
                    }
                    break;
                }
                case GBAScriptNodeType::Sprint: {
                    auto* speedData = findDataIn(action->id, 0);
                    if (speedData) {
                        int speed = resolveInt(speedData);
                        int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                        f << "    afn_move_speed = " << gbaSpeed << ";\n";
                    }
                    break;
                }
                case GBAScriptNodeType::OrbitCamera: {
                    auto* dirData = findDataIn(action->id, 0);
                    auto* speedData = findDataIn(action->id, 1);
                    int dir = dirData ? dirData->paramInt[0] : 1;
                    int speed = speedData ? resolveInt(speedData) : 512;
                    // Map direction to L/R shoulder key check
                    const char* key = (dir == 0) ? "KEY_L" : "KEY_R";
                    const char* sign = (dir == 0) ? "-" : "+";
                    f << "    if (key_is_down(" << key << ")) orbit_angle " << sign << "= " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::DampenJump: {
                    auto* factorData = findDataIn(action->id, 0);
                    float factor = factorData ? resolveFloat(factorData) : 0.75f;
                    int factorFixed = (int)(factor * 256.0f);
                    f << "    if (player_vy > 0) player_vy = (player_vy * " << factorFixed << ") >> 8;\n";
                    break;
                }
                case GBAScriptNodeType::AutoOrbit: {
                    auto* speedData = findDataIn(action->id, 0);
                    int speed = speedData ? resolveInt(speedData) : 205;
                    f << "    afn_auto_orbit_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetGravity: {
                    auto* valData = findDataIn(action->id, 0);
                    float val = valData ? resolveFloat(valData) : 0.09f;
                    int valFixed = (int)(val * 256.0f);
                    f << "    afn_gravity = " << valFixed << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetMaxFall: {
                    auto* valData = findDataIn(action->id, 0);
                    float val = valData ? resolveFloat(valData) : 6.0f;
                    int valFixed = (int)(val * 256.0f);
                    f << "    afn_terminal_vel = " << valFixed << ";\n";
                    break;
                }
                case GBAScriptNodeType::PlayAnim: {
                    auto* animData = findDataIn(action->id, 0);
                    // Animation node: paramInt[0]=sprite asset, paramInt[1]=anim index
                    int animIdx = animData ? animData->paramInt[1] : 0;
                    f << "    afn_play_anim = " << animIdx << ";\n";
                    break;
                }
                case GBAScriptNodeType::ChangeScene: {
                    auto* scData = findDataIn(action->id, 0);
                    int scIdx = scData ? resolveInt(scData) : action->paramInt[0];
                    int scMode = action->paramInt[1]; // 0=3D, 1=Tilemap
                    f << "    afn_pending_scene = " << scIdx << ";\n";
                    f << "    afn_pending_scene_mode = " << scMode << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetFlag: {
                    auto* flagData = findDataIn(action->id, 0);
                    auto* valData = findDataIn(action->id, 1);
                    int flag = flagData ? resolveInt(flagData) : 0;
                    int val = valData ? resolveInt(valData) : 1;
                    f << "    if (" << val << ") afn_flags |= (1u << " << flag << ");\n";
                    f << "    else afn_flags &= ~(1u << " << flag << ");\n";
                    break;
                }
                case GBAScriptNodeType::ToggleFlag: {
                    auto* flagData = findDataIn(action->id, 0);
                    int flag = flagData ? resolveInt(flagData) : 0;
                    f << "    afn_flags ^= (1u << " << flag << ");\n";
                    break;
                }
                case GBAScriptNodeType::FreezePlayer: {
                    auto* bpData = findDataIn(action->id, 0);
                    f << "    afn_player_frozen = 1;\n";
                    f << "    afn_play_anim = -1;\n";
                    f << "    afn_move_speed = 0;\n";
                    if (bpData) {
                        int bpIdx = resolveInt(bpData);
                        f << "    afn_bp_def_frozen[" << bpIdx << "] = 1;\n";
                    }
                    break;
                }
                case GBAScriptNodeType::UnfreezePlayer: {
                    auto* bpData = findDataIn(action->id, 0);
                    f << "    afn_player_frozen = 0;\n";
                    f << "    afn_play_anim = 0;\n";
                    if (bpData) {
                        int bpIdx = resolveInt(bpData);
                        f << "    afn_bp_def_frozen[" << bpIdx << "] = 0;\n";
                    }
                    break;
                }
                case GBAScriptNodeType::SetCameraHeight: {
                    auto* hData = findDataIn(action->id, 0);
                    int h = hData ? resolveInt(hData) : 64;
                    f << "    cam_h = " << h << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetHorizon: {
                    auto* sData = findDataIn(action->id, 0);
                    int s = sData ? resolveInt(sData) : 60;
                    f << "    m7_horizon = " << s << ";\n";
                    break;
                }
                case GBAScriptNodeType::Teleport: {
                    auto* xData = findDataIn(action->id, 0);
                    auto* yData = findDataIn(action->id, 1);
                    auto* zData = findDataIn(action->id, 2);
                    int x = xData ? resolveInt(xData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    int z = zData ? resolveInt(zData) : 0;
                    f << "    player_x = " << x << " << 8;\n";
                    f << "    player_y = " << y << " << 8;\n";
                    f << "    player_z = " << z << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetVisible: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* visData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int vis = visData ? resolveInt(visData) : 1;
                    f << "    afn_sprite_visible[" << obj << "] = " << vis << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetPosition: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* zData = findDataIn(action->id, 2);
                    int obj = objData ? resolveInt(objData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int z = zData ? resolveInt(zData) : 0;
                    f << "    g_sprites[" << obj << "].wx = " << x << " << 8;\n";
                    f << "    g_sprites[" << obj << "].wz = " << z << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::StopAnim:
                    f << "    afn_play_anim = -1;\n";
                    break;
                case GBAScriptNodeType::SetAnimSpeed: {
                    auto* sData = findDataIn(action->id, 0);
                    int speed = sData ? resolveInt(sData) : 1;
                    f << "    afn_anim_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetVelocityY: {
                    auto* vData = findDataIn(action->id, 0);
                    float vel = vData ? resolveFloat(vData) : 0.0f;
                    int velFixed = (int)(vel * 256.0f);
                    f << "    player_vy = " << velFixed << ";\n";
                    break;
                }
                case GBAScriptNodeType::PlaySound: {
                    auto* sndData = findDataIn(action->id, 0);
                    int sndId = sndData ? resolveInt(sndData) : 0;
                    f << "    afn_play_sound(" << sndId << ");\n";
                    break;
                }
                case GBAScriptNodeType::StopSound:
                    f << "    afn_stop_sound();\n";
                    break;
                case GBAScriptNodeType::SetScale: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* scaleData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    float scale = scaleData ? resolveFloat(scaleData) : 1.0f;
                    int scaleFixed = (int)(scale * 256.0f);
                    f << "    g_sprites[" << obj << "].scale = " << scaleFixed << ";\n";
                    break;
                }
                case GBAScriptNodeType::ScreenShake: {
                    auto* intData = findDataIn(action->id, 0);
                    auto* frData = findDataIn(action->id, 1);
                    int intensity = intData ? resolveInt(intData) : 3;
                    int frames = frData ? resolveInt(frData) : 10;
                    f << "    afn_shake_intensity = " << intensity << ";\n";
                    f << "    afn_shake_frames = " << frames << ";\n";
                    break;
                }
                case GBAScriptNodeType::FadeOut: {
                    auto* frData = findDataIn(action->id, 0);
                    int frames = frData ? resolveInt(frData) : 30;
                    f << "    afn_fade_target = 16;\n";
                    f << "    afn_fade_frames = " << frames << ";\n";
                    f << "    afn_fade_counter = 0;\n";
                    break;
                }
                case GBAScriptNodeType::FadeIn: {
                    auto* frData = findDataIn(action->id, 0);
                    int frames = frData ? resolveInt(frData) : 30;
                    f << "    afn_fade_target = 0;\n";
                    f << "    afn_fade_frames = " << frames << ";\n";
                    f << "    afn_fade_counter = 0;\n";
                    break;
                }
                case GBAScriptNodeType::MoveToward: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* tgtData = findDataIn(action->id, 1);
                    auto* spdData = findDataIn(action->id, 2);
                    int obj = objData ? resolveInt(objData) : 0;
                    int tgt = tgtData ? resolveInt(tgtData) : 0;
                    int spd = spdData ? resolveInt(spdData) : 64;
                    f << "    { FIXED dx = g_sprites[" << tgt << "].wx - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = g_sprites[" << tgt << "].wz - g_sprites[" << obj << "].wz;\n";
                    f << "      if (dx > " << spd << ") g_sprites[" << obj << "].wx += " << spd << ";\n";
                    f << "      else if (dx < -" << spd << ") g_sprites[" << obj << "].wx -= " << spd << ";\n";
                    f << "      else g_sprites[" << obj << "].wx = g_sprites[" << tgt << "].wx;\n";
                    f << "      if (dz > " << spd << ") g_sprites[" << obj << "].wz += " << spd << ";\n";
                    f << "      else if (dz < -" << spd << ") g_sprites[" << obj << "].wz -= " << spd << ";\n";
                    f << "      else g_sprites[" << obj << "].wz = g_sprites[" << tgt << "].wz; }\n";
                    break;
                }
                case GBAScriptNodeType::LookAt: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* tgtData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int tgt = tgtData ? resolveInt(tgtData) : 0;
                    f << "    { FIXED dx = g_sprites[" << tgt << "].wx - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = g_sprites[" << tgt << "].wz - g_sprites[" << obj << "].wz;\n";
                    f << "      g_sprites[" << obj << "].facing = ArcTan2(dx, dz); }\n";
                    break;
                }
                case GBAScriptNodeType::SetSpriteAnim: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* animData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int anim = animData ? resolveInt(animData) : 0;
                    f << "    if (afn_current_mode == 1) { tm_obj_anim_idx[" << obj << "] = " << anim << "; tm_obj_anim_play[" << obj << "] = 1; }\n";
                    f << "    else { afn_play_anim = " << anim << "; }\n";
                    break;
                }
                case GBAScriptNodeType::SpawnEffect: {
                    auto* effData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* zData = findDataIn(action->id, 2);
                    int eff = effData ? resolveInt(effData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int z = zData ? resolveInt(zData) : 0;
                    f << "    afn_spawn_effect(" << eff << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::DoOnce:
                    f << "    { static int afn_done_" << action->id << " = 0;\n";
                    f << "      if (afn_done_" << action->id << ") return;\n";
                    f << "      afn_done_" << action->id << " = 1; }\n";
                    break;
                case GBAScriptNodeType::FlipFlop:
                    f << "    // FlipFlop handled at call site\n";
                    break;
                case GBAScriptNodeType::Gate: {
                    auto* openData = findDataIn(action->id, 0);
                    int open = openData ? resolveInt(openData) : 1;
                    f << "    if (!" << open << ") return;\n";
                    break;
                }
                case GBAScriptNodeType::ForLoop: {
                    auto* countData = findDataIn(action->id, 0);
                    int count = countData ? resolveInt(countData) : 1;
                    f << "    // ForLoop: repeats downstream " << count << " times\n";
                    break;
                }
                case GBAScriptNodeType::Sequence:
                    f << "    // Sequence: fires Then 0 then Then 1\n";
                    break;
                case GBAScriptNodeType::SetHP: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* hpData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int hp = hpData ? resolveInt(hpData) : 100;
                    f << "    afn_hp[" << obj << "] = " << hp << ";\n";
                    break;
                }
                case GBAScriptNodeType::DamageHP: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* amtData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int amt = amtData ? resolveInt(amtData) : 1;
                    f << "    afn_hp[" << obj << "] -= " << amt << ";\n";
                    f << "    if (afn_hp[" << obj << "] < 0) afn_hp[" << obj << "] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::AddScore: {
                    auto* amtData = findDataIn(action->id, 0);
                    int amt = amtData ? resolveInt(amtData) : 1;
                    f << "    afn_score += " << amt << ";\n";
                    break;
                }
                case GBAScriptNodeType::Respawn:
                    f << "    player_x = afn_start_x; player_y = afn_start_y; player_z = afn_start_z;\n";
                    f << "    player_vy = 0;\n";
                    break;
                case GBAScriptNodeType::SaveData:
                    f << "    afn_sram_save();\n";
                    break;
                case GBAScriptNodeType::LoadData:
                    f << "    afn_sram_load();\n";
                    break;
                case GBAScriptNodeType::FlipSprite: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* flipData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int flip = flipData ? resolveInt(flipData) : 1;
                    f << "    afn_sprite_flip[" << obj << "] = " << flip << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetDrawDist: {
                    auto* distData = findDataIn(action->id, 0);
                    int dist = distData ? resolveInt(distData) : 0;
                    f << "    afn_draw_distance = " << dist << ";\n";
                    break;
                }
                case GBAScriptNodeType::EnableCollision: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* enData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int en = enData ? resolveInt(enData) : 1;
                    f << "    afn_collision_enabled[" << obj << "] = " << en << ";\n";
                    break;
                }
                case GBAScriptNodeType::CustomCode:
                    break; // handled by customCode[0] check above
                case GBAScriptNodeType::Countdown: {
                    auto* cntData = findDataIn(action->id, 0);
                    int cnt = cntData ? resolveInt(cntData) : 60;
                    f << "    { static int afn_cd_" << action->id << " = " << cnt << ";\n";
                    f << "      if (--afn_cd_" << action->id << " <= 0) { afn_cd_" << action->id << " = " << cnt << ";\n";
                    break;
                }
                case GBAScriptNodeType::ResetTimer:
                    f << "    // reset countdown timers (handled by reinit)\n";
                    break;
                case GBAScriptNodeType::Increment: {
                    auto* slotData = findDataIn(action->id, 0);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    f << "    afn_vars[" << slot << "]++;\n";
                    break;
                }
                case GBAScriptNodeType::Decrement: {
                    auto* slotData = findDataIn(action->id, 0);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    f << "    afn_vars[" << slot << "]--;\n";
                    break;
                }
                case GBAScriptNodeType::SetFOV: {
                    auto* fovData = findDataIn(action->id, 0);
                    int fov = fovData ? resolveInt(fovData) : 256;
                    f << "    cam_fov = " << fov << ";\n";
                    break;
                }
                case GBAScriptNodeType::ShakeStop:
                    f << "    afn_shake_frames = 0;\n";
                    f << "    REG_BG_OFS[2].x = 0; REG_BG_OFS[2].y = 0;\n";
                    break;
                case GBAScriptNodeType::LockCamera:
                    f << "    afn_cam_locked = 1;\n";
                    break;
                case GBAScriptNodeType::UnlockCamera:
                    f << "    afn_cam_locked = 0;\n";
                    break;
                case GBAScriptNodeType::SetCamSpeed: {
                    auto* spdData = findDataIn(action->id, 0);
                    int spd = spdData ? resolveInt(spdData) : 256;
                    f << "    afn_cam_speed = " << spd << ";\n";
                    break;
                }
                case GBAScriptNodeType::ApplyForce: {
                    auto* fxData = findDataIn(action->id, 0);
                    auto* fzData = findDataIn(action->id, 1);
                    int fx = fxData ? resolveInt(fxData) : 0;
                    int fz = fzData ? resolveInt(fzData) : 0;
                    f << "    afn_force_x += " << fx << ";\n";
                    f << "    afn_force_z += " << fz << ";\n";
                    break;
                }
                case GBAScriptNodeType::Bounce: {
                    auto* dampData = findDataIn(action->id, 0);
                    int damp = dampData ? (int)(resolveFloat(dampData) * 256.0f) : 192; // 0.75 * 256
                    f << "    player_vy = -(player_vy * " << damp << ") >> 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetFriction: {
                    auto* fData = findDataIn(action->id, 0);
                    int fr = fData ? (int)(resolveFloat(fData) * 256.0f) : 230; // ~0.9 * 256
                    f << "    afn_friction = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::CloneSprite: {
                    auto* srcData = findDataIn(action->id, 0);
                    int src = srcData ? resolveInt(srcData) : 0;
                    f << "    afn_clone_sprite(" << src << ");\n";
                    break;
                }
                case GBAScriptNodeType::HideAll:
                    f << "    { int i; for (i=0;i<16;i++) afn_sprite_visible[i]=0; }\n";
                    break;
                case GBAScriptNodeType::ShowAll:
                    f << "    { int i; for (i=0;i<16;i++) afn_sprite_visible[i]=1; }\n";
                    break;
                case GBAScriptNodeType::Print: {
                    auto* valData = findDataIn(action->id, 0);
                    int val = valData ? resolveInt(valData) : 0;
                    f << "    mgba_printf(\"val=%d\", " << val << ");\n";
                    break;
                }
                case GBAScriptNodeType::SetColor: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* colData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int col = colData ? resolveInt(colData) : 0;
                    f << "    g_sprites[" << obj << "].pal = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::SwapSprite: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* assetData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int asset = assetData ? resolveInt(assetData) : 0;
                    f << "    g_sprites[" << obj << "].asset = " << asset << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetSpriteY: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* yData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    f << "    g_sprites[" << obj << "].wy = " << y << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::StopAll:
                    f << "    afn_scripts_stopped = 1;\n";
                    break;
                case GBAScriptNodeType::SetLayer: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* layerData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int layer = layerData ? resolveInt(layerData) : 0;
                    f << "    afn_sprite_layer[" << obj << "] = " << layer << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetAlpha: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* alphaData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int alpha = alphaData ? resolveInt(alphaData) : 16;
                    f << "    afn_sprite_alpha[" << obj << "] = " << alpha << ";\n";
                    break;
                }
                case GBAScriptNodeType::Flash: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* framesData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int frames = framesData ? resolveInt(framesData) : 8;
                    f << "    afn_flash_obj[" << obj << "] = " << frames << ";\n";
                    break;
                }
                case GBAScriptNodeType::Delay:
                    f << "    { static int afn_dly_" << action->id << " = 0;\n";
                    f << "      if (afn_dly_" << action->id << " > 0) { afn_dly_" << action->id << "--; return; } }\n";
                    break;
                case GBAScriptNodeType::SetSpriteScale: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* scaleData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int scale = scaleData ? (int)(resolveFloat(scaleData) * 256.0f) : 256;
                    f << "    g_sprites[" << obj << "].scale = " << scale << ";\n";
                    break;
                }
                case GBAScriptNodeType::RotateSprite: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* angData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int ang = angData ? resolveInt(angData) : 0;
                    f << "    afn_sprite_rot[" << obj << "] = " << ang << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetHP2: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* hpData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int hp = hpData ? resolveInt(hpData) : 100;
                    f << "    afn_hp[" << obj << "] = " << hp << ";\n";
                    f << "    if (afn_hp[" << obj << "] > afn_max_hp[" << obj << "]) afn_hp[" << obj << "] = afn_max_hp[" << obj << "];\n";
                    break;
                }
                case GBAScriptNodeType::HealHP: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* amtData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int amt = amtData ? resolveInt(amtData) : 1;
                    f << "    afn_hp[" << obj << "] += " << amt << ";\n";
                    f << "    if (afn_hp[" << obj << "] > afn_max_hp[" << obj << "]) afn_hp[" << obj << "] = afn_max_hp[" << obj << "];\n";
                    break;
                }
                case GBAScriptNodeType::SetMaxHP: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* maxData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int maxhp = maxData ? resolveInt(maxData) : 100;
                    f << "    afn_max_hp[" << obj << "] = " << maxhp << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetBGColor: {
                    auto* colData = findDataIn(action->id, 0);
                    int col = colData ? resolveInt(colData) : 0;
                    f << "    afn_bg_color = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::FacePlayer: {
                    auto* objData = findDataIn(action->id, 0);
                    int obj = objData ? resolveInt(objData) : 0;
                    f << "    afn_sprite_rot[" << obj << "] = ArcTan2(player_z - g_sprites[" << obj << "].wz, player_x - g_sprites[" << obj << "].wx);\n";
                    break;
                }
                case GBAScriptNodeType::MoveForward: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* spdData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int spd = spdData ? resolveInt(spdData) : 1;
                    f << "    { int ang = afn_sprite_rot[" << obj << "];\n";
                    f << "      g_sprites[" << obj << "].wx += (lu_cos(ang) * " << spd << ") >> 12;\n";
                    f << "      g_sprites[" << obj << "].wz -= (lu_sin(ang) * " << spd << ") >> 12; }\n";
                    break;
                }
                case GBAScriptNodeType::Patrol: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* x1Data = findDataIn(action->id, 1);
                    auto* z1Data = findDataIn(action->id, 2);
                    auto* spdData = findDataIn(action->id, 3);
                    int obj = objData ? resolveInt(objData) : 0;
                    int x1 = x1Data ? resolveInt(x1Data) : 0;
                    int z1 = z1Data ? resolveInt(z1Data) : 0;
                    int spd = spdData ? resolveInt(spdData) : 1;
                    f << "    { static int afn_patrol_dir_" << action->id << " = 1;\n";
                    f << "      FIXED tx = afn_patrol_dir_" << action->id << " ? (" << x1 << "<<8) : afn_patrol_home_x[" << obj << "];\n";
                    f << "      FIXED tz = afn_patrol_dir_" << action->id << " ? (" << z1 << "<<8) : afn_patrol_home_z[" << obj << "];\n";
                    f << "      FIXED dx = tx - g_sprites[" << obj << "].wx; FIXED dz = tz - g_sprites[" << obj << "].wz;\n";
                    f << "      if (dx<0) dx=-dx; if (dz<0) dz=-dz;\n";
                    f << "      int dist = (dx>dz)?dx+(dz>>1):dz+(dx>>1);\n";
                    f << "      if ((dist>>8) < 4) afn_patrol_dir_" << action->id << " ^= 1;\n";
                    f << "      else if (dist > 0) { g_sprites[" << obj << "].wx += ((tx-g_sprites[" << obj << "].wx) * " << spd << ") / (dist>>8);\n";
                    f << "        g_sprites[" << obj << "].wz += ((tz-g_sprites[" << obj << "].wz) * " << spd << ") / (dist>>8); } }\n";
                    break;
                }
                case GBAScriptNodeType::ChasePlayer: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* spdData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int spd = spdData ? resolveInt(spdData) : 1;
                    f << "    { FIXED dx = player_x - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = player_z - g_sprites[" << obj << "].wz;\n";
                    f << "      FIXED adx = dx<0?-dx:dx; FIXED adz = dz<0?-dz:dz;\n";
                    f << "      int dist = (adx>adz)?adx+(adz>>1):adz+(adx>>1);\n";
                    f << "      if (dist > 0) { g_sprites[" << obj << "].wx += (dx * " << spd << ") / (dist>>8);\n";
                    f << "        g_sprites[" << obj << "].wz += (dz * " << spd << ") / (dist>>8); } }\n";
                    break;
                }
                case GBAScriptNodeType::FleePlayer: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* spdData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int spd = spdData ? resolveInt(spdData) : 1;
                    f << "    { FIXED dx = g_sprites[" << obj << "].wx - player_x;\n";
                    f << "      FIXED dz = g_sprites[" << obj << "].wz - player_z;\n";
                    f << "      FIXED adx = dx<0?-dx:dx; FIXED adz = dz<0?-dz:dz;\n";
                    f << "      int dist = (adx>adz)?adx+(adz>>1):adz+(adx>>1);\n";
                    f << "      if (dist > 0) { g_sprites[" << obj << "].wx += (dx * " << spd << ") / (dist>>8);\n";
                    f << "        g_sprites[" << obj << "].wz += (dz * " << spd << ") / (dist>>8); } }\n";
                    break;
                }
                case GBAScriptNodeType::FollowPlayer: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* distData = findDataIn(action->id, 1);
                    auto* speedData = findDataIn(action->id, 2);
                    int obj = objData ? resolveInt(objData) : 0;
                    int dist = distData ? resolveInt(distData) : 0;
                    int speed = speedData ? resolveInt(speedData) : 0;
                    // Mode 0: activate generic breadcrumb-trail follow system
                    f << "    if (!tm_fol_active) {\n";
                    f << "      tm_fol_obj = " << obj << ";\n";
                    f << "      tm_fol_prev_ptx = tm_player_tx;\n";
                    f << "      tm_fol_prev_pty = tm_player_ty;\n";
                    f << "      tm_fol_trail_count = 0;\n";
                    f << "      tm_fol_trail_head = 0;\n";
                    f << "      tm_fol_active = 1;\n";
                    f << "      if (" << obj << " >= 0 && " << obj << " < TM_MAX_DIR_OBJS) {\n";
                    f << "        tm_obj_dir_set[" << obj << "] = -1;\n";
                    f << "        tm_obj_dir_facing[" << obj << "] = -1;\n";
                    f << "      }\n";
                    f << "    }\n";
                    f << "    tm_fol_dist = " << dist << ";\n";
                    f << "    tm_fol_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::IsFollowMoving:
                    // Gate node — handled inline in bpEmitActionsWithGates
                    break;
                case GBAScriptNodeType::SetFollowFacing: {
                    f << "    if (tm_fol_active && tm_fol_obj >= 0) {\n";
                    f << "      tm_obj_facing[tm_fol_obj] = tm_fol_facing;\n";
                    f << "    }\n";
                    break;
                }
                case GBAScriptNodeType::SetAI: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* modeData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int mode = modeData ? resolveInt(modeData) : 0;
                    f << "    afn_ai_mode[" << obj << "] = " << mode << ";\n";
                    break;
                }
                case GBAScriptNodeType::EmitParticle: {
                    auto* typeData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* zData = findDataIn(action->id, 2);
                    int type = typeData ? resolveInt(typeData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int z = zData ? resolveInt(zData) : 0;
                    f << "    afn_emit_particle(" << type << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::SetTint: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* colData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int col = colData ? resolveInt(colData) : 0;
                    f << "    afn_sprite_tint[" << obj << "] = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::Shake: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* frData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int fr = frData ? resolveInt(frData) : 8;
                    f << "    afn_sprite_shake[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetText: {
                    auto* slotData = findDataIn(action->id, 0);
                    auto* valData = findDataIn(action->id, 1);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    int val = valData ? resolveInt(valData) : 0;
                    f << "    afn_hud_value[" << slot << "] = " << val << ";\n";
                    break;
                }
                case GBAScriptNodeType::ShowHUD: {
                    auto* slotData = findDataIn(action->id, 0);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    f << "    afn_hud_visible[" << slot << "] = 1;\n";
                    f << "    afn_elem_idx = " << slot << ";\n";
                    f << "    afn_active_element = " << slot << ";\n";
                    f << "    afn_cursor_stop = 0;\n";
                    f << "    afn_player_frozen = 1;\n";
                    f << "    afn_play_anim = -1;\n";
                    f << "    afn_move_speed = 0;\n";
                    f << "    afn_stop_count = afn_hud_elems[" << slot << "].stopCount;\n";
                    f << "    { int si; for (si = 0; si < afn_stop_count && si < 8; si++) afn_stop_links[si] = afn_hud_stops[afn_hud_elems[" << slot << "].stopStart + si].link; }\n";
                    break;
                }
                case GBAScriptNodeType::HideHUD: {
                    auto* slotData = findDataIn(action->id, 0);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    f << "    afn_hud_visible[" << slot << "] = 0;\n";
                    f << "    afn_player_frozen = 0;\n";
                    f << "    afn_play_anim = 0;\n";
                    break;
                }
                case GBAScriptNodeType::PlayHudAnim: {
                    int layIdx = action->paramInt[0];
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_frame[" << layIdx << "] = 0;\n";
                    f << "    afn_hud_layer_tick[" << layIdx << "] = 0;\n";
                    f << "    afn_hud_layer_active[" << layIdx << "] = 1;\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::StopHudAnim: {
                    int layIdx = action->paramInt[0];
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_active[" << layIdx << "] = 0;\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::SetHudAnimSpeed: {
                    int layIdx = action->paramInt[0];
                    auto* speedData = findDataIn(action->id, 0);
                    int spd = speedData ? resolveInt(speedData) : 1;
                    if (spd < 1) spd = 1;
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_speed[" << layIdx << "] = " << spd << ";\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::ArraySet: {
                    auto* idxData = findDataIn(action->id, 0);
                    auto* valData = findDataIn(action->id, 1);
                    int idx = idxData ? resolveInt(idxData) : 0;
                    int val = valData ? resolveInt(valData) : 0;
                    f << "    afn_vars[" << idx << " & 15] = " << val << ";\n";
                    break;
                }
                // Text rendering
                case GBAScriptNodeType::DrawNumber: {
                    auto* valData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* yData = findDataIn(action->id, 2);
                    int val = valData ? resolveInt(valData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    f << "    afn_draw_number(" << val << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::DrawTextID: {
                    auto* idData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* yData = findDataIn(action->id, 2);
                    int id = idData ? resolveInt(idData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    f << "    afn_draw_text(" << id << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::ClearText:
                    f << "    afn_clear_text();\n";
                    break;
                case GBAScriptNodeType::SetTextColor: {
                    auto* colData = findDataIn(action->id, 0);
                    int col = colData ? resolveInt(colData) : 0x7FFF;
                    f << "    afn_text_color = " << col << ";\n";
                    break;
                }
                // Inventory
                case GBAScriptNodeType::AddItem: {
                    auto* slotData = findDataIn(action->id, 0);
                    auto* amtData = findDataIn(action->id, 1);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    int amt = amtData ? resolveInt(amtData) : 1;
                    f << "    afn_inventory[" << slot << " & 15] += " << amt << ";\n";
                    break;
                }
                case GBAScriptNodeType::RemoveItem: {
                    auto* slotData = findDataIn(action->id, 0);
                    auto* amtData = findDataIn(action->id, 1);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    int amt = amtData ? resolveInt(amtData) : 1;
                    f << "    afn_inventory[" << slot << " & 15] -= " << amt << ";\n";
                    f << "    if (afn_inventory[" << slot << " & 15] < 0) afn_inventory[" << slot << " & 15] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::SetItemCount: {
                    auto* slotData = findDataIn(action->id, 0);
                    auto* cntData = findDataIn(action->id, 1);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    int cnt = cntData ? resolveInt(cntData) : 0;
                    f << "    afn_inventory[" << slot << " & 15] = " << cnt << ";\n";
                    break;
                }
                case GBAScriptNodeType::UseItem: {
                    auto* slotData = findDataIn(action->id, 0);
                    int slot = slotData ? resolveInt(slotData) : 0;
                    f << "    if (afn_inventory[" << slot << " & 15] > 0) afn_inventory[" << slot << " & 15]--;\n";
                    break;
                }
                // Dialogue
                case GBAScriptNodeType::ShowDialogue: {
                    auto* txtData = findDataIn(action->id, 0);
                    int txt = txtData ? resolveInt(txtData) : 0;
                    f << "    afn_dlg_text = " << txt << "; afn_dlg_open = 1; afn_dlg_line = 0;\n";
                    break;
                }
                case GBAScriptNodeType::HideDialogue:
                    f << "    afn_dlg_open = 0;\n";
                    break;
                case GBAScriptNodeType::NextLine:
                    f << "    afn_dlg_line++;\n";
                    break;
                case GBAScriptNodeType::SetSpeaker: {
                    auto* spkData = findDataIn(action->id, 0);
                    int spk = spkData ? resolveInt(spkData) : 0;
                    f << "    afn_dlg_speaker = " << spk << ";\n";
                    break;
                }
                // State Machine
                case GBAScriptNodeType::SetState: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* stData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int st = stData ? resolveInt(stData) : 0;
                    f << "    afn_prev_state[" << obj << " & 15] = afn_state[" << obj << " & 15];\n";
                    f << "    afn_state[" << obj << " & 15] = " << st << ";\n";
                    f << "    afn_state_timer[" << obj << " & 15] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::TransitionState: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* stData = findDataIn(action->id, 1);
                    auto* delData = findDataIn(action->id, 2);
                    int obj = objData ? resolveInt(objData) : 0;
                    int st = stData ? resolveInt(stData) : 0;
                    int del = delData ? resolveInt(delData) : 0;
                    if (del > 0) {
                        f << "    { static int afn_trans_" << action->id << " = 0;\n";
                        f << "      if (afn_trans_" << action->id << " > 0) { afn_trans_" << action->id << "--; return; }\n";
                        f << "      afn_trans_" << action->id << " = " << del << "; }\n";
                    }
                    f << "    afn_prev_state[" << obj << " & 15] = afn_state[" << obj << " & 15];\n";
                    f << "    afn_state[" << obj << " & 15] = " << st << "; afn_state_timer[" << obj << " & 15] = 0;\n";
                    break;
                }
                // Batch 8: Collision / Spawning / UI / Scene / Input
                case GBAScriptNodeType::SetCollisionSize: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* radData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int rad = radData ? resolveInt(radData) : 8;
                    f << "    afn_collision_size[" << obj << "] = " << rad << ";\n";
                    break;
                }
                case GBAScriptNodeType::IgnoreCollision: {
                    auto* aData = findDataIn(action->id, 0);
                    auto* bData = findDataIn(action->id, 1);
                    int a = aData ? resolveInt(aData) : 0;
                    int b = bData ? resolveInt(bData) : 0;
                    f << "    afn_collision_ignore[" << a << "] = " << b << ";\n";
                    break;
                }
                case GBAScriptNodeType::SpawnAt: {
                    auto* assetData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* zData = findDataIn(action->id, 2);
                    int asset = assetData ? resolveInt(assetData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int z = zData ? resolveInt(zData) : 0;
                    f << "    afn_spawn_sprite(" << asset << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::DestroyAfter: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* frData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int fr = frData ? resolveInt(frData) : 60;
                    f << "    afn_lifetime[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::SpawnProjectile: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* assetData = findDataIn(action->id, 1);
                    auto* spdData = findDataIn(action->id, 2);
                    int obj = objData ? resolveInt(objData) : 0;
                    int asset = assetData ? resolveInt(assetData) : 0;
                    int spd = spdData ? resolveInt(spdData) : 4;
                    f << "    afn_spawn_projectile(" << obj << ", " << asset << ", " << spd << ");\n";
                    break;
                }
                case GBAScriptNodeType::SetLifetime: {
                    auto* objData = findDataIn(action->id, 0);
                    auto* frData = findDataIn(action->id, 1);
                    int obj = objData ? resolveInt(objData) : 0;
                    int fr = frData ? resolveInt(frData) : 60;
                    f << "    afn_lifetime[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::DrawBar: {
                    auto* xData = findDataIn(action->id, 0);
                    auto* yData = findDataIn(action->id, 1);
                    auto* wData = findDataIn(action->id, 2);
                    auto* fillData = findDataIn(action->id, 3);
                    int x = xData ? resolveInt(xData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    int w = wData ? resolveInt(wData) : 32;
                    int fill = fillData ? resolveInt(fillData) : 0;
                    f << "    afn_draw_bar(" << x << ", " << y << ", " << w << ", " << fill << ");\n";
                    break;
                }
                case GBAScriptNodeType::DrawSpriteIcon: {
                    auto* assetData = findDataIn(action->id, 0);
                    auto* xData = findDataIn(action->id, 1);
                    auto* yData = findDataIn(action->id, 2);
                    int asset = assetData ? resolveInt(assetData) : 0;
                    int x = xData ? resolveInt(xData) : 0;
                    int y = yData ? resolveInt(yData) : 0;
                    f << "    afn_draw_sprite_icon(" << asset << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::ShowTimer:
                    f << "    afn_timer_visible = 1;\n";
                    break;
                case GBAScriptNodeType::HideTimer:
                    f << "    afn_timer_visible = 0;\n";
                    break;
                case GBAScriptNodeType::SetBarColor: {
                    auto* barData = findDataIn(action->id, 0);
                    auto* colData = findDataIn(action->id, 1);
                    int bar = barData ? resolveInt(barData) : 0;
                    int col = colData ? resolveInt(colData) : 0x7FFF;
                    f << "    afn_bar_color[" << bar << " & 3] = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetBarMax: {
                    auto* barData = findDataIn(action->id, 0);
                    auto* maxData = findDataIn(action->id, 1);
                    int bar = barData ? resolveInt(barData) : 0;
                    int mx = maxData ? resolveInt(maxData) : 100;
                    f << "    afn_bar_max[" << bar << " & 3] = " << mx << ";\n";
                    break;
                }
                case GBAScriptNodeType::ReloadScene:
                    f << "    afn_pending_scene = afn_current_scene;\n";
                    break;
                case GBAScriptNodeType::SetCheckpoint:
                    f << "    afn_checkpoint_x = player_x; afn_checkpoint_z = player_z; afn_checkpoint_set = 1;\n";
                    break;
                case GBAScriptNodeType::LoadCheckpoint:
                    f << "    if (afn_checkpoint_set) { player_x = afn_checkpoint_x; player_z = afn_checkpoint_z; }\n";
                    break;
                case GBAScriptNodeType::CursorUp:
                    f << "    if (afn_cursor_stop > 0) afn_cursor_stop--;\n";
                    f << "    else afn_cursor_stop = afn_stop_count - 1;\n";
                    break;
                case GBAScriptNodeType::CursorDown:
                    f << "    afn_cursor_stop++;\n";
                    f << "    if (afn_cursor_stop >= afn_stop_count) afn_cursor_stop = 0;\n";
                    break;
                case GBAScriptNodeType::FollowLink:
                    f << "    { int link = afn_stop_links[afn_cursor_stop];\n";
                    f << "      if (link >= 0) { afn_hud_visible[afn_elem_idx] = 0; afn_hud_visible[link] = 1; afn_active_element = link; } }\n";
                    break;
                default:
                    f << "    // unsupported action: type " << (int)action->type << "\n";
                    break;
                }
            };

            // First pass: emit each action as its own function (deduplicate by node ID)
            auto isGateNode = [](GBAScriptNodeType t) {
                return t == GBAScriptNodeType::IsMoving || t == GBAScriptNodeType::IsOnGround || t == GBAScriptNodeType::IsJumping
                    || t == GBAScriptNodeType::IsFlagSet || t == GBAScriptNodeType::IsHPZero || t == GBAScriptNodeType::IsNear
                    || t == GBAScriptNodeType::Countdown || t == GBAScriptNodeType::IsAlive
                    || t == GBAScriptNodeType::HasItem || t == GBAScriptNodeType::IsDialogueOpen
                    || t == GBAScriptNodeType::IsInState || t == GBAScriptNodeType::IsColliding
                    || t == GBAScriptNodeType::IsTrue || t == GBAScriptNodeType::IsNear2D
                    || t == GBAScriptNodeType::IsFollowMoving;
            };
            std::set<int> emittedActionIds;
            for (auto& c : chains) {
                for (auto* a : c.actions) {
                    if (isGateNode(a->type)) continue;
                    if (emittedActionIds.count(a->id)) continue;
                    emittedActionIds.insert(a->id);
                    const char* suffix = a->funcName[0] ? a->funcName : nullptr;
                    char defaultName[64];
                    if (!suffix) {
                        snprintf(defaultName, sizeof(defaultName), "afn_script%s_%d", actionSuffix(a->type), a->id);
                        suffix = defaultName;
                    }
                    bool hasDataOut = (a->type == GBAScriptNodeType::CustomCode && a->ccDataOut > 0);
                    if (hasDataOut)
                        f << "static inline int " << suffix << "(void) {\n";
                    else
                        f << "static inline void " << suffix << "(void) {\n";
                    emitActionBody(a);
                    if (hasDataOut)
                        f << "    return 0; // default if no explicit return\n";
                    f << "}\n";
                }
            }

            // Emit standalone CustomCode data-out nodes not already in action chains
            for (auto& n : script.nodes) {
                if (n.type != GBAScriptNodeType::CustomCode) continue;
                if (n.ccDataOut <= 0) continue;
                if (emittedActionIds.count(n.id)) continue;
                emittedActionIds.insert(n.id);
                const char* suffix = n.funcName[0] ? n.funcName : nullptr;
                char defaultName[64];
                if (!suffix) {
                    snprintf(defaultName, sizeof(defaultName), "afn_script_custom_%d", n.id);
                    suffix = defaultName;
                }
                f << "static inline int " << suffix << "(void) {\n";
                emitActionBody(&n);
                f << "    return 0;\n}\n";
            }
            f << "\n";

            // Helper: emit a call to an action's function
            auto emitActionCall = [&](const GBAScriptNodeExport* a) {
                char callName[64];
                if (a->funcName[0]) {
                    snprintf(callName, sizeof(callName), "%s", a->funcName);
                } else {
                    snprintf(callName, sizeof(callName), "afn_script%s_%d", actionSuffix(a->type), a->id);
                }
                f << "    " << callName << "();\n";
            };

            // Collect OnStart chains
            bool hasStart = false, hasHeld = false, hasPressed = false, hasReleased = false, hasUpdate = false;
            for (auto& c : chains) {
                if (c.event->type == GBAScriptNodeType::OnStart) hasStart = true;
                if (c.event->type == GBAScriptNodeType::OnKeyHeld) hasHeld = true;
                if (c.event->type == GBAScriptNodeType::OnKeyPressed) hasPressed = true;
                if (c.event->type == GBAScriptNodeType::OnKeyReleased) hasReleased = true;
                if (c.event->type == GBAScriptNodeType::OnUpdate) hasUpdate = true;
            }

            // Always emit all four functions (main.c calls them unconditionally)

            // Helper: find custom func name for an event type (first one wins)
            auto findCustomName = [&](GBAScriptNodeType evType) -> const char* {
                for (auto& c : chains)
                    if (c.event->type == evType && c.event->funcName[0])
                        return c.event->funcName;
                return nullptr;
            };
            // Helper: emit function with optional rename + #define alias
            auto emitFuncStart = [&](const char* defaultName, GBAScriptNodeType evType) {
                const char* custom = findCustomName(evType);
                if (custom) {
                    f << "static inline void " << custom << "(void) {\n";
                } else {
                    f << "static inline void " << defaultName << "(void) {\n";
                }
            };
            auto emitFuncAlias = [&](const char* defaultName, GBAScriptNodeType evType) {
                const char* custom = findCustomName(evType);
                if (custom)
                    f << "#define " << defaultName << " " << custom << "\n";
            };

            // Emit action calls with IsMoving gate support
            auto emitActionsWithGates = [&](const std::vector<const GBAScriptNodeExport*>& actions) {
                int gateDepth = 0;
                for (auto* a : actions) {
                    if (a->type == GBAScriptNodeType::IsMoving) {
                        f << "    if (player_moving) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsOnGround) {
                        f << "    if (player_on_ground) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsJumping) {
                        f << "    if (!player_on_ground && player_vy > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsFlagSet) {
                        f << "    if (afn_flags & (1u << " << a->paramInt[0] << ")) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsHPZero) {
                        f << "    if (afn_hp[" << a->paramInt[0] << "] == 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsNear) {
                        f << "    { FIXED dx = g_sprites[" << a->paramInt[0] << "].wx - g_sprites[" << a->paramInt[1] << "].wx;\n";
                        f << "      FIXED dz = g_sprites[" << a->paramInt[0] << "].wz - g_sprites[" << a->paramInt[1] << "].wz;\n";
                        f << "      if (dx<0) dx=-dx; if (dz<0) dz=-dz;\n";
                        f << "      if (((dx>dz)?dx+(dz>>1):dz+(dx>>1))>>8 < " << a->paramInt[2] << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::Countdown) {
                        f << "    { static int afn_cd_" << a->id << " = " << a->paramInt[0] << ";\n";
                        f << "      if (--afn_cd_" << a->id << " <= 0) { afn_cd_" << a->id << " = " << a->paramInt[0] << ";\n";
                        gateDepth += 2; // two closing braces: inner if + outer block
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsAlive) {
                        f << "    if (afn_hp[" << a->paramInt[0] << "] > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::HasItem) {
                        f << "    if (afn_inventory[" << a->paramInt[0] << " & 15] > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsDialogueOpen) {
                        f << "    if (afn_dlg_open) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsInState) {
                        f << "    if (afn_state[" << a->paramInt[0] << " & 15] == " << a->paramInt[1] << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsColliding) {
                        f << "    { FIXED dx = g_sprites[" << a->paramInt[0] << "].wx - g_sprites[" << a->paramInt[1] << "].wx;\n";
                        f << "      FIXED dz = g_sprites[" << a->paramInt[0] << "].wz - g_sprites[" << a->paramInt[1] << "].wz;\n";
                        f << "      if (dx<0) dx=-dx; if (dz<0) dz=-dz;\n";
                        f << "      int cr = afn_collision_size[" << a->paramInt[0] << "] + afn_collision_size[" << a->paramInt[1] << "];\n";
                        f << "      if (cr <= 0) cr = 16;\n";
                        f << "      if (((dx>dz)?dx+(dz>>1):dz+(dx>>1))>>8 < cr) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsTrue) {
                        auto* valData = findDataIn(a->id, 0);
                        int val = valData ? resolveInt(valData) : 0;
                        f << "    if (" << val << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsNear2D) {
                        f << "    if (afn_collided_tm_obj == afn_bp_cur_tm_obj && afn_bp_cur_tm_obj >= 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsFollowMoving) {
                        f << "    if (tm_fol_moving) {\n";
                        gateDepth++;
                        continue;
                    }
                    emitActionCall(a);
                }
                for (int g = 0; g < gateDepth; g++)
                    f << "    }\n";
            };

            // OnStart → initialization function
            emitFuncStart("afn_script_start", GBAScriptNodeType::OnStart);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnStart) continue;
                emitActionsWithGates(c.actions);
            }
            f << "}\n";
            emitFuncAlias("afn_script_start", GBAScriptNodeType::OnStart);
            f << "\n";

            auto emitKeyBlock = [&](const EventChain& c, const char* keyCheck) {
                // Collect all keys connected to this event node
                std::vector<int> keys;
                for (auto& l : script.links)
                    if (l.toNodeId == c.event->id && l.toPinType == 3 && l.toPinIdx == 0) {
                        auto* dn = findNode(l.fromNodeId);
                        if (dn) keys.push_back(dn->paramInt[0]);
                    }
                if (keys.empty()) keys.push_back(c.event->paramInt[0]);

                if (keys.size() == 1) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), keyCheck, resolveKeyName(keys[0]));
                    f << "  " << buf << " {\n";
                    emitActionsWithGates(c.actions);
                    f << "  }\n";
                } else {
                    // Multiple keys: OR them together
                    std::string kc(keyCheck);
                    std::string condFmt = kc.substr(4, kc.size() - 5); // strip "if (" and ")"
                    f << "  if (";
                    for (size_t ki = 0; ki < keys.size(); ki++) {
                        if (ki > 0) f << " || ";
                        char buf[64];
                        snprintf(buf, sizeof(buf), condFmt.c_str(), resolveKeyName(keys[ki]));
                        f << buf;
                    }
                    f << ") {\n";
                    emitActionsWithGates(c.actions);
                    f << "  }\n";
                }
            };

            // OnKeyHeld → per-frame held-key checks
            emitFuncStart("afn_script_key_held", GBAScriptNodeType::OnKeyHeld);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnKeyHeld) continue;
                emitKeyBlock(c, "if (key_is_down(%s))");
            }
            f << "}\n";
            emitFuncAlias("afn_script_key_held", GBAScriptNodeType::OnKeyHeld);
            f << "\n";

            // OnKeyPressed → per-frame key-hit checks
            emitFuncStart("afn_script_key_pressed", GBAScriptNodeType::OnKeyPressed);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnKeyPressed) continue;
                emitKeyBlock(c, "if (key_hit(%s))");
            }
            f << "}\n";
            emitFuncAlias("afn_script_key_pressed", GBAScriptNodeType::OnKeyPressed);
            f << "\n";

            // OnKeyReleased → edge-triggered release checks
            emitFuncStart("afn_script_key_released", GBAScriptNodeType::OnKeyReleased);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnKeyReleased) continue;
                emitKeyBlock(c, "if (key_released(%s))");
            }
            f << "}\n";
            emitFuncAlias("afn_script_key_released", GBAScriptNodeType::OnKeyReleased);
            f << "\n";

            // OnUpdate → runs every frame
            emitFuncStart("afn_script_update", GBAScriptNodeType::OnUpdate);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnUpdate) continue;
                emitActionsWithGates(c.actions);
            }
            f << "}\n";
            emitFuncAlias("afn_script_update", GBAScriptNodeType::OnUpdate);
            f << "\n";

            // OnCollision → called when player collides with a sprite
            emitFuncStart("afn_script_collision", GBAScriptNodeType::OnCollision);
            bool hasCollision = false;
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnCollision) continue;
                hasCollision = true;
                emitActionsWithGates(c.actions);
            }
            if (!hasCollision)
                f << "  (void)0;\n";
            f << "}\n";
            emitFuncAlias("afn_script_collision", GBAScriptNodeType::OnCollision);
            f << "\n";

            // OnCollision2D → called when player collides with a tilemap object (Mode 0)
            emitFuncStart("afn_script_collision2d", GBAScriptNodeType::OnCollision2D);
            bool hasCollision2D = false;
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnCollision2D) continue;
                hasCollision2D = true;
                emitActionsWithGates(c.actions);
            }
            if (!hasCollision2D)
                f << "  (void)0;\n";
            f << "}\n";
            emitFuncAlias("afn_script_collision2d", GBAScriptNodeType::OnCollision2D);
            f << "\n";
        }
        else if (hasAnyScript)
        {
            // No inline scene chains but blueprints exist — emit empty stubs
            f << "// No inline scene script chains — stubs for blueprint support\n";
            f << "static inline void afn_script_start(void) {}\n";
            f << "static inline void afn_script_key_held(void) {}\n";
            f << "static inline void afn_script_key_pressed(void) {}\n";
            f << "static inline void afn_script_key_released(void) {}\n";
            f << "static inline void afn_script_update(void) {}\n";
            f << "static inline void afn_script_collision(void) {}\n";
            f << "static inline void afn_script_collision2d(void) {}\n\n";
        }
    }
    // If no inline scene script but hasAnyScript, emit stubs
    if (script.nodes.empty() && hasAnyScript)
    {
        f << "// No inline scene script — stubs for blueprint support\n";
        f << "static inline void afn_script_start(void) {}\n";
        f << "static inline void afn_script_key_held(void) {}\n";
        f << "static inline void afn_script_key_pressed(void) {}\n";
        f << "static inline void afn_script_key_released(void) {}\n";
        f << "static inline void afn_script_update(void) {}\n";
        f << "static inline void afn_script_collision(void) {}\n";
        f << "static inline void afn_script_collision2d(void) {}\n\n";
    }

    // ---- Blueprint codegen ----
    f << "\n// ---- Blueprint script functions ----\n";
    f << "#define AFN_BP_COUNT " << (int)blueprints.size() << "\n";
    f << "#define AFN_BP_INSTANCE_COUNT " << (int)bpInstances.size() << "\n\n";

    if (!blueprints.empty()) {
        // For each blueprint, generate parameterized functions using the same emitAction/chain logic
        for (int bi = 0; bi < (int)blueprints.size(); bi++) {
            const auto& bp = blueprints[bi];
            const auto& bpScript = bp.script;
            int paramCount = (int)bp.params.size();

            // Build param signature and call args
            std::string paramSig, paramArgs;
            for (int pi = 0; pi < paramCount; pi++) {
                if (pi > 0) { paramSig += ", "; paramArgs += ", "; }
                paramSig += "int p" + std::to_string(pi);
                paramArgs += "p" + std::to_string(pi);
            }

            // Helper lambdas for this blueprint's nodes/links
            auto bpFindNode = [&](int id) -> const GBAScriptNodeExport* {
                for (auto& n : bpScript.nodes)
                    if (n.id == id) return &n;
                return nullptr;
            };
            auto bpFindDataIn = [&](int nodeId, int pinIdx) -> const GBAScriptNodeExport* {
                for (auto& lk : bpScript.links)
                    if (lk.toNodeId == nodeId && lk.toPinType == 3 && lk.toPinIdx == pinIdx)
                        return bpFindNode(lk.fromNodeId);
                return nullptr;
            };
            auto bpResolveInt = [&](const GBAScriptNodeExport* n) -> std::string {
                if (!n) return "0";
                // Check if this node is a parameter-exposed node
                for (int pi = 0; pi < paramCount; pi++) {
                    // Match by node ID against the param's source — we encode sourceNodeId
                    // in the paramInt[3] slot during export
                    if (n->paramInt[3] == -(pi + 1))  // sentinel: negative (pi+1)
                        return "p" + std::to_string(pi);
                }
                if (n->type == GBAScriptNodeType::CustomCode) {
                    char buf[64];
                    if (n->funcName[0])
                        snprintf(buf, sizeof(buf), "%s()", n->funcName);
                    else
                        snprintf(buf, sizeof(buf), "afn_bp%d_custom_%d()", bi, n->id);
                    return buf;
                }
                if (n->type == GBAScriptNodeType::Animation) return std::to_string(n->paramInt[1]);
                return std::to_string(n->paramInt[0]);
            };
            auto bpResolveFloat = [&](const GBAScriptNodeExport* n) -> std::string {
                if (!n) return "0";
                for (int pi = 0; pi < paramCount; pi++) {
                    if (n->paramInt[3] == -(pi + 1))
                        return "p" + std::to_string(pi);
                }
                float fv; memcpy(&fv, &n->paramInt[0], sizeof(float));
                return std::to_string((int)(fv * 256.0f));
            };
            auto bpResolveKeyName = [](int key) -> const char* {
                const char* names[] = { "KEY_A","KEY_B","KEY_L","KEY_R","KEY_START","KEY_SELECT",
                                        "KEY_UP","KEY_DOWN","KEY_LEFT","KEY_RIGHT" };
                return (key >= 0 && key < 10) ? names[key] : "KEY_A";
            };
            auto bpResolveEventKey = [&](const GBAScriptNodeExport& ev) -> int {
                int count = 0; int keyVal = -1;
                for (auto& lk : bpScript.links)
                    if (lk.toNodeId == ev.id && lk.toPinType == 3 && lk.toPinIdx == 0) {
                        auto* dn = bpFindNode(lk.fromNodeId);
                        if (dn) { keyVal = dn->paramInt[0]; count++; }
                    }
                if (count == 1) return keyVal;
                if (count == 0) return ev.paramInt[0];
                return -1;
            };

            // Build event chains for this blueprint
            struct BpChain { const GBAScriptNodeExport* event; std::vector<const GBAScriptNodeExport*> actions; };
            std::vector<BpChain> bpChains;
            for (auto& n : bpScript.nodes) {
                bool isEvent = (n.type == GBAScriptNodeType::OnStart || n.type == GBAScriptNodeType::OnKeyHeld ||
                                n.type == GBAScriptNodeType::OnKeyPressed || n.type == GBAScriptNodeType::OnKeyReleased ||
                                n.type == GBAScriptNodeType::OnUpdate || n.type == GBAScriptNodeType::OnCollision ||
                                n.type == GBAScriptNodeType::OnCollision2D);
                if (!isEvent) continue;
                BpChain chain; chain.event = &n;
                std::vector<int> front;
                for (auto& lk : bpScript.links)
                    if (lk.fromNodeId == n.id && lk.fromPinType == 0) front.push_back(lk.toNodeId);
                std::set<int> visited;
                while (!front.empty()) {
                    int nid = front.front(); front.erase(front.begin());
                    if (visited.count(nid)) continue;
                    visited.insert(nid);
                    auto* an = bpFindNode(nid);
                    if (!an) continue;
                    chain.actions.push_back(an);
                    // Don't follow exec outs of FlipFlop/CheckFlag — they dispatch their own branches
                    if (an->type != GBAScriptNodeType::FlipFlop && an->type != GBAScriptNodeType::CheckFlag) {
                        for (auto& lk : bpScript.links)
                            if (lk.fromNodeId == an->id && lk.fromPinType == 0) front.push_back(lk.toNodeId);
                    }
                }
                if (!chain.actions.empty()) bpChains.push_back(chain);
            }

            // Helper: collect downstream actions from a node's specific exec out pin via BFS
            auto bpCollectBranch = [&](int nodeId, int pinIdx) -> std::vector<const GBAScriptNodeExport*> {
                std::vector<const GBAScriptNodeExport*> result;
                std::vector<int> fr2;
                for (auto& lk : bpScript.links)
                    if (lk.fromNodeId == nodeId && lk.fromPinType == 0 && lk.fromPinIdx == pinIdx)
                        fr2.push_back(lk.toNodeId);
                std::set<int> vis2;
                while (!fr2.empty()) {
                    int nid2 = fr2.front(); fr2.erase(fr2.begin());
                    if (vis2.count(nid2)) continue;
                    vis2.insert(nid2);
                    auto* an2 = bpFindNode(nid2);
                    if (!an2) continue;
                    result.push_back(an2);
                    if (an2->type != GBAScriptNodeType::FlipFlop && an2->type != GBAScriptNodeType::CheckFlag) {
                        for (auto& lk : bpScript.links)
                            if (lk.fromNodeId == an2->id && lk.fromPinType == 0)
                                fr2.push_back(lk.toNodeId);
                    }
                }
                return result;
            };

            // Emit action body lines for blueprint
            // Helper: replace $0..$7 in a string with resolved blueprint data-in values
            auto bpResolveCustomPlaceholders = [&](const std::string& src, int nodeId) -> std::string {
                std::string code = src;
                for (int pi = 7; pi >= 0; pi--) {
                    char placeholder[4]; snprintf(placeholder, sizeof(placeholder), "$%d", pi);
                    size_t pos = 0;
                    while ((pos = code.find(placeholder, pos)) != std::string::npos) {
                        auto* din = bpFindDataIn(nodeId, pi);
                        std::string val = din ? bpResolveInt(din) : "0";
                        code.replace(pos, strlen(placeholder), val);
                        pos += val.size();
                    }
                }
                return code;
            };

            auto bpEmitActionBody = [&](const GBAScriptNodeExport* action) {
                if (action->customCode[0]) {
                    // Emit per-pin code blocks first
                    for (int pi = 0; pi < action->ccPinCount && pi < 8; pi++) {
                        if (action->ccPinCode[pi][0]) {
                            std::string pinCode = bpResolveCustomPlaceholders(action->ccPinCode[pi], action->id);
                            f << "    " << pinCode << "\n";
                        }
                    }
                    // Emit main custom code with placeholders resolved
                    std::string code = bpResolveCustomPlaceholders(action->customCode, action->id);
                    f << "    " << code << "\n";
                    return;
                }
                switch (action->type) {
                case GBAScriptNodeType::MovePlayer: {
                    auto* dirData = bpFindDataIn(action->id, 0);
                    int dir = dirData ? dirData->paramInt[0] : 0;
                    const char* dirKeys[] = { "KEY_LEFT", "KEY_RIGHT", "KEY_UP", "KEY_DOWN" };
                    const char* dirVars[] = { "afn_input_right -= 256", "afn_input_right += 256",
                                              "afn_input_fwd += 256", "afn_input_fwd -= 256" };
                    const int dirFacing[] = { 6, 2, 0, 4 };
                    if (dir >= 0 && dir < 4)
                        f << "    if (!afn_player_frozen && key_is_down(" << dirKeys[dir] << ")) { " << dirVars[dir] << "; if (tm_move_timer == 0) tm_player_facing = " << dirFacing[dir] << "; }\n";
                    break;
                }
                case GBAScriptNodeType::Jump: {
                    auto* forceData = bpFindDataIn(action->id, 0);
                    std::string force = forceData ? bpResolveFloat(forceData) : std::to_string((int)(2.0f * 256.0f));
                    f << "    if (player_on_ground) player_vy = " << force << ";\n";
                    break;
                }
                case GBAScriptNodeType::Walk: {
                    auto* speedData = bpFindDataIn(action->id, 0);
                    std::string speed = speedData ? bpResolveInt(speedData) : "37";
                    f << "    afn_move_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::Sprint: {
                    auto* speedData = bpFindDataIn(action->id, 0);
                    std::string speed = speedData ? bpResolveInt(speedData) : "56";
                    f << "    afn_move_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::OrbitCamera: {
                    auto* dirData = bpFindDataIn(action->id, 0);
                    auto* speedData = bpFindDataIn(action->id, 1);
                    int dir = dirData ? dirData->paramInt[0] : 1;
                    std::string speed = speedData ? bpResolveInt(speedData) : "512";
                    const char* key = (dir == 0) ? "KEY_L" : "KEY_R";
                    const char* sign = (dir == 0) ? "-" : "+";
                    f << "    if (key_is_down(" << key << ")) orbit_angle " << sign << "= " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::AutoOrbit: {
                    auto* speedData = bpFindDataIn(action->id, 0);
                    std::string speed = speedData ? bpResolveInt(speedData) : "205";
                    f << "    afn_auto_orbit_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::DampenJump: {
                    auto* factorData = bpFindDataIn(action->id, 0);
                    std::string factor = factorData ? bpResolveFloat(factorData) : std::to_string((int)(0.75f * 256.0f));
                    f << "    if (player_vy > 0) player_vy = (player_vy * " << factor << ") >> 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetGravity: {
                    auto* valData = bpFindDataIn(action->id, 0);
                    std::string val = valData ? bpResolveFloat(valData) : std::to_string((int)(0.09f * 256.0f));
                    f << "    afn_gravity = " << val << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetMaxFall: {
                    auto* valData = bpFindDataIn(action->id, 0);
                    std::string val = valData ? bpResolveFloat(valData) : std::to_string((int)(6.0f * 256.0f));
                    f << "    afn_terminal_vel = " << val << ";\n";
                    break;
                }
                case GBAScriptNodeType::PlayAnim: {
                    auto* animData = bpFindDataIn(action->id, 0);
                    // Animation node: paramInt[0]=sprite asset, paramInt[1]=anim index
                    // Check if param-exposed, otherwise read paramInt[1]
                    std::string animIdx = "0";
                    if (animData) {
                        bool isParam = false;
                        for (int pi = 0; pi < paramCount; pi++) {
                            if (animData->paramInt[3] == -(pi + 1)) {
                                animIdx = "p" + std::to_string(pi);
                                isParam = true;
                                break;
                            }
                        }
                        if (!isParam)
                            animIdx = std::to_string(animData->paramInt[1]);
                    }
                    f << "    afn_play_anim = " << animIdx << ";\n";
                    break;
                }
                case GBAScriptNodeType::ChangeScene: {
                    auto* scData = bpFindDataIn(action->id, 0);
                    std::string scIdx = scData ? bpResolveInt(scData) : std::to_string(action->paramInt[0]);
                    int scMode = action->paramInt[1];
                    f << "    afn_pending_scene = " << scIdx << ";\n";
                    f << "    afn_pending_scene_mode = " << scMode << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetFlag: {
                    auto* flagData = bpFindDataIn(action->id, 0);
                    auto* valData = bpFindDataIn(action->id, 1);
                    std::string flag = flagData ? bpResolveInt(flagData) : "0";
                    std::string val = valData ? bpResolveInt(valData) : "1";
                    f << "    if (" << val << ") afn_flags |= (1u << " << flag << ");\n";
                    f << "    else afn_flags &= ~(1u << " << flag << ");\n";
                    break;
                }
                case GBAScriptNodeType::ToggleFlag: {
                    auto* flagData = bpFindDataIn(action->id, 0);
                    std::string flag = flagData ? bpResolveInt(flagData) : "0";
                    f << "    afn_flags ^= (1u << " << flag << ");\n";
                    break;
                }
                case GBAScriptNodeType::FreezePlayer: {
                    auto* bpData = bpFindDataIn(action->id, 0);
                    f << "    afn_player_frozen = 1;\n";
                    f << "    afn_play_anim = -1;\n";
                    f << "    afn_move_speed = 0;\n";
                    if (bpData) {
                        std::string bpIdx = bpResolveInt(bpData);
                        f << "    afn_bp_def_frozen[" << bpIdx << "] = 1;\n";
                    }
                    break;
                }
                case GBAScriptNodeType::UnfreezePlayer: {
                    auto* bpData = bpFindDataIn(action->id, 0);
                    f << "    afn_player_frozen = 0;\n";
                    f << "    afn_play_anim = 0;\n";
                    if (bpData) {
                        std::string bpIdx = bpResolveInt(bpData);
                        f << "    afn_bp_def_frozen[" << bpIdx << "] = 0;\n";
                    }
                    break;
                }
                case GBAScriptNodeType::SetCameraHeight: {
                    auto* hData = bpFindDataIn(action->id, 0);
                    std::string h = hData ? bpResolveInt(hData) : "64";
                    f << "    cam_h = " << h << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetHorizon: {
                    auto* sData = bpFindDataIn(action->id, 0);
                    std::string s = sData ? bpResolveInt(sData) : "60";
                    f << "    m7_horizon = " << s << ";\n";
                    break;
                }
                case GBAScriptNodeType::Teleport: {
                    auto* xData = bpFindDataIn(action->id, 0);
                    auto* yData = bpFindDataIn(action->id, 1);
                    auto* zData = bpFindDataIn(action->id, 2);
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    std::string z = zData ? bpResolveInt(zData) : "0";
                    f << "    player_x = " << x << " << 8;\n";
                    f << "    player_y = " << y << " << 8;\n";
                    f << "    player_z = " << z << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetVisible: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* visData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string vis = visData ? bpResolveInt(visData) : "1";
                    f << "    afn_sprite_visible[" << obj << "] = " << vis << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetPosition: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* zData = bpFindDataIn(action->id, 2);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string z = zData ? bpResolveInt(zData) : "0";
                    f << "    g_sprites[" << obj << "].wx = " << x << " << 8;\n";
                    f << "    g_sprites[" << obj << "].wz = " << z << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::StopAnim:
                    f << "    afn_play_anim = -1;\n";
                    break;
                case GBAScriptNodeType::SetAnimSpeed: {
                    auto* sData = bpFindDataIn(action->id, 0);
                    std::string speed = sData ? bpResolveInt(sData) : "1";
                    f << "    afn_anim_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetVelocityY: {
                    auto* vData = bpFindDataIn(action->id, 0);
                    std::string vel = vData ? bpResolveFloat(vData) : "0";
                    f << "    player_vy = " << vel << ";\n";
                    break;
                }
                case GBAScriptNodeType::PlaySound: {
                    auto* sndData = bpFindDataIn(action->id, 0);
                    std::string sndId = sndData ? bpResolveInt(sndData) : "0";
                    f << "    afn_play_sound(" << sndId << ");\n";
                    break;
                }
                case GBAScriptNodeType::StopSound:
                    f << "    afn_stop_sound();\n";
                    break;
                case GBAScriptNodeType::SetScale: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* scaleData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string scale = scaleData ? bpResolveFloat(scaleData) : "256";
                    f << "    g_sprites[" << obj << "].scale = " << scale << ";\n";
                    break;
                }
                case GBAScriptNodeType::ScreenShake: {
                    auto* intData = bpFindDataIn(action->id, 0);
                    auto* frData = bpFindDataIn(action->id, 1);
                    std::string intensity = intData ? bpResolveInt(intData) : "3";
                    std::string frames = frData ? bpResolveInt(frData) : "10";
                    f << "    afn_shake_intensity = " << intensity << ";\n";
                    f << "    afn_shake_frames = " << frames << ";\n";
                    break;
                }
                case GBAScriptNodeType::FadeOut: {
                    auto* frData = bpFindDataIn(action->id, 0);
                    std::string frames = frData ? bpResolveInt(frData) : "30";
                    f << "    afn_fade_target = 16;\n";
                    f << "    afn_fade_frames = " << frames << ";\n";
                    f << "    afn_fade_counter = 0;\n";
                    break;
                }
                case GBAScriptNodeType::FadeIn: {
                    auto* frData = bpFindDataIn(action->id, 0);
                    std::string frames = frData ? bpResolveInt(frData) : "30";
                    f << "    afn_fade_target = 0;\n";
                    f << "    afn_fade_frames = " << frames << ";\n";
                    f << "    afn_fade_counter = 0;\n";
                    break;
                }
                case GBAScriptNodeType::MoveToward: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* tgtData = bpFindDataIn(action->id, 1);
                    auto* spdData = bpFindDataIn(action->id, 2);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string tgt = tgtData ? bpResolveInt(tgtData) : "0";
                    std::string spd = spdData ? bpResolveInt(spdData) : "64";
                    f << "    { FIXED dx = g_sprites[" << tgt << "].wx - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = g_sprites[" << tgt << "].wz - g_sprites[" << obj << "].wz;\n";
                    f << "      if (dx > " << spd << ") g_sprites[" << obj << "].wx += " << spd << ";\n";
                    f << "      else if (dx < -" << spd << ") g_sprites[" << obj << "].wx -= " << spd << ";\n";
                    f << "      else g_sprites[" << obj << "].wx = g_sprites[" << tgt << "].wx;\n";
                    f << "      if (dz > " << spd << ") g_sprites[" << obj << "].wz += " << spd << ";\n";
                    f << "      else if (dz < -" << spd << ") g_sprites[" << obj << "].wz -= " << spd << ";\n";
                    f << "      else g_sprites[" << obj << "].wz = g_sprites[" << tgt << "].wz; }\n";
                    break;
                }
                case GBAScriptNodeType::LookAt: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* tgtData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string tgt = tgtData ? bpResolveInt(tgtData) : "0";
                    f << "    { FIXED dx = g_sprites[" << tgt << "].wx - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = g_sprites[" << tgt << "].wz - g_sprites[" << obj << "].wz;\n";
                    f << "      g_sprites[" << obj << "].facing = ArcTan2(dx, dz); }\n";
                    break;
                }
                case GBAScriptNodeType::SetSpriteAnim: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* animData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string anim = animData ? bpResolveInt(animData) : "0";
                    f << "    if (afn_current_mode == 1) { tm_obj_anim_idx[" << obj << "] = " << anim << "; tm_obj_anim_play[" << obj << "] = 1; }\n";
                    f << "    else { afn_play_anim = " << anim << "; }\n";
                    break;
                }
                case GBAScriptNodeType::SpawnEffect: {
                    auto* effData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* zData = bpFindDataIn(action->id, 2);
                    std::string eff = effData ? bpResolveInt(effData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string z = zData ? bpResolveInt(zData) : "0";
                    f << "    afn_spawn_effect(" << eff << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::DoOnce:
                    f << "    { static int afn_done_" << action->id << " = 0;\n";
                    f << "      if (afn_done_" << action->id << ") return;\n";
                    f << "      afn_done_" << action->id << " = 1; }\n";
                    break;
                case GBAScriptNodeType::Gate: {
                    auto* openData = bpFindDataIn(action->id, 0);
                    std::string open = openData ? bpResolveInt(openData) : "1";
                    f << "    if (!" << open << ") return;\n";
                    break;
                }
                case GBAScriptNodeType::SetHP: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* hpData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string hp = hpData ? bpResolveInt(hpData) : "100";
                    f << "    afn_hp[" << obj << "] = " << hp << ";\n";
                    break;
                }
                case GBAScriptNodeType::DamageHP: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* amtData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string amt = amtData ? bpResolveInt(amtData) : "1";
                    f << "    afn_hp[" << obj << "] -= " << amt << ";\n";
                    f << "    if (afn_hp[" << obj << "] < 0) afn_hp[" << obj << "] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::AddScore: {
                    auto* amtData = bpFindDataIn(action->id, 0);
                    std::string amt = amtData ? bpResolveInt(amtData) : "1";
                    f << "    afn_score += " << amt << ";\n";
                    break;
                }
                case GBAScriptNodeType::Respawn:
                    f << "    player_x = afn_start_x; player_y = afn_start_y; player_z = afn_start_z;\n";
                    f << "    player_vy = 0;\n";
                    break;
                case GBAScriptNodeType::SaveData:
                    f << "    afn_sram_save();\n";
                    break;
                case GBAScriptNodeType::LoadData:
                    f << "    afn_sram_load();\n";
                    break;
                case GBAScriptNodeType::FlipSprite: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* flipData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string flip = flipData ? bpResolveInt(flipData) : "1";
                    f << "    afn_sprite_flip[" << obj << "] = " << flip << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetDrawDist: {
                    auto* distData = bpFindDataIn(action->id, 0);
                    std::string dist = distData ? bpResolveInt(distData) : "0";
                    f << "    afn_draw_distance = " << dist << ";\n";
                    break;
                }
                case GBAScriptNodeType::EnableCollision: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* enData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string en = enData ? bpResolveInt(enData) : "1";
                    f << "    afn_collision_enabled[" << obj << "] = " << en << ";\n";
                    break;
                }
                case GBAScriptNodeType::CustomCode:
                    break; // handled by customCode[0] check above
                case GBAScriptNodeType::Countdown: {
                    auto* cntData = bpFindDataIn(action->id, 0);
                    std::string cnt = cntData ? bpResolveInt(cntData) : "60";
                    f << "    { static int afn_cd_" << action->id << " = " << cnt << ";\n";
                    f << "      if (--afn_cd_" << action->id << " <= 0) { afn_cd_" << action->id << " = " << cnt << ";\n";
                    break;
                }
                case GBAScriptNodeType::ResetTimer:
                    f << "    // reset countdown timers (handled by reinit)\n";
                    break;
                case GBAScriptNodeType::Increment: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    f << "    afn_vars[" << slot << "]++;\n";
                    break;
                }
                case GBAScriptNodeType::Decrement: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    f << "    afn_vars[" << slot << "]--;\n";
                    break;
                }
                case GBAScriptNodeType::SetFOV: {
                    auto* fovData = bpFindDataIn(action->id, 0);
                    std::string fov = fovData ? bpResolveInt(fovData) : "256";
                    f << "    cam_fov = " << fov << ";\n";
                    break;
                }
                case GBAScriptNodeType::ShakeStop:
                    f << "    afn_shake_frames = 0;\n";
                    f << "    REG_BG_OFS[2].x = 0; REG_BG_OFS[2].y = 0;\n";
                    break;
                case GBAScriptNodeType::LockCamera:
                    f << "    afn_cam_locked = 1;\n";
                    break;
                case GBAScriptNodeType::UnlockCamera:
                    f << "    afn_cam_locked = 0;\n";
                    break;
                case GBAScriptNodeType::SetCamSpeed: {
                    auto* spdData = bpFindDataIn(action->id, 0);
                    std::string spd = spdData ? bpResolveInt(spdData) : "256";
                    f << "    afn_cam_speed = " << spd << ";\n";
                    break;
                }
                case GBAScriptNodeType::ApplyForce: {
                    auto* fxData = bpFindDataIn(action->id, 0);
                    auto* fzData = bpFindDataIn(action->id, 1);
                    std::string fx = fxData ? bpResolveInt(fxData) : "0";
                    std::string fz = fzData ? bpResolveInt(fzData) : "0";
                    f << "    afn_force_x += " << fx << ";\n";
                    f << "    afn_force_z += " << fz << ";\n";
                    break;
                }
                case GBAScriptNodeType::Bounce: {
                    auto* dampData = bpFindDataIn(action->id, 0);
                    std::string damp = dampData ? bpResolveFloat(dampData) : "192";
                    f << "    player_vy = -(player_vy * " << damp << ") >> 8;\n";
                    break;
                }
                case GBAScriptNodeType::SetFriction: {
                    auto* fData = bpFindDataIn(action->id, 0);
                    std::string fr = fData ? bpResolveFloat(fData) : "230";
                    f << "    afn_friction = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::CloneSprite: {
                    auto* srcData = bpFindDataIn(action->id, 0);
                    std::string src = srcData ? bpResolveInt(srcData) : "0";
                    f << "    afn_clone_sprite(" << src << ");\n";
                    break;
                }
                case GBAScriptNodeType::HideAll:
                    f << "    { int i; for (i=0;i<16;i++) afn_sprite_visible[i]=0; }\n";
                    break;
                case GBAScriptNodeType::ShowAll:
                    f << "    { int i; for (i=0;i<16;i++) afn_sprite_visible[i]=1; }\n";
                    break;
                case GBAScriptNodeType::Print: {
                    auto* valData = bpFindDataIn(action->id, 0);
                    std::string val = valData ? bpResolveInt(valData) : "0";
                    f << "    mgba_printf(\"val=%d\", " << val << ");\n";
                    break;
                }
                case GBAScriptNodeType::SetColor: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* colData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string col = colData ? bpResolveInt(colData) : "0";
                    f << "    g_sprites[" << obj << "].pal = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::SwapSprite: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* assetData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string asset = assetData ? bpResolveInt(assetData) : "0";
                    f << "    g_sprites[" << obj << "].asset = " << asset << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetSpriteY: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* yData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    f << "    g_sprites[" << obj << "].wy = " << y << " << 8;\n";
                    break;
                }
                case GBAScriptNodeType::StopAll:
                    f << "    afn_scripts_stopped = 1;\n";
                    break;
                case GBAScriptNodeType::SetLayer: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* layerData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string layer = layerData ? bpResolveInt(layerData) : "0";
                    f << "    afn_sprite_layer[" << obj << "] = " << layer << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetAlpha: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* alphaData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string alpha = alphaData ? bpResolveInt(alphaData) : "16";
                    f << "    afn_sprite_alpha[" << obj << "] = " << alpha << ";\n";
                    break;
                }
                case GBAScriptNodeType::Flash: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* framesData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string frames = framesData ? bpResolveInt(framesData) : "8";
                    f << "    afn_flash_obj[" << obj << "] = " << frames << ";\n";
                    break;
                }
                case GBAScriptNodeType::Delay:
                    f << "    { static int afn_dly_" << action->id << " = 0;\n";
                    f << "      if (afn_dly_" << action->id << " > 0) { afn_dly_" << action->id << "--; return; } }\n";
                    break;
                case GBAScriptNodeType::SetSpriteScale: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* scaleData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string scale = scaleData ? bpResolveFloat(scaleData) : "256";
                    f << "    g_sprites[" << obj << "].scale = " << scale << ";\n";
                    break;
                }
                case GBAScriptNodeType::RotateSprite: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* angData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string ang = angData ? bpResolveInt(angData) : "0";
                    f << "    afn_sprite_rot[" << obj << "] = " << ang << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetHP2: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* hpData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string hp = hpData ? bpResolveInt(hpData) : "100";
                    f << "    afn_hp[" << obj << "] = " << hp << ";\n";
                    f << "    if (afn_hp[" << obj << "] > afn_max_hp[" << obj << "]) afn_hp[" << obj << "] = afn_max_hp[" << obj << "];\n";
                    break;
                }
                case GBAScriptNodeType::HealHP: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* amtData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string amt = amtData ? bpResolveInt(amtData) : "1";
                    f << "    afn_hp[" << obj << "] += " << amt << ";\n";
                    f << "    if (afn_hp[" << obj << "] > afn_max_hp[" << obj << "]) afn_hp[" << obj << "] = afn_max_hp[" << obj << "];\n";
                    break;
                }
                case GBAScriptNodeType::SetMaxHP: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* maxData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string maxhp = maxData ? bpResolveInt(maxData) : "100";
                    f << "    afn_max_hp[" << obj << "] = " << maxhp << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetBGColor: {
                    auto* colData = bpFindDataIn(action->id, 0);
                    std::string col = colData ? bpResolveInt(colData) : "0";
                    f << "    afn_bg_color = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::FacePlayer: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    f << "    afn_sprite_rot[" << obj << "] = ArcTan2(player_z - g_sprites[" << obj << "].wz, player_x - g_sprites[" << obj << "].wx);\n";
                    break;
                }
                case GBAScriptNodeType::MoveForward: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* spdData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string spd = spdData ? bpResolveInt(spdData) : "1";
                    f << "    { int ang = afn_sprite_rot[" << obj << "];\n";
                    f << "      g_sprites[" << obj << "].wx += (lu_cos(ang) * " << spd << ") >> 12;\n";
                    f << "      g_sprites[" << obj << "].wz -= (lu_sin(ang) * " << spd << ") >> 12; }\n";
                    break;
                }
                case GBAScriptNodeType::ChasePlayer: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* spdData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string spd = spdData ? bpResolveInt(spdData) : "1";
                    f << "    { FIXED dx = player_x - g_sprites[" << obj << "].wx;\n";
                    f << "      FIXED dz = player_z - g_sprites[" << obj << "].wz;\n";
                    f << "      FIXED adx = dx<0?-dx:dx; FIXED adz = dz<0?-dz:dz;\n";
                    f << "      int dist = (adx>adz)?adx+(adz>>1):adz+(adx>>1);\n";
                    f << "      if (dist > 0) { g_sprites[" << obj << "].wx += (dx * " << spd << ") / (dist>>8);\n";
                    f << "        g_sprites[" << obj << "].wz += (dz * " << spd << ") / (dist>>8); } }\n";
                    break;
                }
                case GBAScriptNodeType::FleePlayer: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* spdData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string spd = spdData ? bpResolveInt(spdData) : "1";
                    f << "    { FIXED dx = g_sprites[" << obj << "].wx - player_x;\n";
                    f << "      FIXED dz = g_sprites[" << obj << "].wz - player_z;\n";
                    f << "      FIXED adx = dx<0?-dx:dx; FIXED adz = dz<0?-dz:dz;\n";
                    f << "      int dist = (adx>adz)?adx+(adz>>1):adz+(adx>>1);\n";
                    f << "      if (dist > 0) { g_sprites[" << obj << "].wx += (dx * " << spd << ") / (dist>>8);\n";
                    f << "        g_sprites[" << obj << "].wz += (dz * " << spd << ") / (dist>>8); } }\n";
                    break;
                }
                case GBAScriptNodeType::FollowPlayer: {
                    // Mode 0: activate the generic breadcrumb-trail follow system in main.c
                    auto* distData = bpFindDataIn(action->id, 1);
                    auto* speedData = bpFindDataIn(action->id, 2);
                    std::string dist = distData ? bpResolveInt(distData) : "0";
                    std::string speed = speedData ? bpResolveInt(speedData) : "0";
                    f << "    if (!tm_fol_active) {\n";
                    f << "      tm_fol_obj = afn_bp_cur_tm_obj;\n";
                    f << "      tm_fol_prev_ptx = tm_player_tx;\n";
                    f << "      tm_fol_prev_pty = tm_player_ty;\n";
                    f << "      tm_fol_trail_count = 0;\n";
                    f << "      tm_fol_trail_head = 0;\n";
                    f << "      tm_fol_active = 1;\n";
                    f << "      if (afn_bp_cur_tm_obj >= 0 && afn_bp_cur_tm_obj < TM_MAX_DIR_OBJS) {\n";
                    f << "        tm_obj_dir_set[afn_bp_cur_tm_obj] = -1;\n";
                    f << "        tm_obj_dir_facing[afn_bp_cur_tm_obj] = -1;\n";
                    f << "      }\n";
                    f << "    }\n";
                    f << "    tm_fol_dist = " << dist << ";\n";
                    f << "    tm_fol_speed = " << speed << ";\n";
                    break;
                }
                case GBAScriptNodeType::IsFollowMoving:
                    // Gate node — handled inline in bpEmitActionsWithGates
                    break;
                case GBAScriptNodeType::SetFollowFacing: {
                    f << "    if (tm_fol_active && tm_fol_obj >= 0) {\n";
                    f << "      tm_obj_facing[tm_fol_obj] = tm_fol_facing;\n";
                    f << "    }\n";
                    break;
                }
                case GBAScriptNodeType::SetAI: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* modeData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string mode = modeData ? bpResolveInt(modeData) : "0";
                    f << "    afn_ai_mode[" << obj << "] = " << mode << ";\n";
                    break;
                }
                case GBAScriptNodeType::EmitParticle: {
                    auto* typeData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* zData = bpFindDataIn(action->id, 2);
                    std::string type = typeData ? bpResolveInt(typeData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string z = zData ? bpResolveInt(zData) : "0";
                    f << "    afn_emit_particle(" << type << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::SetTint: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* colData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string col = colData ? bpResolveInt(colData) : "0";
                    f << "    afn_sprite_tint[" << obj << "] = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::Shake: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* frData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string fr = frData ? bpResolveInt(frData) : "8";
                    f << "    afn_sprite_shake[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetText: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    auto* valData = bpFindDataIn(action->id, 1);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    std::string val = valData ? bpResolveInt(valData) : "0";
                    f << "    afn_hud_value[" << slot << "] = " << val << ";\n";
                    break;
                }
                case GBAScriptNodeType::ShowHUD: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    f << "    afn_hud_visible[" << slot << "] = 1;\n";
                    f << "    afn_elem_idx = " << slot << ";\n";
                    f << "    afn_active_element = " << slot << ";\n";
                    f << "    afn_cursor_stop = 0;\n";
                    f << "    afn_player_frozen = 1;\n";
                    f << "    afn_play_anim = -1;\n";
                    f << "    afn_move_speed = 0;\n";
                    f << "    afn_stop_count = afn_hud_elems[" << slot << "].stopCount;\n";
                    f << "    { int si; for (si = 0; si < afn_stop_count && si < 8; si++) afn_stop_links[si] = afn_hud_stops[afn_hud_elems[" << slot << "].stopStart + si].link; }\n";
                    break;
                }
                case GBAScriptNodeType::HideHUD: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    f << "    afn_hud_visible[" << slot << "] = 0;\n";
                    f << "    afn_player_frozen = 0;\n";
                    f << "    afn_play_anim = 0;\n";
                    break;
                }
                case GBAScriptNodeType::PlayHudAnim: {
                    int layIdx = action->paramInt[0];
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_frame[" << layIdx << "] = 0;\n";
                    f << "    afn_hud_layer_tick[" << layIdx << "] = 0;\n";
                    f << "    afn_hud_layer_active[" << layIdx << "] = 1;\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::StopHudAnim: {
                    int layIdx = action->paramInt[0];
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_active[" << layIdx << "] = 0;\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::SetHudAnimSpeed: {
                    int layIdx = action->paramInt[0];
                    auto* speedData = bpFindDataIn(action->id, 0);
                    std::string spd = speedData ? bpResolveInt(speedData) : "1";
                    f << "#ifdef AFN_HUD_HAS_LAYERS\n";
                    f << "    afn_hud_layer_speed[" << layIdx << "] = " << spd << ";\n";
                    f << "#endif\n";
                    break;
                }
                case GBAScriptNodeType::ArraySet: {
                    auto* idxData = bpFindDataIn(action->id, 0);
                    auto* valData = bpFindDataIn(action->id, 1);
                    std::string idx = idxData ? bpResolveInt(idxData) : "0";
                    std::string val = valData ? bpResolveInt(valData) : "0";
                    f << "    afn_vars[" << idx << " & 15] = " << val << ";\n";
                    break;
                }
                case GBAScriptNodeType::DrawNumber: {
                    auto* valData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* yData = bpFindDataIn(action->id, 2);
                    std::string val = valData ? bpResolveInt(valData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    f << "    afn_draw_number(" << val << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::DrawTextID: {
                    auto* idData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* yData = bpFindDataIn(action->id, 2);
                    std::string id = idData ? bpResolveInt(idData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    f << "    afn_draw_text(" << id << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::ClearText:
                    f << "    afn_clear_text();\n";
                    break;
                case GBAScriptNodeType::SetTextColor: {
                    auto* colData = bpFindDataIn(action->id, 0);
                    std::string col = colData ? bpResolveInt(colData) : "0x7FFF";
                    f << "    afn_text_color = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::AddItem: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    auto* amtData = bpFindDataIn(action->id, 1);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    std::string amt = amtData ? bpResolveInt(amtData) : "1";
                    f << "    afn_inventory[" << slot << " & 15] += " << amt << ";\n";
                    break;
                }
                case GBAScriptNodeType::RemoveItem: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    auto* amtData = bpFindDataIn(action->id, 1);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    std::string amt = amtData ? bpResolveInt(amtData) : "1";
                    f << "    afn_inventory[" << slot << " & 15] -= " << amt << ";\n";
                    f << "    if (afn_inventory[" << slot << " & 15] < 0) afn_inventory[" << slot << " & 15] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::SetItemCount: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    auto* cntData = bpFindDataIn(action->id, 1);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    std::string cnt = cntData ? bpResolveInt(cntData) : "0";
                    f << "    afn_inventory[" << slot << " & 15] = " << cnt << ";\n";
                    break;
                }
                case GBAScriptNodeType::UseItem: {
                    auto* slotData = bpFindDataIn(action->id, 0);
                    std::string slot = slotData ? bpResolveInt(slotData) : "0";
                    f << "    if (afn_inventory[" << slot << " & 15] > 0) afn_inventory[" << slot << " & 15]--;\n";
                    break;
                }
                case GBAScriptNodeType::ShowDialogue: {
                    auto* txtData = bpFindDataIn(action->id, 0);
                    std::string txt = txtData ? bpResolveInt(txtData) : "0";
                    f << "    afn_dlg_text = " << txt << "; afn_dlg_open = 1; afn_dlg_line = 0;\n";
                    break;
                }
                case GBAScriptNodeType::HideDialogue:
                    f << "    afn_dlg_open = 0;\n";
                    break;
                case GBAScriptNodeType::NextLine:
                    f << "    afn_dlg_line++;\n";
                    break;
                case GBAScriptNodeType::SetSpeaker: {
                    auto* spkData = bpFindDataIn(action->id, 0);
                    std::string spk = spkData ? bpResolveInt(spkData) : "0";
                    f << "    afn_dlg_speaker = " << spk << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetState: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* stData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string st = stData ? bpResolveInt(stData) : "0";
                    f << "    afn_prev_state[" << obj << " & 15] = afn_state[" << obj << " & 15];\n";
                    f << "    afn_state[" << obj << " & 15] = " << st << "; afn_state_timer[" << obj << " & 15] = 0;\n";
                    break;
                }
                case GBAScriptNodeType::TransitionState: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* stData = bpFindDataIn(action->id, 1);
                    auto* delData = bpFindDataIn(action->id, 2);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string st = stData ? bpResolveInt(stData) : "0";
                    std::string del = delData ? bpResolveInt(delData) : "0";
                    f << "    afn_prev_state[" << obj << " & 15] = afn_state[" << obj << " & 15];\n";
                    f << "    afn_state[" << obj << " & 15] = " << st << "; afn_state_timer[" << obj << " & 15] = 0;\n";
                    break;
                }
                // Batch 8: Collision / Spawning / UI / Scene / Input
                case GBAScriptNodeType::SetCollisionSize: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* radData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string rad = radData ? bpResolveInt(radData) : "8";
                    f << "    afn_collision_size[" << obj << "] = " << rad << ";\n";
                    break;
                }
                case GBAScriptNodeType::IgnoreCollision: {
                    auto* aData = bpFindDataIn(action->id, 0);
                    auto* bData = bpFindDataIn(action->id, 1);
                    std::string a = aData ? bpResolveInt(aData) : "0";
                    std::string b = bData ? bpResolveInt(bData) : "0";
                    f << "    afn_collision_ignore[" << a << "] = " << b << ";\n";
                    break;
                }
                case GBAScriptNodeType::SpawnAt: {
                    auto* assetData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* zData = bpFindDataIn(action->id, 2);
                    std::string asset = assetData ? bpResolveInt(assetData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string z = zData ? bpResolveInt(zData) : "0";
                    f << "    afn_spawn_sprite(" << asset << ", " << x << " << 8, " << z << " << 8);\n";
                    break;
                }
                case GBAScriptNodeType::DestroyAfter: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* frData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string fr = frData ? bpResolveInt(frData) : "60";
                    f << "    afn_lifetime[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::SpawnProjectile: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* assetData = bpFindDataIn(action->id, 1);
                    auto* spdData = bpFindDataIn(action->id, 2);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string asset = assetData ? bpResolveInt(assetData) : "0";
                    std::string spd = spdData ? bpResolveInt(spdData) : "4";
                    f << "    afn_spawn_projectile(" << obj << ", " << asset << ", " << spd << ");\n";
                    break;
                }
                case GBAScriptNodeType::SetLifetime: {
                    auto* objData = bpFindDataIn(action->id, 0);
                    auto* frData = bpFindDataIn(action->id, 1);
                    std::string obj = objData ? bpResolveInt(objData) : "0";
                    std::string fr = frData ? bpResolveInt(frData) : "60";
                    f << "    afn_lifetime[" << obj << "] = " << fr << ";\n";
                    break;
                }
                case GBAScriptNodeType::DrawBar: {
                    auto* xData = bpFindDataIn(action->id, 0);
                    auto* yData = bpFindDataIn(action->id, 1);
                    auto* wData = bpFindDataIn(action->id, 2);
                    auto* fillData = bpFindDataIn(action->id, 3);
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    std::string w = wData ? bpResolveInt(wData) : "32";
                    std::string fill = fillData ? bpResolveInt(fillData) : "0";
                    f << "    afn_draw_bar(" << x << ", " << y << ", " << w << ", " << fill << ");\n";
                    break;
                }
                case GBAScriptNodeType::DrawSpriteIcon: {
                    auto* assetData = bpFindDataIn(action->id, 0);
                    auto* xData = bpFindDataIn(action->id, 1);
                    auto* yData = bpFindDataIn(action->id, 2);
                    std::string asset = assetData ? bpResolveInt(assetData) : "0";
                    std::string x = xData ? bpResolveInt(xData) : "0";
                    std::string y = yData ? bpResolveInt(yData) : "0";
                    f << "    afn_draw_sprite_icon(" << asset << ", " << x << ", " << y << ");\n";
                    break;
                }
                case GBAScriptNodeType::ShowTimer:
                    f << "    afn_timer_visible = 1;\n";
                    break;
                case GBAScriptNodeType::HideTimer:
                    f << "    afn_timer_visible = 0;\n";
                    break;
                case GBAScriptNodeType::SetBarColor: {
                    auto* barData = bpFindDataIn(action->id, 0);
                    auto* colData = bpFindDataIn(action->id, 1);
                    std::string bar = barData ? bpResolveInt(barData) : "0";
                    std::string col = colData ? bpResolveInt(colData) : "0x7FFF";
                    f << "    afn_bar_color[" << bar << " & 3] = " << col << ";\n";
                    break;
                }
                case GBAScriptNodeType::SetBarMax: {
                    auto* barData = bpFindDataIn(action->id, 0);
                    auto* maxData = bpFindDataIn(action->id, 1);
                    std::string bar = barData ? bpResolveInt(barData) : "0";
                    std::string mx = maxData ? bpResolveInt(maxData) : "100";
                    f << "    afn_bar_max[" << bar << " & 3] = " << mx << ";\n";
                    break;
                }
                case GBAScriptNodeType::ReloadScene:
                    f << "    afn_pending_scene = afn_current_scene;\n";
                    break;
                case GBAScriptNodeType::SetCheckpoint:
                    f << "    afn_checkpoint_x = player_x; afn_checkpoint_z = player_z; afn_checkpoint_set = 1;\n";
                    break;
                case GBAScriptNodeType::LoadCheckpoint:
                    f << "    if (afn_checkpoint_set) { player_x = afn_checkpoint_x; player_z = afn_checkpoint_z; }\n";
                    break;
                case GBAScriptNodeType::FlipFlop:
                    f << "    // FlipFlop: handled inline at dispatch site\n";
                    break;
                case GBAScriptNodeType::CursorUp:
                    f << "    if (afn_cursor_stop > 0) afn_cursor_stop--;\n";
                    f << "    else afn_cursor_stop = afn_stop_count - 1;\n";
                    break;
                case GBAScriptNodeType::CursorDown:
                    f << "    afn_cursor_stop++;\n";
                    f << "    if (afn_cursor_stop >= afn_stop_count) afn_cursor_stop = 0;\n";
                    break;
                case GBAScriptNodeType::FollowLink:
                    f << "    { int link = afn_stop_links[afn_cursor_stop];\n";
                    f << "      if (link >= 0) { afn_hud_visible[afn_elem_idx] = 0; afn_hud_visible[link] = 1; afn_active_element = link; } }\n";
                    break;
                default:
                    f << "    // unsupported bp action: type " << (int)action->type << "\n";
                    break;
                }
            };

            // First pass: emit each blueprint action as its own function (deduplicate by node ID)
            auto bpIsGateNode = [](GBAScriptNodeType t) {
                return t == GBAScriptNodeType::IsMoving || t == GBAScriptNodeType::IsOnGround || t == GBAScriptNodeType::IsJumping
                    || t == GBAScriptNodeType::IsFlagSet || t == GBAScriptNodeType::IsHPZero || t == GBAScriptNodeType::IsNear
                    || t == GBAScriptNodeType::Countdown || t == GBAScriptNodeType::IsAlive
                    || t == GBAScriptNodeType::HasItem || t == GBAScriptNodeType::IsDialogueOpen
                    || t == GBAScriptNodeType::IsInState || t == GBAScriptNodeType::IsColliding
                    || t == GBAScriptNodeType::IsTrue || t == GBAScriptNodeType::FlipFlop
                    || t == GBAScriptNodeType::CheckFlag || t == GBAScriptNodeType::IsNear2D
                    || t == GBAScriptNodeType::IsFollowMoving;
            };
            std::set<int> bpEmittedIds;
            for (auto& c : bpChains) {
                for (auto* a : c.actions) {
                    if (bpIsGateNode(a->type)) continue;
                    if (bpEmittedIds.count(a->id)) continue;
                    bpEmittedIds.insert(a->id);
                    const char* fname = a->funcName[0] ? a->funcName : nullptr;
                    char defaultName[64];
                    if (!fname) {
                        snprintf(defaultName, sizeof(defaultName), "afn_bp%d%s_%d", bi, actionSuffix(a->type), a->id);
                        fname = defaultName;
                    }
                    bool hasDataOut = (a->type == GBAScriptNodeType::CustomCode && a->ccDataOut > 0);
                    if (hasDataOut)
                        f << "static inline int " << fname << "(" << paramSig << ") {\n";
                    else
                        f << "static inline void " << fname << "(" << paramSig << ") {\n";
                    bpEmitActionBody(a);
                    if (hasDataOut)
                        f << "    return 0; // default if no explicit return\n";
                    f << "}\n";
                }
            }

            // Emit functions for FlipFlop branch actions not in main chains
            for (auto& c : bpChains) {
                for (auto* a : c.actions) {
                    if (a->type != GBAScriptNodeType::FlipFlop && a->type != GBAScriptNodeType::CheckFlag) continue;
                    for (int pin = 0; pin < 2; pin++) {
                        auto branch = bpCollectBranch(a->id, pin);
                        for (auto* ba : branch) {
                            if (bpIsGateNode(ba->type)) continue;
                            if (bpEmittedIds.count(ba->id)) continue;
                            bpEmittedIds.insert(ba->id);
                            const char* fname2 = ba->funcName[0] ? ba->funcName : nullptr;
                            char dn2[64];
                            if (!fname2) { snprintf(dn2, sizeof(dn2), "afn_bp%d%s_%d", bi, actionSuffix(ba->type), ba->id); fname2 = dn2; }
                            f << "static inline void " << fname2 << "(" << paramSig << ") {\n";
                            bpEmitActionBody(ba);
                            f << "}\n";
                        }
                    }
                }
            }

            // Emit standalone CustomCode data-out nodes not already in action chains
            for (auto& n : bpScript.nodes) {
                if (n.type != GBAScriptNodeType::CustomCode) continue;
                if (n.ccDataOut <= 0) continue;
                if (bpEmittedIds.count(n.id)) continue;
                bpEmittedIds.insert(n.id);
                const char* fname = n.funcName[0] ? n.funcName : nullptr;
                char defaultName[64];
                if (!fname) {
                    snprintf(defaultName, sizeof(defaultName), "afn_bp%d_custom_%d", bi, n.id);
                    fname = defaultName;
                }
                f << "static inline int " << fname << "(" << paramSig << ") {\n";
                bpEmitActionBody(&n);
                f << "    return 0;\n}\n";
            }

            // Helper: emit a call to a blueprint action's function
            auto bpEmitActionCall = [&](const GBAScriptNodeExport* a) {
                char callName[64];
                if (a->funcName[0]) {
                    snprintf(callName, sizeof(callName), "%s", a->funcName);
                } else {
                    snprintf(callName, sizeof(callName), "afn_bp%d%s_%d", bi, actionSuffix(a->type), a->id);
                }
                f << "    " << callName << "(" << paramArgs << ");\n";
            };

            // Emit blueprint actions with IsMoving gate support
            auto bpEmitActionsWithGates = [&](const std::vector<const GBAScriptNodeExport*>& actions) {
                int gateDepth = 0;
                for (auto* a : actions) {
                    if (a->type == GBAScriptNodeType::IsMoving) {
                        f << "    if (player_moving) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsOnGround) {
                        f << "    if (player_on_ground) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsJumping) {
                        f << "    if (!player_on_ground && player_vy > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsFlagSet) {
                        f << "    if (afn_flags & (1u << " << a->paramInt[0] << ")) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsHPZero) {
                        f << "    if (afn_hp[" << a->paramInt[0] << "] == 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsNear) {
                        f << "    { FIXED dx = g_sprites[" << a->paramInt[0] << "].wx - g_sprites[" << a->paramInt[1] << "].wx;\n";
                        f << "      FIXED dz = g_sprites[" << a->paramInt[0] << "].wz - g_sprites[" << a->paramInt[1] << "].wz;\n";
                        f << "      if (dx<0) dx=-dx; if (dz<0) dz=-dz;\n";
                        f << "      if (((dx>dz)?dx+(dz>>1):dz+(dx>>1))>>8 < " << a->paramInt[2] << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::Countdown) {
                        f << "    { static int afn_cd_" << a->id << " = " << a->paramInt[0] << ";\n";
                        f << "      if (--afn_cd_" << a->id << " <= 0) { afn_cd_" << a->id << " = " << a->paramInt[0] << ";\n";
                        gateDepth += 2;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsAlive) {
                        f << "    if (afn_hp[" << a->paramInt[0] << "] > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::HasItem) {
                        f << "    if (afn_inventory[" << a->paramInt[0] << " & 15] > 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsDialogueOpen) {
                        f << "    if (afn_dlg_open) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsInState) {
                        f << "    if (afn_state[" << a->paramInt[0] << " & 15] == " << a->paramInt[1] << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsColliding) {
                        f << "    { FIXED dx = g_sprites[" << a->paramInt[0] << "].wx - g_sprites[" << a->paramInt[1] << "].wx;\n";
                        f << "      FIXED dz = g_sprites[" << a->paramInt[0] << "].wz - g_sprites[" << a->paramInt[1] << "].wz;\n";
                        f << "      if (dx<0) dx=-dx; if (dz<0) dz=-dz;\n";
                        f << "      int cr = afn_collision_size[" << a->paramInt[0] << "] + afn_collision_size[" << a->paramInt[1] << "];\n";
                        f << "      if (cr <= 0) cr = 16;\n";
                        f << "      if (((dx>dz)?dx+(dz>>1):dz+(dx>>1))>>8 < cr) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsTrue) {
                        auto* valData = bpFindDataIn(a->id, 0);
                        std::string val = valData ? bpResolveInt(valData) : "0";
                        f << "    if (" << val << ") {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsNear2D) {
                        f << "    if (afn_collided_tm_obj == afn_bp_cur_tm_obj && afn_bp_cur_tm_obj >= 0) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::IsFollowMoving) {
                        f << "    if (tm_fol_moving) {\n";
                        gateDepth++;
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::CheckFlag) {
                        auto* flagData = bpFindDataIn(a->id, 0);
                        std::string flag = flagData ? bpResolveInt(flagData) : std::to_string(a->paramInt[0]);
                        auto brSet = bpCollectBranch(a->id, 0);
                        auto brClear = bpCollectBranch(a->id, 1);
                        f << "    if (afn_flags & (1u << " << flag << ")) {\n";
                        for (auto* a2 : brSet) {
                            if (bpIsGateNode(a2->type)) {
                                if (a2->type == GBAScriptNodeType::IsFollowMoving)
                                    f << "    if (tm_fol_moving) {\n";
                                else if (a2->type == GBAScriptNodeType::IsMoving)
                                    f << "    if (player_moving) {\n";
                                else if (a2->type == GBAScriptNodeType::IsOnGround)
                                    f << "    if (player_on_ground) {\n";
                                else if (a2->type == GBAScriptNodeType::IsFlagSet)
                                    f << "    if (afn_flags & (1u << " << a2->paramInt[0] << ")) {\n";
                                else bpEmitActionCall(a2);
                                gateDepth++;
                            } else bpEmitActionCall(a2);
                        }
                        for (int g2 = 0; g2 < gateDepth; g2++) f << "    }\n";
                        gateDepth = 0;
                        if (!brClear.empty()) {
                            f << "    } else {\n";
                            for (auto* a2 : brClear) bpEmitActionCall(a2);
                        }
                        f << "    }\n";
                        continue;
                    }
                    if (a->type == GBAScriptNodeType::FlipFlop) {
                        f << "    { static int afn_ff_" << a->id << " = 0;\n";
                        f << "      afn_ff_" << a->id << " = !afn_ff_" << a->id << ";\n";
                        auto brA = bpCollectBranch(a->id, 0);
                        auto brB = bpCollectBranch(a->id, 1);
                        f << "      if (afn_ff_" << a->id << ") {\n";
                        for (auto* a2 : brA) bpEmitActionCall(a2);
                        f << "      } else {\n";
                        for (auto* a2 : brB) bpEmitActionCall(a2);
                        f << "      } }\n";
                        continue;
                    }
                    bpEmitActionCall(a);
                }
                for (int g = 0; g < gateDepth; g++)
                    f << "    }\n";
            };

            auto bpEmitKeyBlock = [&](const BpChain& c, const char* keyCheck) {
                // Collect all keys connected to this event node
                std::vector<int> keys;
                for (auto& lk : bpScript.links)
                    if (lk.toNodeId == c.event->id && lk.toPinType == 3 && lk.toPinIdx == 0) {
                        auto* dn = bpFindNode(lk.fromNodeId);
                        if (dn) keys.push_back(dn->paramInt[0]);
                    }
                if (keys.empty()) keys.push_back(c.event->paramInt[0]);

                if (keys.size() == 1) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), keyCheck, bpResolveKeyName(keys[0]));
                    f << "  " << buf << " {\n";
                    bpEmitActionsWithGates(c.actions);
                    f << "  }\n";
                } else {
                    // Multiple keys: OR them together
                    std::string kc(keyCheck);
                    std::string condFmt = kc.substr(4, kc.size() - 5);
                    f << "  if (";
                    for (size_t ki = 0; ki < keys.size(); ki++) {
                        if (ki > 0) f << " || ";
                        char buf[64];
                        snprintf(buf, sizeof(buf), condFmt.c_str(), bpResolveKeyName(keys[ki]));
                        f << buf;
                    }
                    f << ") {\n";
                    bpEmitActionsWithGates(c.actions);
                    f << "  }\n";
                }
            };

            // Emit the 5 functions for this blueprint
            f << "static inline void afn_bp" << bi << "_start(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnStart)
                    bpEmitActionsWithGates(c.actions);
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_key_held(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnKeyHeld)
                    bpEmitKeyBlock(c, "if (key_is_down(%s))");
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_key_pressed(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnKeyPressed)
                    bpEmitKeyBlock(c, "if (key_hit(%s))");
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_key_released(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnKeyReleased)
                    bpEmitKeyBlock(c, "if (key_released(%s))");
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_update(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnUpdate)
                    bpEmitActionsWithGates(c.actions);
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_collision(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnCollision)
                    bpEmitActionsWithGates(c.actions);
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_collision2d(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnCollision2D)
                    bpEmitActionsWithGates(c.actions);
            f << "}\n\n";
        }

        // Instance dispatch table
        if (!bpInstances.empty()) {
            int maxParams = 0;
            for (auto& bp : blueprints) maxParams = std::max(maxParams, (int)bp.params.size());
            if (maxParams < 1) maxParams = 1;

            f << "static const struct { int bpIdx; int sprIdx; int tmObjIdx; int sceneMode; unsigned int sceneMask; int params[" << maxParams << "]; }\n";
            f << "    afn_bp_instances[" << (int)bpInstances.size() << "] = {\n";
            for (int ii = 0; ii < (int)bpInstances.size(); ii++) {
                const auto& inst = bpInstances[ii];
                f << "    {" << inst.blueprintIdx << ", " << inst.spriteIdx << ", " << inst.tmObjIdx << ", " << inst.sceneMode << ", 0x" << std::hex << inst.sceneMask << std::dec << "u, {";
                for (int pi = 0; pi < maxParams; pi++) {
                    if (pi > 0) f << ",";
                    f << ((pi < inst.paramCount) ? inst.paramValues[pi] : 0);
                }
                f << "}},\n";
            }
            f << "};\n\n";

            // Dispatch functions — only run instances matching the current scene mode + scene index
            f << "static inline void afn_bp_dispatch_start(void) {\n";
            f << "  for (int i = 0; i < " << (int)bpInstances.size() << "; i++) {\n";
            f << "    if (afn_bp_instances[i].sceneMode != afn_current_mode) continue;\n";
            f << "    if (afn_bp_instances[i].sceneMask != 0xFFFFFFFFu && !(afn_bp_instances[i].sceneMask & (1u << tm_scene_idx))) continue;\n";
            f << "    switch (afn_bp_instances[i].bpIdx) {\n";
            for (int bi = 0; bi < (int)blueprints.size(); bi++) {
                int pc = (int)blueprints[bi].params.size();
                f << "    case " << bi << ": afn_bp" << bi << "_start(";
                for (int pi = 0; pi < pc; pi++) { if (pi > 0) f << ","; f << "afn_bp_instances[i].params[" << pi << "]"; }
                f << "); break;\n";
            }
            f << "    }\n  }\n}\n";

            auto emitDispatch = [&](const char* funcName, const char* suffix) {
                f << "static inline void afn_bp_dispatch_" << suffix << "(void) {\n";
                f << "  for (int i = 0; i < " << (int)bpInstances.size() << "; i++) {\n";
                f << "    if (afn_bp_instances[i].sceneMode != afn_current_mode) continue;\n";
                f << "    if (afn_bp_instances[i].sceneMask != 0xFFFFFFFFu && !(afn_bp_instances[i].sceneMask & (1u << tm_scene_idx))) continue;\n";
                f << "    if (afn_bp_def_frozen[afn_bp_instances[i].bpIdx]) continue;\n";
                f << "    afn_bp_cur_tm_obj = afn_bp_instances[i].tmObjIdx;\n";
                f << "    afn_bp_cur_spr_idx = afn_bp_instances[i].sprIdx;\n";
                f << "    switch (afn_bp_instances[i].bpIdx) {\n";
                for (int bi = 0; bi < (int)blueprints.size(); bi++) {
                    int pc = (int)blueprints[bi].params.size();
                    f << "    case " << bi << ": afn_bp" << bi << "_" << suffix << "(";
                    for (int pi = 0; pi < pc; pi++) { if (pi > 0) f << ","; f << "afn_bp_instances[i].params[" << pi << "]"; }
                    f << "); break;\n";
                }
                f << "    }\n  }\n}\n";
            };
            emitDispatch("key_held", "key_held");
            emitDispatch("key_pressed", "key_pressed");
            emitDispatch("key_released", "key_released");
            emitDispatch("update", "update");

            // Collision dispatch — only fire for matching mode + collided sprite
            f << "static inline void afn_bp_dispatch_collision(void) {\n";
            f << "  for (int i = 0; i < " << (int)bpInstances.size() << "; i++) {\n";
            f << "    if (afn_bp_instances[i].sceneMode != afn_current_mode) continue;\n";
            f << "    if (afn_bp_instances[i].sceneMask != 0xFFFFFFFFu && !(afn_bp_instances[i].sceneMask & (1u << tm_scene_idx))) continue;\n";
            f << "    if (afn_bp_def_frozen[afn_bp_instances[i].bpIdx]) continue;\n";
            f << "    if (afn_bp_instances[i].sprIdx != afn_collided_sprite) continue;\n";
            f << "    switch (afn_bp_instances[i].bpIdx) {\n";
            for (int bi = 0; bi < (int)blueprints.size(); bi++) {
                int pc = (int)blueprints[bi].params.size();
                f << "    case " << bi << ": afn_bp" << bi << "_collision(";
                for (int pi = 0; pi < pc; pi++) { if (pi > 0) f << ","; f << "afn_bp_instances[i].params[" << pi << "]"; }
                f << "); break;\n";
            }
            f << "    }\n  }\n}\n";

            // Collision2D dispatch — only fire for matching mode + collided tilemap object
            f << "static inline void afn_bp_dispatch_collision2d(void) {\n";
            f << "  for (int i = 0; i < " << (int)bpInstances.size() << "; i++) {\n";
            f << "    if (afn_bp_instances[i].sceneMode != afn_current_mode) continue;\n";
            f << "    if (afn_bp_instances[i].sceneMask != 0xFFFFFFFFu && !(afn_bp_instances[i].sceneMask & (1u << tm_scene_idx))) continue;\n";
            f << "    if (afn_bp_def_frozen[afn_bp_instances[i].bpIdx]) continue;\n";
            f << "    if (afn_bp_instances[i].tmObjIdx != afn_collided_tm_obj) continue;\n";
            f << "    switch (afn_bp_instances[i].bpIdx) {\n";
            for (int bi = 0; bi < (int)blueprints.size(); bi++) {
                int pc = (int)blueprints[bi].params.size();
                f << "    case " << bi << ": afn_bp" << bi << "_collision2d(";
                for (int pi = 0; pi < pc; pi++) { if (pi > 0) f << ","; f << "afn_bp_instances[i].params[" << pi << "]"; }
                f << "); break;\n";
            }
            f << "    }\n  }\n}\n";
        }
    }

    if (bpInstances.empty()) {
        f << "static inline void afn_bp_dispatch_start(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_held(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_pressed(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_released(void) {}\n";
        f << "static inline void afn_bp_dispatch_update(void) {}\n";
        f << "static inline void afn_bp_dispatch_collision(void) {}\n";
        f << "static inline void afn_bp_dispatch_collision2d(void) {}\n";
    }

    // ---- Mode 0 Tilemap Data ----
    if (!tmScenes.empty())
    {
        f << "\n// ---- Mode 0 Tilemap ----\n";
        f << "#define AFN_HAS_MODE0 1\n";
        f << "#define AFN_TM_SCENE_COUNT " << (int)tmScenes.size() << "\n\n";

        // Obj struct typedef (shared by all scenes)
        f << "typedef struct { s16 tx,ty; u8 type; s8 assetIdx; u8 camFollow; u8 collision; s8 teleScene; u16 scale8; u8 layer; u8 animPlay; s8 animIdx; u8 facing; } AfnTmObj;\n\n";

        // Emit per-scene data
        for (int si = 0; si < (int)tmScenes.size(); si++)
        {
            const auto& sc = tmScenes[si];
            f << "// Scene " << si << ": " << sc.mapW << "x" << sc.mapH << "\n";
            f << "#define AFN_TM" << si << "_W " << sc.mapW << "\n";
            f << "#define AFN_TM" << si << "_H " << sc.mapH << "\n";
            f << "#define AFN_TM" << si << "_ZOOM " << (int)(sc.zoom * 256.0f) << "\n";
            f << "#define AFN_TM" << si << "_TILE_SIZE " << (8 * sc.pixelScale) << "\n";
            f << "#define AFN_TM" << si << "_LOGICAL_W " << (sc.mapW / sc.pixelScale) << "\n";
            f << "#define AFN_TM" << si << "_LOGICAL_H " << (sc.mapH / sc.pixelScale) << "\n";
            // BG size: 0=32x32, 1=64x32, 2=32x64, 3=64x64
            int bgSz = 0;
            if (sc.mapW > 32 && sc.mapH > 32) bgSz = 3;
            else if (sc.mapW > 32) bgSz = 1;
            else if (sc.mapH > 32) bgSz = 2;
            f << "#define AFN_TM" << si << "_BG_SIZE " << bgSz << "\n";

            // BG palette -> RGB15 (256 entries = 16 banks of 16 colors)
            f << "static const u16 afn_tm" << si << "_pal[256] = {\n";
            for (int bank = 0; bank < 16; bank++)
            {
                f << "    ";
                for (int pi = 0; pi < 16; pi++)
                {
                    uint32_t c = sc.palette[bank * 16 + pi];
                    int r = (c & 0xFF) >> 3;
                    int g = ((c >> 8) & 0xFF) >> 3;
                    int b = ((c >> 16) & 0xFF) >> 3;
                    uint16_t rgb15 = (uint16_t)(r | (g << 5) | (b << 10));
                    f << "0x" << std::hex << rgb15 << std::dec;
                    if (bank < 15 || pi < 15) f << ",";
                }
                f << "\n";
            }
            f << "};\n";

            // Tile pixel data -> 4bpp packed (each tile = 32 bytes = 8 u32)
            int nTiles = sc.tileCount;
            if (nTiles < 1) nTiles = 1;
            f << "#define AFN_TM" << si << "_TILE_COUNT " << nTiles << "\n";
            int tileU32Count = nTiles * 8; // 8 u32 per 4bpp tile
            f << "static const u32 afn_tm" << si << "_tiles[" << tileU32Count << "] = {\n";
            for (int ti = 0; ti < nTiles; ti++)
            {
                f << "    ";
                // Convert 8bpp tile to 4bpp packed u32
                for (int row = 0; row < 8; row++)
                {
                    uint32_t packed = 0;
                    for (int col = 0; col < 8; col++)
                    {
                        int srcIdx = ti * 64 + row * 8 + col;
                        uint8_t pix = (srcIdx < (int)sc.tilePixels.size()) ? (sc.tilePixels[srcIdx] & 0x0F) : 0;
                        packed |= ((uint32_t)pix) << (col * 4);
                    }
                    f << "0x" << std::hex << packed << std::dec;
                    if (row < 7 || ti < nTiles - 1) f << ",";
                }
                f << "\n";
            }
            f << "};\n";
            f << "#define AFN_TM" << si << "_TILES_LEN " << (tileU32Count * 4) << "\n";

            // Tilemap screen entries (tile index in bits 0-9, palette bank in bits 12-15)
            int mapSize = sc.mapW * sc.mapH;
            f << "static const u16 afn_tm" << si << "_map[" << mapSize << "] = {\n    ";
            for (int mi = 0; mi < mapSize; mi++)
            {
                uint16_t idx = (mi < (int)sc.tileIndices.size()) ? sc.tileIndices[mi] : 0;
                f << "0x" << std::hex << idx << std::dec;
                if (mi < mapSize - 1) f << ",";
                if ((mi & 31) == 31 && mi < mapSize - 1) f << "\n    ";
            }
            f << "\n};\n";

            // Objects
            int objCount = (int)sc.objects.size();
            f << "#define AFN_TM" << si << "_OBJ_COUNT " << objCount << "\n";
            {
                // { tileX, tileY, type, spriteAssetIdx, camFollow, teleportScene, scale8 }
                // scale8: 8.8 fixed point (256 = 1.0x, 128 = 0.5x, 64 = 0.25x)
                int arrSize = (objCount > 0) ? objCount : 1;
                f << "static const AfnTmObj "
                  << "afn_tm" << si << "_objs[" << arrSize << "] = {\n";
                for (int oi = 0; oi < objCount; oi++)
                {
                    const auto& obj = sc.objects[oi];
                    int scale8 = (int)(obj.displayScale * 256.0f);
                    if (scale8 < 1) scale8 = 256;
                    f << "    {" << obj.tileX << "," << obj.tileY << ","
                      << obj.type << "," << obj.spriteAssetIdx << ","
                      << (obj.camFollow ? 1 : 0) << "," << (obj.collision ? 1 : 0) << "," << obj.teleportScene << "," << scale8 << "," << obj.layer << "," << (obj.animPlay ? 1 : 0) << "," << obj.animIdx << "," << obj.facing << "},\n";
                }
                if (objCount == 0)
                    f << "    {0,0,0,0,0,0,0,256,0,0,0,0},\n";
                f << "};\n";
            }
            f << "\n";
        }

        // Scene indirection tables for runtime scene switching
        {
            int nsc = (int)tmScenes.size();

            // Palette pointers
            f << "static const u16 * const afn_tm_scene_pal[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "afn_tm" << si << "_pal";
            f << "};\n";

            // Tile data pointers + lengths
            f << "static const u32 * const afn_tm_scene_tiles[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "afn_tm" << si << "_tiles";
            f << "};\n";
            f << "static const int afn_tm_scene_tiles_len[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_TILES_LEN";
            f << "};\n";

            // Map data pointers + dimensions
            f << "static const u16 * const afn_tm_scene_map[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "afn_tm" << si << "_map";
            f << "};\n";
            f << "static const int afn_tm_scene_w[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_W";
            f << "};\n";
            f << "static const int afn_tm_scene_h[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_H";
            f << "};\n";

            // BG size
            f << "static const int afn_tm_scene_bg_size[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_BG_SIZE";
            f << "};\n";

            // Tile size
            f << "static const int afn_tm_scene_tile_size[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_TILE_SIZE";
            f << "};\n";

            // Logical dimensions
            f << "static const int afn_tm_scene_logical_w[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_LOGICAL_W";
            f << "};\n";
            f << "static const int afn_tm_scene_logical_h[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_LOGICAL_H";
            f << "};\n";

            // Object pointers + counts
            f << "static const AfnTmObj * const afn_tm_scene_objs[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "afn_tm" << si << "_objs";
            f << "};\n";
            f << "static const int afn_tm_scene_obj_count[" << nsc << "] = {";
            for (int si = 0; si < nsc; si++) f << (si?",":"") << "AFN_TM" << si << "_OBJ_COUNT";
            f << "};\n\n";
        }

        // Find player object across all scenes (first scene, first Player type)
        int tmPlayerScene = -1, tmPlayerObj = -1;
        for (int si = 0; si < (int)tmScenes.size() && tmPlayerScene < 0; si++)
            for (int oi = 0; oi < (int)tmScenes[si].objects.size(); oi++)
                if (tmScenes[si].objects[oi].type == 0) // Player
                { tmPlayerScene = si; tmPlayerObj = oi; break; }
        f << "#define AFN_TM_PLAYER_SCENE " << tmPlayerScene << "\n";
        f << "#define AFN_TM_PLAYER_OBJ " << tmPlayerObj << "\n";
        f << "#define AFN_TM_START_SCENE 0\n";
    }

    // (HUD Element Data emitted earlier, before script codegen)

    // ---- Scene mode config ----
    // startMode is determined by caller based on active editor tab
    // Fallback: if the requested mode's data doesn't exist, pick what's available
    {
        int sm = startMode;
        if (sm == 1 && tmScenes.empty()) sm = meshes.empty() ? 2 : 0;
        if (sm == 0 && meshes.empty())   sm = tmScenes.empty() ? 2 : 1;
        f << "\n// Runtime scene mode: 0=Mode4/3D, 1=Mode0/tilemap, 2=Mode1/Mode7\n";
        f << "#define AFN_START_MODE " << sm << "\n";
        if (!meshes.empty())
            f << "#define AFN_HAS_MESHES 1\n";
    }

    // ---- Sound / DMA Audio Data ----
    if (!soundSamples.empty()) {
        f << "\n// ---- DMA Audio: PCM Samples ----\n";
        f << "#define AFN_SOUND_SAMPLE_COUNT " << soundSamples.size() << "\n";
        f << "#define AFN_SOUND_INSTANCE_COUNT " << soundInstances.size() << "\n\n";

        // Emit each sample as a const s8 array
        for (int i = 0; i < (int)soundSamples.size(); i++) {
            auto& smp = soundSamples[i];
            f << "static const s8 afn_pcm_" << i << "[] __attribute__((aligned(4))) = {\n    ";
            for (int j = 0; j < (int)smp.data.size(); j++) {
                f << (int)smp.data[j];
                if (j < (int)smp.data.size() - 1) f << ",";
                if ((j & 31) == 31) f << "\n    ";
            }
            f << "\n};\n";
            f << "#define AFN_PCM_" << i << "_LEN " << smp.data.size() << "\n";
            f << "#define AFN_PCM_" << i << "_RATE " << smp.sampleRate << "\n\n";
        }

        // Sample pointer + length table
        f << "static const s8* const afn_pcm_ptrs[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    afn_pcm_" << i << ",\n";
        f << "};\n";
        f << "static const int afn_pcm_lens[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    AFN_PCM_" << i << "_LEN,\n";
        f << "};\n";
        f << "static const int afn_pcm_rates[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    AFN_PCM_" << i << "_RATE,\n";
        f << "};\n";
        f << "#define AFN_PCM_HAS_LOOP 1\n";
        f << "static const u8 afn_pcm_loop[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    " << (soundSamples[i].loop ? 1 : 0) << ",\n";
        f << "};\n";
        // Loop start/end arrays for DLS sustain loops
        f << "static const int afn_pcm_loop_start[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    " << soundSamples[i].loopStart << ",\n";
        f << "};\n";
        f << "static const int afn_pcm_loop_end[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++) {
            int le = soundSamples[i].loopEnd > 0 ? soundSamples[i].loopEnd : (int)soundSamples[i].data.size();
            f << "    " << le << ",\n";
        }
        f << "};\n";
        // Decay percentage per sample (0-100)
        f << "static const u8 afn_pcm_decay[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    " << soundSamples[i].decayPct << ",\n";
        f << "};\n";
        // Minimum note duration for decay (in output samples, pre-converted from ms)
        f << "static const int afn_pcm_decay_min[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    " << (soundSamples[i].decayMinMs * 18157 / 1000) << ",\n";
        f << "};\n\n";

        f << "static const int afn_pcm_release[" << soundSamples.size() << "] = {\n";
        for (int i = 0; i < (int)soundSamples.size(); i++)
            f << "    " << (soundSamples[i].releaseMs * 18157 / 1000) << ",\n";
        f << "};\n\n";

        // Note event struct type definition (must come before note arrays)
        f << "typedef struct { int tick; u8 note; u8 vel; u8 smpIdx; int dur; } AfnSndNote;\n\n";

        // Emit note sequences per instance
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            auto& inst = soundInstances[i];
            if (inst.notes.empty()) continue;
            f << "// Instance " << i << ": " << inst.name << "\n";
            f << "static const AfnSndNote afn_snd_notes_" << i << "[] = {\n";
            for (auto& n : inst.notes) {
                f << "    {" << n.tick << "," << n.note << "," << n.velocity << "," << n.sampleIdx << "," << n.duration << "},\n";
            }
            f << "};\n";
            f << "#define AFN_SND_" << i << "_NOTE_COUNT " << inst.notes.size() << "\n";
            f << "#define AFN_SND_" << i << "_TEMPO " << inst.tempo << "\n";
            f << "#define AFN_SND_" << i << "_TPB " << inst.ticksPerBeat << "\n\n";
        }

        // Instance note pointers + counts + ticks-per-frame table
        f << "static const AfnSndNote* const afn_snd_note_ptrs[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            if (soundInstances[i].notes.empty())
                f << "    0,\n";
            else
                f << "    afn_snd_notes_" << i << ",\n";
        }
        f << "};\n";

        f << "static const int afn_snd_note_counts[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            f << "    " << soundInstances[i].notes.size() << ",\n";
        }
        f << "};\n";

        f << "static const int afn_snd_tpf[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            int tpf = soundInstances[i].tempo * soundInstances[i].ticksPerBeat / 3600;
            if (tpf < 1) tpf = 1;
            f << "    " << tpf << ",\n";
        }
        f << "};\n";

        f << "static const u8 afn_snd_interp[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            f << "    " << soundInstances[i].interpolation << ",\n";
        }
        f << "};\n";

        f << "static const u8 afn_snd_gain[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            f << "    " << soundInstances[i].mixerGain << ",\n";
        }
        f << "};\n\n";

        f << "static const u8 afn_snd_voices[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            f << "    " << soundInstances[i].voiceCount << ",\n";
        }
        f << "};\n";

        f << "static const u8 afn_snd_softfade[" << soundInstances.size() << "] = {\n";
        for (int i = 0; i < (int)soundInstances.size(); i++) {
            f << "    " << soundInstances[i].softFade << ",\n";
        }
        f << "};\n\n";

        f << "#define AFN_HAS_SOUND 1\n";
    }

    // ---- Mode 7 floor texture (8bpp tiles + palette + map) ----
    if (m7FloorPixels && m7FloorW > 0 && m7FloorH > 0) {
        // Quantize RGBA image to 256-color palette (simple popularity)
        // Build frequency map of GBA RGB555 colors
        struct Color15 { uint16_t v; };
        std::unordered_map<uint16_t, int> colorFreq;
        int totalPx = m7FloorW * m7FloorH;
        for (int i = 0; i < totalPx; i++) {
            int r = m7FloorPixels[i * 4 + 0] >> 3;
            int g = m7FloorPixels[i * 4 + 1] >> 3;
            int b = m7FloorPixels[i * 4 + 2] >> 3;
            uint16_t c15 = r | (g << 5) | (b << 10);
            colorFreq[c15]++;
        }
        // Sort by frequency, take top 256
        std::vector<std::pair<uint16_t, int>> sorted(colorFreq.begin(), colorFreq.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
        uint16_t palette[256] = {};
        int palCount = std::min(256, (int)sorted.size());
        std::unordered_map<uint16_t, uint8_t> palLookup;
        for (int i = 0; i < palCount; i++) {
            palette[i] = sorted[i].first;
            palLookup[sorted[i].first] = (uint8_t)i;
        }
        // Map pixels to palette indices (nearest for colors not in top 256)
        std::vector<uint8_t> indexed(totalPx);
        for (int i = 0; i < totalPx; i++) {
            int r = m7FloorPixels[i * 4 + 0] >> 3;
            int g = m7FloorPixels[i * 4 + 1] >> 3;
            int b = m7FloorPixels[i * 4 + 2] >> 3;
            uint16_t c15 = r | (g << 5) | (b << 10);
            auto it = palLookup.find(c15);
            if (it != palLookup.end()) {
                indexed[i] = it->second;
            } else {
                // Find nearest palette entry
                int bestDist = 999999, bestIdx = 0;
                for (int p = 0; p < palCount; p++) {
                    int pr = palette[p] & 31, pg = (palette[p] >> 5) & 31, pb = (palette[p] >> 10) & 31;
                    int dr = r - pr, dg = g - pg, db = b - pb;
                    int d = dr*dr + dg*dg + db*db;
                    if (d < bestDist) { bestDist = d; bestIdx = p; }
                }
                indexed[i] = (uint8_t)bestIdx;
                palLookup[c15] = (uint8_t)bestIdx; // cache
            }
        }
        // Build 8x8 tiles and deduplicate
        int tilesX = m7FloorW / 8;
        int tilesY = m7FloorH / 8;
        std::vector<std::array<uint8_t, 64>> uniqueTiles;
        std::unordered_map<size_t, int> tileHash;
        std::vector<uint8_t> tilemap(tilesX * tilesY, 0);
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                std::array<uint8_t, 64> tile = {};
                for (int py = 0; py < 8; py++)
                    for (int px = 0; px < 8; px++)
                        tile[py * 8 + px] = indexed[(ty * 8 + py) * m7FloorW + (tx * 8 + px)];
                // Hash tile for dedup
                size_t h = 0;
                for (int k = 0; k < 64; k++) h = h * 131 + tile[k];
                auto it = tileHash.find(h);
                if (it != tileHash.end()) {
                    tilemap[ty * tilesX + tx] = (uint8_t)it->second;
                } else {
                    int idx = (int)uniqueTiles.size();
                    if (idx < 256) {
                        tileHash[h] = idx;
                        uniqueTiles.push_back(tile);
                        tilemap[ty * tilesX + tx] = (uint8_t)idx;
                    }
                }
            }
        }
        // Emit tile data
        f << "\n// ---- Mode 7 floor tile data ----\n";
        f << "#define AFFINITY_HAS_MAPDATA\n";
        f << "static const unsigned char afn_tiles[" << uniqueTiles.size() * 64 << "] = {\n";
        for (int t = 0; t < (int)uniqueTiles.size(); t++) {
            f << "    ";
            for (int k = 0; k < 64; k++) {
                f << (int)uniqueTiles[t][k] << ",";
            }
            f << "\n";
        }
        f << "};\n";
        f << "static const unsigned int afn_tilesLen = " << uniqueTiles.size() * 64 << ";\n\n";
        // Emit palette
        f << "static const unsigned short afn_palette[256] = {\n    ";
        for (int i = 0; i < 256; i++) {
            f << "0x" << std::hex << std::setw(4) << std::setfill('0') << palette[i] << std::dec << ",";
            if ((i & 15) == 15) f << "\n    ";
        }
        f << "\n};\n";
        f << "static const unsigned int afn_paletteLen = 512;\n\n";
        // Emit tilemap
        f << "static const unsigned char afn_tilemap[" << tilesX * tilesY << "] = {\n";
        for (int ty = 0; ty < tilesY; ty++) {
            f << "    ";
            for (int tx = 0; tx < tilesX; tx++)
                f << (int)tilemap[ty * tilesX + tx] << ",";
            f << "\n";
        }
        f << "};\n";
        f << "#define AFN_FLOOR_TILES_X " << tilesX << "\n";
        f << "#define AFN_FLOOR_TILES_Y " << tilesY << "\n\n";
    }

    f << "\n#endif // MAPDATA_H\n";
    f.close();
    return true;
}

bool PackageGBA(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                const GBAScriptExport& script,
                const std::vector<GBABlueprintExport>& blueprints,
                const std::vector<GBABlueprintInstanceExport>& bpInstances,
                const std::vector<GBATmSceneExport>& tmScenes,
                const std::vector<GBAHudElementExport>& hudElements,
                const std::vector<GBASoundSampleExport>& soundSamples,
                const std::vector<GBASoundInstanceExport>& soundInstances,
                int startMode,
                std::string& errorMsg,
                const unsigned char* m7FloorPixels,
                int m7FloorW, int m7FloorH)
{
    std::string msysDir = ToMsysPath(runtimeDir);

    // --- Step 0: Kill mGBA if running (so it doesn't lock the .gba file) ---
    {
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        char cmd[] = "taskkill /IM mGBA.exe /F";
        CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 3000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }

    // --- Step 1: Generate mapdata.h with sprite/camera/asset/player data ---
    if (!GenerateMapData(runtimeDir, sprites, assets, camera, meshes, m7FloorPixels, m7FloorW, m7FloorH, orbitDist, script, blueprints, bpInstances, tmScenes, hudElements, soundSamples, soundInstances, startMode))
    {
        errorMsg = "Failed to write mapdata.h";
        return false;
    }

    // --- Step 2: Clean previous build ---
    std::string cleanOut;
    RunDevkitBash("cd " + msysDir + " && make clean", cleanOut);

    // --- Step 3: Build ---
    std::string buildOut;
    int ret = RunDevkitBash("cd " + msysDir + " && make", buildOut);

    if (ret != 0)
    {
        errorMsg = "GBA build failed:\n" + buildOut;
        return false;
    }

    // --- Step 4: Verify .gba exists ---
    fs::path srcPath = fs::path(runtimeDir) / "affinity.gba";
    fs::path dstPath(outputPath);

    if (!fs::exists(srcPath))
    {
        errorMsg = "Build succeeded but affinity.gba not found at: " + srcPath.string();
        return false;
    }

    // If output path differs from source, copy it
    std::error_code ec;
    fs::path srcCanon = fs::canonical(srcPath, ec);
    fs::path dstCanon = fs::canonical(dstPath, ec);
    if (srcCanon != dstCanon)
    {
        ec.clear();
        fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMsg = "Failed to copy ROM: " + ec.message();
            return false;
        }
    }

    // Return build log on success
    errorMsg = buildOut;
    return true;
}

} // namespace Affinity

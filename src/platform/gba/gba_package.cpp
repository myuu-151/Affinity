#include "gba_package.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <set>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
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

static std::vector<uint32_t> FrameToGBATiles(const GBASpriteFrameExport& frame)
{
    int fSize = SnapToOBJSize(frame.width);
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
                            float orbitDist,
                            const GBAScriptExport& script,
                            const std::vector<GBABlueprintExport>& blueprints,
                            const std::vector<GBABlueprintInstanceExport>& bpInstances)
{
    fs::path outPath = fs::path(runtimeDir) / "include" / "mapdata.h";
    std::ofstream f(outPath);
    if (!f.is_open()) return false;

    f << "// Generated by Affinity editor\n";
    f << "#ifndef MAPDATA_H\n";
    f << "#define MAPDATA_H\n\n";
    f << "#define AFFINITY_HAS_SPRITES\n\n";

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
    for (size_t si = 0; si < sprites.size(); si++)
        if (sprites[si].assetIdx >= 0 && sprites[si].assetIdx < (int)assets.size())
            assetReferencedBySprite[sprites[si].assetIdx] = true;

    // Build one combined tile data array: all assets, all frames, packed contiguously
    std::vector<uint32_t> allTiles;
    std::vector<int> assetTileStart;
    std::vector<int> assetTilesPerFrame;

    std::vector<int> assetObjSize; // snapped OBJ size per asset
    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        const auto& asset = assets[ai];
        int objSize = SnapToOBJSize(asset.baseSize);
        assetObjSize.push_back(objSize);
        int tilesPerFrame = (objSize / 8) * (objSize / 8);
        assetTileStart.push_back((int)allTiles.size() / 8);
        assetTilesPerFrame.push_back(tilesPerFrame);

        // Skip static frames for direction-based assets — they use DMA direction tiles
        if (asset.hasDirections && !asset.dirAnimSets.empty()) continue;
        // Skip unreferenced assets — no sprite uses them, saves OBJ VRAM
        if (!assetReferencedBySprite[ai]) continue;

        for (size_t fi = 0; fi < asset.frames.size(); fi++)
        {
            auto td = FrameToGBATiles(asset.frames[fi]);
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
        if (assetReferencedBySprite[ai])
            assetDirInfos[ai].vramTile0 = (int)allTiles.size() / 8 + dirVramNextTile;

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

        // Pass 1: assign banks to independent assets (paletteSrc < 0 or self)
        for (size_t ai = 0; ai < assets.size(); ai++) {
            int src = assets[ai].paletteSrc;
            if (src >= 0 && src < (int)assets.size() && src != (int)ai) continue; // shared — handle later
            int bank = assets[ai].palBank & 15;
            if (bank == 0) bank = 1; // avoid bank 0
            if (!usedBanks[bank]) {
                usedBanks[bank] = true;
            } else {
                // Conflict — find next free bank
                for (int b = 1; b < 16; b++)
                    if (!usedBanks[b]) { bank = b; break; }
                usedBanks[bank] = true;
            }
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
    f << "#define AFN_ALL_TILES_LEN " << (int)allTiles.size() * 4 << "\n\n";

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

    // Emit per-asset palette arrays
    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        f << "static const u16 afn_pal" << ai << "[16] = { ";
        for (int c = 0; c < 16; c++)
        {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", EditorColorToRGB15(assets[ai].palette[c]));
            f << hex;
            if (c < 15) f << ", ";
        }
        f << " };\n";
    }

    // Per-asset direction palettes (emit for all assets; zeros if no dirs)
    if (!assets.empty())
    {
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "static const u16 afn_pal_assetdir" << ai << "[16] = { ";
            for (int c = 0; c < 16; c++)
            {
                char hex[8];
                snprintf(hex, sizeof(hex), "0x%04X", EditorColorToRGB15(assetDirInfos[ai].palette[c]));
                f << hex;
                if (c < 15) f << ", ";
            }
            f << " };\n";
        }
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
            f << "    { " << (assetTileStart[ai] + tileOffset) << ", " << assetTilesPerFrame[ai]
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
                  << cf.flags << " },\n";
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
        f << "static int   afn_pending_scene;\n";
        f << "static int   afn_pending_scene_mode;\n";
        f << "static int   afn_collided_sprite;\n";
        f << "static FIXED afn_gravity;\n";
        f << "static FIXED afn_terminal_vel;\n";
        f << "static FIXED player_vy;\n";
        f << "static int   player_on_ground;\n";
        f << "static u16   orbit_angle;\n\n";
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
        case GBAScriptNodeType::ChangeScene:   return "_change_scene";
        case GBAScriptNodeType::CustomCode:    return "_custom";
        default: return "";
        }
    };

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
            return dn->paramInt[0];
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
                n.type != GBAScriptNodeType::OnCollision)
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

                // Follow this node's exec outputs too (chained actions)
                auto next = findExecOuts(an->id, 0);
                for (int t : next) frontier.push_back(t);
            }
            if (!chain.actions.empty())
                chains.push_back(chain);
        }

        if (!chains.empty())
        {
            f << "// ---- Generated script code from visual node graph ----\n";

            // Emit action body lines for a single action node
            auto emitActionBody = [&](const GBAScriptNodeExport* action) {
                // Use custom code override if set
                if (action->customCode[0]) {
                    f << "    " << action->customCode << "\n";
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
                    if (dir >= 0 && dir < 4)
                        f << "    if (key_is_down(" << dirKeys[dir] << ")) " << dirVars[dir] << ";\n";
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
                    int speed = speedData ? resolveInt(speedData) : 37;
                    int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                    f << "    afn_move_speed = " << gbaSpeed << ";\n";
                    break;
                }
                case GBAScriptNodeType::Sprint: {
                    auto* speedData = findDataIn(action->id, 0);
                    int speed = speedData ? resolveInt(speedData) : 56;
                    int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                    f << "    afn_move_speed = " << gbaSpeed << ";\n";
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
                    int animIdx = animData ? resolveInt(animData) : 0;
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
                case GBAScriptNodeType::CustomCode:
                    break; // handled by customCode[0] check above
                default:
                    f << "    // unsupported action: type " << (int)action->type << "\n";
                    break;
                }
            };

            // First pass: emit each action as its own function
            for (auto& c : chains) {
                for (auto* a : c.actions) {
                    const char* suffix = a->funcName[0] ? a->funcName : nullptr;
                    char defaultName[64];
                    if (!suffix) {
                        snprintf(defaultName, sizeof(defaultName), "afn_script%s_%d", actionSuffix(a->type), a->id);
                        suffix = defaultName;
                    }
                    f << "static inline void " << suffix << "(void) {\n";
                    emitActionBody(a);
                    f << "}\n";
                }
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

            // OnStart → initialization function
            emitFuncStart("afn_script_start", GBAScriptNodeType::OnStart);
            for (auto& c : chains) {
                if (c.event->type != GBAScriptNodeType::OnStart) continue;
                for (auto* a : c.actions)
                    emitActionCall(a);
            }
            f << "}\n";
            emitFuncAlias("afn_script_start", GBAScriptNodeType::OnStart);
            f << "\n";

            // Helper: emit action calls with optional key guard
            auto emitKeyBlock = [&](const EventChain& c, const char* keyCheck) {
                int key = resolveEventKey(*c.event);
                if (key >= 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), keyCheck, resolveKeyName(key));
                    f << "  " << buf << " {\n";
                    for (auto* a : c.actions)
                        emitActionCall(a);
                    f << "  }\n";
                } else {
                    // Ambiguous key — emit actions without guard (actions have own checks)
                    f << "  { // (all d-pad)\n";
                    for (auto* a : c.actions)
                        emitActionCall(a);
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
                for (auto* a : c.actions)
                    emitActionCall(a);
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
                for (auto* a : c.actions)
                    emitActionCall(a);
            }
            if (!hasCollision)
                f << "  (void)0;\n";
            f << "}\n";
            emitFuncAlias("afn_script_collision", GBAScriptNodeType::OnCollision);
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
            f << "static inline void afn_script_collision(void) {}\n\n";
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
        f << "static inline void afn_script_collision(void) {}\n\n";
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
                                n.type == GBAScriptNodeType::OnUpdate || n.type == GBAScriptNodeType::OnCollision);
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
                    for (auto& lk : bpScript.links)
                        if (lk.fromNodeId == an->id && lk.fromPinType == 0) front.push_back(lk.toNodeId);
                }
                if (!chain.actions.empty()) bpChains.push_back(chain);
            }

            // Emit action body lines for blueprint
            auto bpEmitActionBody = [&](const GBAScriptNodeExport* action) {
                if (action->customCode[0]) {
                    f << "    " << action->customCode << "\n";
                    return;
                }
                switch (action->type) {
                case GBAScriptNodeType::MovePlayer: {
                    auto* dirData = bpFindDataIn(action->id, 0);
                    int dir = dirData ? dirData->paramInt[0] : 0;
                    const char* dirKeys[] = { "KEY_LEFT", "KEY_RIGHT", "KEY_UP", "KEY_DOWN" };
                    const char* dirVars[] = { "afn_input_right -= 256", "afn_input_right += 256",
                                              "afn_input_fwd += 256", "afn_input_fwd -= 256" };
                    if (dir >= 0 && dir < 4)
                        f << "    if (key_is_down(" << dirKeys[dir] << ")) " << dirVars[dir] << ";\n";
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
                    std::string animIdx = animData ? bpResolveInt(animData) : "0";
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
                case GBAScriptNodeType::CustomCode:
                    break; // handled by customCode[0] check above
                default:
                    f << "    // unsupported bp action: type " << (int)action->type << "\n";
                    break;
                }
            };

            // First pass: emit each blueprint action as its own function
            for (auto& c : bpChains) {
                for (auto* a : c.actions) {
                    const char* fname = a->funcName[0] ? a->funcName : nullptr;
                    char defaultName[64];
                    if (!fname) {
                        snprintf(defaultName, sizeof(defaultName), "afn_bp%d%s_%d", bi, actionSuffix(a->type), a->id);
                        fname = defaultName;
                    }
                    f << "static inline void " << fname << "(" << paramSig << ") {\n";
                    bpEmitActionBody(a);
                    f << "}\n";
                }
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

            auto bpEmitKeyBlock = [&](const BpChain& c, const char* keyCheck) {
                int key = bpResolveEventKey(*c.event);
                if (key >= 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), keyCheck, bpResolveKeyName(key));
                    f << "  " << buf << " {\n";
                    for (auto* a : c.actions) bpEmitActionCall(a);
                    f << "  }\n";
                } else {
                    f << "  {\n";
                    for (auto* a : c.actions) bpEmitActionCall(a);
                    f << "  }\n";
                }
            };

            // Emit the 5 functions for this blueprint
            f << "static inline void afn_bp" << bi << "_start(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnStart)
                    for (auto* a : c.actions) bpEmitActionCall(a);
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
                    for (auto* a : c.actions) bpEmitActionCall(a);
            f << "}\n";

            f << "static inline void afn_bp" << bi << "_collision(" << paramSig << ") {\n";
            for (auto& c : bpChains)
                if (c.event->type == GBAScriptNodeType::OnCollision)
                    for (auto* a : c.actions) bpEmitActionCall(a);
            f << "}\n\n";
        }

        // Instance dispatch table
        if (!bpInstances.empty()) {
            int maxParams = 0;
            for (auto& bp : blueprints) maxParams = std::max(maxParams, (int)bp.params.size());
            if (maxParams < 1) maxParams = 1;

            f << "static const struct { int bpIdx; int sprIdx; int params[" << maxParams << "]; }\n";
            f << "    afn_bp_instances[" << (int)bpInstances.size() << "] = {\n";
            for (int ii = 0; ii < (int)bpInstances.size(); ii++) {
                const auto& inst = bpInstances[ii];
                f << "    {" << inst.blueprintIdx << ", " << inst.spriteIdx << ", {";
                for (int pi = 0; pi < maxParams; pi++) {
                    if (pi > 0) f << ",";
                    f << ((pi < inst.paramCount) ? inst.paramValues[pi] : 0);
                }
                f << "}},\n";
            }
            f << "};\n\n";

            // Dispatch functions
            f << "static inline void afn_bp_dispatch_start(void) {\n";
            f << "  for (int i = 0; i < " << (int)bpInstances.size() << "; i++) {\n";
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
            emitDispatch("collision", "collision");
        }
    }

    if (bpInstances.empty()) {
        f << "static inline void afn_bp_dispatch_start(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_held(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_pressed(void) {}\n";
        f << "static inline void afn_bp_dispatch_key_released(void) {}\n";
        f << "static inline void afn_bp_dispatch_update(void) {}\n";
        f << "static inline void afn_bp_dispatch_collision(void) {}\n";
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
                std::string& errorMsg)
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
    if (!GenerateMapData(runtimeDir, sprites, assets, camera, meshes, orbitDist, script, blueprints, bpInstances))
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

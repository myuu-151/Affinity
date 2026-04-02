#include "gba_package.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
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
    int outSize = SnapToOBJSize(frame.width);
    const int outTilesPerRow = outSize / 8;
    const int outTotalTiles = outTilesPerRow * outTilesPerRow;
    std::vector<uint32_t> data(outTotalTiles * 8, 0);

#if AFN_DEBUG_TEST_PATTERN
    // Diagnostic: 4-quadrant test pattern with border
    for (int oy = 0; oy < outSize; oy++)
    {
        for (int ox = 0; ox < outSize; ox++)
        {
            uint8_t palIdx;
            int half = outSize / 2;
            if (ox == 0 || ox == outSize - 1 || oy == 0 || oy == outSize - 1)
                palIdx = 5;  // border
            else if (oy < half && ox < half) palIdx = 1;
            else if (oy < half)              palIdx = 2;
            else if (ox < half)              palIdx = 3;
            else                              palIdx = 4;

            int tileIdx = (oy / 8) * outTilesPerRow + (ox / 8);
            int lx = ox & 7;
            int ly = oy & 7;
            int rowIdx = tileIdx * 8 + ly;
            int bit = lx * 4;
            data[rowIdx] |= ((uint32_t)palIdx << bit);
        }
    }
    return data;
#endif

    int fSize = frame.width;
    if (fSize <= 0) return data;

    // Map each output pixel back to source frame pixel (upscale if needed)
    for (int oy = 0; oy < outSize; oy++)
    {
        int sy = oy * fSize / outSize;
        for (int ox = 0; ox < outSize; ox++)
        {
            int sx = ox * fSize / outSize;
            uint8_t palIdx = frame.pixels[sy * kExportMaxFrameSize + sx] & 0xF;
            if (palIdx == 0) continue;

            int tileIdx = (oy / 8) * outTilesPerRow + (ox / 8);
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

static void QuantizePlayerDirSprites(
    const GBAPlayerDirExport dirs[8],
    QuantizedDirFrame outFrames[8],
    uint32_t outPalette[16])
{
    const int dirSize = 64; // quantize direction sprites at 64x64
    memset(outPalette, 0, sizeof(uint32_t) * 16);
    for (int i = 0; i < 8; i++)
        memset(outFrames[i].pixels, 0, sizeof(outFrames[i].pixels));

    // Collect unique colors (as RGB15) from all direction images
    struct ColorFreq { unsigned short rgb15; int count; };
    std::vector<ColorFreq> colorFreqs;

    auto findOrAdd = [&](unsigned short c15) -> int {
        for (size_t i = 0; i < colorFreqs.size(); i++)
            if (colorFreqs[i].rgb15 == c15) { colorFreqs[i].count++; return (int)i; }
        colorFreqs.push_back({c15, 1});
        return (int)colorFreqs.size() - 1;
    };

    for (int d = 0; d < 8; d++)
    {
        if (!dirs[d].pixels || dirs[d].width <= 0 || dirs[d].height <= 0) continue;
        int w = dirs[d].width, h = dirs[d].height;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int idx = (y * w + x) * 4;
                if (dirs[d].pixels[idx + 3] < 128) continue; // transparent
                unsigned r = dirs[d].pixels[idx + 0] >> 3;
                unsigned g = dirs[d].pixels[idx + 1] >> 3;
                unsigned b = dirs[d].pixels[idx + 2] >> 3;
                unsigned short c15 = (unsigned short)(r | (g << 5) | (b << 10));
                findOrAdd(c15);
            }
    }

    // Reduce to 15 colors by merging closest pairs (preserves visually distinct colors)
    while ((int)colorFreqs.size() > 15)
    {
        int bestI = 0, bestJ = 1, bestDist = 999999;
        for (size_t i = 0; i < colorFreqs.size(); i++)
        {
            for (size_t j = i + 1; j < colorFreqs.size(); j++)
            {
                int dr = (int)(colorFreqs[i].rgb15 & 0x1F) - (int)(colorFreqs[j].rgb15 & 0x1F);
                int dg = (int)((colorFreqs[i].rgb15 >> 5) & 0x1F) - (int)((colorFreqs[j].rgb15 >> 5) & 0x1F);
                int db = (int)((colorFreqs[i].rgb15 >> 10) & 0x1F) - (int)((colorFreqs[j].rgb15 >> 10) & 0x1F);
                int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) { bestDist = dist; bestI = (int)i; bestJ = (int)j; }
            }
        }
        // Merge j into i (keep the more frequent color's RGB, sum counts)
        if (colorFreqs[bestI].count < colorFreqs[bestJ].count)
            colorFreqs[bestI].rgb15 = colorFreqs[bestJ].rgb15;
        colorFreqs[bestI].count += colorFreqs[bestJ].count;
        colorFreqs.erase(colorFreqs.begin() + bestJ);
    }

    int palCount = (int)colorFreqs.size();

    // Build palette (index 0 = transparent)
    outPalette[0] = 0;
    for (int i = 0; i < palCount; i++)
    {
        unsigned short c = colorFreqs[i].rgb15;
        unsigned r = (c & 0x1F) << 3;
        unsigned g = ((c >> 5) & 0x1F) << 3;
        unsigned b = ((c >> 10) & 0x1F) << 3;
        outPalette[i + 1] = r | (g << 8) | (b << 16) | 0xFF000000;
    }

    // Nearest palette match helper
    auto nearestPal = [&](unsigned short c15) -> uint8_t {
        int bestDist = 999999;
        uint8_t bestIdx = 1;
        for (int i = 0; i < palCount; i++)
        {
            int dr = (int)(c15 & 0x1F) - (int)(colorFreqs[i].rgb15 & 0x1F);
            int dg = (int)((c15 >> 5) & 0x1F) - (int)((colorFreqs[i].rgb15 >> 5) & 0x1F);
            int db = (int)((c15 >> 10) & 0x1F) - (int)((colorFreqs[i].rgb15 >> 10) & 0x1F);
            int dist = dr * dr + dg * dg + db * db;
            if (dist < bestDist) { bestDist = dist; bestIdx = (uint8_t)(i + 1); }
        }
        return bestIdx;
    };

    // Quantize each direction image to dirSize x dirSize palette-indexed
    for (int d = 0; d < 8; d++)
    {
        if (!dirs[d].pixels || dirs[d].width <= 0 || dirs[d].height <= 0) continue;
        int srcW = dirs[d].width, srcH = dirs[d].height;
        for (int oy = 0; oy < dirSize; oy++)
        {
            int sy = oy * srcH / dirSize;
            for (int ox = 0; ox < dirSize; ox++)
            {
                int sx = ox * srcW / dirSize;
                int idx = (sy * srcW + sx) * 4;
                if (dirs[d].pixels[idx + 3] < 128) continue;
                // Skip edge pixels to prevent stray anti-aliased artifacts
                if (ox == 0 || oy == 0 || ox == dirSize - 1 || oy == dirSize - 1)
                {
                    // Only keep edge pixel if fully opaque (alpha == 255)
                    if (dirs[d].pixels[idx + 3] < 255) continue;
                }
                unsigned r = dirs[d].pixels[idx + 0] >> 3;
                unsigned g = dirs[d].pixels[idx + 1] >> 3;
                unsigned b = dirs[d].pixels[idx + 2] >> 3;
                unsigned short c15 = (unsigned short)(r | (g << 5) | (b << 10));
                outFrames[d].pixels[oy * dirSize + ox] = nearestPal(c15);
            }
        }
    }
}

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
                            const GBAPlayerDirExport playerDirs[8],
                            float orbitDist)
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
    f << "#define AFN_CAM_HORIZON " << (int)camera.horizon << "\n\n";

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

    // Build one combined tile data array: all assets, all frames, packed contiguously
    std::vector<uint32_t> allTiles;
    std::vector<int> assetTileStart;
    std::vector<int> assetTilesPerFrame;

    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        const auto& asset = assets[ai];
        int objSize = SnapToOBJSize(asset.baseSize);
        int tilesPerRow = objSize / 8;
        int tilesPerFrame = tilesPerRow * tilesPerRow;
        assetTileStart.push_back((int)allTiles.size() / 8);
        assetTilesPerFrame.push_back(tilesPerFrame);

        for (size_t fi = 0; fi < asset.frames.size(); fi++)
        {
            auto td = FrameToGBATiles(asset.frames[fi]);
            allTiles.insert(allTiles.end(), td.begin(), td.end());
        }
    }

    // Quantize and append player direction sprites (8 frames x 64 tiles each at 64x64)
    bool hasPlayerDirs = false;
    int playerDirTile0 = 0;
    int playerDirPalBank = 7; // use palette bank 7 for player direction sprites
    int playerDirSize = 64;   // direction sprite size
    QuantizedDirFrame dirFrames[8];
    uint32_t dirPalette[16];

    if (playerDirs)
    {
        // Check if at least one direction image exists
        for (int d = 0; d < 8; d++)
        {
            if (playerDirs[d].pixels && playerDirs[d].width > 0)
            { hasPlayerDirs = true; break; }
        }
    }

    if (hasPlayerDirs)
    {
        QuantizePlayerDirSprites(playerDirs, dirFrames, dirPalette);
        playerDirTile0 = (int)allTiles.size() / 8;

        for (int d = 0; d < 8; d++)
        {
            auto td = RawPixelsToGBATiles(dirFrames[d].pixels, playerDirSize);
            allTiles.insert(allTiles.end(), td.begin(), td.end());
        }
    }

    // Quantize and append per-asset directional sprites (multi-set support)
    // Set 0's tiles go into allTiles (VRAM init). All sets go into dirAnimAllTiles (ROM for DMA).
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
        assetDirInfos[ai].palBank = assets[ai].palBank;

        // Collect ALL unique colors across ALL sets for shared palette
        struct ColorFreq { unsigned short rgb15; int count; };
        std::vector<ColorFreq> colorFreqs;
        auto findOrAdd = [&](unsigned short c15) -> int {
            for (size_t i = 0; i < colorFreqs.size(); i++)
                if (colorFreqs[i].rgb15 == c15) { colorFreqs[i].count++; return (int)i; }
            colorFreqs.push_back({c15, 1});
            return (int)colorFreqs.size() - 1;
        };

        for (size_t si = 0; si < assets[ai].dirAnimSets.size(); si++)
            for (int d = 0; d < 8; d++)
            {
                const auto& img = assets[ai].dirAnimSets[si].dirImages[d];
                if (!img.pixels || img.width <= 0 || img.height <= 0) continue;
                for (int y = 0; y < img.height; y++)
                    for (int x = 0; x < img.width; x++)
                    {
                        int idx = (y * img.width + x) * 4;
                        if (img.pixels[idx + 3] < 128) continue;
                        unsigned r = img.pixels[idx + 0] >> 3;
                        unsigned g = img.pixels[idx + 1] >> 3;
                        unsigned b = img.pixels[idx + 2] >> 3;
                        findOrAdd((unsigned short)(r | (g << 5) | (b << 10)));
                    }
            }

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

        int palCount = (int)colorFreqs.size();
        memset(assetDirInfos[ai].palette, 0, sizeof(assetDirInfos[ai].palette));
        for (int i = 0; i < palCount; i++)
        {
            unsigned short c = colorFreqs[i].rgb15;
            unsigned r = (c & 0x1F) << 3;
            unsigned g = ((c >> 5) & 0x1F) << 3;
            unsigned b = ((c >> 10) & 0x1F) << 3;
            assetDirInfos[ai].palette[i + 1] = r | (g << 8) | (b << 16) | 0xFF000000;
        }

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
        // Share player direction VRAM tiles if player dirs exist (avoids VRAM overflow)
        if (hasPlayerDirs && playerDirTile0 > 0)
            assetDirInfos[ai].vramTile0 = playerDirTile0;
        else
            assetDirInfos[ai].vramTile0 = (int)allTiles.size() / 8;

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
    }

    int totalTileCount = (int)allTiles.size() / 8;
    int minimapTile = totalTileCount;

    // Emit combined tile data
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
        f << "#define AFN_ALL_TILES_LEN " << (int)allTiles.size() * 4 << "\n\n";
    }

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

    // Player direction palette
    if (hasPlayerDirs)
    {
        f << "static const u16 afn_pal_playerdir[16] = { ";
        for (int c = 0; c < 16; c++)
        {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", EditorColorToRGB15(dirPalette[c]));
            f << hex;
            if (c < 15) f << ", ";
        }
        f << " };\n";
        int dirTilesPerFrame = (playerDirSize / 8) * (playerDirSize / 8);
        f << "#define AFN_PLAYER_DIR_TILE0 " << playerDirTile0 << "\n";
        f << "#define AFN_PLAYER_DIR_SIZE " << playerDirSize << "\n";
        f << "#define AFN_PLAYER_DIR_TPF " << dirTilesPerFrame << "\n";
        f << "#define AFN_PLAYER_DIR_PALBANK " << playerDirPalBank << "\n";
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

    // Asset direction descriptor table: { setCount, tpf, dirSize, palBank, hasDirs, vramTile0 }
    if (!assets.empty())
    {
        f << "#define AFN_HAS_ASSET_DIRS 1\n";
        f << "#define AFN_MAX_DIR_SETS 8\n";
        f << "static const int afn_asset_dir_desc[][6] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            if (assetDirInfos[ai].has)
            {
                int tpf = (assetDirInfos[ai].dirSize / 8) * (assetDirInfos[ai].dirSize / 8);
                f << "    { " << assetDirInfos[ai].setCount << ", " << tpf
                  << ", " << assetDirInfos[ai].dirSize
                  << ", " << assetDirInfos[ai].palBank
                  << ", 1, " << assetDirInfos[ai].vramTile0 << " },\n";
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
            for (int si = 0; si < 8; si++)
            {
                if (assetDirInfos[ai].has && si < (int)assetDirInfos[ai].romSetU32Offset.size())
                    f << assetDirInfos[ai].romSetU32Offset[si];
                else
                    f << -1;
                if (si < 7) f << ", ";
            }
            f << " },\n";
        }
        f << "};\n\n";
    }

    // Minimap tile index
    f << "#define AFN_MINIMAP_TILE " << minimapTile << "\n\n";

    // Asset descriptor table
    if (!assets.empty())
    {
        f << "static const int afn_asset_desc[][5] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { " << assetTileStart[ai] << ", " << assetTilesPerFrame[ai]
              << ", " << (int)assets[ai].frames.size()
              << ", " << SnapToOBJSize(assets[ai].baseSize)
              << ", " << assets[ai].palBank << " },\n";
        }
        f << "};\n\n";
    }

    // Sprites
    f << "#define AFN_SPRITE_COUNT " << (int)sprites.size() << "\n\n";

    if (!sprites.empty())
    {
        f << "static const int afn_sprite_data[][8] = {\n";
        f << "    // { x_fixed, y_fixed, z_fixed, palIdx, assetIdx, scale_8_8, spriteType, rotation_brad }\n";
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
            f << "    { " << gx << ", " << gy << ", " << gz << ", "
              << pal << ", " << aIdx << ", " << scaleFixed << ", " << sType << ", " << rotBrad << " },\n";
        }
        f << "};\n";
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
                const GBAPlayerDirExport playerDirs[8],
                float orbitDist,
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
    if (!GenerateMapData(runtimeDir, sprites, assets, camera, playerDirs, orbitDist))
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

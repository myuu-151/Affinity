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

        // Skip static frames for direction-based assets — they use DMA direction tiles
        // and including them wastes OBJ VRAM (critical when 2+ direction assets fill 32KB)
        if (asset.hasDirections && !asset.dirAnimSets.empty()) continue;

        for (size_t fi = 0; fi < asset.frames.size(); fi++)
        {
            auto td = FrameToGBATiles(asset.frames[fi]);
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

        // Collect ALL unique colors across ALL sets for shared palette
        struct ColorFreq { unsigned short rgb15; int count; };
        std::vector<ColorFreq> colorFreqs;
        auto findOrAdd = [&](unsigned short c15) -> int {
            for (size_t i = 0; i < colorFreqs.size(); i++)
                if (colorFreqs[i].rgb15 == c15) { colorFreqs[i].count++; return (int)i; }
            colorFreqs.push_back({c15, 1});
            return (int)colorFreqs.size() - 1;
        };

        // If sharing palette from another asset, collect colors from source asset too
        // so both assets share the same quantized palette
        auto collectColorsFromAsset = [&](int assetIdx) {
            for (size_t si2 = 0; si2 < assets[assetIdx].dirAnimSets.size(); si2++)
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
        // Each asset needs a unique VRAM region for DMA: base + accumulated tiles from previous assets
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

        // Advance VRAM offset: reserve space for one set (8 dirs × tpf tiles)
        int tpf = (dirSize / 8) * (dirSize / 8); // tiles per direction frame
        dirVramNextTile += 8 * tpf;
    }

    int totalTileCount = (int)allTiles.size() / 8;
    int minimapTile = totalTileCount + dirVramNextTile;

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
            for (int si = 0; si < maxDirSets; si++)
            {
                if (assetDirInfos[ai].has && si < (int)assetDirInfos[ai].romSetU32Offset.size())
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
            bool didWeld = false;
            bool preSorted = false;

            if (mesh.exportMode >= 1) // Performance or Barebones
            {
                std::vector<int> weldedNormCount;
                std::vector<int> vertRemap(origVc, -1);
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

        // Mesh descriptor table: { vertCount, indexCount, colorRGB15, cullMode, lit, sorted, halfRes, textured, texW, texShift, texPalBase }
        f << "static const int afn_mesh_desc[][11] = {\n";
        for (size_t mi = 0; mi < meshes.size(); mi++)
        {
            int vc = finalVertCounts[mi];
            int ic = (int)meshes[mi].indices.size();
            int lit = meshes[mi].lit;
            int sorted = 0;
            int halfRes = meshes[mi].halfRes;
            int textured = meshes[mi].textured;
            int texW = meshes[mi].texW;
            int texShift = 0;
            { int tw = texW; while (tw > 1) { texShift++; tw >>= 1; } }
            // Barebones: force unlit; mark as pre-sorted only for non-textured
            // (textured meshes need runtime depth sort since static sort is view-dependent)
            if (meshes[mi].exportMode == 2) { lit = 0; if (!meshes[mi].textured) sorted = 1; }
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%04X", meshes[mi].colorRGB15);
            f << "    { " << vc << ", " << ic << ", " << hex << ", " << meshes[mi].cullMode << ", " << lit << ", " << sorted << ", " << halfRes << ", " << textured << ", " << texW << ", " << texShift << ", " << texPalBases[mi] << " },\n";
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
    if (!GenerateMapData(runtimeDir, sprites, assets, camera, meshes, orbitDist))
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

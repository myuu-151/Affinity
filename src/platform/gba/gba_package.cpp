#include "gba_package.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
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

// Convert editor height to GBA 16.8 fixed-point for floor rendering.
// This must be height * 256 so the HBlank floor looks correct.
static int EditorHeightToGBAFixed(float editorH)
{
    return (int)(editorH * 256.0f);
}

// Convert editor sprite Y to GBA 16.8 fixed-point
static int EditorSpriteYToGBAFixed(float editorY)
{
    return (int)(editorY * 256.0f);
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

// Convert a sprite frame to 4bpp GBA tile u32 data.
// Always emits 32x32 (16 tiles), upscaling smaller frames to fill the space.
// Returns the u32s in row-major tile order for 1D OBJ mapping.
static std::vector<uint32_t> FrameToGBATiles(const GBASpriteFrameExport& frame)
{
    const int outSize = 32; // always 32x32 OBJ
    const int outTilesPerRow = outSize / 8; // 4
    const int outTotalTiles = outTilesPerRow * outTilesPerRow; // 16
    std::vector<uint32_t> data(outTotalTiles * 8, 0);

    int fSize = frame.width;
    if (fSize <= 0) return data;

    // Upscale: map each 32x32 output pixel back to source frame pixel
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

// Generate mapdata.h with sprite asset tile data, palettes, and camera data
static bool GenerateMapData(const std::string& runtimeDir,
                            const std::vector<GBASpriteExport>& sprites,
                            const std::vector<GBASpriteAssetExport>& assets,
                            const GBACameraExport& camera)
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

    // Sprite assets
    f << "#define AFN_ASSET_COUNT " << (int)assets.size() << "\n\n";

    // Build one combined tile data array: all assets, all frames, packed contiguously
    // Each tile = 8 u32s (32 bytes). Tiles packed in 1D OBJ mapping order.
    std::vector<uint32_t> allTiles;
    std::vector<int> assetTileStart;
    std::vector<int> assetTilesPerFrame;

    for (size_t ai = 0; ai < assets.size(); ai++)
    {
        const auto& asset = assets[ai];
        int tilesPerFrame = 16; // always 32x32 OBJ (4x4 tiles)
        assetTileStart.push_back((int)allTiles.size() / 8); // tile index
        assetTilesPerFrame.push_back(tilesPerFrame);

        for (size_t fi = 0; fi < asset.frames.size(); fi++)
        {
            auto td = FrameToGBATiles(asset.frames[fi]);
            allTiles.insert(allTiles.end(), td.begin(), td.end());
        }
    }

    int totalTileCount = (int)allTiles.size() / 8;
    int minimapTile = totalTileCount; // minimap dot goes right after

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

    // Emit per-asset palette arrays (16 RGB15 values each)
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
    f << "\n";

    // Minimap tile index
    f << "#define AFN_MINIMAP_TILE " << minimapTile << "\n\n";

    // Asset descriptor table
    // { tileStart, tilesPerFrame, frameCount, baseSize, palBank }
    if (!assets.empty())
    {
        f << "static const int afn_asset_desc[][5] = {\n";
        for (size_t ai = 0; ai < assets.size(); ai++)
        {
            f << "    { " << assetTileStart[ai] << ", " << assetTilesPerFrame[ai]
              << ", " << (int)assets[ai].frames.size()
              << ", " << 32 // always 32x32 OBJ
              << ", " << assets[ai].palBank << " },\n";
        }
        f << "};\n\n";
    }

    // Sprites — includes assetIdx
    f << "#define AFN_SPRITE_COUNT " << (int)sprites.size() << "\n\n";

    if (!sprites.empty())
    {
        f << "static const int afn_sprite_data[][6] = {\n";
        f << "    // { x_fixed, y_fixed, z_fixed, palIdx, assetIdx, scale_8_8 }\n";
        for (size_t i = 0; i < sprites.size(); i++)
        {
            int gx = EditorToGBAFixed(sprites[i].x);
            int gy = EditorSpriteYToGBAFixed(sprites[i].y);
            int gz = EditorToGBAFixed(sprites[i].z);
            int pal = sprites[i].palIdx;
            int aIdx = sprites[i].assetIdx;
            int scaleFixed = (int)(sprites[i].scale * 256.0f); // 8.8 fixed
            f << "    { " << gx << ", " << gy << ", " << gz << ", " << pal << ", " << aIdx << ", " << scaleFixed << " },\n";
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

    // --- Step 1: Generate mapdata.h with sprite/camera/asset data ---
    if (!GenerateMapData(runtimeDir, sprites, assets, camera))
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

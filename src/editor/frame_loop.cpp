#include "frame_loop.h"
#include "../viewport/mode7_preview.h"
#include "../map/map_types.h"
#include "../platform/gba/gba_package.h"
#include "imgui.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#endif
#include <GL/gl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace Affinity
{

static Mode7Camera sCamera;
static bool sInitialized = false;

// Editor mode tabs
enum class EditorTab { Map, Sprites, Tiles, Skybox, Player, ThreeD };
static EditorTab sActiveTab = EditorTab::Map;

// Dummy tileset: 16 colors for the palette display
static uint32_t sPalette[16] = {
    0xFF000000, 0xFF1D2B53, 0xFF7E2553, 0xFF008751,
    0xFFAB5236, 0xFF5F574F, 0xFFC2C3C7, 0xFFFFF1E8,
    0xFFFF004D, 0xFFFFA300, 0xFFFFEC27, 0xFF00E436,
    0xFF29ADFF, 0xFF83769C, 0xFFFF77A8, 0xFFFFCCAA,
};

// Selected tile / palette index
static int sSelectedTile = 0;
static int sSelectedPalColor = 1;

// Dummy tile grid for tileset display (8x16 = 128 tiles)
static constexpr int kTilesetCols = 16;
static constexpr int kTilesetRows = 8;

// World size — supports up to 1024x1024 pixel tilemaps (128x128 tiles)
static constexpr float kWorldSize = 1024.0f;   // total extent
static constexpr float kWorldHalf = kWorldSize * 0.5f; // ±512

// Floor sprites
static FloorSprite sSprites[kMaxFloorSprites];
static int sSpriteCount = 0;
static int sSelectedSprite = -1;

// Sprite assets
static std::vector<SpriteAsset> sSpriteAssets;
static int sSelectedAsset = -1;
static int sSelectedFrame = 0;
static int sSelectedAnim  = -1;
static int sAssetPreviewFrame = 0;
static float sAssetPreviewTimer = 0.0f;
static unsigned int sAssetPreviewTex = 0; // GL texture for frame preview
static int sSpriteEditorPalColor = 1; // current paint color in frame editor
static float sViewportAnimTime = 0.0f; // animation timer for viewport sprite preview
static bool  sAnimFramePlaying = false; // play button state for animation frame preview
static float sAnimFrameTime    = 0.0f; // timer for animation frame preview playback
static int   sAnimFrameCurrent = 0;    // current frame index during playback

// Per-asset directional sprite images (runtime loaded data)
struct AssetDirSprite
{
    unsigned char* pixels = nullptr; // RGBA pixel data
    int width = 0, height = 0;
    GLuint texture = 0;
};
static constexpr int kAssetDirCount = 8;
static const char* const kDirNames[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

// sAssetDirSprites[assetIdx][setIdx][dir] — parallel to sSpriteAssets
static std::vector<std::vector<std::array<AssetDirSprite, 8>>> sAssetDirSprites;
static int sSelectedDirAnimSet = 0;

// Compute the base index into the flat dirAnimSets/sAssetDirSprites vector for a given animation
static int GetAnimDirBase(const SpriteAsset& asset, int animIdx)
{
    int base = 0;
    for (int i = 0; i < animIdx && i < (int)asset.anims.size(); i++)
        base += asset.anims[i].endFrame; // endFrame = direction frame count
    return base;
}

static void EnsureAssetDirSet(int assetIdx, int setIdx)
{
    if (assetIdx < 0 || assetIdx >= (int)sAssetDirSprites.size()) return;
    while ((int)sAssetDirSprites[assetIdx].size() <= setIdx)
        sAssetDirSprites[assetIdx].push_back({});
    SpriteAsset& sa = sSpriteAssets[assetIdx];
    while ((int)sa.dirAnimSets.size() <= setIdx)
        sa.dirAnimSets.push_back({});
}

static void LoadAssetDirImage(int assetIdx, int setIdx, int dir, const std::string& filepath)
{
    if (assetIdx < 0 || assetIdx >= (int)sAssetDirSprites.size()) return;
    EnsureAssetDirSet(assetIdx, setIdx);
    AssetDirSprite& d = sAssetDirSprites[assetIdx][setIdx][dir];
    if (d.pixels) { stbi_image_free(d.pixels); d.pixels = nullptr; }
    if (d.texture) { glDeleteTextures(1, &d.texture); d.texture = 0; }

    int w, h, channels;
    unsigned char* data = stbi_load(filepath.c_str(), &w, &h, &channels, 4);
    if (!data) return;

    d.pixels = data;
    d.width = w;
    d.height = h;

    glGenTextures(1, &d.texture);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    sSpriteAssets[assetIdx].dirAnimSets[setIdx].dirPaths[dir] = filepath;
    // Check if asset has any directions
    sSpriteAssets[assetIdx].hasDirections = false;
    for (int si = 0; si < (int)sAssetDirSprites[assetIdx].size(); si++)
        for (int i = 0; i < 8; i++)
            if (sAssetDirSprites[assetIdx][si][i].pixels)
            { sSpriteAssets[assetIdx].hasDirections = true; goto done_check; }
    done_check:;
}

static void FreeAssetDirSprites(int assetIdx)
{
    if (assetIdx < 0 || assetIdx >= (int)sAssetDirSprites.size()) return;
    for (int si = 0; si < (int)sAssetDirSprites[assetIdx].size(); si++)
    {
        for (int d = 0; d < 8; d++)
        {
            AssetDirSprite& s = sAssetDirSprites[assetIdx][si][d];
            if (s.pixels) { stbi_image_free(s.pixels); s.pixels = nullptr; }
            if (s.texture) { glDeleteTextures(1, &s.texture); s.texture = 0; }
            s.width = s.height = 0;
        }
    }
    sAssetDirSprites[assetIdx].clear();
}

// Sprite placement colors (cycle through these)
static const uint32_t kSpriteColors[] = {
    0xFF00FFFF, // yellow
    0xFF4444FF, // red
    0xFFFF4444, // blue
    0xFF44FF44, // green
    0xFFFF88FF, // pink
    0xFF00CCFF, // orange
    0xFFFFFF44, // cyan
    0xFFAA66FF, // purple
};
static constexpr int kNumSpriteColors = sizeof(kSpriteColors) / sizeof(kSpriteColors[0]);

// Play/Edit mode
enum class EditorMode { Edit, Play };
enum class SelectedObjType { None, Sprite, Camera };
static EditorMode sEditorMode = EditorMode::Edit;
static float sOrbitAngle = 0.0f;  // play mode: angle from player to camera
static float sOrbitDist = 60.0f; // play mode: distance from player to camera
static float sPlayerMoveAngle = 0.0f; // player movement direction (camera-relative)
static bool  sPlayerMoving = false;   // is the player moving this frame
static bool  sPlayerSprinting = false; // is the player holding sprint
static float sAutoOrbitCurrent = 0.0f; // smoothed auto-orbit speed
static float sManualOrbitCurrent = 0.0f; // smoothed manual orbit speed (J/L)
static float sPlayerVelX = 0.0f, sPlayerVelZ = 0.0f; // smoothed player velocity
static FloorSprite sSavedPlayerSprite; // saved player state before Play
static int sSavedPlayerIdx = -1;
static SelectedObjType sSelectedObjType = SelectedObjType::None;
static CameraStartObject sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
static float sCamObjEditorScale = 0.05f; // editor-only visual size
static Mode7Camera sSavedEditorCam;

// Preferences
static float sUiScale = 1.0f;
static bool  sShowPrefs = false;
static char  sMgbaPath[512] = "";
static bool  sMgbaFound = false;

// GBA packaging state
static bool sPackaging = false;
static bool sPackageDone = false;
static bool sPackageSuccess = false;
static std::string sPackageMsg;
static std::string sPackageOutputPath;

// Project file
static std::string sProjectPath;  // empty = no project loaded
static bool sProjectDirty = false; // unsaved changes

// Mesh assets
static std::vector<MeshAsset> sMeshAssets;
static int sSelectedMesh = -1;

// ---- 3D View state ----
static float s3DOrbitYaw   = 0.4f;   // radians
static float s3DOrbitPitch = 0.6f;   // radians (clamped)
static float s3DOrbitDist  = 400.0f; // distance from target
static float s3DTargetX    = 0.0f;   // look-at X
static float s3DTargetY    = 0.0f;   // look-at Y (up)
static float s3DTargetZ    = 0.0f;   // look-at Z
static bool  s3DDragging   = false;
static bool  s3DPanning    = false;
static ImVec2 s3DDragStart = {};
static GLuint s3DFloorTex  = 0;
static bool  s3DRenderNeeded = false; // set true during FrameTick, consumed by Render3DViewport
static ImVec2 s3DViewPos = {};        // viewport position for GL rendering
static ImVec2 s3DViewSize = {};       // viewport size for GL rendering

// ---- Win32 file dialogs ----
#ifdef _WIN32
static std::string OpenFileDialog(const char* filter, const char* defaultExt)
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        return std::string(path);
    return {};
}

static std::string SaveFileDialog(const char* filter, const char* defaultExt)
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn))
        return std::string(path);
    return {};
}
static std::string OpenFolderDialog()
{
    std::string result;
    CoInitialize(nullptr);
    IFileDialog* pfd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileDialog, (void**)&pfd)))
    {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS);
        if (SUCCEEDED(pfd->Show(nullptr)))
        {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    char buf[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, MAX_PATH, nullptr, nullptr);
                    result = buf;
                    CoTaskMemFree(path);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    CoUninitialize();
    return result;
}
#endif

// ---- OBJ mesh loader ----
static bool LoadOBJ(const std::string& path, MeshAsset& out)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    struct V3 { float x, y, z; };
    std::vector<V3> positions;
    std::vector<V3> normals;
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idxs;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            V3 v;
            if (sscanf(line + 2, "%f %f %f", &v.x, &v.y, &v.z) == 3)
                positions.push_back(v);
        }
        else if (line[0] == 'v' && line[1] == 'n')
        {
            V3 n;
            if (sscanf(line + 3, "%f %f %f", &n.x, &n.y, &n.z) == 3)
                normals.push_back(n);
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            // Parse face — supports v, v/vt, v/vt/vn, v//vn
            int vi[4] = {}, ni[4] = {};
            int count = 0;
            char* p = line + 2;
            while (*p && count < 4)
            {
                int v = 0, vt = 0, vn = 0;
                int nread = 0;
                if (sscanf(p, "%d/%d/%d%n", &v, &vt, &vn, &nread) >= 3 && nread > 0) {}
                else if (sscanf(p, "%d//%d%n", &v, &vn, &nread) >= 2 && nread > 0) {}
                else if (sscanf(p, "%d/%d%n", &v, &vt, &nread) >= 2 && nread > 0) {}
                else if (sscanf(p, "%d%n", &v, &nread) >= 1 && nread > 0) {}
                else break;
                vi[count] = v;
                ni[count] = vn;
                count++;
                p += nread;
                while (*p == ' ' || *p == '\t') p++;
            }

            // Triangulate (fan from first vertex)
            for (int t = 1; t + 1 < count; t++)
            {
                int face[3] = { 0, t, t + 1 };
                for (int fi = 0; fi < 3; fi++)
                {
                    int pi = vi[face[fi]] - 1;
                    int nni = ni[face[fi]] - 1;
                    MeshVertex mv = {};
                    if (pi >= 0 && pi < (int)positions.size())
                    { mv.px = positions[pi].x; mv.py = positions[pi].y; mv.pz = positions[pi].z; }
                    if (nni >= 0 && nni < (int)normals.size())
                    { mv.nx = normals[nni].x; mv.ny = normals[nni].y; mv.nz = normals[nni].z; }
                    mv.r = mv.g = mv.b = 1.0f;
                    idxs.push_back((uint32_t)verts.size());
                    verts.push_back(mv);
                }
            }
        }
    }
    fclose(f);

    if (verts.empty()) return false;

    out.vertices = std::move(verts);
    out.indices = std::move(idxs);
    out.sourcePath = path;

    // Extract filename as name
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (slash != std::string::npos && dot != std::string::npos && dot > slash)
        out.name = path.substr(slash + 1, dot - slash - 1);
    else if (dot != std::string::npos)
        out.name = path.substr(0, dot);

    // Compute AABB
    out.boundsMin[0] = out.boundsMin[1] = out.boundsMin[2] = 1e30f;
    out.boundsMax[0] = out.boundsMax[1] = out.boundsMax[2] = -1e30f;
    for (const auto& v : out.vertices)
    {
        if (v.px < out.boundsMin[0]) out.boundsMin[0] = v.px;
        if (v.py < out.boundsMin[1]) out.boundsMin[1] = v.py;
        if (v.pz < out.boundsMin[2]) out.boundsMin[2] = v.pz;
        if (v.px > out.boundsMax[0]) out.boundsMax[0] = v.px;
        if (v.py > out.boundsMax[1]) out.boundsMax[1] = v.py;
        if (v.pz > out.boundsMax[2]) out.boundsMax[2] = v.pz;
    }

    return true;
}

// ---- Project save/load ----
static bool SaveProject(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    fprintf(f, "[Affinity Project]\n");
    fprintf(f, "version=1\n\n");

    // Camera start object
    fprintf(f, "[CameraStart]\n");
    fprintf(f, "x=%.6f\n", sCamObj.x);
    fprintf(f, "z=%.6f\n", sCamObj.z);
    fprintf(f, "height=%.6f\n", sCamObj.height);
    fprintf(f, "angle=%.6f\n", sCamObj.angle);
    fprintf(f, "horizon=%.6f\n", sCamObj.horizon);
    fprintf(f, "walk_speed=%.1f\n", sCamObj.walkSpeed);
    fprintf(f, "sprint_speed=%.1f\n", sCamObj.sprintSpeed);
    fprintf(f, "walk_ease_in=%.1f\n", sCamObj.walkEaseIn);
    fprintf(f, "walk_ease_out=%.1f\n", sCamObj.walkEaseOut);
    fprintf(f, "sprint_ease_in=%.1f\n", sCamObj.sprintEaseIn);
    fprintf(f, "sprint_ease_out=%.1f\n", sCamObj.sprintEaseOut);
    fprintf(f, "icon_scale=%.6f\n\n", sCamObjEditorScale);

    // Editor camera
    fprintf(f, "[EditorCamera]\n");
    fprintf(f, "x=%.6f\n", sCamera.x);
    fprintf(f, "z=%.6f\n", sCamera.z);
    fprintf(f, "height=%.6f\n", sCamera.height);
    fprintf(f, "angle=%.6f\n", sCamera.angle);
    fprintf(f, "fov=%.6f\n", sCamera.fov);
    fprintf(f, "horizon=%.6f\n\n", sCamera.horizon);

    // Sprites
    fprintf(f, "[Sprites]\n");
    fprintf(f, "count=%d\n", sSpriteCount);
    for (int i = 0; i < sSpriteCount; i++)
    {
        const FloorSprite& sp = sSprites[i];
        fprintf(f, "sprite=%d,%.6f,%.6f,%.6f,%.6f,%u,%d,%d,%d,%.6f,%d,%d\n",
                sp.spriteId, sp.x, sp.y, sp.z, sp.scale, sp.color,
                sp.assetIdx, sp.animIdx, (int)sp.type, sp.rotation, sp.animEnabled ? 1 : 0,
                sp.meshIdx);
    }
    fprintf(f, "\n");

    // Sprite Assets
    fprintf(f, "[SpriteAssets]\n");
    fprintf(f, "count=%d\n", (int)sSpriteAssets.size());
    for (int ai = 0; ai < (int)sSpriteAssets.size(); ai++)
    {
        const SpriteAsset& sa = sSpriteAssets[ai];
        fprintf(f, "asset_begin=%s\n", sa.name.c_str());
        fprintf(f, "baseSize=%d\n", sa.baseSize);
        fprintf(f, "palBank=%d\n", sa.palBank);
        fprintf(f, "paletteSrc=%d\n", sa.paletteSrc);
        // Palette
        for (int c = 0; c < 16; c++)
            fprintf(f, "pal=%d,%u\n", c, sa.palette[c]);
        // Frames
        fprintf(f, "frameCount=%d\n", (int)sa.frames.size());
        for (int fi = 0; fi < (int)sa.frames.size(); fi++)
        {
            const SpriteFrame& fr = sa.frames[fi];
            fprintf(f, "frame=%d,%d", fr.width, fr.height);
            for (int p = 0; p < fr.height; p++)
                for (int q = 0; q < fr.width; q++)
                    fprintf(f, ",%d", fr.pixels[p * kMaxFrameSize + q]);
            fprintf(f, "\n");
        }
        // Animations
        fprintf(f, "animCount=%d\n", (int)sa.anims.size());
        for (int ani = 0; ani < (int)sa.anims.size(); ani++)
        {
            const SpriteAnim& an = sa.anims[ani];
            fprintf(f, "anim=%s,%d,%d,%d,%d,%.2f,%d\n",
                    an.name.c_str(), an.startFrame, an.endFrame, an.fps, an.loop ? 1 : 0, an.speed, (int)an.gameState);
        }
        // LOD
        fprintf(f, "lodCount=%d\n", sa.lodCount);
        for (int li = 0; li < sa.lodCount; li++)
        {
            const SpriteLOD& lod = sa.lod[li];
            fprintf(f, "lod=%d,%d,%d,%.1f\n", lod.size, lod.frameStart, lod.frameCount, lod.maxDist);
        }
        // Directional animation sets
        for (int si = 0; si < (int)sa.dirAnimSets.size(); si++)
        {
            const auto& das = sa.dirAnimSets[si];
            fprintf(f, "diranimset=%s\n", das.name.c_str());
            for (int d = 0; d < 8; d++)
            {
                if (!das.dirPaths[d].empty())
                    fprintf(f, "diranimdir=%d,%s\n", d, das.dirPaths[d].c_str());
            }
            fprintf(f, "diranimset_end\n");
        }
        fprintf(f, "asset_end\n");
    }
    fprintf(f, "\n");

    // Mesh assets
    fprintf(f, "[MeshAssets]\n");
    fprintf(f, "count=%d\n", (int)sMeshAssets.size());
    for (int mi = 0; mi < (int)sMeshAssets.size(); mi++)
    {
        const MeshAsset& ma = sMeshAssets[mi];
        fprintf(f, "mesh=%s|%s\n", ma.name.c_str(), ma.sourcePath.c_str());
    }
    fprintf(f, "\n");

    // Palette
    fprintf(f, "[Palette]\n");
    for (int i = 0; i < 16; i++)
        fprintf(f, "color=%d,%u\n", i, sPalette[i]);

    fclose(f);
    sProjectPath = path;
    sProjectDirty = false;
    return true;
}

static bool LoadProject(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    // Reset state before loading
    sSpriteCount = 0;
    sSelectedSprite = -1;
    sSelectedObjType = SelectedObjType::None;
    sEditorMode = EditorMode::Edit;
    sSpriteAssets.clear();
    for (int i = 0; i < (int)sAssetDirSprites.size(); i++) FreeAssetDirSprites(i);
    sAssetDirSprites.clear();
    sSelectedAsset = -1;
    sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
    sCamObjEditorScale = 0.05f;

    char line[32768]; // large buffer for frame pixel data lines (64x64 = ~16KB worst case)
    char section[64] = {};

    while (fgets(line, sizeof(line), f))
    {
        // Strip newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        // Skip empty/comment
        if (line[0] == '\0' || line[0] == '#') continue;

        // Section header
        if (line[0] == '[')
        {
            sscanf(line, "[%63[^]]]", section);
            continue;
        }

        float fval;
        int ival;
        unsigned int uval;

        if (strcmp(section, "CameraStart") == 0)
        {
            if (sscanf(line, "x=%f", &fval) == 1) sCamObj.x = fval;
            else if (sscanf(line, "z=%f", &fval) == 1) sCamObj.z = fval;
            else if (sscanf(line, "height=%f", &fval) == 1) sCamObj.height = fval;
            else if (sscanf(line, "angle=%f", &fval) == 1) sCamObj.angle = fval;
            else if (sscanf(line, "horizon=%f", &fval) == 1) sCamObj.horizon = fval;
            else if (sscanf(line, "walk_speed=%f", &fval) == 1) sCamObj.walkSpeed = fval;
            else if (sscanf(line, "sprint_speed=%f", &fval) == 1) sCamObj.sprintSpeed = fval;
            else if (sscanf(line, "walk_ease_in=%f", &fval) == 1) sCamObj.walkEaseIn = fval;
            else if (sscanf(line, "walk_ease_out=%f", &fval) == 1) sCamObj.walkEaseOut = fval;
            else if (sscanf(line, "sprint_ease_in=%f", &fval) == 1) sCamObj.sprintEaseIn = fval;
            else if (sscanf(line, "sprint_ease_out=%f", &fval) == 1) sCamObj.sprintEaseOut = fval;
            else if (sscanf(line, "icon_scale=%f", &fval) == 1) sCamObjEditorScale = fval;
        }
        else if (strcmp(section, "EditorCamera") == 0)
        {
            if (sscanf(line, "x=%f", &fval) == 1) sCamera.x = fval;
            else if (sscanf(line, "z=%f", &fval) == 1) sCamera.z = fval;
            else if (sscanf(line, "height=%f", &fval) == 1) sCamera.height = fval;
            else if (sscanf(line, "angle=%f", &fval) == 1) sCamera.angle = fval;
            else if (sscanf(line, "fov=%f", &fval) == 1) sCamera.fov = fval;
            else if (sscanf(line, "horizon=%f", &fval) == 1) sCamera.horizon = fval;
        }
        else if (strcmp(section, "Sprites") == 0)
        {
            if (sscanf(line, "count=%d", &ival) == 1) { /* just informational */ }
            else if (sSpriteCount < kMaxFloorSprites)
            {
                int sid, aIdx = -1, anIdx = 0, typeVal = 0, animEn = 1, mIdx = -1;
                float sx, sy, sz, sc;
                unsigned int col;
                // Try extended format (with assetIdx, animIdx, type, rotation, animEnabled, meshIdx)
                float rot = 0.0f;
                int matched = sscanf(line, "sprite=%d,%f,%f,%f,%f,%u,%d,%d,%d,%f,%d,%d", &sid, &sx, &sy, &sz, &sc, &col, &aIdx, &anIdx, &typeVal, &rot, &animEn, &mIdx);
                if (matched >= 6)
                {
                    FloorSprite& sp = sSprites[sSpriteCount];
                    sp.spriteId = sid;
                    sp.x = sx;
                    sp.y = sy;
                    sp.z = sz;
                    sp.scale = sc;
                    sp.color = col;
                    sp.assetIdx = aIdx;
                    sp.animIdx = anIdx;
                    sp.type = (matched >= 9 && typeVal >= 0 && typeVal < (int)SpriteType::Count)
                        ? (SpriteType)typeVal : SpriteType::Prop;
                    sp.rotation = (matched >= 10) ? rot : 0.0f;
                    sp.animEnabled = (matched >= 11) ? (animEn != 0) : true;
                    sp.meshIdx = (matched >= 12) ? mIdx : -1;
                    sp.selected = false;
                    sSpriteCount++;
                }
            }
        }
        else if (strcmp(section, "PlayerDirs") == 0)
        {
            // Legacy section — player direction sprites now use per-asset direction system.
            // Skip any entries silently for backward compatibility.
        }
        else if (strcmp(section, "SpriteAssets") == 0)
        {
            // Parse sprite asset blocks
            if (strncmp(line, "asset_begin=", 12) == 0)
            {
                SpriteAsset sa;
                sa.name = line + 12;
                sa.frames.clear();
                sa.anims.clear();
                sa.lodCount = 0;
                int lodIdx = 0;

                // Read lines until asset_end
                while (fgets(line, sizeof(line), f))
                {
                    char* enl = strchr(line, '\n'); if (enl) *enl = '\0';
                    enl = strchr(line, '\r'); if (enl) *enl = '\0';
                    if (strcmp(line, "asset_end") == 0) break;
                    if (line[0] == '\0') continue;

                    int iv; unsigned int uv; float fv;
                    if (sscanf(line, "baseSize=%d", &iv) == 1) sa.baseSize = iv;
                    else if (sscanf(line, "palBank=%d", &iv) == 1) sa.palBank = iv;
                    else if (sscanf(line, "paletteSrc=%d", &iv) == 1) sa.paletteSrc = iv;
                    else if (sscanf(line, "pal=%d,%u", &iv, &uv) == 2 && iv >= 0 && iv < 16) sa.palette[iv] = uv;
                    else if (sscanf(line, "frameCount=%d", &iv) == 1) { /* informational */ }
                    else if (strncmp(line, "frame=", 6) == 0)
                    {
                        SpriteFrame fr;
                        memset(fr.pixels, 0, sizeof(fr.pixels));
                        // Parse: frame=w,h,p0,p1,p2,...
                        const char* p = line + 6;
                        int w = 0, h = 0;
                        sscanf(p, "%d,%d", &w, &h);
                        fr.width = w; fr.height = h;
                        // Skip past "w,h"
                        int commas = 0;
                        while (*p && commas < 2) { if (*p == ',') commas++; p++; }
                        for (int py = 0; py < h && *p; py++)
                            for (int px = 0; px < w && *p; px++)
                            {
                                int pv = 0;
                                sscanf(p, "%d", &pv);
                                fr.pixels[py * kMaxFrameSize + px] = (uint8_t)pv;
                                // Skip to next comma or end
                                while (*p && *p != ',') p++;
                                if (*p == ',') p++;
                            }
                        sa.frames.push_back(fr);
                    }
                    else if (sscanf(line, "animCount=%d", &iv) == 1) { /* informational */ }
                    else if (strncmp(line, "anim=", 5) == 0)
                    {
                        SpriteAnim an;
                        char aname[64] = {};
                        int sf, ef, afps, aloop;
                        float aspeed = 1.0f;
                        int agstate = 0;
                        int nread = sscanf(line + 5, "%63[^,],%d,%d,%d,%d,%f,%d", aname, &sf, &ef, &afps, &aloop, &aspeed, &agstate);
                        if (nread >= 5)
                        {
                            an.name = aname;
                            an.startFrame = sf;
                            an.endFrame = ef;
                            an.fps = afps;
                            an.loop = (aloop != 0);
                            if (nread >= 6) an.speed = aspeed;
                            if (nread >= 7) an.gameState = (AnimState)agstate;
                            sa.anims.push_back(an);
                        }
                    }
                    else if (sscanf(line, "lodCount=%d", &iv) == 1) { sa.lodCount = iv; lodIdx = 0; }
                    else if (strncmp(line, "lod=", 4) == 0)
                    {
                        int ls, lfs, lfc; float ld;
                        if (sscanf(line + 4, "%d,%d,%d,%f", &ls, &lfs, &lfc, &ld) == 4
                            && lodIdx < kMaxSpriteLODs)
                        {
                            sa.lod[lodIdx].size = ls;
                            sa.lod[lodIdx].frameStart = lfs;
                            sa.lod[lodIdx].frameCount = lfc;
                            sa.lod[lodIdx].maxDist = ld;
                            lodIdx++;
                        }
                    }
                    else if (strncmp(line, "assetdir=", 9) == 0)
                    {
                        // Legacy: single direction set (migrate to set 0)
                        int dirIdx2;
                        char dirPath2[512] = {};
                        if (sscanf(line + 9, "%d,%511[^\n]", &dirIdx2, dirPath2) == 2
                            && dirIdx2 >= 0 && dirIdx2 < 8)
                        {
                            if (sa.dirAnimSets.empty())
                                sa.dirAnimSets.push_back({"idle"});
                            sa.dirAnimSets[0].dirPaths[dirIdx2] = dirPath2;
                        }
                    }
                    else if (strncmp(line, "diranimset=", 11) == 0)
                    {
                        SpriteAsset::DirAnimSet das;
                        das.name = line + 11;
                        // Trim trailing whitespace/newline
                        while (!das.name.empty() && (das.name.back() == '\n' || das.name.back() == '\r'))
                            das.name.pop_back();
                        // Read dir paths until diranimset_end
                        while (fgets(line, sizeof(line), f))
                        {
                            // Trim newline
                            { char* nl = strchr(line, '\n'); if (nl) *nl = 0; }
                            { char* nl = strchr(line, '\r'); if (nl) *nl = 0; }
                            if (strncmp(line, "diranimset_end", 14) == 0) break;
                            if (strncmp(line, "diranimdir=", 11) == 0)
                            {
                                int dd; char dp[512] = {};
                                if (sscanf(line + 11, "%d,%511[^\n]", &dd, dp) == 2
                                    && dd >= 0 && dd < 8)
                                    das.dirPaths[dd] = dp;
                            }
                        }
                        sa.dirAnimSets.push_back(das);
                    }
                }
                sSpriteAssets.push_back(sa);
                sAssetDirSprites.push_back({});
                // Load directional sprites for all sets
                int newIdx = (int)sSpriteAssets.size() - 1;
                for (int si = 0; si < (int)sa.dirAnimSets.size(); si++)
                {
                    for (int d = 0; d < 8; d++)
                    {
                        if (!sa.dirAnimSets[si].dirPaths[d].empty())
                            LoadAssetDirImage(newIdx, si, d, sa.dirAnimSets[si].dirPaths[d]);
                    }
                }
            }
        }
        else if (strcmp(section, "MeshAssets") == 0)
        {
            char mname[256], mpath[512];
            if (sscanf(line, "mesh=%255[^|]|%511[^\n]", mname, mpath) == 2)
            {
                MeshAsset ma;
                ma.name = mname;
                ma.sourcePath = mpath;
                // Reload from source OBJ
                if (!ma.sourcePath.empty())
                    LoadOBJ(ma.sourcePath, ma);
                ma.name = mname; // restore name in case LoadOBJ overwrote it
                sMeshAssets.push_back(std::move(ma));
            }
        }
        else if (strcmp(section, "Palette") == 0)
        {
            int idx;
            if (sscanf(line, "color=%d,%u", &idx, &uval) == 2 && idx >= 0 && idx < 16)
                sPalette[idx] = uval;
        }
    }

    fclose(f);
    sProjectPath = path;
    sProjectDirty = false;
    return true;
}

static void CloseProject()
{
    // Reset all project state to defaults
    sSpriteCount = 0;
    sSelectedSprite = -1;
    sSelectedObjType = SelectedObjType::None;
    sEditorMode = EditorMode::Edit;
    sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
    sCamObjEditorScale = 0.05f;

    sCamera.x = 0.0f;
    sCamera.z = 0.0f;
    sCamera.height = 64.0f;
    sCamera.angle = 0.0f;
    sCamera.fov = 128.0f;
    sCamera.horizon = 54.0f;

    sSpriteAssets.clear();
    for (int i = 0; i < (int)sAssetDirSprites.size(); i++) FreeAssetDirSprites(i);
    sAssetDirSprites.clear();
    sSelectedAsset = -1;
    sSelectedFrame = 0;
    sSelectedAnim = -1;

    sMeshAssets.clear();
    sSelectedMesh = -1;

    sProjectPath.clear();
    sProjectDirty = false;
}

void FrameInit()
{
    sCamera.x      = 0.0f;
    sCamera.z      = 0.0f;
    sCamera.height = 64.0f;
    sCamera.angle  = 0.0f;
    sCamera.fov    = 128.0f;
    Mode7::Init();

    // Load preferences
    FILE* pf = fopen("affinity_prefs.ini", "r");
    if (pf)
    {
        char line[600];
        while (fgets(line, sizeof(line), pf))
        {
            float fval;
            char sval[512];
            if (sscanf(line, "ui_scale=%f", &fval) == 1)
            {
                sUiScale = fval;
                ImGui::GetIO().FontGlobalScale = sUiScale;
            }
            else if (sscanf(line, "mgba_path=%511[^\n]", sval) == 1)
            {
                strncpy(sMgbaPath, sval, sizeof(sMgbaPath) - 1);
            }
        }
        fclose(pf);
    }

    sInitialized = true;
}

// Helper: draw a colored rect in ImGui drawlist
static void DrawColorBox(ImDrawList* dl, ImVec2 pos, ImVec2 size, uint32_t col, bool selected)
{
    // ImGui colors are ABGR, our palette is ABGR already
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col);
    if (selected)
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), 0xFFFFFFFF, 0.0f, 0, 2.0f);
    else
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), 0x80FFFFFF);
}

// Helper: get scaled size based on current UI scale
static float Scaled(float base) { return base * sUiScale; }

// ---- 3D View rendering ----
static void Draw3DView(ImVec2 pos, ImVec2 size)
{
    // Side panel width (computed early for viewport sizing)
    float vpPanelW = std::max(Scaled(240), size.x * 0.22f);
    float vpAreaW = size.x - vpPanelW;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(vpAreaW, size.y));
    ImGui::Begin("##3DTab", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground);

    // Handle mouse input for orbit/pan/zoom
    ImVec2 mpos = ImGui::GetMousePos();
    bool hovered = (mpos.x >= pos.x && mpos.x < pos.x + vpAreaW &&
                    mpos.y >= pos.y && mpos.y < pos.y + size.y);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureKeyboard)
    {
        s3DDragging = true;
        s3DDragStart = mpos;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        s3DPanning = true;
        s3DDragStart = mpos;
    }
    if (s3DDragging)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float dx = mpos.x - s3DDragStart.x;
            float dy = mpos.y - s3DDragStart.y;
            s3DOrbitYaw   -= dx * 0.005f;
            s3DOrbitPitch -= dy * 0.005f;
            if (s3DOrbitPitch < 0.05f) s3DOrbitPitch = 0.05f;
            if (s3DOrbitPitch > 1.5f)  s3DOrbitPitch = 1.5f;
            s3DDragStart = mpos;
        }
        else
            s3DDragging = false;
    }
    if (s3DPanning)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            float dx = mpos.x - s3DDragStart.x;
            float dy = mpos.y - s3DDragStart.y;
            // Pan in camera-local right/up
            float cy = cosf(s3DOrbitYaw), sy = sinf(s3DOrbitYaw);
            float rightX = cy, rightZ = -sy;
            float speed = s3DOrbitDist * 0.002f;
            s3DTargetX -= rightX * dx * speed;
            s3DTargetZ -= rightZ * dx * speed;
            s3DTargetY += dy * speed;
            s3DDragStart = mpos;
        }
        else
            s3DPanning = false;
    }
    if (hovered)
    {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f)
        {
            s3DOrbitDist *= (1.0f - scroll * 0.1f);
            if (s3DOrbitDist < 10.0f)  s3DOrbitDist = 10.0f;
            if (s3DOrbitDist > 2000.0f) s3DOrbitDist = 2000.0f;
        }
    }

    // Store viewport rect for post-ImGui GL rendering (exclude side panel)
    s3DRenderNeeded = true;
    s3DViewPos = pos;
    s3DViewSize = ImVec2(vpAreaW, size.y);

    // Overlay info text
    ImGui::SetCursorPos(ImVec2(8, 8));
    ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.8f, 0.8f), "LMB: Orbit  |  MMB: Pan  |  Scroll: Zoom");

    ImGui::End();

    // ---- 3D Tab side panel ----
    ImVec2 panelPos(pos.x + vpAreaW, pos.y);
    ImVec2 panelSize(vpPanelW, size.y);
    ImGui::SetNextWindowPos(panelPos);
    ImGui::SetNextWindowSize(panelSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::Begin("##3DPanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "3D Objects");
    ImGui::Separator();

    // Mesh asset library
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "Mesh Assets");
    float listH = std::min(Scaled(150), size.y * 0.25f);
    ImGui::BeginChild("##MeshAssetList", ImVec2(0, listH), true);
    for (int mi = 0; mi < (int)sMeshAssets.size(); mi++)
    {
        bool sel = (sSelectedMesh == mi);
        if (ImGui::Selectable(sMeshAssets[mi].name.c_str(), sel))
            sSelectedMesh = mi;
    }
    ImGui::EndChild();

    if (ImGui::Button("Import OBJ...##3d"))
    {
#ifdef _WIN32
        std::string objPath = OpenFileDialog("OBJ Files\0*.obj\0All Files\0*.*\0", "obj");
        if (!objPath.empty())
        {
            MeshAsset ma;
            if (LoadOBJ(objPath, ma))
            {
                sSelectedMesh = (int)sMeshAssets.size();
                sMeshAssets.push_back(std::move(ma));
                sProjectDirty = true;
            }
        }
#endif
    }
    ImGui::SameLine();
    if (sSelectedMesh >= 0 && sSelectedMesh < (int)sMeshAssets.size())
    {
        if (ImGui::Button("Delete##meshDel"))
        {
            // Remove mesh asset and fix up references
            for (int i = 0; i < sSpriteCount; i++)
            {
                if (sSprites[i].meshIdx == sSelectedMesh)
                    sSprites[i].meshIdx = -1;
                else if (sSprites[i].meshIdx > sSelectedMesh)
                    sSprites[i].meshIdx--;
            }
            sMeshAssets.erase(sMeshAssets.begin() + sSelectedMesh);
            sSelectedMesh = -1;
            sProjectDirty = true;
        }
    }

    // Selected mesh info
    if (sSelectedMesh >= 0 && sSelectedMesh < (int)sMeshAssets.size())
    {
        MeshAsset& ma = sMeshAssets[sSelectedMesh];
        ImGui::Separator();
        char nameBuf[256];
        strncpy(nameBuf, ma.name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[255] = '\0';
        ImGui::PushItemWidth(-1);
        if (ImGui::InputText("##meshName", nameBuf, sizeof(nameBuf)))
            ma.name = nameBuf;
        ImGui::PopItemWidth();
        ImGui::Text("Verts: %d  Tris: %d", (int)ma.vertices.size(), (int)ma.indices.size() / 3);
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Place mesh button
    if (sSelectedMesh >= 0 && sSelectedMesh < (int)sMeshAssets.size())
    {
        if (ImGui::Button("Place Mesh##3dplace", ImVec2(-1, 0)))
        {
            if (sSpriteCount < kMaxFloorSprites)
            {
                FloorSprite& sp = sSprites[sSpriteCount];
                sp = FloorSprite{}; // reset to defaults
                sp.type = SpriteType::Mesh;
                sp.meshIdx = sSelectedMesh;
                sp.x = s3DTargetX;
                sp.y = 0.0f;
                sp.z = s3DTargetZ;
                sp.color = kSpriteColors[sSpriteCount % kNumSpriteColors];
                sp.selected = false;
                sSelectedSprite = sSpriteCount;
                sSelectedObjType = SelectedObjType::Sprite;
                sSpriteCount++;
                sProjectDirty = true;
            }
        }
    }
    else
    {
        ImGui::TextWrapped("Select a mesh asset above, then click Place Mesh.");
    }

    // Mesh object list
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "Placed Meshes");
    ImGui::BeginChild("##MeshObjList", ImVec2(0, 0), true);
    for (int i = 0; i < sSpriteCount; i++)
    {
        if (sSprites[i].type != SpriteType::Mesh) continue;
        char label[64];
        const char* mname = (sSprites[i].meshIdx >= 0 && sSprites[i].meshIdx < (int)sMeshAssets.size())
            ? sMeshAssets[sSprites[i].meshIdx].name.c_str() : "???";
        snprintf(label, sizeof(label), "%s [%d]##meshobj%d", mname, i, i);
        bool sel = (sSelectedSprite == i);
        if (ImGui::Selectable(label, sel))
        {
            if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
                sSprites[sSelectedSprite].selected = false;
            sSelectedSprite = i;
            sSelectedObjType = SelectedObjType::Sprite;
            sSprites[i].selected = true;
        }
    }
    ImGui::EndChild();

    // Properties for selected mesh object
    if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount && sSprites[sSelectedSprite].type == SpriteType::Mesh)
    {
        FloorSprite& sp = sSprites[sSelectedSprite];
        ImGui::Separator();
        ImGui::PushItemWidth(-1);
        ImGui::DragFloat("X##m3d", &sp.x, 1.0f, -kWorldHalf, kWorldHalf);
        ImGui::DragFloat("Y##m3d", &sp.y, 0.5f, -200.0f, 200.0f);
        ImGui::DragFloat("Z##m3d", &sp.z, 1.0f, -kWorldHalf, kWorldHalf);
        ImGui::DragFloat("Scale##m3d", &sp.scale, 0.1f, 0.01f, 100.0f);
        ImGui::DragFloat("Rotation##m3d", &sp.rotation, 1.0f, 0.0f, 360.0f, "%.0f deg");
        ImGui::PopItemWidth();

        if (ImGui::Button("Delete##meshObjDel"))
        {
            for (int j = sSelectedSprite; j < sSpriteCount - 1; j++)
                sSprites[j] = sSprites[j + 1];
            sSpriteCount--;
            sSelectedSprite = -1;
            sSelectedObjType = SelectedObjType::None;
            sProjectDirty = true;
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// ---- Tab bar (NEXXT-style top tabs) ----
static void DrawTabBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float tabH = ImGui::GetFrameHeight() + Scaled(6);

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, tabH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Scaled(4), Scaled(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
    ImGui::Begin("##TabBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float btnW = Scaled(80);
    float btnH = ImGui::GetFrameHeight();
    auto TabButton = [btnW, btnH](const char* label, EditorTab tab) {
        bool active = (sActiveTab == tab);
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.38f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(btnW, btnH)))
            sActiveTab = tab;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };

    TabButton("Scene",   EditorTab::Map);
    TabButton("Sprites", EditorTab::Sprites);
    TabButton("Tiles",   EditorTab::Tiles);
    TabButton("Skybox",  EditorTab::Skybox);
    TabButton("Player",  EditorTab::Player);
    TabButton("3D",      EditorTab::ThreeD);

    ImGui::SameLine(0, Scaled(20));
    // Play/Stop button
    if (sEditorMode == EditorMode::Edit)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.45f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Play", ImVec2(btnW, btnH)))
        {
            sEditorMode = EditorMode::Play;
            sSavedEditorCam = sCamera;
            sCamera.x = sCamObj.x;
            sCamera.z = sCamObj.z;
            sCamera.height = sCamObj.height;
            sCamera.angle = sCamObj.angle;
            sCamera.horizon = sCamObj.horizon;
            sOrbitAngle = 0.0f;
            sAutoOrbitCurrent = 0.0f;
            // Snapshot orbit distance and save player state
            sOrbitDist = 60.0f;
            sSavedPlayerIdx = -1;
            for (int i = 0; i < sSpriteCount; i++)
            {
                if (sSprites[i].type == SpriteType::Player)
                {
                    sSavedPlayerSprite = sSprites[i];
                    sSavedPlayerIdx = i;
                    float dx = sCamObj.x - sSprites[i].x;
                    float dz = sCamObj.z - sSprites[i].z;
                    sOrbitDist = sqrtf(dx * dx + dz * dz);
                    if (sOrbitDist < 10.0f) sOrbitDist = 10.0f;
                    break;
                }
            }
        }
        ImGui::PopStyleColor(2);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(btnW, btnH)))
        {
            sEditorMode = EditorMode::Edit;
            sCamera = sSavedEditorCam;
            sOrbitAngle = 0.0f;
            if (sSavedPlayerIdx >= 0 && sSavedPlayerIdx < sSpriteCount)
                sSprites[sSavedPlayerIdx] = sSavedPlayerSprite;
            sSavedPlayerIdx = -1;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();

    // Right-aligned status text
    float labelW = ImGui::CalcTextSize("Affinity GBA Engine").x + Scaled(20);
    ImGui::SameLine(ImGui::GetWindowWidth() - labelW);
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.55f, 1.0f), "Affinity GBA Engine");

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---- Left: Mode 7 Viewport ----
static void DrawViewport(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
    ImGui::Begin("##Viewport", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::min(avail.x / (float)kGBAWidth, avail.y / (float)kGBAHeight);
    if (scale < 1.0f) scale = 1.0f;
    float w = kGBAWidth * scale;
    float h = kGBAHeight * scale;
    float offX = (avail.x - w) * 0.5f;
    float offY = (avail.y - h) * 0.5f;
    if (offX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offX);
    if (offY > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);

    ImVec2 imgPos = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)Mode7::GetTexture(), ImVec2(w, h));

    // Click on sprite in viewport to select it
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_R))
    {
        ImVec2 mouse = ImGui::GetMousePos();
        // Convert mouse pos to GBA framebuffer coordinates
        float gbaX = (mouse.x - imgPos.x) / scale;
        float gbaY = (mouse.y - imgPos.y) / scale;

        const Mode7::SpriteScreenPos* proj;
        int projCount = Mode7::GetProjectedSprites(&proj);

        // Deselect previous
        if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            sSprites[sSelectedSprite].selected = false;
        sSelectedSprite = -1;

        // Check front-to-back (last drawn = front)
        for (int i = projCount - 1; i >= 0; i--)
        {
            float dx = gbaX - proj[i].screenX;
            float dy = gbaY - proj[i].screenY;
            if (fabsf(dx) <= proj[i].halfW && fabsf(dy) <= proj[i].halfH)
            {
                sSelectedSprite = proj[i].spriteIdx;
                sSprites[sSelectedSprite].selected = true;
                break;
            }
        }
    }

    // Right-click on viewport to place object
    static float sVPPlaceX = 0.0f;
    static float sVPPlaceZ = 0.0f;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && sSpriteCount < kMaxFloorSprites)
    {
        ImVec2 mouse = ImGui::GetMousePos();
        float gbaX = (mouse.x - imgPos.x) / scale;
        float gbaY = (mouse.y - imgPos.y) / scale;

        // Inverse Mode 7 projection: screen -> world
        int horizon2 = (int)sCamera.horizon;
        if (gbaY > horizon2 + 1)
        {
            float cosA = cosf(-sCamera.angle);
            float sinA = sinf(-sCamera.angle);
            float lambda = sCamera.height / (gbaY - horizon2);
            float lcf = lambda * cosA;
            float lsf = lambda * sinA;
            sVPPlaceX = sCamera.x + (gbaX - 120.0f) * lcf + sCamera.fov * lsf;
            sVPPlaceZ = sCamera.z + (gbaX - 120.0f) * lsf - sCamera.fov * lcf;
        }
        else
        {
            // Clicked above horizon — place at camera position
            sVPPlaceX = sCamera.x;
            sVPPlaceZ = sCamera.z;
        }
        ImGui::OpenPopup("##VPPlaceObject");
    }
    if (ImGui::BeginPopup("##VPPlaceObject"))
    {
        ImGui::TextDisabled("Place Object");
        ImGui::Separator();
        for (int t = 0; t < (int)SpriteType::Count; t++)
        {
            if (ImGui::MenuItem(kSpriteTypeNames[t]))
            {
                FloorSprite& sp = sSprites[sSpriteCount];
                sp = FloorSprite();
                sp.x = sVPPlaceX;
                sp.z = sVPPlaceZ;
                sp.y = 0.0f;
                sp.scale = 1.0f;
                sp.type = (SpriteType)t;
                sp.color = kSpriteColors[sSpriteCount % kNumSpriteColors];
                sp.selected = true;

                if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
                    sSprites[sSelectedSprite].selected = false;
                sSelectedSprite = sSpriteCount;
                sSelectedObjType = SelectedObjType::Sprite;
                sSpriteCount++;
            }
        }
        ImGui::EndPopup();
    }

    // Viewport label overlay
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(pos.x + 6, pos.y + 4),
        0x80FFFFFF, "Mode 7 Preview (WASD + Q/E + I/K)");

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---- Right top: Tileset grid ----
static void DrawTilesetPanel(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##Tileset", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "CHR / Tileset");
    ImGui::Separator();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float tileDrawSize = std::max(10.0f, (size.x - 24.0f) / (float)kTilesetCols);

    for (int row = 0; row < kTilesetRows; row++)
    {
        for (int col = 0; col < kTilesetCols; col++)
        {
            int idx = row * kTilesetCols + col;
            ImVec2 tPos(cursor.x + col * tileDrawSize, cursor.y + row * tileDrawSize);

            // Checkerboard fill to represent tile content
            uint32_t c1 = sPalette[(idx) % 16] | 0xFF000000;
            uint32_t c2 = sPalette[(idx + 3) % 16] | 0xFF000000;
            dl->AddRectFilled(tPos,
                ImVec2(tPos.x + tileDrawSize * 0.5f, tPos.y + tileDrawSize * 0.5f), c1);
            dl->AddRectFilled(ImVec2(tPos.x + tileDrawSize * 0.5f, tPos.y),
                ImVec2(tPos.x + tileDrawSize, tPos.y + tileDrawSize * 0.5f), c2);
            dl->AddRectFilled(ImVec2(tPos.x, tPos.y + tileDrawSize * 0.5f),
                ImVec2(tPos.x + tileDrawSize * 0.5f, tPos.y + tileDrawSize), c2);
            dl->AddRectFilled(ImVec2(tPos.x + tileDrawSize * 0.5f, tPos.y + tileDrawSize * 0.5f),
                ImVec2(tPos.x + tileDrawSize, tPos.y + tileDrawSize), c1);

            // Grid lines
            dl->AddRect(tPos, ImVec2(tPos.x + tileDrawSize, tPos.y + tileDrawSize),
                0x40FFFFFF);

            // Selection highlight
            if (idx == sSelectedTile)
                dl->AddRect(tPos, ImVec2(tPos.x + tileDrawSize, tPos.y + tileDrawSize),
                    0xFFFFFFFF, 0.0f, 0, 2.0f);
        }
    }

    // Invisible button for tile selection
    ImVec2 gridSize(kTilesetCols * tileDrawSize, kTilesetRows * tileDrawSize);
    ImGui::SetCursorScreenPos(cursor);
    ImGui::InvisibleButton("##TileGrid", gridSize);
    if (ImGui::IsItemClicked())
    {
        ImVec2 mouse = ImGui::GetMousePos();
        int col = (int)((mouse.x - cursor.x) / tileDrawSize);
        int row = (int)((mouse.y - cursor.y) / tileDrawSize);
        col = std::clamp(col, 0, kTilesetCols - 1);
        row = std::clamp(row, 0, kTilesetRows - 1);
        sSelectedTile = row * kTilesetCols + col;
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + gridSize.y + 4));
    ImGui::Text("Tile: %d", sSelectedTile);

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Sprites Tab: full-screen sprite asset editor ----
static void DrawSpritesTab(ImVec2 pos, ImVec2 size, float dt)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##SpritesTab", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoTitleBar);

    // ---- Left column: Asset List (20% width) ----
    float colW1 = size.x * 0.20f;
    float colW2 = size.x * 0.45f;
    float colW3 = size.x - colW1 - colW2;

    ImGui::BeginChild("##AssetList", ImVec2(colW1, 0), true);
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Sprite Assets");
    ImGui::Separator();

    for (int i = 0; i < (int)sSpriteAssets.size(); i++)
    {
        bool sel = (sSelectedAsset == i);
        if (ImGui::Selectable(sSpriteAssets[i].name.c_str(), sel))
        {
            sSelectedAsset = i;
            sSelectedFrame = 0;
            sSelectedAnim = -1;
            sAssetPreviewFrame = 0;
            sAssetPreviewTimer = 0.0f;
        }
    }

    ImGui::Separator();
    if (ImGui::Button("+ New", ImVec2(-1, 0)))
    {
        SpriteAsset a;
        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "Sprite_%d", (int)sSpriteAssets.size());
        a.name = nameBuf;
        // Default: 1 blank frame, 1 idle anim, 1 LOD tier
        SpriteFrame f;
        memset(f.pixels, 0, sizeof(f.pixels));
        f.width = 8; f.height = 8;
        a.frames.push_back(f);
        SpriteAnim anim;
        anim.name = "idle";
        anim.startFrame = 0;
        anim.endFrame = 0;
        anim.fps = 8;
        anim.loop = true;
        a.anims.push_back(anim);
        a.lod[0].size = 8;
        a.lod[0].frameStart = 0;
        a.lod[0].frameCount = 1;
        a.lod[0].maxDist = 9999.0f;
        a.lodCount = 1;
        // Default palette: transparent + white
        a.palette[0] = 0x00000000;
        a.palette[1] = 0xFFFFFFFF;
        sSpriteAssets.push_back(a);
        sAssetDirSprites.push_back({});
        sSelectedAsset = (int)sSpriteAssets.size() - 1;
        sSelectedFrame = 0;
    }
    if (sSelectedAsset >= 0 && sSelectedAsset < (int)sSpriteAssets.size())
    {
        if (ImGui::Button("Delete", ImVec2(-1, 0)))
        {
            FreeAssetDirSprites(sSelectedAsset);
            sAssetDirSprites.erase(sAssetDirSprites.begin() + sSelectedAsset);
            sSpriteAssets.erase(sSpriteAssets.begin() + sSelectedAsset);
            // Fix up FloorSprite references
            for (int i = 0; i < sSpriteCount; i++)
            {
                if (sSprites[i].assetIdx == sSelectedAsset)
                    sSprites[i].assetIdx = -1;
                else if (sSprites[i].assetIdx > sSelectedAsset)
                    sSprites[i].assetIdx--;
            }
            if (sSelectedAsset >= (int)sSpriteAssets.size())
                sSelectedAsset = (int)sSpriteAssets.size() - 1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Middle column: Frame Editor + Animation Timeline ----
    ImGui::BeginChild("##FrameEditor", ImVec2(colW2, 0), false);

    if (sSelectedAsset >= 0 && sSelectedAsset < (int)sSpriteAssets.size())
    {
        SpriteAsset& asset = sSpriteAssets[sSelectedAsset];

        // Asset name
        char nameBuf[64];
        strncpy(nameBuf, asset.name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::PushItemWidth(colW2 * 0.5f);
        if (ImGui::InputText("Name##asset", nameBuf, sizeof(nameBuf)))
            asset.name = nameBuf;
        ImGui::PopItemWidth();

        ImGui::SameLine();
        // Base size selector
        const char* sizes[] = { "8x8", "16x16", "32x32", "64x64" };
        int sizeIdx = (asset.baseSize == 64) ? 3 : (asset.baseSize == 32) ? 2 : (asset.baseSize == 16) ? 1 : 0;
        ImGui::PushItemWidth(Scaled(80));
        if (ImGui::Combo("Size##base", &sizeIdx, sizes, 4))
        {
            int newSize = (sizeIdx == 3) ? 64 : (sizeIdx == 2) ? 32 : (sizeIdx == 1) ? 16 : 8;
            asset.baseSize = newSize;
            // Resize all frames
            for (auto& fr : asset.frames)
            {
                fr.width = newSize;
                fr.height = newSize;
            }
        }
        ImGui::PopItemWidth();

        ImGui::Separator();

        // ---- Frame Grid: pixel editor / animation preview ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Frame Editor");

        // Check if we should show animation preview instead of pixel editor
        bool showAnimPreview = false;
        GLuint animPreviewTex = 0;
        int animPreviewW = 0, animPreviewH = 0;
        if (sAnimFramePlaying && sSelectedAnim >= 0 && sSelectedAnim < (int)asset.anims.size())
        {
            const SpriteAnim& selAnim = asset.anims[sSelectedAnim];
            if (selAnim.endFrame > 0 && sSelectedAsset < (int)sAssetDirSprites.size())
            {
                int animBase = GetAnimDirBase(asset, sSelectedAnim);
                int frameIdx = animBase + sAnimFrameCurrent;
                // Show N (direction 0) by default
                if (frameIdx < (int)sAssetDirSprites[sSelectedAsset].size())
                {
                    AssetDirSprite& ads = sAssetDirSprites[sSelectedAsset][frameIdx][0];
                    if (ads.texture)
                    {
                        showAnimPreview = true;
                        animPreviewTex = ads.texture;
                        animPreviewW = ads.width;
                        animPreviewH = ads.height;
                    }
                }
            }
        }

        if (showAnimPreview)
        {
            // Show the direction sprite image scaled to fill the editor area
            float gridAvail = std::min(colW2 - Scaled(20), size.y * 0.5f);
            float aspect = (float)animPreviewW / (float)animPreviewH;
            float drawW = gridAvail, drawH = gridAvail;
            if (aspect > 1.0f) drawH = gridAvail / aspect;
            else drawW = gridAvail * aspect;

            ImVec2 imgStart = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Dark background
            dl->AddRectFilled(imgStart, ImVec2(imgStart.x + drawW, imgStart.y + drawH), 0xFF1A1A1A);
            // Draw the sprite
            dl->AddImage((ImTextureID)(uintptr_t)animPreviewTex,
                imgStart, ImVec2(imgStart.x + drawW, imgStart.y + drawH));
            // Border
            dl->AddRect(imgStart, ImVec2(imgStart.x + drawW, imgStart.y + drawH), 0xFF44AAFF, 0.0f, 0, 2.0f);

            // Frame label
            const SpriteAnim& selAnim = asset.anims[sSelectedAnim];
            char frameLbl[64];
            snprintf(frameLbl, sizeof(frameLbl), "%s  Frame %d / %d", selAnim.name.c_str(), sAnimFrameCurrent + 1, selAnim.endFrame);
            ImVec2 lblSz = ImGui::CalcTextSize(frameLbl);
            dl->AddText(ImVec2(imgStart.x + (drawW - lblSz.x) * 0.5f, imgStart.y + drawH + 4), 0xFFFFFF88, frameLbl);

            ImGui::SetCursorScreenPos(ImVec2(imgStart.x, imgStart.y + drawH + 24));
        }
        else if (!asset.frames.empty())
        {
            if (sSelectedFrame >= (int)asset.frames.size())
                sSelectedFrame = 0;

            SpriteFrame& frame = asset.frames[sSelectedFrame];
            int fSize = frame.width;

            // Draw pixel grid
            float gridAvail = std::min(colW2 - Scaled(20), size.y * 0.5f);
            float cellSize = gridAvail / (float)fSize;
            cellSize = std::max(cellSize, 4.0f);
            float gridPx = cellSize * fSize;

            ImVec2 gridStart = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw pixels
            for (int py = 0; py < fSize; py++)
            {
                for (int px = 0; px < fSize; px++)
                {
                    uint8_t palIdx = frame.pixels[py * kMaxFrameSize + px];
                    uint32_t col = asset.palette[palIdx & 0xF];
                    // Convert RGBA to ABGR for ImGui
                    uint32_t r = (col >> 0) & 0xFF;
                    uint32_t g = (col >> 8) & 0xFF;
                    uint32_t b = (col >> 16) & 0xFF;
                    uint32_t a = (col >> 24) & 0xFF;
                    if (palIdx == 0) a = 0; // transparent
                    uint32_t imCol = (a << 24) | (b << 16) | (g << 8) | r;
                    if (palIdx == 0)
                        imCol = 0xFF1A1A1A; // dark bg for transparent

                    ImVec2 p0(gridStart.x + px * cellSize, gridStart.y + py * cellSize);
                    ImVec2 p1(p0.x + cellSize, p0.y + cellSize);
                    dl->AddRectFilled(p0, p1, imCol);
                }
            }

            // Grid lines
            for (int i = 0; i <= fSize; i++)
            {
                float x = gridStart.x + i * cellSize;
                float y = gridStart.y + i * cellSize;
                dl->AddLine(ImVec2(x, gridStart.y), ImVec2(x, gridStart.y + gridPx), 0x40FFFFFF);
                dl->AddLine(ImVec2(gridStart.x, y), ImVec2(gridStart.x + gridPx, y), 0x40FFFFFF);
            }

            // Click to paint
            ImGui::SetCursorScreenPos(gridStart);
            ImGui::InvisibleButton("##PixelGrid", ImVec2(gridPx, gridPx));
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 mouse = ImGui::GetMousePos();
                int px = (int)((mouse.x - gridStart.x) / cellSize);
                int py = (int)((mouse.y - gridStart.y) / cellSize);
                if (px >= 0 && px < fSize && py >= 0 && py < fSize)
                    frame.pixels[py * kMaxFrameSize + px] = (uint8_t)sSpriteEditorPalColor;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                ImVec2 mouse = ImGui::GetMousePos();
                int px = (int)((mouse.x - gridStart.x) / cellSize);
                int py = (int)((mouse.y - gridStart.y) / cellSize);
                if (px >= 0 && px < fSize && py >= 0 && py < fSize)
                    frame.pixels[py * kMaxFrameSize + px] = 0; // erase to transparent
            }

            ImGui::SetCursorScreenPos(ImVec2(gridStart.x, gridStart.y + gridPx + 4));

            // Paint color selector (palette row)
            ImGui::Text("Paint:");
            ImGui::SameLine();
            for (int c = 0; c < 16; c++)
            {
                ImGui::PushID(c + 1000);
                uint32_t col = asset.palette[c];
                uint32_t r = (col >> 0) & 0xFF;
                uint32_t g = (col >> 8) & 0xFF;
                uint32_t b = (col >> 16) & 0xFF;
                ImVec4 cv(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
                if (c == 0) cv = ImVec4(0.1f, 0.1f, 0.1f, 1.0f); // transparent shown as dark

                bool isSel = (sSpriteEditorPalColor == c);
                if (isSel)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, cv);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 1));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, cv);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                }
                if (ImGui::Button("##pal", ImVec2(Scaled(16), Scaled(16))))
                    sSpriteEditorPalColor = c;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::NewLine();

            // Frame list strip
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Frames (%d)", (int)asset.frames.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("+##addframe"))
            {
                SpriteFrame nf;
                memset(nf.pixels, 0, sizeof(nf.pixels));
                nf.width = asset.baseSize;
                nf.height = asset.baseSize;
                asset.frames.push_back(nf);
            }
            ImGui::SameLine();
            if (asset.frames.size() > 1 && ImGui::SmallButton("-##delframe"))
            {
                asset.frames.erase(asset.frames.begin() + sSelectedFrame);
                if (sSelectedFrame >= (int)asset.frames.size())
                    sSelectedFrame = (int)asset.frames.size() - 1;
                // Fix anim references
                for (auto& a : asset.anims)
                {
                    if (a.endFrame >= (int)asset.frames.size())
                        a.endFrame = (int)asset.frames.size() - 1;
                    if (a.startFrame > a.endFrame)
                        a.startFrame = a.endFrame;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Dup##dupframe"))
            {
                if (sSelectedFrame < (int)asset.frames.size())
                {
                    SpriteFrame dup = asset.frames[sSelectedFrame];
                    asset.frames.insert(asset.frames.begin() + sSelectedFrame + 1, dup);
                }
            }

            // Thumbnail strip
            float thumbSize = Scaled(32);
            for (int fi = 0; fi < (int)asset.frames.size(); fi++)
            {
                ImGui::PushID(fi + 2000);
                bool fsel = (sSelectedFrame == fi);

                ImVec2 thumbStart = ImGui::GetCursorScreenPos();
                uint32_t borderCol = fsel ? 0xFFFFFFFF : 0xFF444444;
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                tdl->AddRect(thumbStart, ImVec2(thumbStart.x + thumbSize, thumbStart.y + thumbSize), borderCol);

                // Mini preview of frame
                int fs = asset.frames[fi].width;
                float thumbCell = thumbSize / (float)fs;
                for (int ty = 0; ty < fs; ty++)
                    for (int tx = 0; tx < fs; tx++)
                    {
                        uint8_t pi = asset.frames[fi].pixels[ty * kMaxFrameSize + tx];
                        if (pi == 0) continue;
                        uint32_t col = asset.palette[pi & 0xF];
                        uint32_t cr = (col >> 0) & 0xFF;
                        uint32_t cg = (col >> 8) & 0xFF;
                        uint32_t cb = (col >> 16) & 0xFF;
                        uint32_t imCol = 0xFF000000 | (cb << 16) | (cg << 8) | cr;
                        ImVec2 tp0(thumbStart.x + tx * thumbCell, thumbStart.y + ty * thumbCell);
                        ImVec2 tp1(tp0.x + thumbCell, tp0.y + thumbCell);
                        tdl->AddRectFilled(tp0, tp1, imCol);
                    }

                ImGui::SetCursorScreenPos(thumbStart);
                if (ImGui::InvisibleButton("##fthumb", ImVec2(thumbSize, thumbSize)))
                    sSelectedFrame = fi;
                ImGui::SameLine();
                ImGui::PopID();
            }
            ImGui::NewLine();
        }

        // ---- Animation Timeline ----
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Animations (%d)", (int)asset.anims.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("+##addanim"))
        {
            SpriteAnim na;
            char abuf[32];
            snprintf(abuf, sizeof(abuf), "anim_%d", (int)asset.anims.size());
            na.name = abuf;
            na.startFrame = 0;  // viewFrame: which direction frame to view
            na.endFrame = 0;    // frameCount: number of direction frame sets
            na.fps = 8;
            na.loop = true;
            asset.anims.push_back(na);
        }

        // Auto-select if none selected or out of range
        if (sSelectedAnim < 0 || sSelectedAnim >= (int)asset.anims.size())
            sSelectedAnim = asset.anims.empty() ? -1 : 0;

        for (int ai = 0; ai < (int)asset.anims.size(); ai++)
        {
            ImGui::PushID(ai + 3000);
            SpriteAnim& anim = asset.anims[ai];
            bool animSel = (sSelectedAnim == ai);

            if (ImGui::Selectable(anim.name.c_str(), animSel, 0, ImVec2(Scaled(80), 0)))
            {
                if (sSelectedAnim != ai) { sAnimFramePlaying = false; sAnimFrameTime = 0.0f; sAnimFrameCurrent = 0; }
                sSelectedAnim = ai;
            }
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(60));
            char nbuf[32];
            strncpy(nbuf, anim.name.c_str(), sizeof(nbuf) - 1); nbuf[sizeof(nbuf)-1] = '\0';
            if (ImGui::InputText("##aname", nbuf, sizeof(nbuf)))
                anim.name = nbuf;
            ImGui::PopItemWidth();
            ImGui::SameLine();

            // viewFrame (1-indexed in UI, 0-indexed internally)
            // Auto-expand: if user sets viewFrame beyond frameCount, grow to match
            ImGui::PushItemWidth(Scaled(40));
            int viewUI = anim.endFrame > 0 ? anim.startFrame + 1 : 0; // display 1-indexed, 0 when no frames
            if (ImGui::DragInt("##astart", &viewUI, 1.0f, 0, 16))
            {
                if (viewUI < 0) viewUI = 0;
                anim.startFrame = viewUI > 0 ? viewUI - 1 : 0; // back to 0-indexed
                if (viewUI > anim.endFrame && viewUI <= 16)
                {
                    // Auto-expand frameCount to match
                    int newCount = viewUI;
                    int base = GetAnimDirBase(asset, ai);
                    for (int n = anim.endFrame; n < newCount; n++)
                    {
                        SpriteAsset::DirAnimSet das;
                        das.name = anim.name;
                        int insertAt = base + n;
                        if (insertAt > (int)asset.dirAnimSets.size())
                            asset.dirAnimSets.resize(insertAt);
                        asset.dirAnimSets.insert(asset.dirAnimSets.begin() + insertAt, das);
                        if (sSelectedAsset < (int)sAssetDirSprites.size())
                        {
                            std::array<AssetDirSprite, 8> emptyDir = {};
                            if (insertAt > (int)sAssetDirSprites[sSelectedAsset].size())
                                sAssetDirSprites[sSelectedAsset].resize(insertAt);
                            sAssetDirSprites[sSelectedAsset].insert(
                                sAssetDirSprites[sSelectedAsset].begin() + insertAt, emptyDir);
                        }
                    }
                    anim.endFrame = newCount;
                    sProjectDirty = true;
                }
                if (viewUI == 0) anim.startFrame = 0;
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::Text("-");
            ImGui::SameLine();

            // frameCount (1-indexed in UI = same as internal count)
            ImGui::PushItemWidth(Scaled(40));
            int fc = anim.endFrame;
            ImGui::DragInt("##aend", &fc, 1.0f, 0, 16);
            if (fc != anim.endFrame)
            {
                fc = std::clamp(fc, 0, 16);
                int base = GetAnimDirBase(asset, ai);
                if (fc > anim.endFrame)
                {
                    for (int n = anim.endFrame; n < fc; n++)
                    {
                        SpriteAsset::DirAnimSet das;
                        das.name = anim.name;
                        int insertAt = base + n;
                        if (insertAt > (int)asset.dirAnimSets.size())
                            asset.dirAnimSets.resize(insertAt);
                        asset.dirAnimSets.insert(asset.dirAnimSets.begin() + insertAt, das);
                        if (sSelectedAsset < (int)sAssetDirSprites.size())
                        {
                            std::array<AssetDirSprite, 8> emptyDir = {};
                            if (insertAt > (int)sAssetDirSprites[sSelectedAsset].size())
                                sAssetDirSprites[sSelectedAsset].resize(insertAt);
                            sAssetDirSprites[sSelectedAsset].insert(
                                sAssetDirSprites[sSelectedAsset].begin() + insertAt, emptyDir);
                        }
                    }
                }
                else
                {
                    int removeStart = base + fc;
                    int removeEnd = base + anim.endFrame;
                    if (sSelectedAsset < (int)sAssetDirSprites.size())
                    {
                        int rEnd = std::min(removeEnd, (int)sAssetDirSprites[sSelectedAsset].size());
                        for (int ri = removeStart; ri < rEnd; ri++)
                            for (int d2 = 0; d2 < 8; d2++)
                            {
                                auto& s = sAssetDirSprites[sSelectedAsset][ri][d2];
                                if (s.pixels) { stbi_image_free(s.pixels); s.pixels = nullptr; }
                                if (s.texture) { glDeleteTextures(1, &s.texture); s.texture = 0; }
                            }
                        sAssetDirSprites[sSelectedAsset].erase(
                            sAssetDirSprites[sSelectedAsset].begin() + removeStart,
                            sAssetDirSprites[sSelectedAsset].begin() + std::min(removeEnd, (int)sAssetDirSprites[sSelectedAsset].size()));
                    }
                    asset.dirAnimSets.erase(
                        asset.dirAnimSets.begin() + removeStart,
                        asset.dirAnimSets.begin() + std::min(removeEnd, (int)asset.dirAnimSets.size()));
                }
                anim.endFrame = fc;
                anim.startFrame = std::clamp(anim.startFrame, 0, std::max(0, anim.endFrame - 1));
                // Update hasDirections
                asset.hasDirections = false;
                if (sSelectedAsset < (int)sAssetDirSprites.size())
                    for (int s2 = 0; s2 < (int)sAssetDirSprites[sSelectedAsset].size(); s2++)
                        for (int d2 = 0; d2 < 8; d2++)
                            if (sAssetDirSprites[sSelectedAsset][s2][d2].pixels)
                            { asset.hasDirections = true; goto done_resize; }
                done_resize:;
                sProjectDirty = true;
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::Checkbox("Loop##aloop", &anim.loop);
            ImGui::SameLine();
            ImGui::PushItemWidth(Scaled(50));
            ImGui::DragFloat("##aspeed", &anim.speed, 0.05f, 0.0f, 10.0f, "%.1f");
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed");
            ImGui::SameLine();

            // Game state dropdown
            ImGui::PushItemWidth(Scaled(55));
            int gs = (int)anim.gameState;
            if (ImGui::Combo("##astate", &gs, kAnimStateNames, (int)AnimState::Count))
                anim.gameState = (AnimState)gs;
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Game State");
            ImGui::SameLine();

            if (asset.anims.size() > 1 && ImGui::SmallButton("X##delanim"))
            {
                // Remove this animation's direction sets
                int base = GetAnimDirBase(asset, ai);
                int count = anim.endFrame;
                if (sSelectedAsset < (int)sAssetDirSprites.size() && count > 0)
                {
                    int rEnd = std::min(base + count, (int)sAssetDirSprites[sSelectedAsset].size());
                    for (int ri = base; ri < rEnd; ri++)
                        for (int d2 = 0; d2 < 8; d2++)
                        {
                            auto& s = sAssetDirSprites[sSelectedAsset][ri][d2];
                            if (s.pixels) { stbi_image_free(s.pixels); s.pixels = nullptr; }
                            if (s.texture) { glDeleteTextures(1, &s.texture); s.texture = 0; }
                        }
                    sAssetDirSprites[sSelectedAsset].erase(
                        sAssetDirSprites[sSelectedAsset].begin() + base,
                        sAssetDirSprites[sSelectedAsset].begin() + rEnd);
                }
                if (count > 0 && base < (int)asset.dirAnimSets.size())
                    asset.dirAnimSets.erase(
                        asset.dirAnimSets.begin() + base,
                        asset.dirAnimSets.begin() + std::min(base + count, (int)asset.dirAnimSets.size()));
                asset.anims.erase(asset.anims.begin() + ai);
                if (sSelectedAnim >= (int)asset.anims.size())
                    sSelectedAnim = (int)asset.anims.size() - 1;
                sProjectDirty = true;
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        // (Direction grid is in the right panel "Animation Frames" section)
    }
    else
    {
        ImGui::TextWrapped("Select or create a sprite asset.");
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right column: LOD + Palette + Linked Objects + Animation Frames ----
    ImGui::BeginChild("##LODPalette", ImVec2(colW3, 0), true);

    if (sSelectedAsset >= 0 && sSelectedAsset < (int)sSpriteAssets.size())
    {
        SpriteAsset& asset = sSpriteAssets[sSelectedAsset];

        // ---- LOD Tiers ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "LOD Tiers");
        ImGui::Separator();

        for (int li = 0; li < asset.lodCount; li++)
        {
            ImGui::PushID(li + 4000);
            SpriteLOD& lod = asset.lod[li];

            const char* lodSizes[] = { "8x8", "16x16", "32x32" };
            int lodSzIdx = (lod.size == 32) ? 2 : (lod.size == 16) ? 1 : 0;
            ImGui::PushItemWidth(Scaled(70));
            if (ImGui::Combo("##lodsize", &lodSzIdx, lodSizes, 3))
                lod.size = (lodSzIdx == 2) ? 32 : (lodSzIdx == 1) ? 16 : 8;
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(60));
            ImGui::DragFloat("dist##lod", &lod.maxDist, 10.0f, 0.0f, 9999.0f, "%.0f");
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(40));
            ImGui::DragInt("start##lod", &lod.frameStart, 1.0f, 0, std::max(0, (int)asset.frames.size() - 1));
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(40));
            ImGui::DragInt("cnt##lod", &lod.frameCount, 1.0f, 0, (int)asset.frames.size());
            ImGui::PopItemWidth();

            ImGui::PopID();
        }

        if (asset.lodCount < kMaxSpriteLODs && ImGui::SmallButton("+##addlod"))
        {
            SpriteLOD& nl = asset.lod[asset.lodCount];
            nl.size = 8;
            nl.frameStart = 0;
            nl.frameCount = 1;
            nl.maxDist = 500.0f;
            asset.lodCount++;
        }
        if (asset.lodCount > 1)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("-##dellod"))
                asset.lodCount--;
        }

        ImGui::Spacing();

        // ---- Palette Editor ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Palette (16 colors)");
        ImGui::Separator();

        // Palette source selector
        {
            const char* palSrcPreview = (asset.paletteSrc >= 0 && asset.paletteSrc < (int)sSpriteAssets.size()
                                         && asset.paletteSrc != sSelectedAsset)
                ? sSpriteAssets[asset.paletteSrc].name.c_str() : "Own";
            if (ImGui::BeginCombo("Source##palsrc", palSrcPreview))
            {
                if (ImGui::Selectable("Own", asset.paletteSrc < 0))
                    asset.paletteSrc = -1;
                for (int pi = 0; pi < (int)sSpriteAssets.size(); pi++)
                {
                    if (pi == sSelectedAsset) continue; // can't reference self
                    bool sel = (asset.paletteSrc == pi);
                    if (ImGui::Selectable(sSpriteAssets[pi].name.c_str(), sel))
                    {
                        asset.paletteSrc = pi;
                        // Copy palette and bank from source
                        memcpy(asset.palette, sSpriteAssets[pi].palette, sizeof(asset.palette));
                        asset.palBank = sSpriteAssets[pi].palBank;
                    }
                }
                ImGui::EndCombo();
            }
        }

        // If sharing palette, sync from source and show read-only
        bool palReadOnly = false;
        if (asset.paletteSrc >= 0 && asset.paletteSrc < (int)sSpriteAssets.size()
            && asset.paletteSrc != sSelectedAsset)
        {
            SpriteAsset& srcAsset = sSpriteAssets[asset.paletteSrc];
            memcpy(asset.palette, srcAsset.palette, sizeof(asset.palette));
            asset.palBank = srcAsset.palBank;
            palReadOnly = true;
            ImGui::TextDisabled("Shared from %s (bank %d)", srcAsset.name.c_str(), srcAsset.palBank);
        }
        else
        {
            ImGui::PushItemWidth(Scaled(50));
            ImGui::DragInt("Bank##palbank", &asset.palBank, 1.0f, 0, 15);
            ImGui::PopItemWidth();
        }

        for (int c = 0; c < 16; c++)
        {
            ImGui::PushID(c + 5000);
            uint32_t col = asset.palette[c];
            float rgba[4] = {
                ((col >> 0)  & 0xFF) / 255.0f,
                ((col >> 8)  & 0xFF) / 255.0f,
                ((col >> 16) & 0xFF) / 255.0f,
                ((col >> 24) & 0xFF) / 255.0f
            };
            char clbl[16];
            snprintf(clbl, sizeof(clbl), "%d##pcol", c);
            if (palReadOnly)
            {
                // Show color swatch but don't allow editing
                ImGui::ColorButton(clbl, ImVec4(rgba[0], rgba[1], rgba[2], 1.0f),
                    ImGuiColorEditFlags_NoTooltip, ImVec2(Scaled(16), Scaled(16)));
            }
            else if (ImGui::ColorEdit4(clbl, rgba,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha))
            {
                asset.palette[c] =
                    ((uint32_t)(rgba[0] * 255) << 0) |
                    ((uint32_t)(rgba[1] * 255) << 8) |
                    ((uint32_t)(rgba[2] * 255) << 16) |
                    0xFF000000;
                if (c == 0) asset.palette[c] = 0x00000000; // index 0 always transparent
            }
            if ((c & 3) != 3) ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::Spacing();

        // ---- Import Strip ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Import");
        ImGui::Separator();

        ImGui::PushItemWidth(Scaled(50));
        ImGui::DragInt("Frame W##stripw", &asset.stripFrameW, 1.0f, 1, 256);
        ImGui::DragInt("Frame H##striph", &asset.stripFrameH, 1.0f, 1, 256);
        ImGui::PopItemWidth();

        if (!asset.sourceImagePath.empty())
        {
            std::string fname = std::filesystem::path(asset.sourceImagePath).filename().string();
            ImGui::Text("Source: %s", fname.c_str());
        }

#ifdef _WIN32
        if (ImGui::Button("Import PNG...##importstrip"))
        {
            std::string path = OpenFileDialog(
                "PNG Images\0*.png\0All Files\0*.*\0", "png");
            if (!path.empty())
            {
                int imgW, imgH, channels;
                unsigned char* imgData = stbi_load(path.c_str(), &imgW, &imgH, &channels, 4);
                if (imgData)
                {
                    asset.sourceImagePath = path;

                    // Auto-detect frame size if not set
                    if (asset.stripFrameW <= 0) asset.stripFrameW = asset.baseSize;
                    if (asset.stripFrameH <= 0) asset.stripFrameH = asset.baseSize;
                    int fw = asset.stripFrameW;
                    int fh = asset.stripFrameH;

                    // Count frames in the strip (horizontal layout)
                    int framesX = imgW / fw;
                    int framesY = imgH / fh;
                    int totalFrames = framesX * framesY;
                    if (totalFrames < 1) totalFrames = 1;

                    // Extract unique colors for palette (index 0 = transparent)
                    // Collect ALL unique colors, then merge closest pairs down to 15
                    asset.palette[0] = 0x00000000;

                    struct ColorEntry { uint32_t col; int count; };
                    std::vector<ColorEntry> allColors;
                    for (int py = 0; py < imgH; py++)
                    {
                        for (int px = 0; px < imgW; px++)
                        {
                            const unsigned char* p = imgData + (py * imgW + px) * 4;
                            if (p[3] < 128) continue;
                            uint32_t col = p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000;
                            bool found = false;
                            for (size_t c = 0; c < allColors.size(); c++)
                            {
                                if (allColors[c].col == col) { allColors[c].count++; found = true; break; }
                            }
                            if (!found)
                                allColors.push_back({col, 1});
                        }
                    }

                    // Merge closest color pairs until <= 15 remain
                    while ((int)allColors.size() > 15)
                    {
                        int bestI = 0, bestJ = 1, bestDist = INT_MAX;
                        for (size_t i = 0; i < allColors.size(); i++)
                        {
                            for (size_t j = i + 1; j < allColors.size(); j++)
                            {
                                int dr = (int)(allColors[i].col & 0xFF) - (int)(allColors[j].col & 0xFF);
                                int dg = (int)((allColors[i].col >> 8) & 0xFF) - (int)((allColors[j].col >> 8) & 0xFF);
                                int db = (int)((allColors[i].col >> 16) & 0xFF) - (int)((allColors[j].col >> 16) & 0xFF);
                                int dist = dr*dr + dg*dg + db*db;
                                if (dist < bestDist) { bestDist = dist; bestI = (int)i; bestJ = (int)j; }
                            }
                        }
                        if (allColors[bestI].count < allColors[bestJ].count)
                            allColors[bestI].col = allColors[bestJ].col;
                        allColors[bestI].count += allColors[bestJ].count;
                        allColors.erase(allColors.begin() + bestJ);
                    }

                    uint32_t uniqueColors[16] = {};
                    int numUnique = (int)allColors.size();
                    for (int c = 0; c < numUnique; c++)
                    {
                        uniqueColors[c] = allColors[c].col;
                        asset.palette[c + 1] = allColors[c].col;
                    }

                    // Slice into frames
                    asset.frames.clear();
                    for (int fy = 0; fy < framesY; fy++)
                    {
                        for (int fx = 0; fx < framesX; fx++)
                        {
                            SpriteFrame frame;
                            frame.width = asset.baseSize;
                            frame.height = asset.baseSize;
                            memset(frame.pixels, 0, sizeof(frame.pixels));

                            for (int py = 0; py < fh && py < asset.baseSize; py++)
                            {
                                for (int px = 0; px < fw && px < asset.baseSize; px++)
                                {
                                    int srcX = fx * fw + px;
                                    int srcY = fy * fh + py;
                                    if (srcX >= imgW || srcY >= imgH) continue;

                                    const unsigned char* p = imgData + (srcY * imgW + srcX) * 4;
                                    if (p[3] < 128) { frame.pixels[py * kMaxFrameSize + px] = 0; continue; }

                                    uint32_t col = p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000;
                                    // Find closest palette entry
                                    uint8_t bestIdx = 1;
                                    int bestDist = INT_MAX;
                                    for (int c = 0; c < numUnique; c++)
                                    {
                                        uint32_t pc = uniqueColors[c];
                                        int dr = (int)(col & 0xFF) - (int)(pc & 0xFF);
                                        int dg = (int)((col >> 8) & 0xFF) - (int)((pc >> 8) & 0xFF);
                                        int db = (int)((col >> 16) & 0xFF) - (int)((pc >> 16) & 0xFF);
                                        int dist = dr*dr + dg*dg + db*db;
                                        if (dist < bestDist) { bestDist = dist; bestIdx = (uint8_t)(c + 1); }
                                    }
                                    frame.pixels[py * kMaxFrameSize + px] = bestIdx;
                                }
                            }
                            asset.frames.push_back(frame);
                        }
                    }

                    // Add default animation if none exist
                    if (asset.anims.empty())
                    {
                        SpriteAnim anim;
                        anim.name = "idle";
                        anim.startFrame = 0;
                        anim.endFrame = 0; // no direction frames by default
                        anim.fps = 8;
                        anim.loop = true;
                        asset.anims.push_back(anim);
                    }

                    sProjectDirty = true;
                    stbi_image_free(imgData);
                }
            }
        }
#endif
        if (asset.sourceImagePath.empty())
            ImGui::TextDisabled("No image imported.");

        ImGui::Spacing();

        // ---- Object Link Info ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Linked Objects");
        ImGui::Separator();

        int linkCount = 0;
        for (int i = 0; i < sSpriteCount; i++)
        {
            if (sSprites[i].assetIdx == sSelectedAsset)
            {
                ImGui::Text("  Sprite %d", i);
                linkCount++;
            }
        }
        if (linkCount == 0)
            ImGui::TextDisabled("  No objects linked");

        ImGui::Spacing();

        // ---- Animation Frames (8-direction grids) ----
        // Show ALL frames for the selected animation, with a play button
        if (sSelectedAnim >= 0 && sSelectedAnim < (int)asset.anims.size())
        {
            SpriteAnim& anim = asset.anims[sSelectedAnim];
            int base = GetAnimDirBase(asset, sSelectedAnim);

            // Header with play button
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Animation Frames - %s", anim.name.c_str());
            ImGui::SameLine();

            // Play/Pause button
            if (anim.endFrame > 1)
            {
                if (ImGui::SmallButton(sAnimFramePlaying ? "||##animstop" : ">##animplay"))
                {
                    sAnimFramePlaying = !sAnimFramePlaying;
                    if (sAnimFramePlaying)
                    {
                        sAnimFrameTime = 0.0f;
                        sAnimFrameCurrent = 0;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(sAnimFramePlaying ? "Pause" : "Play");
            }

            ImGui::Separator();

            // Advance playback timer
            if (sAnimFramePlaying && anim.endFrame > 1)
            {
                float dt = ImGui::GetIO().DeltaTime;
                int fps = anim.fps > 0 ? anim.fps : 8;
                float effectiveFps = fps * anim.speed;
                if (effectiveFps > 0.0f)
                {
                    sAnimFrameTime += dt;
                    float frameDur = 1.0f / effectiveFps;
                    if (sAnimFrameTime >= frameDur)
                    {
                        sAnimFrameTime -= frameDur;
                        sAnimFrameCurrent++;
                        if (sAnimFrameCurrent >= anim.endFrame)
                            sAnimFrameCurrent = anim.loop ? 0 : anim.endFrame - 1;
                    }
                }
            }

            // Clamp playback frame
            if (sAnimFrameCurrent >= anim.endFrame)
                sAnimFrameCurrent = 0;

            // Show frame indicator when playing
            if (sAnimFramePlaying && anim.endFrame > 1)
            {
                ImGui::Text("Frame %d / %d", sAnimFrameCurrent + 1, anim.endFrame);
            }

            // Show all frames for this animation
            float dirCellSz = Scaled(52);
            float dirPreviewSz = dirCellSz - 22.0f;
            int dirCols = 4;

            for (int fi = 0; fi < anim.endFrame; fi++)
            {
                int dirSetIdx = base + fi;
                EnsureAssetDirSet(sSelectedAsset, dirSetIdx);

                ImGui::PushID(fi + 8000);

                // Frame header with highlight for current playback frame
                bool isPlayFrame = sAnimFramePlaying && (fi == sAnimFrameCurrent);
                if (isPlayFrame)
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Frame %d", fi + 1);
                else
                    ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.9f, 1.0f), "Frame %d", fi + 1);

                for (int d = 0; d < 8; d++)
                {
                    if (d % dirCols != 0) ImGui::SameLine();
                    ImGui::PushID(d + 5000 + dirSetIdx * 100);

                    ImVec2 cpos = ImGui::GetCursorScreenPos();
                    ImVec2 cmin = cpos;
                    ImVec2 cmax = ImVec2(cpos.x + dirCellSz - 4, cpos.y + dirCellSz - 4);
                    ImDrawList* dl2 = ImGui::GetWindowDrawList();

                    bool clicked = ImGui::InvisibleButton("##adirbtn", ImVec2(dirCellSz - 4, dirCellSz - 4));
                    bool hovered = ImGui::IsItemHovered();

                    dl2->AddRectFilled(cmin, cmax, isPlayFrame ? 0xFF202530 : 0xFF151520);
                    uint32_t borderCol = isPlayFrame ? 0xFF44AAFF : (hovered ? 0xFFFFFFFF : 0xFF444466);
                    dl2->AddRect(cmin, cmax, borderCol, 0.0f, 0, (isPlayFrame || hovered) ? 2.0f : 1.0f);

                    char dirLabel[16];
                    int frameNum = fi + 1; // 1-indexed
                    if (frameNum <= 1)
                        snprintf(dirLabel, sizeof(dirLabel), "%s", kDirNames[d]);
                    else
                        snprintf(dirLabel, sizeof(dirLabel), "%s%d", kDirNames[d], frameNum);
                    ImVec2 lsz2 = ImGui::CalcTextSize(dirLabel);
                    dl2->AddText(ImVec2(cmin.x + (dirCellSz - 4 - lsz2.x) * 0.5f, cmin.y + 2), 0xFFAAAACC, dirLabel);

                    AssetDirSprite& ads = sAssetDirSprites[sSelectedAsset][dirSetIdx][d];
                    if (ads.texture)
                    {
                        float aspect = (float)ads.width / (float)ads.height;
                        float drawW = dirPreviewSz, drawH = dirPreviewSz;
                        if (aspect > 1.0f) drawH = dirPreviewSz / aspect;
                        else drawW = dirPreviewSz * aspect;
                        float imgX = cmin.x + (dirCellSz - 4 - drawW) * 0.5f;
                        float imgY = cmin.y + 16.0f + (dirPreviewSz - drawH) * 0.5f;
                        dl2->AddImage((ImTextureID)(uintptr_t)ads.texture,
                            ImVec2(imgX, imgY), ImVec2(imgX + drawW, imgY + drawH));
                    }
                    else
                    {
                        const char* emptyTxt = "Click";
                        ImVec2 esz2 = ImGui::CalcTextSize(emptyTxt);
                        dl2->AddText(
                            ImVec2(cmin.x + (dirCellSz - 4 - esz2.x) * 0.5f,
                                   cmin.y + 16.0f + (dirPreviewSz - esz2.y) * 0.5f),
                            0xFF666688, emptyTxt);
                    }

                    if (clicked)
                    {
#ifdef _WIN32
                        std::string path = OpenFileDialog(
                            "PNG Images\0*.png\0All Files\0*.*\0", "png");
                        if (!path.empty())
                        {
                            LoadAssetDirImage(sSelectedAsset, dirSetIdx, d, path);
                            sProjectDirty = true;
                        }
#endif
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && ads.pixels)
                    {
                        stbi_image_free(ads.pixels); ads.pixels = nullptr;
                        if (ads.texture) { glDeleteTextures(1, &ads.texture); ads.texture = 0; }
                        ads.width = ads.height = 0;
                        asset.dirAnimSets[dirSetIdx].dirPaths[d].clear();
                        asset.hasDirections = false;
                        for (int s2 = 0; s2 < (int)sAssetDirSprites[sSelectedAsset].size(); s2++)
                            for (int d2 = 0; d2 < 8; d2++)
                                if (sAssetDirSprites[sSelectedAsset][s2][d2].pixels)
                                { asset.hasDirections = true; goto done_clear_r; }
                        done_clear_r:;
                        sProjectDirty = true;
                    }

                    ImGui::PopID();
                }

#ifdef _WIN32
                ImGui::Spacing();
                if (ImGui::Button("Load Dir Folder...##adf"))
                {
                    // Set viewFrame to this frame so folder load targets the right set
                    anim.startFrame = fi;
                    std::string folder = OpenFolderDialog();
                    if (!folder.empty())
                    {
                        static const char* const altNames[][4] = {
                            { "N", "n", "forward", "up" },
                            { "NE", "ne", "rightup", "upright" },
                            { "E", "e", "right", nullptr },
                            { "SE", "se", "rightdown", "downright" },
                            { "S", "s", "backwards", "down" },
                            { "SW", "sw", "leftdown", "downleft" },
                            { "W", "w", "left", nullptr },
                            { "NW", "nw", "leftup", "upleft" },
                        };
                        for (int d = 0; d < 8; d++)
                        {
                            for (int a = 0; a < 4; a++)
                            {
                                if (!altNames[d][a]) continue;
                                std::string tryPath = folder + "\\" + altNames[d][a] + ".png";
                                if (std::filesystem::exists(tryPath))
                                {
                                    LoadAssetDirImage(sSelectedAsset, dirSetIdx, d, tryPath);
                                    sProjectDirty = true;
                                    break;
                                }
                            }
                        }
                    }
                }
#endif
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::PopID();
            }

            if (anim.endFrame <= 0)
                ImGui::TextDisabled("No frames. Set frame count in the animation row.");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Animation Frames");
            ImGui::Separator();
            ImGui::TextDisabled("Select an animation.");
        }
    }

    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Right top: Object Editor (replaces Tileset in Edit mode) ----
static void DrawObjectEditorPanel(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##ObjectEditor", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Objects");
    ImGui::Separator();

    float listH = size.y * 0.35f;
    ImGui::BeginChild("##ObjList", ImVec2(0, listH), true);

    // Camera object entry
    bool camSel = (sSelectedObjType == SelectedObjType::Camera);
    if (ImGui::Selectable("Camera Start", camSel))
    {
        if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            sSprites[sSelectedSprite].selected = false;
        sSelectedSprite = -1;
        sSelectedObjType = SelectedObjType::Camera;
    }

    // Sprite entries
    for (int i = 0; i < sSpriteCount; i++)
    {
        char label[64];
        const char* typeName = kSpriteTypeNames[(int)sSprites[i].type];
        snprintf(label, sizeof(label), "%s %d", typeName, i);
        bool sel = (sSelectedObjType == SelectedObjType::Sprite && sSelectedSprite == i);

        // Color dot
        uint32_t col = sSprites[i].color;
        ImVec4 dotCol((col & 0xFF) / 255.0f, ((col >> 8) & 0xFF) / 255.0f,
                      ((col >> 16) & 0xFF) / 255.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, dotCol);
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();

        if (ImGui::Selectable(label, sel))
        {
            if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
                sSprites[sSelectedSprite].selected = false;
            sSelectedSprite = i;
            sSelectedObjType = SelectedObjType::Sprite;
            sSprites[i].selected = true;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Properties for selected object
    if (sSelectedObjType == SelectedObjType::Camera)
    {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Camera Properties");
        ImGui::PushItemWidth(size.x * 0.5f);
        ImGui::DragFloat("X##cam", &sCamObj.x, 1.0f);
        ImGui::DragFloat("Z##cam", &sCamObj.z, 1.0f);
        ImGui::DragFloat("Height##cam", &sCamObj.height, 0.5f, 4.0f, 256.0f);
        ImGui::SliderAngle("Angle##cam", &sCamObj.angle, -180.0f, 180.0f);
        ImGui::DragFloat("Horizon##cam", &sCamObj.horizon, 0.5f, 10.0f, 120.0f);
        ImGui::Separator();
        ImGui::Text("Movement");
        ImGui::DragFloat("Walk Speed##cam",   &sCamObj.walkSpeed,   0.5f, 5.0f, 100.0f, "%.0f");
        ImGui::DragFloat("Sprint Speed##cam", &sCamObj.sprintSpeed, 0.5f, 5.0f, 150.0f, "%.0f");
        ImGui::Separator();
        ImGui::Text("Camera Follow");
        ImGui::DragFloat("Walk Ease In##cam",  &sCamObj.walkEaseIn,  0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Walk Ease Out##cam", &sCamObj.walkEaseOut, 0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Sprint Ease In##cam",  &sCamObj.sprintEaseIn,  0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Sprint Ease Out##cam", &sCamObj.sprintEaseOut, 0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::Separator();
        ImGui::DragFloat("Icon Size##cam", &sCamObjEditorScale, 0.01f, 0.1f, 2.0f, "%.2f");
        ImGui::PopItemWidth();
    }
    else if (sSelectedObjType == SelectedObjType::Sprite && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
    {
        FloorSprite& sp = sSprites[sSelectedSprite];
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Object Properties");
        ImGui::PushItemWidth(size.x * 0.5f);
        if (ImGui::BeginCombo("Type##sprtype", kSpriteTypeNames[(int)sp.type]))
        {
            for (int t = 0; t < (int)SpriteType::Count; t++)
            {
                bool sel = ((int)sp.type == t);
                if (ImGui::Selectable(kSpriteTypeNames[t], sel))
                    sp.type = (SpriteType)t;
            }
            ImGui::EndCombo();
        }
        ImGui::DragFloat("X##spr", &sp.x, 1.0f, -kWorldHalf, kWorldHalf);
        ImGui::DragFloat("Y##spr", &sp.y, 0.5f, 0.0f, 200.0f);
        ImGui::DragFloat("Z##spr", &sp.z, 1.0f, -kWorldHalf, kWorldHalf);
        ImGui::DragFloat("Scale##spr", &sp.scale, 0.1f, 0.1f, 50.0f);
        ImGui::DragFloat("Rotation##spr", &sp.rotation, 1.0f, 0.0f, 360.0f, "%.0f deg");

        // Sprite asset link
        {
            const char* preview = (sp.assetIdx >= 0 && sp.assetIdx < (int)sSpriteAssets.size())
                ? sSpriteAssets[sp.assetIdx].name.c_str() : "(none)";
            if (ImGui::BeginCombo("Asset##sprlink", preview))
            {
                if (ImGui::Selectable("(none)", sp.assetIdx < 0))
                    sp.assetIdx = -1;
                for (int ai = 0; ai < (int)sSpriteAssets.size(); ai++)
                {
                    bool sel = (sp.assetIdx == ai);
                    if (ImGui::Selectable(sSpriteAssets[ai].name.c_str(), sel))
                        sp.assetIdx = ai;
                }
                ImGui::EndCombo();
            }
        }
        if (sp.assetIdx >= 0 && sp.assetIdx < (int)sSpriteAssets.size())
        {
            SpriteAsset& linkedAsset = sSpriteAssets[sp.assetIdx];
            if (!linkedAsset.anims.empty())
            {
                // Animation slots with per-row enable checkbox
                for (int ai = 0; ai < (int)linkedAsset.anims.size(); ai++)
                {
                    ImGui::PushID(ai + 9000);
                    bool sel = (sp.animEnabled && sp.animIdx == ai);
                    char slotLabel[64];
                    snprintf(slotLabel, sizeof(slotLabel), "%s (frames %d)", linkedAsset.anims[ai].name.c_str(), linkedAsset.anims[ai].endFrame);
                    if (ImGui::Selectable(slotLabel, sel, 0, ImVec2(ImGui::GetContentRegionAvail().x - Scaled(30), 0)))
                    {
                        sp.animIdx = ai;
                        sp.animEnabled = true;
                    }
                    ImGui::SameLine();
                    bool rowEnabled = (sp.animEnabled && sp.animIdx == ai);
                    if (ImGui::Checkbox("##animrow", &rowEnabled))
                    {
                        if (rowEnabled)
                        {
                            sp.animIdx = ai;
                            sp.animEnabled = true;
                        }
                        else
                        {
                            sp.animEnabled = false;
                        }
                    }
                    ImGui::PopID();
                }
            }
        }
        // Mesh asset link (only for Mesh type)
        if (sp.type == SpriteType::Mesh)
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 1.0f, 1.0f), "Mesh");
            const char* meshPreview = (sp.meshIdx >= 0 && sp.meshIdx < (int)sMeshAssets.size())
                ? sMeshAssets[sp.meshIdx].name.c_str() : "(none)";
            if (ImGui::BeginCombo("Mesh##meshlink", meshPreview))
            {
                if (ImGui::Selectable("(none)", sp.meshIdx < 0))
                    sp.meshIdx = -1;
                for (int mi = 0; mi < (int)sMeshAssets.size(); mi++)
                {
                    bool msel = (sp.meshIdx == mi);
                    if (ImGui::Selectable(sMeshAssets[mi].name.c_str(), msel))
                        sp.meshIdx = mi;
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Import OBJ...##mesh"))
            {
#ifdef _WIN32
                std::string objPath = OpenFileDialog("OBJ Files\0*.obj\0All Files\0*.*\0", "obj");
                if (!objPath.empty())
                {
                    MeshAsset ma;
                    if (LoadOBJ(objPath, ma))
                    {
                        sp.meshIdx = (int)sMeshAssets.size();
                        sMeshAssets.push_back(std::move(ma));
                    }
                }
#endif
            }
            if (sp.meshIdx >= 0 && sp.meshIdx < (int)sMeshAssets.size())
            {
                const MeshAsset& ma = sMeshAssets[sp.meshIdx];
                ImGui::Text("Verts: %d  Tris: %d", (int)ma.vertices.size(), (int)ma.indices.size() / 3);
            }
        }

        ImGui::PopItemWidth();

        // Color presets
        ImGui::Text("Color:");
        ImGui::SameLine();
        for (int c = 0; c < kNumSpriteColors; c++)
        {
            ImGui::PushID(c);
            uint32_t cc = kSpriteColors[c];
            ImVec4 cv((cc & 0xFF) / 255.0f, ((cc >> 8) & 0xFF) / 255.0f,
                      ((cc >> 16) & 0xFF) / 255.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, cv);
            if (ImGui::Button("##col", ImVec2(Scaled(18), Scaled(18))))
                sp.color = cc;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();

        if (ImGui::Button("Delete Sprite"))
        {
            for (int j = sSelectedSprite; j < sSpriteCount - 1; j++)
                sSprites[j] = sSprites[j + 1];
            sSpriteCount--;
            sSelectedSprite = -1;
            sSelectedObjType = SelectedObjType::None;
        }
    }
    else
    {
        ImGui::TextWrapped("Select an object or right-click minimap to place a sprite.");
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Right middle: Tilemap (top-down minimap) ----
static float sTilemapZoom = 1.0f;
static float sTilemapPanX = 0.0f;
static float sTilemapPanY = 0.0f;

static void DrawTilemapPanel(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##Tilemap", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Nametable / Tilemap");
    ImGui::Separator();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // Draw a 32x32 minimap grid
    int mapCols = 32, mapRows = 32;
    float baseCell = std::max(3.0f, (size.x - 24.0f) / (float)mapCols);

    // Clamp base to fit vertically
    float maxH = size.y - (cursor.y - pos.y) - 30.0f;
    if (mapRows * baseCell > maxH)
        baseCell = maxH / (float)mapRows;

    // Scroll wheel zoom (scroll up = zoom in)
    if (ImGui::IsWindowHovered())
    {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            float oldZoom = sTilemapZoom;
            sTilemapZoom *= (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
            sTilemapZoom = std::clamp(sTilemapZoom, 0.5f, 5.0f);

            // Zoom toward mouse cursor
            ImVec2 mouse = ImGui::GetMousePos();
            float mx = mouse.x - cursor.x - sTilemapPanX;
            float mz = mouse.y - cursor.y - sTilemapPanY;
            float ratio = 1.0f - sTilemapZoom / oldZoom;
            sTilemapPanX += mx * ratio;
            sTilemapPanY += mz * ratio;
        }

        // Middle mouse drag to pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            sTilemapPanX += delta.x;
            sTilemapPanY += delta.y;
        }
    }

    float cellW = baseCell * sTilemapZoom;
    float cellH = cellW;

    // Apply pan offset and clip drawing to panel
    ImVec2 clipMin = cursor;
    ImVec2 clipMax(pos.x + size.x, pos.y + size.y);
    dl->PushClipRect(clipMin, clipMax, true);
    cursor.x += sTilemapPanX;
    cursor.y += sTilemapPanY;

    for (int row = 0; row < mapRows; row++)
    {
        for (int col = 0; col < mapCols; col++)
        {
            ImVec2 cPos(cursor.x + col * cellW, cursor.y + row * cellH);
            // Checkerboard pattern — matches Mode 7 floor (1 cell = 1 checker square)
            int check = (col + row) % 2;
            uint32_t c = check ? 0xFF50A050 : 0xFF285028;
            dl->AddRectFilled(cPos, ImVec2(cPos.x + cellW, cPos.y + cellH), c);
        }
    }

    // Grid border
    dl->AddRect(cursor,
        ImVec2(cursor.x + mapCols * cellW, cursor.y + mapRows * cellH),
        0x60FFFFFF);

    // Draw sprites on minimap (skip Mesh objects — they're 3D only)
    float mapPixW = mapCols * cellW;
    float mapPixH = mapRows * cellH;
    for (int i = 0; i < sSpriteCount; i++)
    {
        float sx = (sSprites[i].x / kWorldSize + 0.5f) * mapPixW;
        float sz = (sSprites[i].z / kWorldSize + 0.5f) * mapPixH;
        float dotX = cursor.x + sx;
        float dotZ = cursor.y + sz;
        uint32_t col = sSprites[i].color;
        float baseR = 5.0f * sSprites[i].scale;
        if (baseR < 2.0f) baseR = 2.0f;
        float r = (i == sSelectedSprite) ? baseR + 2.0f : baseR;
        dl->AddCircleFilled(ImVec2(dotX, dotZ), r, col);
        if (i == sSelectedSprite)
            dl->AddCircle(ImVec2(dotX, dotZ), r + 1.0f, 0xFFFFFFFF, 0, 2.0f);
    }

    // Camera start object (cyan triangle)
    {
        float cx = cursor.x + (sCamObj.x / kWorldSize + 0.5f) * mapPixW;
        float cz = cursor.y + (sCamObj.z / kWorldSize + 0.5f) * mapPixH;
        float aLen = 8.0f;
        float ca = sCamObj.angle;
        ImVec2 tip(cx - sinf(ca) * aLen, cz - cosf(ca) * aLen);
        ImVec2 bl(cx - sinf(ca + 2.3f) * aLen * 0.6f, cz - cosf(ca + 2.3f) * aLen * 0.6f);
        ImVec2 br(cx - sinf(ca - 2.3f) * aLen * 0.6f, cz - cosf(ca - 2.3f) * aLen * 0.6f);
        uint32_t camCol = (sSelectedObjType == SelectedObjType::Camera) ? 0xFFFFFFFF : 0xFFFFFF00;
        dl->AddTriangleFilled(tip, bl, br, camCol);
        dl->AddTriangle(tip, bl, br, 0xFF000000, 1.5f);
    }

    // Editor camera indicator (red dot, Edit mode only)
    float camMapX = (sCamera.x / kWorldSize + 0.5f) * mapPixW;
    float camMapZ = (sCamera.z / kWorldSize + 0.5f) * mapPixH;
    float indX = cursor.x + camMapX;
    float indZ = cursor.y + camMapZ;
    dl->AddCircleFilled(ImVec2(indX, indZ), 4.0f, 0xFF4444FF);
    float arrowLen = 10.0f;
    dl->AddLine(ImVec2(indX, indZ),
        ImVec2(indX - sinf(sCamera.angle) * arrowLen,
               indZ - cosf(sCamera.angle) * arrowLen),
        0xFF4444FF, 2.0f);

    dl->PopClipRect();

    // Invisible button over the map for interaction
    ImGui::SetCursorScreenPos(cursor);
    ImGui::InvisibleButton("##MinimapInteract", ImVec2(mapPixW, mapPixH));

    if (sEditorMode == EditorMode::Edit)
    {
        // Right-click on minimap to open type picker for placing an object
        static float sPendingPlaceX = 0.0f;
        static float sPendingPlaceZ = 0.0f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && sSpriteCount < kMaxFloorSprites)
        {
            ImVec2 mouse = ImGui::GetMousePos();
            float normX = (mouse.x - cursor.x) / mapPixW;
            float normZ = (mouse.y - cursor.y) / mapPixH;
            sPendingPlaceX = (normX - 0.5f) * kWorldSize;
            sPendingPlaceZ = (normZ - 0.5f) * kWorldSize;
            ImGui::OpenPopup("##PlaceObjectType");
        }
        if (ImGui::BeginPopup("##PlaceObjectType"))
        {
            ImGui::TextDisabled("Place Object");
            ImGui::Separator();
            for (int t = 0; t < (int)SpriteType::Count; t++)
            {
                if (ImGui::MenuItem(kSpriteTypeNames[t]))
                {
                    FloorSprite& sp = sSprites[sSpriteCount];
                    sp.scale = 1.0f;
                    sp.y = 0.0f;
                    sp.type = (SpriteType)t;
                    sp.color = kSpriteColors[sSpriteCount % kNumSpriteColors];
                    sp.selected = false;

                    if ((SpriteType)t == SpriteType::Player)
                    {
                        // Place in front of camera start object
                        float dist = 30.0f;
                        sp.x = sCamObj.x + sinf(sCamObj.angle) * dist;
                        sp.z = sCamObj.z - cosf(sCamObj.angle) * dist;
                    }
                    else
                    {
                        sp.x = sPendingPlaceX;
                        sp.z = sPendingPlaceZ;
                    }

                    sSelectedSprite = sSpriteCount;
                    sSelectedObjType = SelectedObjType::Sprite;
                    sSpriteCount++;
                }
            }
            ImGui::EndPopup();
        }

        // Left-click to select nearest object (camera object first, then sprites)
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_G))
        {
            ImVec2 mouse = ImGui::GetMousePos();

            // Check camera object first
            float camObjX = cursor.x + (sCamObj.x / kWorldSize + 0.5f) * mapPixW;
            float camObjZ = cursor.y + (sCamObj.z / kWorldSize + 0.5f) * mapPixH;
            float camDist = sqrtf((mouse.x - camObjX)*(mouse.x - camObjX) +
                                  (mouse.y - camObjZ)*(mouse.y - camObjZ));

            float bestDist = 100.0f;
            int bestIdx = -1;
            for (int i = 0; i < sSpriteCount; i++)
            {
                float sx = cursor.x + (sSprites[i].x / kWorldSize + 0.5f) * mapPixW;
                float sz = cursor.y + (sSprites[i].z / kWorldSize + 0.5f) * mapPixH;
                float d = sqrtf((mouse.x - sx)*(mouse.x - sx) + (mouse.y - sz)*(mouse.y - sz));
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }

            // Deselect previous sprite
            if (sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
                sSprites[sSelectedSprite].selected = false;

            if (camDist < 12.0f && camDist <= bestDist)
            {
                sSelectedSprite = -1;
                sSelectedObjType = SelectedObjType::Camera;
            }
            else if (bestDist < 10.0f)
            {
                sSelectedSprite = bestIdx;
                sSelectedObjType = SelectedObjType::Sprite;
                sSprites[bestIdx].selected = true;
            }
            else
            {
                sSelectedSprite = -1;
                sSelectedObjType = SelectedObjType::None;
            }
        }

        // G + left-drag to move selected object on minimap
        if (ImGui::IsKeyDown(ImGuiKey_G) && ImGui::IsItemActive()
            && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            ImVec2 mouse = ImGui::GetMousePos();
            float normX = (mouse.x - cursor.x) / mapPixW;
            float normZ = (mouse.y - cursor.y) / mapPixH;
            float wx = std::clamp((normX - 0.5f) * kWorldSize, -kWorldHalf, kWorldHalf);
            float wz = std::clamp((normZ - 0.5f) * kWorldSize, -kWorldHalf, kWorldHalf);

            if (sSelectedObjType == SelectedObjType::Sprite && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                sSprites[sSelectedSprite].x = wx;
                sSprites[sSelectedSprite].z = wz;
            }
            else if (sSelectedObjType == SelectedObjType::Camera)
            {
                sCamObj.x = wx;
                sCamObj.z = wz;
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + mapPixH + 4));
    if (sEditorMode == EditorMode::Play)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "PLAYING  |  Press Stop to edit");
    else if (sSelectedObjType == SelectedObjType::Sprite && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
        ImGui::Text("Sprite %d  Y:%.0f  S:%.1fx  |  G+drag  |  R+drag: size",
            sSelectedSprite, sSprites[sSelectedSprite].y, sSprites[sSelectedSprite].scale);
    else if (sSelectedObjType == SelectedObjType::Camera)
        ImGui::Text("Camera Start  |  G+drag to move");
    else
        ImGui::Text("Sprites: %d  |  RClick: place  |  LClick: select", sSpriteCount);

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Right bottom: Palette ----
static void DrawPalettePanel(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##Palette", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Palette");
    ImGui::Separator();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    float swatchSize = std::max(16.0f, (size.x - 24.0f) / 16.0f);
    // Row 1: 16 colors (4bpp palette)
    for (int i = 0; i < 16; i++)
    {
        ImVec2 sPos(cursor.x + i * swatchSize, cursor.y);
        DrawColorBox(dl, sPos, ImVec2(swatchSize - 1, swatchSize - 1),
            sPalette[i], i == sSelectedPalColor);
    }

    // Click to select
    ImGui::SetCursorScreenPos(cursor);
    ImGui::InvisibleButton("##PalRow", ImVec2(16 * swatchSize, swatchSize));
    if (ImGui::IsItemClicked())
    {
        float mx = ImGui::GetMousePos().x - cursor.x;
        int idx = (int)(mx / swatchSize);
        sSelectedPalColor = std::clamp(idx, 0, 15);
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + swatchSize + 6));
    ImGui::Text("Color: %d  (#%06X)", sSelectedPalColor,
        sPalette[sSelectedPalColor] & 0x00FFFFFF);

    // Camera properties below palette
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Camera");
    ImGui::PushItemWidth(size.x * 0.45f);
    ImGui::DragFloat("X",  &sCamera.x, 1.0f);
    ImGui::SameLine();
    ImGui::DragFloat("Z",  &sCamera.z, 1.0f);
    ImGui::DragFloat("H",  &sCamera.height, 0.5f, 4.0f, 256.0f);
    ImGui::SameLine();
    ImGui::SliderAngle("A", &sCamera.angle, -180.0f, 180.0f);
    ImGui::DragFloat("Pitch", &sCamera.horizon, 0.5f, 10.0f, 120.0f, "%.1f");
    ImGui::PopItemWidth();

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Bottom status bar ----
static void DrawStatusBar(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Scaled(8), Scaled(3)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.16f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Project name for status bar
    std::string projLabel = sProjectPath.empty() ? "Untitled" :
        std::filesystem::path(sProjectPath).stem().string();

    if (sEditorMode == EditorMode::Play)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
            "[PLAY]  %s  |  WASD: Move  |  Q/E: Height  |  I/K: Pitch  |  Esc: Stop",
            projLabel.c_str());
    else
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.55f, 1.0f),
            "[EDIT]  %s  |  Cam: (%.0f, %.0f) H:%.0f  |  Sprites: %d",
            projLabel.c_str(), sCamera.x, sCamera.z, sCamera.height, sSpriteCount);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void FrameTick(float dt)
{
    if (!sInitialized) FrameInit();
    s3DRenderNeeded = false;

    // ---- Global hotkeys ----
    if (sEditorMode != EditorMode::Play && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        if (!sProjectPath.empty())
            SaveProject(sProjectPath);
        else
        {
#ifdef _WIN32
            std::string p = SaveFileDialog(
                "Affinity Project (*.afnproj)\0*.afnproj\0All Files (*.*)\0*.*\0", "afnproj");
            if (!p.empty())
                SaveProject(p);
#endif
        }
    }

    // ---- Main Menu ----
    float menuBarH = 0.0f;
    if (ImGui::BeginMainMenuBar())
    {
        menuBarH = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Project"))
            {
                CloseProject();
            }
            if (ImGui::MenuItem("Open Project..."))
            {
#ifdef _WIN32
                std::string p = OpenFileDialog(
                    "Affinity Project (*.afnproj)\0*.afnproj\0All Files (*.*)\0*.*\0", "afnproj");
                if (!p.empty())
                    LoadProject(p);
#endif
            }
            if (ImGui::MenuItem("Save Project", "Ctrl+S"))
            {
                if (!sProjectPath.empty())
                    SaveProject(sProjectPath);
                else
                {
#ifdef _WIN32
                    std::string p = SaveFileDialog(
                        "Affinity Project (*.afnproj)\0*.afnproj\0All Files (*.*)\0*.*\0", "afnproj");
                    if (!p.empty())
                        SaveProject(p);
#endif
                }
            }
            if (ImGui::MenuItem("Save Project As..."))
            {
#ifdef _WIN32
                std::string p = SaveFileDialog(
                    "Affinity Project (*.afnproj)\0*.afnproj\0All Files (*.*)\0*.*\0", "afnproj");
                if (!p.empty())
                    SaveProject(p);
#endif
            }
            if (ImGui::MenuItem("Close Project"))
            {
                CloseProject();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences"))
                sShowPrefs = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z"))  { /* TODO */ }
            if (ImGui::MenuItem("Redo", "Ctrl+Y"))   { /* TODO */ }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Grid Overlay", nullptr, nullptr);
            ImGui::MenuItem("Camera Bounds", nullptr, nullptr);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Build", nullptr, false, !sPackaging))
        {
            sPackaging = true;
            sPackageDone = false;
            sPackageSuccess = false;
            sPackageMsg = "Building...";

            namespace fs = std::filesystem;
            char exeBuf[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
            fs::path exeDir = fs::path(exeBuf).parent_path();
            fs::path cwdDir = fs::current_path();
            fs::path rtDir;
            for (auto& candidate : {
                exeDir / "gba_runtime",
                exeDir / ".." / "gba_runtime",
                exeDir / ".." / ".." / "gba_runtime",
                exeDir / ".." / ".." / ".." / "gba_runtime",
                cwdDir / "gba_runtime",
                cwdDir / ".." / "gba_runtime",
            })
            {
                if (fs::exists(candidate / "Makefile"))
                { rtDir = fs::canonical(candidate); break; }
            }

            if (rtDir.empty())
            {
                sPackaging = false;
                sPackageDone = true;
                sPackageSuccess = false;
                sPackageMsg = "Cannot find gba_runtime/Makefile\n\nSearched from:\n  exe: " + exeDir.string() + "\n  cwd: " + cwdDir.string();
            }
            else
            {
                sPackageOutputPath = (rtDir / "affinity.gba").string();
                std::string rtDirStr = rtDir.string();
                std::string outPath = sPackageOutputPath;

                std::vector<GBASpriteExport> exportSprites;
                for (int i = 0; i < sSpriteCount; i++)
                {
                    GBASpriteExport se;
                    se.x = sSprites[i].x;
                    se.y = sSprites[i].y;
                    se.z = sSprites[i].z;
                    se.scale = sSprites[i].scale;
                    se.rotation = sSprites[i].rotation;
                    se.assetIdx = sSprites[i].assetIdx;
                    se.animIdx = sSprites[i].animIdx;
                    se.animEnabled = sSprites[i].animEnabled;
                    se.spriteType = (int)sSprites[i].type;
                    se.meshIdx = sSprites[i].meshIdx;
                    // Use asset palette bank if linked, otherwise cycle 1-5
                    if (se.assetIdx >= 0 && se.assetIdx < (int)sSpriteAssets.size())
                        se.palIdx = sSpriteAssets[se.assetIdx].palBank;
                    else
                        se.palIdx = (i % 5) + 1;
                    exportSprites.push_back(se);
                }
                GBACameraExport exportCam;
                exportCam.x = sCamObj.x;
                exportCam.z = sCamObj.z;
                exportCam.height = sCamObj.height;
                exportCam.angle = sCamObj.angle;
                exportCam.horizon = sCamObj.horizon;
                exportCam.walkSpeed = sCamObj.walkSpeed;
                exportCam.sprintSpeed = sCamObj.sprintSpeed;
                exportCam.walkEaseIn = sCamObj.walkEaseIn;
                exportCam.walkEaseOut = sCamObj.walkEaseOut;
                exportCam.sprintEaseIn = sCamObj.sprintEaseIn;
                exportCam.sprintEaseOut = sCamObj.sprintEaseOut;

                // Collect sprite assets for export
                std::vector<GBASpriteAssetExport> exportAssets;
                for (const auto& sa : sSpriteAssets)
                {
                    GBASpriteAssetExport ea;
                    ea.name = sa.name;
                    ea.baseSize = sa.baseSize;
                    ea.palBank = sa.palBank;
                    ea.paletteSrc = sa.paletteSrc;
                    ea.defaultAnim = sa.defaultAnim;
                    memcpy(ea.palette, sa.palette, sizeof(ea.palette));
                    for (const auto& fr : sa.frames)
                    {
                        GBASpriteFrameExport ef;
                        memcpy(ef.pixels, fr.pixels, sizeof(ef.pixels));
                        ef.width = fr.width;
                        ef.height = fr.height;
                        ea.frames.push_back(ef);
                    }
                    for (const auto& an : sa.anims)
                    {
                        GBASpriteAnimExport ean;
                        ean.startFrame = an.startFrame;
                        ean.endFrame = an.endFrame;
                        ean.fps = an.fps;
                        ean.loop = an.loop;
                        ean.speed = an.speed;
                        ean.gameState = (int)an.gameState;
                        ea.anims.push_back(ean);
                    }
                    // Asset directional sprite animation sets
                    int saIdx = (int)exportAssets.size(); // index about to be pushed
                    if (saIdx < (int)sAssetDirSprites.size() && sa.hasDirections)
                    {
                        ea.hasDirections = true;
                        for (int si = 0; si < (int)sAssetDirSprites[saIdx].size(); si++)
                        {
                            GBASpriteAssetExport::DirAnimSetExport dase;
                            if (si < (int)sa.dirAnimSets.size())
                                dase.name = sa.dirAnimSets[si].name;
                            for (int d = 0; d < 8; d++)
                            {
                                dase.dirImages[d].pixels = sAssetDirSprites[saIdx][si][d].pixels;
                                dase.dirImages[d].width  = sAssetDirSprites[saIdx][si][d].width;
                                dase.dirImages[d].height = sAssetDirSprites[saIdx][si][d].height;
                            }
                            ea.dirAnimSets.push_back(dase);
                        }
                    }
                    exportAssets.push_back(ea);
                }

                // Collect mesh assets for export
                std::vector<GBAMeshExport> exportMeshes;
                for (const auto& ma : sMeshAssets)
                {
                    GBAMeshExport me;
                    for (const auto& v : ma.vertices)
                    {
                        me.positions.push_back(v.px);
                        me.positions.push_back(v.py);
                        me.positions.push_back(v.pz);
                        me.normals.push_back(v.nx);
                        me.normals.push_back(v.ny);
                        me.normals.push_back(v.nz);
                    }
                    me.indices = ma.indices;
                    // Convert sprite color to RGB15 (use magenta as default)
                    me.colorRGB15 = 0x7C1F; // magenta
                    exportMeshes.push_back(me);
                }

                float exportOrbitDist = sOrbitDist;

                std::thread([rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                             exportMeshes, exportOrbitDist]() {
                    std::string err;
                    bool ok = PackageGBA(rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                                         exportMeshes, exportOrbitDist, err);
                    sPackageSuccess = ok;
                    sPackageMsg = ok
                        ? ("ROM saved: " + outPath + "\n\n" + err)
                        : err;
                    sPackageDone = true;
                    sPackaging = false;
                }).detach();
            }
        }
        ImGui::EndMainMenuBar();
    }

    // ---- Camera Controls (only when no ImGui widget has focus) ----
    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        // Escape exits Play mode
        if (sEditorMode == EditorMode::Play && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            sEditorMode = EditorMode::Edit;
            sCamera = sSavedEditorCam;
            sOrbitAngle = 0.0f;
            if (sSavedPlayerIdx >= 0 && sSavedPlayerIdx < sSpriteCount)
                sSprites[sSavedPlayerIdx] = sSavedPlayerSprite;
            sSavedPlayerIdx = -1;
        }

        if (sEditorMode == EditorMode::Edit)
        {
            // ---- EDIT MODE: free camera ----
            float moveSpeed = 80.0f * dt;
            float rotSpeed  = 2.0f * dt;

            if (ImGui::IsKeyDown(ImGuiKey_A))
                sCamera.angle += rotSpeed;
            if (ImGui::IsKeyDown(ImGuiKey_D))
                sCamera.angle -= rotSpeed;

            if (ImGui::IsKeyDown(ImGuiKey_W))
            {
                sCamera.x -= sinf(sCamera.angle) * moveSpeed;
                sCamera.z -= cosf(sCamera.angle) * moveSpeed;
            }
            if (ImGui::IsKeyDown(ImGuiKey_S))
            {
                sCamera.x += sinf(sCamera.angle) * moveSpeed;
                sCamera.z += cosf(sCamera.angle) * moveSpeed;
            }

            if (ImGui::IsKeyDown(ImGuiKey_I))
                sCamera.horizon = std::min(120.0f, sCamera.horizon + 60.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_K))
                sCamera.horizon = std::max(10.0f, sCamera.horizon - 60.0f * dt);

            if (ImGui::IsKeyDown(ImGuiKey_Q))
                sCamera.height = std::max(4.0f, sCamera.height - 40.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_E))
                sCamera.height = std::min(256.0f, sCamera.height + 40.0f * dt);

            // G + scroll wheel to adjust selected sprite height
            if (ImGui::IsKeyDown(ImGuiKey_G) && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f)
                    sSprites[sSelectedSprite].y = std::max(0.0f, sSprites[sSelectedSprite].y + wheel * 5.0f);
            }

            // R + mouse drag up/down to resize selected sprite
            if (ImGui::IsKeyDown(ImGuiKey_R) && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    float dragY = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y;
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    sSprites[sSelectedSprite].scale = std::clamp(
                        sSprites[sSelectedSprite].scale - dragY * 0.1f, 0.1f, 50.0f);
                }
            }

            // Delete selected sprite
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                for (int i = sSelectedSprite; i < sSpriteCount - 1; i++)
                    sSprites[i] = sSprites[i + 1];
                sSpriteCount--;
                sSelectedSprite = -1;
                sSelectedObjType = SelectedObjType::None;
            }
        }
        else
        {
            // ---- PLAY MODE: third-person orbit camera ----
            float moveSpeed = (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? sCamObj.sprintSpeed : sCamObj.walkSpeed) * dt;
            float rotSpeed  = 3.0f * dt;

            // Find player sprite
            int playerIdx = -1;
            for (int i = 0; i < sSpriteCount; i++)
            {
                if (sSprites[i].type == SpriteType::Player)
                { playerIdx = i; break; }
            }

            if (playerIdx >= 0)
            {
                FloorSprite& player = sSprites[playerIdx];

                // The camera look direction = from camera toward player
                // Camera view angle = sOrbitAngle + PI (opposite of orbit offset)
                float viewAngle = sOrbitAngle + 3.14159265f;

                // WASD input
                float inputX = 0.0f, inputZ = 0.0f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) { inputX += 1.0f; }
                if (ImGui::IsKeyDown(ImGuiKey_S)) { inputX -= 1.0f; }
                if (ImGui::IsKeyDown(ImGuiKey_A)) { inputZ -= 1.0f; }
                if (ImGui::IsKeyDown(ImGuiKey_D)) { inputZ += 1.0f; }

                bool wasMoving = sPlayerMoving;
                sPlayerMoving = (inputX != 0.0f || inputZ != 0.0f);
                sPlayerSprinting = sPlayerMoving && ImGui::IsKeyDown(ImGuiKey_LeftShift);

                // J/L manual orbit — smooth ease-in/out on orbit angle
                {
                    float manualTarget = 0.0f;
                    if (ImGui::IsKeyDown(ImGuiKey_J)) manualTarget += rotSpeed;
                    if (ImGui::IsKeyDown(ImGuiKey_L)) manualTarget -= rotSpeed;
                    if (fabsf(manualTarget) > 0.001f)
                        sManualOrbitCurrent += (manualTarget - sManualOrbitCurrent) * std::min(1.0f, 6.0f * dt);
                    else
                        sManualOrbitCurrent *= 0.85f;
                    if (fabsf(sManualOrbitCurrent) < fabsf(rotSpeed * 0.02f))
                        sManualOrbitCurrent = 0.0f;
                    sOrbitAngle += sManualOrbitCurrent;
                }

                // Auto-orbit when strafing (A/D) with drag on release
                {
                    float autoOrbitTarget = 0.0f;
                    if (inputZ != 0.0f)
                    {
                        autoOrbitTarget = rotSpeed * 0.4f * inputZ;
                        if (ImGui::IsKeyDown(ImGuiKey_J) || ImGui::IsKeyDown(ImGuiKey_L))
                            autoOrbitTarget *= 2.0f;
                    }
                    if (fabsf(autoOrbitTarget) > 0.001f)
                        sAutoOrbitCurrent += (autoOrbitTarget - sAutoOrbitCurrent) * std::min(1.0f, 6.0f * dt);
                    else
                        sAutoOrbitCurrent *= 0.85f;
                    if (fabsf(sAutoOrbitCurrent) < fabsf(rotSpeed * 0.02f))
                        sAutoOrbitCurrent = 0.0f;
                    sOrbitAngle -= sAutoOrbitCurrent;
                }

                if (sPlayerMoving)
                {
                    // Normalize diagonal
                    float len = sqrtf(inputX * inputX + inputZ * inputZ);
                    inputX /= len;
                    inputZ /= len;

                    // Track movement direction relative to camera (for sprite facing)
                    sPlayerMoveAngle = atan2f(inputZ, inputX);

                    // Transform to world space using viewAngle
                    float fwdX = sinf(viewAngle), fwdZ = cosf(viewAngle);
                    float rightX = -cosf(viewAngle), rightZ = sinf(viewAngle);
                    player.x += (fwdX * inputX + rightX * inputZ) * moveSpeed;
                    player.z += (fwdZ * inputX + rightZ * inputZ) * moveSpeed;
                }
                else if (wasMoving)
                {
                    sPlayerMoveAngle = sPlayerMoveAngle - sOrbitAngle;
                }

                // Clamp player to world
                player.x = std::clamp(player.x, -kWorldHalf, kWorldHalf);
                player.z = std::clamp(player.z, -kWorldHalf, kWorldHalf);

                // Place camera at orbit offset from player with smooth follow
                {
                    float targetX = player.x + sinf(sOrbitAngle) * sOrbitDist;
                    float targetZ = player.z + cosf(sOrbitAngle) * sOrbitDist;
                    // Camera follow using exposed ease values
                    bool orbiting = fabsf(sManualOrbitCurrent) > 0.0f;
                    float followPct;
                    if (orbiting)
                        followPct = 50.0f; // fast follow during orbit to prevent drift
                    else if (sPlayerSprinting)
                        followPct = sPlayerMoving ? sCamObj.sprintEaseIn : sCamObj.sprintEaseOut;
                    else
                        followPct = sPlayerMoving ? sCamObj.walkEaseIn : sCamObj.walkEaseOut;
                    float followRate = std::min(1.0f, (followPct / 100.0f) * 12.0f * dt);
                    float dx = targetX - sCamera.x;
                    float dz = targetZ - sCamera.z;
                    if (fabsf(dx) < 0.1f && fabsf(dz) < 0.1f)
                    { sCamera.x = targetX; sCamera.z = targetZ; }
                    else
                    { sCamera.x += dx * followRate; sCamera.z += dz * followRate; }
                    sCamera.angle = sOrbitAngle;
                }
            }
            else
            {
                // No player sprite — fallback to free camera
                if (ImGui::IsKeyDown(ImGuiKey_W))
                {
                    sCamera.x -= sinf(sCamera.angle) * moveSpeed;
                    sCamera.z -= cosf(sCamera.angle) * moveSpeed;
                }
                if (ImGui::IsKeyDown(ImGuiKey_S))
                {
                    sCamera.x += sinf(sCamera.angle) * moveSpeed;
                    sCamera.z += cosf(sCamera.angle) * moveSpeed;
                }
                if (ImGui::IsKeyDown(ImGuiKey_J))
                    sCamera.angle += rotSpeed;
                if (ImGui::IsKeyDown(ImGuiKey_L))
                    sCamera.angle -= rotSpeed;
            }

            // Q/E height, I/K horizon still work
            if (ImGui::IsKeyDown(ImGuiKey_Q))
                sCamera.height = std::max(4.0f, sCamera.height - 20.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_E))
                sCamera.height = std::min(200.0f, sCamera.height + 20.0f * dt);

            if (ImGui::IsKeyDown(ImGuiKey_I))
                sCamera.horizon = std::min(120.0f, sCamera.horizon + 40.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_K))
                sCamera.horizon = std::max(10.0f, sCamera.horizon - 40.0f * dt);
        }

        // Clamp camera to world bounds
        sCamera.x = std::clamp(sCamera.x, -kWorldHalf, kWorldHalf);
        sCamera.z = std::clamp(sCamera.z, -kWorldHalf, kWorldHalf);
    }

    // Player position is freely editable — only set on creation, not per-frame

    // ---- Render Mode 7 ----
    // Only show camera object in Edit mode (in Play mode you ARE the camera)
    const CameraStartObject* camObjPtr = (sEditorMode == EditorMode::Edit) ? &sCamObj : nullptr;
    sViewportAnimTime += dt;
    const SpriteAsset* assetsPtr = sSpriteAssets.empty() ? nullptr : sSpriteAssets.data();
    bool isPlaying = (sEditorMode == EditorMode::Play);
    // Sprite direction: orbit-based when idle, movement-based when moving
    float spriteAngle = sPlayerMoving ? sPlayerMoveAngle : sOrbitAngle + sPlayerMoveAngle;
    // Build per-asset direction image arrays (idle fallback)
    std::vector<Mode7::AssetDirImages> assetDirImgs(sSpriteAssets.size());
    for (int ai = 0; ai < (int)sSpriteAssets.size() && ai < (int)sAssetDirSprites.size(); ai++)
    {
        if (sAssetDirSprites[ai].empty()) continue;
        // Default: idle anim frame 0
        for (int d = 0; d < 8; d++)
        {
            assetDirImgs[ai].dirs[d].pixels = sAssetDirSprites[ai][0][d].pixels;
            assetDirImgs[ai].dirs[d].width  = sAssetDirSprites[ai][0][d].width;
            assetDirImgs[ai].dirs[d].height = sAssetDirSprites[ai][0][d].height;
        }
    }

    // Build per-sprite direction images — each sprite gets its own animation state
    std::vector<Mode7::AssetDirImages> spriteDirImgs(sSpriteCount);
    for (int si = 0; si < sSpriteCount; si++)
    {
        const FloorSprite& sp = sSprites[si];
        int ai = sp.assetIdx;
        if (ai < 0 || ai >= (int)sSpriteAssets.size() || ai >= (int)sAssetDirSprites.size())
            continue;
        if (sAssetDirSprites[ai].empty()) continue;

        const SpriteAsset& asset = sSpriteAssets[ai];
        int dirSetIdx = 0; // default: idle anim frame 0

        if (isPlaying && sp.animEnabled && asset.anims.size() >= 2)
        {
            // Pick animation by game state assignment
            // Determine desired state: Idle, Walk/Run, or Sprint
            AnimState desiredState = AnimState::Idle;
            if (sp.type == SpriteType::Player && sPlayerMoving)
                desiredState = sPlayerSprinting ? AnimState::Sprint : AnimState::Walk;

            // Find the best matching anim slot
            int animIdx = 0; // fallback to slot 0
            for (int a = 0; a < (int)asset.anims.size(); a++)
            {
                AnimState as = asset.anims[a].gameState;
                if (as == desiredState) { animIdx = a; break; }
                // Walk and Run are interchangeable
                if (desiredState == AnimState::Walk && as == AnimState::Run) { animIdx = a; break; }
                if (desiredState == AnimState::Run && as == AnimState::Walk) { animIdx = a; break; }
            }
            const SpriteAnim& anim = asset.anims[animIdx];
            int base = GetAnimDirBase(asset, animIdx);
            int frameCount = anim.endFrame;
            if (frameCount > 0)
            {
                int fps = anim.fps > 0 ? anim.fps : 8;
                float effectiveFps = fps * anim.speed;
                int frame = effectiveFps > 0.0f ? ((int)(sViewportAnimTime * effectiveFps)) % frameCount : 0;
                dirSetIdx = base + frame;
            }
        }

        if (dirSetIdx >= (int)sAssetDirSprites[ai].size())
            dirSetIdx = 0;
        for (int d = 0; d < 8; d++)
        {
            spriteDirImgs[si].dirs[d].pixels = sAssetDirSprites[ai][dirSetIdx][d].pixels;
            spriteDirImgs[si].dirs[d].width  = sAssetDirSprites[ai][dirSetIdx][d].width;
            spriteDirImgs[si].dirs[d].height = sAssetDirSprites[ai][dirSetIdx][d].height;
        }
    }
    Mode7::Render(sCamera, nullptr, sSprites, sSpriteCount, camObjPtr, sCamObjEditorScale,
                  assetsPtr, (int)sSpriteAssets.size(), sViewportAnimTime, isPlaying,
                  nullptr, spriteAngle,
                  assetDirImgs.empty() ? nullptr : assetDirImgs.data(), (int)assetDirImgs.size(),
                  spriteDirImgs.empty() ? nullptr : spriteDirImgs.data(), (int)spriteDirImgs.size(),
                  sMeshAssets.empty() ? nullptr : sMeshAssets.data(), (int)sMeshAssets.size());
    Mode7::UploadTexture();

    // ---- Layout: NEXXT-style fixed panels ----
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float totalW = vp->WorkSize.x;
    float totalH = vp->WorkSize.y;
    float topY   = vp->WorkPos.y + menuBarH;

    float tabH     = ImGui::GetFrameHeight() + Scaled(6);
    float statusH  = ImGui::GetFrameHeight() + Scaled(2);
    float bodyY    = topY + tabH;
    float bodyH    = totalH - menuBarH - tabH - statusH;

    // Right panel width: ~35% of window, min 280 scaled
    float rightW = std::max(Scaled(280), totalW * 0.35f);
    float leftW  = totalW - rightW;

    // Right panel split: tileset 35%, tilemap 40%, palette+props 25%
    float tilesetH  = bodyH * 0.35f;
    float tilemapH  = bodyH * 0.40f;
    float paletteH  = bodyH - tilesetH - tilemapH;

    // Draw everything
    DrawTabBar();

    if (sActiveTab == EditorTab::Skybox)
    {
        // Skybox tab: placeholder
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, bodyY));
        ImGui::SetNextWindowSize(ImVec2(totalW, bodyH));
        ImGui::Begin("##SkyboxTab", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Text("Skybox Editor — coming soon");
        ImGui::End();
    }
    else if (sActiveTab == EditorTab::Player)
    {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, bodyY));
        ImGui::SetNextWindowSize(ImVec2(totalW, bodyH));
        ImGui::Begin("##PlayerTab", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Player Settings");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("Player directional sprites are now managed through the per-asset direction system on the Sprites tab. "
                           "Assign a sprite asset with directions to the Player sprite for directional rendering.");
        ImGui::End();
    }
    else if (sActiveTab == EditorTab::ThreeD)
    {
        Draw3DView(ImVec2(vp->WorkPos.x, bodyY), ImVec2(totalW, bodyH));
    }
    else if (sActiveTab == EditorTab::Sprites)
    {
        // Sprites tab: full-width sprite asset editor
        DrawSpritesTab(
            ImVec2(vp->WorkPos.x, bodyY),
            ImVec2(totalW, bodyH), dt);
    }
    else
    {
        // Map / Tiles tabs: viewport + right panels
        DrawViewport(
            ImVec2(vp->WorkPos.x, bodyY),
            ImVec2(leftW, bodyH));

        if (sEditorMode == EditorMode::Edit)
            DrawObjectEditorPanel(
                ImVec2(vp->WorkPos.x + leftW, bodyY),
                ImVec2(rightW, tilesetH));
        else
            DrawTilesetPanel(
                ImVec2(vp->WorkPos.x + leftW, bodyY),
                ImVec2(rightW, tilesetH));

        DrawTilemapPanel(
            ImVec2(vp->WorkPos.x + leftW, bodyY + tilesetH),
            ImVec2(rightW, tilemapH));

        DrawPalettePanel(
            ImVec2(vp->WorkPos.x + leftW, bodyY + tilesetH + tilemapH),
            ImVec2(rightW, paletteH));
    }

    DrawStatusBar(
        ImVec2(vp->WorkPos.x, bodyY + bodyH),
        ImVec2(totalW, statusH));

    // ---- Preferences window ----
    if (sShowPrefs)
    {
        ImGui::SetNextWindowSize(ImVec2(Scaled(400), 0), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::Begin("Preferences", &sShowPrefs, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "General");
        ImGui::Separator();

        // UI Scale
        float prevScale = sUiScale;
        ImGui::SliderFloat("UI Scale", &sUiScale, 0.75f, 2.0f, "%.2f");
        if (sUiScale != prevScale)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.FontGlobalScale = sUiScale;
        }
        if (ImGui::Button("Reset to 1x"))
        {
            sUiScale = 1.0f;
            ImGui::GetIO().FontGlobalScale = 1.0f;
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "mGBA");
        ImGui::Separator();

        // mGBA path
        static float sMgbaToastTimer = 0.0f;

        ImGui::InputText("mGBA Path", sMgbaPath, sizeof(sMgbaPath));
        ImGui::SameLine();
        if (ImGui::Button("Browse"))
        {
            OPENFILENAMEA ofn = {};
            char path[512] = "";
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "Executables\0*.exe\0All Files\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = sizeof(path);
            ofn.lpstrTitle = "Select mGBA executable";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
            {
                strncpy(sMgbaPath, path, sizeof(sMgbaPath) - 1);
            }
        }

        if (ImGui::Button("Test Connection"))
        {
            namespace fs = std::filesystem;
            fs::path p(sMgbaPath);
            sMgbaFound = fs::exists(p) &&
                (p.filename().string().find("mGBA") != std::string::npos ||
                 p.filename().string().find("mgba") != std::string::npos ||
                 p.extension() == ".exe");
            if (sMgbaFound)
                sMgbaToastTimer = 3.0f;
        }

        if (sMgbaFound)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "OK!");
        }

        ImGui::Spacing();
        ImGui::Separator();
        static float sPrefsSaveTimer = 0.0f;
        if (ImGui::Button("Save Preferences"))
        {
            FILE* f = fopen("affinity_prefs.ini", "w");
            if (f)
            {
                fprintf(f, "ui_scale=%.2f\n", sUiScale);
                fprintf(f, "mgba_path=%s\n", sMgbaPath);
                fclose(f);
                sPrefsSaveTimer = 2.0f;
            }
        }
        if (sPrefsSaveTimer > 0.0f)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Saved!");
            sPrefsSaveTimer -= dt;
        }

        // Toast notification
        if (sMgbaToastTimer > 0.0f)
        {
            sMgbaToastTimer -= dt;
            ImVec2 toastPos(vp->WorkPos.x + totalW - 260, bodyY + 10);
            ImGui::SetNextWindowPos(toastPos);
            ImGui::SetNextWindowSize(ImVec2(250, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.35f, 0.15f, 0.95f));
            ImGui::Begin("##Toast", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "mGBA connected successfully");
            ImGui::End();
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    // ---- Package status popup ----
    if (sPackaging || sPackageDone)
    {
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::Begin("GBA Package", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_AlwaysAutoResize);

        if (sPackaging)
        {
            ImGui::Text("Building GBA ROM...");
            // Simple spinner
            const char* spinner = "|/-\\";
            static int frame = 0;
            ImGui::SameLine();
            ImGui::Text("%c", spinner[(frame++ / 8) % 4]);
        }
        else
        {
            if (sPackageSuccess)
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Success!");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed!");

            ImGui::Separator();
            float logH = std::min(300.0f, std::max(80.0f, ImGui::CalcTextSize(sPackageMsg.c_str()).y + 20.0f));
            ImGui::InputTextMultiline("##buildlog", (char*)sPackageMsg.c_str(), sPackageMsg.size() + 1,
                ImVec2(-1, logH), ImGuiInputTextFlags_ReadOnly);
            ImGui::Separator();

            float btnH = ImGui::GetFrameHeight() * 1.3f;
            float okW  = std::max(80.0f * sUiScale, ImGui::CalcTextSize("  OK  ").x + 20.0f);
            if (ImGui::Button("OK", ImVec2(okW, btnH)))
                sPackageDone = false;

            if (sPackageSuccess)
            {
                ImGui::SameLine();
                float mgbaW = std::max(140.0f * sUiScale, ImGui::CalcTextSize("  Open in mGBA  ").x + 20.0f);
                if (ImGui::Button("Open in mGBA", ImVec2(mgbaW, btnH)))
                {
                    if (sMgbaPath[0])
                    {
                        std::string cmd = "\"" + std::string(sMgbaPath) + "\" \"" + sPackageOutputPath + "\"";
                        STARTUPINFOA si = {}; si.cb = sizeof(si);
                        PROCESS_INFORMATION pi = {};
                        CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
                        if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
                    }
                    else
                    {
                        // Fallback: open with default .gba association
                        ShellExecuteA(nullptr, "open", sPackageOutputPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    sPackageDone = false;
                }
            }
        }

        ImGui::End();
    }
}

const Mode7Camera& GetCamera()
{
    return sCamera;
}

void Render3DViewport()
{
    if (!s3DRenderNeeded) return;
    s3DRenderNeeded = false;

    ImVec2 pos = s3DViewPos;
    ImVec2 size = s3DViewSize;

    float camX = s3DTargetX + s3DOrbitDist * cosf(s3DOrbitPitch) * sinf(s3DOrbitYaw);
    float camY = s3DTargetY + s3DOrbitDist * sinf(s3DOrbitPitch);
    float camZ = s3DTargetZ + s3DOrbitDist * cosf(s3DOrbitPitch) * cosf(s3DOrbitYaw);

    // GL viewport coords (Y is bottom-up in GL)
    float dispH = ImGui::GetIO().DisplaySize.y;
    int vpX = (int)pos.x;
    int vpY = (int)(dispH - pos.y - size.y);
    int vpW = (int)size.x;
    int vpH = (int)size.y;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    glViewport(vpX, vpY, vpW, vpH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vpX, vpY, vpW, vpH);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Perspective projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)vpW / (float)vpH;
    float fovY = 45.0f;
    float nearP = 1.0f, farP = 3000.0f;
    float top = nearP * tanf(fovY * 3.14159265f / 360.0f);
    float right = top * aspect;
    glFrustum(-right, right, -top, top, nearP, farP);

    // Look-at
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    {
        float fx = s3DTargetX - camX;
        float fy = s3DTargetY - camY;
        float fz = s3DTargetZ - camZ;
        float fLen = sqrtf(fx*fx + fy*fy + fz*fz);
        if (fLen > 0.0f) { fx /= fLen; fy /= fLen; fz /= fLen; }
        float sx = -fz, sy = 0.0f, sz = fx;
        float sLen = sqrtf(sx*sx + sz*sz);
        if (sLen > 0.0f) { sx /= sLen; sz /= sLen; }
        float ux = sy*fz - sz*fy;
        float uy = sz*fx - sx*fz;
        float uz = sx*fy - sy*fx;
        float m[16] = {
            sx,  ux, -fx, 0,
            sy,  uy, -fy, 0,
            sz,  uz, -fz, 0,
            0,   0,   0,  1
        };
        glLoadMatrixf(m);
        glTranslatef(-camX, -camY, -camZ);
    }

    // ---- Ground grid ----
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_LINES);
    for (int i = -8; i <= 8; i++)
    {
        float v = i * 64.0f;
        if (i == 0) glColor3f(0.35f, 0.35f, 0.40f);
        else        glColor3f(0.18f, 0.18f, 0.22f);
        glVertex3f(v, 0, -512.0f); glVertex3f(v, 0, 512.0f);
        glVertex3f(-512.0f, 0, v); glVertex3f(512.0f, 0, v);
    }
    glEnd();

    // World bounds
    glColor3f(0.4f, 0.25f, 0.25f);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-512, 0.1f, -512); glVertex3f(512, 0.1f, -512);
    glVertex3f(512, 0.1f, 512);   glVertex3f(-512, 0.1f, 512);
    glEnd();

    // Floor quad
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.15f, 0.25f, 0.15f, 0.3f);
    glBegin(GL_QUADS);
    glVertex3f(-512, -0.1f, -512); glVertex3f(512, -0.1f, -512);
    glVertex3f(512, -0.1f, 512);   glVertex3f(-512, -0.1f, 512);
    glEnd();
    glDisable(GL_BLEND);

    // ---- Sprites ----
    for (int i = 0; i < sSpriteCount; i++)
    {
        const FloorSprite& fs = sSprites[i];
        float sx = fs.x, sy = fs.y, sz = fs.z;

        // Mesh objects
        if (fs.type == SpriteType::Mesh && fs.meshIdx >= 0 && fs.meshIdx < (int)sMeshAssets.size())
        {
            const MeshAsset& ma = sMeshAssets[fs.meshIdx];
            glPushMatrix();
            glTranslatef(sx, sy, sz);
            glRotatef(fs.rotation, 0, 1, 0);
            glScalef(fs.scale, fs.scale, fs.scale);

            glEnable(GL_LIGHTING);
            glEnable(GL_LIGHT0);
            float lightDir[] = { 0.3f, 1.0f, 0.5f, 0.0f };
            float lightAmb[] = { 0.3f, 0.3f, 0.35f, 1.0f };
            float lightDif[] = { 0.7f, 0.7f, 0.65f, 1.0f };
            glLightfv(GL_LIGHT0, GL_POSITION, lightDir);
            glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmb);
            glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDif);

            uint8_t cr = (fs.color >> 0) & 0xFF;
            uint8_t cg = (fs.color >> 8) & 0xFF;
            uint8_t cb = (fs.color >> 16) & 0xFF;
            float matDif[] = { cr/255.0f, cg/255.0f, cb/255.0f, 1.0f };
            glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, matDif);
            glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, matDif);

            glBegin(GL_TRIANGLES);
            for (size_t ti = 0; ti < ma.indices.size(); ti++)
            {
                const MeshVertex& v = ma.vertices[ma.indices[ti]];
                glNormal3f(v.nx, v.ny, v.nz);
                glVertex3f(v.px, v.py, v.pz);
            }
            glEnd();

            glDisable(GL_LIGHTING);
            glDisable(GL_LIGHT0);

            if (fs.selected)
            {
                glColor3f(1, 1, 1);
                float x0 = ma.boundsMin[0], y0 = ma.boundsMin[1], z0 = ma.boundsMin[2];
                float x1 = ma.boundsMax[0], y1 = ma.boundsMax[1], z1 = ma.boundsMax[2];
                glBegin(GL_LINE_LOOP);
                glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1); glVertex3f(x0,y0,z1);
                glEnd();
                glBegin(GL_LINE_LOOP);
                glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);
                glEnd();
                glBegin(GL_LINES);
                glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0);
                glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0);
                glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1);
                glVertex3f(x0,y0,z1); glVertex3f(x0,y1,z1);
                glEnd();
            }
            glPopMatrix();
            continue;
        }

        // Billboard sprites
        float h = 16.0f * fs.scale;
        uint8_t cr = (fs.color >> 0) & 0xFF;
        uint8_t cg = (fs.color >> 8) & 0xFF;
        uint8_t cb = (fs.color >> 16) & 0xFF;
        float r = cr / 255.0f, g = cg / 255.0f, b = cb / 255.0f;

        float dx = camX - sx, dz = camZ - sz;
        float dist = sqrtf(dx*dx + dz*dz);
        float bx = 0, bz = 1;
        if (dist > 0.01f) { bx = -dz / dist; bz = dx / dist; }
        float hw = h * 0.5f;

        if (fs.selected)
        {
            glColor3f(1, 1, 1);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex3f(sx-bx*hw*1.2f, sy, sz-bz*hw*1.2f);
            glVertex3f(sx+bx*hw*1.2f, sy, sz+bz*hw*1.2f);
            glVertex3f(sx+bx*hw*1.2f, sy+h*1.1f, sz+bz*hw*1.2f);
            glVertex3f(sx-bx*hw*1.2f, sy+h*1.1f, sz-bz*hw*1.2f);
            glEnd();
            glLineWidth(1.0f);
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(r, g, b, 0.85f);
        glBegin(GL_QUADS);
        glVertex3f(sx-bx*hw, sy, sz-bz*hw);
        glVertex3f(sx+bx*hw, sy, sz+bz*hw);
        glVertex3f(sx+bx*hw, sy+h, sz+bz*hw);
        glVertex3f(sx-bx*hw, sy+h, sz-bz*hw);
        glEnd();

        glColor4f(r*0.5f, g*0.5f, b*0.5f, 1.0f);
        glBegin(GL_LINE_LOOP);
        glVertex3f(sx-bx*hw, sy, sz-bz*hw);
        glVertex3f(sx+bx*hw, sy, sz+bz*hw);
        glVertex3f(sx+bx*hw, sy+h, sz+bz*hw);
        glVertex3f(sx-bx*hw, sy+h, sz-bz*hw);
        glEnd();
        glDisable(GL_BLEND);

        // Ground shadow
        glEnable(GL_BLEND);
        glColor4f(0, 0, 0, 0.3f);
        glBegin(GL_TRIANGLE_FAN);
        glVertex3f(sx, 0.05f, sz);
        for (int a = 0; a <= 16; a++)
        {
            float ang = a * 6.28318530f / 16.0f;
            glVertex3f(sx + cosf(ang)*hw*0.8f, 0.05f, sz + sinf(ang)*hw*0.8f);
        }
        glEnd();
        glDisable(GL_BLEND);

        // Type dot
        if (fs.type == SpriteType::Player) glColor3f(0.2f, 0.8f, 1.0f);
        else if (fs.type == SpriteType::Enemy) glColor3f(1.0f, 0.2f, 0.2f);
        else if (fs.type == SpriteType::NPC) glColor3f(0.2f, 1.0f, 0.4f);
        else if (fs.type == SpriteType::Trigger) glColor3f(1.0f, 1.0f, 0.2f);
        else glColor3f(0.6f, 0.6f, 0.6f);
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        glVertex3f(sx, sy + h + 2.0f, sz);
        glEnd();
    }

    // ---- Camera start object ----
    {
        float cx = sCamObj.x, cz = sCamObj.z;
        float ch = 8.0f;
        glColor3f(0.4f, 0.7f, 1.0f);
        glBegin(GL_QUADS);
        glVertex3f(cx-3,1,cz-3); glVertex3f(cx+3,1,cz-3); glVertex3f(cx+3,ch,cz-3); glVertex3f(cx-3,ch,cz-3);
        glVertex3f(cx-3,1,cz+3); glVertex3f(cx+3,1,cz+3); glVertex3f(cx+3,ch,cz+3); glVertex3f(cx-3,ch,cz+3);
        glVertex3f(cx-3,1,cz-3); glVertex3f(cx-3,1,cz+3); glVertex3f(cx-3,ch,cz+3); glVertex3f(cx-3,ch,cz-3);
        glVertex3f(cx+3,1,cz-3); glVertex3f(cx+3,1,cz+3); glVertex3f(cx+3,ch,cz+3); glVertex3f(cx+3,ch,cz-3);
        glVertex3f(cx-3,ch,cz-3); glVertex3f(cx+3,ch,cz-3); glVertex3f(cx+3,ch,cz+3); glVertex3f(cx-3,ch,cz+3);
        glEnd();

        float dirLen = 15.0f;
        float ax = cx + sinf(sCamObj.angle) * dirLen;
        float az = cz - cosf(sCamObj.angle) * dirLen;
        glColor3f(1.0f, 0.9f, 0.3f);
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glVertex3f(cx, ch*0.5f, cz); glVertex3f(ax, ch*0.5f, az);
        glEnd();
        glLineWidth(1.0f);
    }

    // ---- Origin axes ----
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor3f(1,0,0); glVertex3f(0,0.2f,0); glVertex3f(30,0.2f,0);
    glColor3f(0,1,0); glVertex3f(0,0.2f,0); glVertex3f(0,30.2f,0);
    glColor3f(0,0,1); glVertex3f(0,0.2f,0); glVertex3f(0,0.2f,30);
    glEnd();
    glLineWidth(1.0f);

    // Restore
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
}

} // namespace Affinity

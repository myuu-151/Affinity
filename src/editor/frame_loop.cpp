#include "frame_loop.h"
#include "../viewport/mode7_preview.h"
#include "../map/map_types.h"
#include "../platform/gba/gba_package.h"
#include "imgui.h"

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

// Player directional sprites (8 directions: N, NE, E, SE, S, SW, W, NW)
static constexpr int kPlayerDirCount = 8;
static const char* const kPlayerDirNames[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
struct PlayerDirSprite
{
    std::string path;           // PNG file path
    unsigned char* pixels = nullptr; // RGBA pixel data
    int width = 0, height = 0;
    GLuint texture = 0;         // GL texture for preview
};
static PlayerDirSprite sPlayerDirs[kPlayerDirCount];

static void LoadPlayerDirImage(int dir, const std::string& filepath)
{
    PlayerDirSprite& d = sPlayerDirs[dir];
    // Free old data
    if (d.pixels) { stbi_image_free(d.pixels); d.pixels = nullptr; }
    if (d.texture) { glDeleteTextures(1, &d.texture); d.texture = 0; }

    int w, h, channels;
    unsigned char* data = stbi_load(filepath.c_str(), &w, &h, &channels, 4);
    if (!data) return;

    d.path = filepath;
    d.pixels = data;
    d.width = w;
    d.height = h;

    glGenTextures(1, &d.texture);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Project file
static std::string sProjectPath;  // empty = no project loaded
static bool sProjectDirty = false; // unsaved changes

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
        fprintf(f, "sprite=%d,%.6f,%.6f,%.6f,%.6f,%u,%d,%d,%d\n",
                sp.spriteId, sp.x, sp.y, sp.z, sp.scale, sp.color,
                sp.assetIdx, sp.animIdx, (int)sp.type);
    }
    fprintf(f, "\n");

    // Player directional sprites
    fprintf(f, "[PlayerDirs]\n");
    for (int d = 0; d < kPlayerDirCount; d++)
    {
        if (!sPlayerDirs[d].path.empty())
            fprintf(f, "dir=%d,%s\n", d, sPlayerDirs[d].path.c_str());
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
            fprintf(f, "anim=%s,%d,%d,%d,%d\n",
                    an.name.c_str(), an.startFrame, an.endFrame, an.fps, an.loop ? 1 : 0);
        }
        // LOD
        fprintf(f, "lodCount=%d\n", sa.lodCount);
        for (int li = 0; li < sa.lodCount; li++)
        {
            const SpriteLOD& lod = sa.lod[li];
            fprintf(f, "lod=%d,%d,%d,%.1f\n", lod.size, lod.frameStart, lod.frameCount, lod.maxDist);
        }
        fprintf(f, "asset_end\n");
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
    sSelectedAsset = -1;
    sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
    for (int d = 0; d < kPlayerDirCount; d++)
    {
        if (sPlayerDirs[d].pixels) { stbi_image_free(sPlayerDirs[d].pixels); sPlayerDirs[d].pixels = nullptr; }
        if (sPlayerDirs[d].texture) { glDeleteTextures(1, &sPlayerDirs[d].texture); sPlayerDirs[d].texture = 0; }
        sPlayerDirs[d].path.clear();
        sPlayerDirs[d].width = sPlayerDirs[d].height = 0;
    }
    sCamObjEditorScale = 0.05f;

    char line[8192]; // large buffer for frame pixel data lines
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
                int sid, aIdx = -1, anIdx = 0, typeVal = 0;
                float sx, sy, sz, sc;
                unsigned int col;
                // Try extended format (with assetIdx, animIdx, type)
                int matched = sscanf(line, "sprite=%d,%f,%f,%f,%f,%u,%d,%d,%d", &sid, &sx, &sy, &sz, &sc, &col, &aIdx, &anIdx, &typeVal);
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
                    sp.selected = false;
                    sSpriteCount++;
                }
            }
        }
        else if (strcmp(section, "PlayerDirs") == 0)
        {
            int dirIdx;
            char dirPath[512];
            if (sscanf(line, "dir=%d,%511[^\n]", &dirIdx, dirPath) == 2)
            {
                if (dirIdx >= 0 && dirIdx < kPlayerDirCount)
                    LoadPlayerDirImage(dirIdx, std::string(dirPath));
            }
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
                        if (sscanf(line + 5, "%63[^,],%d,%d,%d,%d", aname, &sf, &ef, &afps, &aloop) == 5)
                        {
                            an.name = aname;
                            an.startFrame = sf;
                            an.endFrame = ef;
                            an.fps = afps;
                            an.loop = (aloop != 0);
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
                }
                sSpriteAssets.push_back(sa);
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
    sSelectedAsset = -1;
    sSelectedFrame = 0;
    sSelectedAnim = -1;

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
        sSelectedAsset = (int)sSpriteAssets.size() - 1;
        sSelectedFrame = 0;
    }
    if (sSelectedAsset >= 0 && sSelectedAsset < (int)sSpriteAssets.size())
    {
        if (ImGui::Button("Delete", ImVec2(-1, 0)))
        {
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

        // ---- Frame Grid: pixel editor ----
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Frame Editor");

        if (!asset.frames.empty())
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
            na.startFrame = 0;
            na.endFrame = std::max(0, (int)asset.frames.size() - 1);
            na.fps = 8;
            na.loop = true;
            asset.anims.push_back(na);
        }

        for (int ai = 0; ai < (int)asset.anims.size(); ai++)
        {
            ImGui::PushID(ai + 3000);
            SpriteAnim& anim = asset.anims[ai];
            bool animSel = (sSelectedAnim == ai);

            if (ImGui::Selectable(anim.name.c_str(), animSel, 0, ImVec2(Scaled(80), 0)))
            {
                sSelectedAnim = ai;
                sAssetPreviewFrame = anim.startFrame;
                sAssetPreviewTimer = 0.0f;
            }
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(60));
            char nbuf[32];
            strncpy(nbuf, anim.name.c_str(), sizeof(nbuf) - 1); nbuf[sizeof(nbuf)-1] = '\0';
            if (ImGui::InputText("##aname", nbuf, sizeof(nbuf)))
                anim.name = nbuf;
            ImGui::PopItemWidth();
            ImGui::SameLine();

            int maxF = std::max(0, (int)asset.frames.size() - 1);
            ImGui::PushItemWidth(Scaled(40));
            ImGui::DragInt("##astart", &anim.startFrame, 1.0f, 0, maxF);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::Text("-");
            ImGui::SameLine();
            ImGui::PushItemWidth(Scaled(40));
            ImGui::DragInt("##aend", &anim.endFrame, 1.0f, 0, maxF);
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::PushItemWidth(Scaled(35));
            ImGui::DragInt("fps##afps", &anim.fps, 1.0f, 1, 30);
            ImGui::PopItemWidth();
            ImGui::SameLine();

            ImGui::Checkbox("Loop##aloop", &anim.loop);
            ImGui::SameLine();

            if (asset.anims.size() > 1 && ImGui::SmallButton("X##delanim"))
            {
                asset.anims.erase(asset.anims.begin() + ai);
                if (sSelectedAnim >= (int)asset.anims.size())
                    sSelectedAnim = (int)asset.anims.size() - 1;
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        // Animated preview (plays selected animation)
        if (sSelectedAnim >= 0 && sSelectedAnim < (int)asset.anims.size() && !asset.frames.empty())
        {
            SpriteAnim& anim = asset.anims[sSelectedAnim];
            sAssetPreviewTimer += dt;
            float frameTime = (anim.fps > 0) ? (1.0f / anim.fps) : 0.125f;
            if (sAssetPreviewTimer >= frameTime)
            {
                sAssetPreviewTimer -= frameTime;
                sAssetPreviewFrame++;
                if (sAssetPreviewFrame > anim.endFrame)
                    sAssetPreviewFrame = anim.loop ? anim.startFrame : anim.endFrame;
            }
            sAssetPreviewFrame = std::clamp(sAssetPreviewFrame, anim.startFrame, anim.endFrame);

            ImGui::Text("Preview: frame %d", sAssetPreviewFrame);
            if (sAssetPreviewFrame < (int)asset.frames.size())
            {
                SpriteFrame& pf = asset.frames[sAssetPreviewFrame];
                float prevSize = Scaled(64);
                float prevCell = prevSize / (float)pf.width;
                ImVec2 prevStart = ImGui::GetCursorScreenPos();
                ImDrawList* pdl = ImGui::GetWindowDrawList();
                pdl->AddRectFilled(prevStart, ImVec2(prevStart.x + prevSize, prevStart.y + prevSize), 0xFF1A1A1A);
                for (int py = 0; py < pf.width; py++)
                    for (int px = 0; px < pf.width; px++)
                    {
                        uint8_t pi = pf.pixels[py * kMaxFrameSize + px];
                        if (pi == 0) continue;
                        uint32_t col = asset.palette[pi & 0xF];
                        uint32_t cr = (col >> 0) & 0xFF;
                        uint32_t cg = (col >> 8) & 0xFF;
                        uint32_t cb = (col >> 16) & 0xFF;
                        uint32_t imCol = 0xFF000000 | (cb << 16) | (cg << 8) | cr;
                        ImVec2 pp0(prevStart.x + px * prevCell, prevStart.y + py * prevCell);
                        ImVec2 pp1(pp0.x + prevCell, pp0.y + prevCell);
                        pdl->AddRectFilled(pp0, pp1, imCol);
                    }
                ImGui::Dummy(ImVec2(prevSize, prevSize));
            }
        }
    }
    else
    {
        ImGui::TextWrapped("Select or create a sprite asset.");
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right column: LOD + Palette Editor + Asset Link ----
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

        ImGui::PushItemWidth(Scaled(50));
        ImGui::DragInt("Bank##palbank", &asset.palBank, 1.0f, 0, 15);
        ImGui::PopItemWidth();

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
            if (ImGui::ColorEdit4(clbl, rgba,
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
                    // First pass: collect unique colors (skip fully transparent pixels)
                    uint32_t uniqueColors[16] = {};
                    int numUnique = 0;
                    // Index 0 reserved for transparent
                    asset.palette[0] = 0x00000000;

                    for (int py = 0; py < imgH && numUnique < 15; py++)
                    {
                        for (int px = 0; px < imgW && numUnique < 15; px++)
                        {
                            const unsigned char* p = imgData + (py * imgW + px) * 4;
                            if (p[3] < 128) continue; // transparent
                            uint32_t col = p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000;
                            bool found = false;
                            for (int c = 0; c < numUnique; c++)
                            {
                                if (uniqueColors[c] == col) { found = true; break; }
                            }
                            if (!found)
                            {
                                uniqueColors[numUnique] = col;
                                asset.palette[numUnique + 1] = col;
                                numUnique++;
                            }
                        }
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

                    // Update anim if empty
                    if (asset.anims.empty())
                    {
                        SpriteAnim anim;
                        anim.name = "idle";
                        anim.startFrame = 0;
                        anim.endFrame = (int)asset.frames.size() - 1;
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
                const char* animPreview = (sp.animIdx >= 0 && sp.animIdx < (int)linkedAsset.anims.size())
                    ? linkedAsset.anims[sp.animIdx].name.c_str() : "idle";
                if (ImGui::BeginCombo("Anim##spranimlink", animPreview))
                {
                    for (int ai = 0; ai < (int)linkedAsset.anims.size(); ai++)
                    {
                        bool sel = (sp.animIdx == ai);
                        if (ImGui::Selectable(linkedAsset.anims[ai].name.c_str(), sel))
                            sp.animIdx = ai;
                    }
                    ImGui::EndCombo();
                }
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

    // Draw sprites on minimap
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

    // ---- Global hotkeys ----
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
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
                    se.assetIdx = sSprites[i].assetIdx;
                    se.animIdx = sSprites[i].animIdx;
                    se.spriteType = (int)sSprites[i].type;
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

                // Collect sprite assets for export
                std::vector<GBASpriteAssetExport> exportAssets;
                for (const auto& sa : sSpriteAssets)
                {
                    GBASpriteAssetExport ea;
                    ea.name = sa.name;
                    ea.baseSize = sa.baseSize;
                    ea.palBank = sa.palBank;
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
                        ea.anims.push_back(ean);
                    }
                    exportAssets.push_back(ea);
                }

                // Collect player direction sprites for export
                GBAPlayerDirExport exportPlayerDirs[8];
                for (int d = 0; d < 8; d++)
                {
                    exportPlayerDirs[d].pixels = sPlayerDirs[d].pixels;
                    exportPlayerDirs[d].width = sPlayerDirs[d].width;
                    exportPlayerDirs[d].height = sPlayerDirs[d].height;
                }
                float exportOrbitDist = sOrbitDist;

                std::thread([rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                             exportPlayerDirs, exportOrbitDist]() {
                    std::string err;
                    bool ok = PackageGBA(rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                                         exportPlayerDirs, exportOrbitDist, err);
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
            float moveSpeed = 35.0f * dt;
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

                // J/L manual orbit — always applies
                if (ImGui::IsKeyDown(ImGuiKey_J))
                    sOrbitAngle += rotSpeed;
                if (ImGui::IsKeyDown(ImGuiKey_L))
                    sOrbitAngle -= rotSpeed;

                if (sPlayerMoving)
                {
                    // Normalize diagonal
                    float len = sqrtf(inputX * inputX + inputZ * inputZ);
                    inputX /= len;
                    inputZ /= len;

                    // Track movement direction relative to camera (for sprite facing)
                    sPlayerMoveAngle = atan2f(inputZ, inputX);

                    // Auto-orbit when strafing (A/D)
                    // J (GBA L shoulder) doubles orbit, L (GBA R shoulder) slows it
                    if (inputZ != 0.0f)
                    {
                        float autoOrbitSpeed = rotSpeed * 0.4f * inputZ; // left = negative, right = positive
                        if (ImGui::IsKeyDown(ImGuiKey_J))
                            autoOrbitSpeed *= 2.0f;  // L shoulder: double orbit speed
                        else if (ImGui::IsKeyDown(ImGuiKey_L))
                            autoOrbitSpeed *= 0.25f; // R shoulder: slow down orbit
                        sOrbitAngle -= autoOrbitSpeed;
                    }

                    // Transform to world space using viewAngle
                    float fwdX = sinf(viewAngle), fwdZ = cosf(viewAngle);
                    float rightX = -cosf(viewAngle), rightZ = sinf(viewAngle);
                    player.x += (fwdX * inputX + rightX * inputZ) * moveSpeed;
                    player.z += (fwdZ * inputX + rightZ * inputZ) * moveSpeed;
                }
                else if (wasMoving)
                {
                    // Just stopped moving — sync sPlayerMoveAngle so idle doesn't snap
                    sPlayerMoveAngle = sPlayerMoveAngle - sOrbitAngle;
                }

                // Clamp player to world
                player.x = std::clamp(player.x, -kWorldHalf, kWorldHalf);
                player.z = std::clamp(player.z, -kWorldHalf, kWorldHalf);

                // Place camera at orbit offset from player (distance set on Play)
                sCamera.x = player.x + sinf(sOrbitAngle) * sOrbitDist;
                sCamera.z = player.z + cosf(sOrbitAngle) * sOrbitDist;
                // Camera looks from its position toward the player
                sCamera.angle = sOrbitAngle;
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
    // Build player direction image array for renderer
    Mode7::PlayerDirImage playerDirImages[Mode7::kPlayerDirCount] = {};
    for (int d = 0; d < Mode7::kPlayerDirCount; d++)
    {
        playerDirImages[d].pixels = sPlayerDirs[d].pixels;
        playerDirImages[d].width  = sPlayerDirs[d].width;
        playerDirImages[d].height = sPlayerDirs[d].height;
    }
    // Sprite direction: orbit-based when idle, movement-based when moving
    float spriteAngle = sPlayerMoving ? sPlayerMoveAngle : sOrbitAngle + sPlayerMoveAngle;
    Mode7::Render(sCamera, nullptr, sSprites, sSpriteCount, camObjPtr, sCamObjEditorScale,
                  assetsPtr, (int)sSpriteAssets.size(), sViewportAnimTime, isPlaying,
                  playerDirImages, spriteAngle);
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

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Player Directional Sprites");
        ImGui::Separator();
        ImGui::Spacing();

        // Layout: 3x3 grid for 8 directions + center preview
        //   NW  N  NE
        //   W  [P]  E
        //   SW  S  SE
        // Direction indices: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
        static const int gridMap[3][3] = {
            { 7, 0, 1 },   // NW, N, NE
            { 6, -1, 2 },  // W, center, E
            { 5, 4, 3 },   // SW, S, SE
        };

        float cellSz = std::min((totalW - 40.0f) / 3.0f, (bodyH - 80.0f) / 3.0f);
        cellSz = std::min(cellSz, 180.0f);
        float previewSz = cellSz - 30.0f;
        if (previewSz < 32.0f) previewSz = 32.0f;

        float gridW = cellSz * 3.0f;
        float padX = (totalW - gridW) * 0.5f;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (int row = 0; row < 3; row++)
        {
            ImGui::Dummy(ImVec2(0, 0)); // ensure row starts
            for (int col = 0; col < 3; col++)
            {
                int dir = gridMap[row][col];

                // Position each cell using cursor + indent
                if (col == 0)
                    ImGui::SetCursorPosX(padX + 2);
                else
                    ImGui::SameLine(padX + col * cellSz + 2);

                ImGui::PushID(row * 3 + col);

                ImVec2 cellMin = ImGui::GetCursorScreenPos();
                ImVec2 cellMax(cellMin.x + cellSz - 4, cellMin.y + cellSz - 4);

                if (dir == -1)
                {
                    // Center cell: PLAYER label
                    ImGui::InvisibleButton("##center", ImVec2(cellSz - 4, cellSz - 4));
                    dl->AddRectFilled(cellMin, cellMax, 0xFF1A1A2A);
                    dl->AddRect(cellMin, cellMax, 0xFF555577);
                    const char* label = "PLAYER";
                    ImVec2 tsz = ImGui::CalcTextSize(label);
                    dl->AddText(
                        ImVec2(cellMin.x + (cellSz - 4 - tsz.x) * 0.5f,
                               cellMin.y + (cellSz - 4 - tsz.y) * 0.5f),
                        0xFFFFFFFF, label);
                }
                else
                {
                    // Direction cell — clickable
                    bool clicked = ImGui::InvisibleButton("##dirbtn", ImVec2(cellSz - 4, cellSz - 4));
                    bool hovered = ImGui::IsItemHovered();

                    dl->AddRectFilled(cellMin, cellMax, 0xFF151520);
                    dl->AddRect(cellMin, cellMax, hovered ? 0xFFFFFFFF : 0xFF444466, 0.0f, 0, hovered ? 2.0f : 1.0f);

                    // Direction label
                    const char* dirName = kPlayerDirNames[dir];
                    ImVec2 lsz = ImGui::CalcTextSize(dirName);
                    dl->AddText(ImVec2(cellMin.x + (cellSz - 4 - lsz.x) * 0.5f, cellMin.y + 4), 0xFFAAAACC, dirName);

                    // Preview image
                    PlayerDirSprite& pds = sPlayerDirs[dir];
                    float imgY = cellMin.y + 22.0f;
                    float imgX = cellMin.x + (cellSz - 4 - previewSz) * 0.5f;

                    if (pds.texture)
                    {
                        float aspect = (float)pds.width / (float)pds.height;
                        float drawW = previewSz, drawH = previewSz;
                        if (aspect > 1.0f) drawH = previewSz / aspect;
                        else drawW = previewSz * aspect;
                        float offX = imgX + (previewSz - drawW) * 0.5f;
                        float offY = imgY + (previewSz - drawH) * 0.5f;
                        dl->AddImage((ImTextureID)(uintptr_t)pds.texture,
                            ImVec2(offX, offY),
                            ImVec2(offX + drawW, offY + drawH));
                    }
                    else
                    {
                        const char* empty = "Click to\nassign PNG";
                        ImVec2 esz = ImGui::CalcTextSize(empty);
                        dl->AddText(
                            ImVec2(cellMin.x + (cellSz - 4 - esz.x) * 0.5f,
                                   imgY + (previewSz - esz.y) * 0.5f),
                            0xFF666688, empty);
                    }

                    if (clicked)
                    {
                        std::string path = OpenFileDialog(
                            "PNG Images\0*.png\0All Files\0*.*\0", "png");
                        if (!path.empty())
                            LoadPlayerDirImage(dir, path);
                    }
                    // Right-click to clear slot
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && sPlayerDirs[dir].pixels)
                    {
                        stbi_image_free(sPlayerDirs[dir].pixels);
                        sPlayerDirs[dir].pixels = nullptr;
                        if (sPlayerDirs[dir].texture) { glDeleteTextures(1, &sPlayerDirs[dir].texture); sPlayerDirs[dir].texture = 0; }
                        sPlayerDirs[dir].path.clear();
                        sPlayerDirs[dir].width = sPlayerDirs[dir].height = 0;
                    }
                }

                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Batch load button
#ifdef _WIN32
        if (ImGui::Button("Load Folder..."))
        {
            std::string folder = OpenFolderDialog();
            if (!folder.empty())
            {
                // Try to match files: N.png, NE.png, E.png, SE.png, S.png, SW.png, W.png, NW.png
                // Also try lowercase and common alternates
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
                int loaded = 0;
                for (int d = 0; d < kPlayerDirCount; d++)
                {
                    bool found = false;
                    for (int a = 0; a < 4 && !found; a++)
                    {
                        if (!altNames[d][a]) continue;
                        std::string tryPath = folder + "\\" + altNames[d][a] + ".png";
                        if (std::filesystem::exists(tryPath))
                        {
                            LoadPlayerDirImage(d, tryPath);
                            found = true;
                            loaded++;
                        }
                    }
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(matches N/NE/E/SE/S/SW/W/NW .png or forward/left/right/etc)");
#endif

        ImGui::Spacing();

        // File paths below grid
        for (int d = 0; d < kPlayerDirCount; d++)
        {
            if (!sPlayerDirs[d].path.empty())
            {
                std::string filename = std::filesystem::path(sPlayerDirs[d].path).filename().string();
                ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.9f, 1.0f), "%s:", kPlayerDirNames[d]);
                ImGui::SameLine();
                ImGui::Text("%s", filename.c_str());
            }
        }

        ImGui::End();
    }
    else if (sActiveTab == EditorTab::ThreeD)
    {
        // 3D tab: placeholder
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, bodyY));
        ImGui::SetNextWindowSize(ImVec2(totalW, bodyH));
        ImGui::Begin("##3DTab", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Text("3D View — coming soon");
        ImGui::End();
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

} // namespace Affinity

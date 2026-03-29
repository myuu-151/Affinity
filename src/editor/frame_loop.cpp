#include "frame_loop.h"
#include "../viewport/mode7_preview.h"
#include "../map/map_types.h"
#include "../platform/gba/gba_package.h"
#include "imgui.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace Affinity
{

static Mode7Camera sCamera;
static bool sInitialized = false;

// Editor mode tabs
enum class EditorTab { Map, Sprites, Tiles };
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

// GBA packaging state
static bool sPackaging = false;
static bool sPackageDone = false;
static bool sPackageSuccess = false;
static std::string sPackageMsg;
static std::string sPackageOutputPath;

void FrameInit()
{
    sCamera.x      = 0.0f;
    sCamera.z      = 0.0f;
    sCamera.height = 64.0f;
    sCamera.angle  = 0.0f;
    sCamera.fov    = 128.0f;
    Mode7::Init();
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

// ---- Tab bar (NEXXT-style top tabs) ----
static void DrawTabBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float menuH = ImGui::GetFrameHeight(); // main menu bar height
    float tabH = 28.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, tabH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
    ImGui::Begin("##TabBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    auto TabButton = [](const char* label, EditorTab tab) {
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
        if (ImGui::Button(label, ImVec2(80, 22)))
            sActiveTab = tab;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };

    TabButton("Map",     EditorTab::Map);
    TabButton("Sprites", EditorTab::Sprites);
    TabButton("Tiles",   EditorTab::Tiles);

    // Right-aligned status text
    ImGui::SameLine(ImGui::GetWindowWidth() - 180);
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

    ImGui::Image((ImTextureID)(intptr_t)Mode7::GetTexture(), ImVec2(w, h));

    // Viewport label overlay
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(pos.x + 6, pos.y + 4),
        0x80FFFFFF, "Mode 7 Preview (WASD + Q/E)");

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

// ---- Right middle: Tilemap (top-down minimap) ----
static void DrawTilemapPanel(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##Tilemap", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Nametable / Tilemap");
    ImGui::Separator();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // Draw a 32x32 minimap grid
    int mapCols = 32, mapRows = 32;
    float cellW = std::max(3.0f, (size.x - 24.0f) / (float)mapCols);
    float cellH = cellW; // square cells

    // Clamp rows to fit
    float maxH = size.y - (cursor.y - pos.y) - 30.0f;
    if (mapRows * cellH > maxH)
        cellH = cellW = maxH / (float)mapRows;

    for (int row = 0; row < mapRows; row++)
    {
        for (int col = 0; col < mapCols; col++)
        {
            ImVec2 cPos(cursor.x + col * cellW, cursor.y + row * cellH);
            // Checkerboard pattern as placeholder map content
            int check = ((col / 4) + (row / 4)) % 2;
            uint32_t c = check ? 0xFF2A4A2A : 0xFF1A3A1A;
            dl->AddRectFilled(cPos, ImVec2(cPos.x + cellW, cPos.y + cellH), c);
        }
    }

    // Grid border
    dl->AddRect(cursor,
        ImVec2(cursor.x + mapCols * cellW, cursor.y + mapRows * cellH),
        0x60FFFFFF);

    // Camera position indicator
    float camMapX = (sCamera.x / 512.0f + 0.5f) * mapCols * cellW;
    float camMapZ = (sCamera.z / 512.0f + 0.5f) * mapRows * cellH;
    float indX = cursor.x + camMapX;
    float indZ = cursor.y + camMapZ;
    dl->AddCircleFilled(ImVec2(indX, indZ), 4.0f, 0xFF4444FF);
    // Direction arrow — forward is -Z in world = up on minimap
    float arrowLen = 10.0f;
    dl->AddLine(ImVec2(indX, indZ),
        ImVec2(indX - sinf(sCamera.angle) * arrowLen,
               indZ - cosf(sCamera.angle) * arrowLen),
        0xFF4444FF, 2.0f);

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + mapRows * cellH + 4));
    ImGui::Text("32x32 tiles  |  Click to paint");

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
    ImGui::DragFloat("H",  &sCamera.height, 0.5f, 8.0f, 256.0f);
    ImGui::SameLine();
    ImGui::SliderAngle("A", &sCamera.angle, -180.0f, 180.0f);
    ImGui::PopItemWidth();

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ---- Bottom status bar ----
static void DrawStatusBar(ImVec2 pos, ImVec2 size)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.16f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.55f, 1.0f),
        "Tile: %d  |  Pal: %d  |  Cam: (%.0f, %.0f) H:%.0f A:%.0f",
        sSelectedTile, sSelectedPalColor,
        sCamera.x, sCamera.z, sCamera.height, sCamera.angle * 57.2958f);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void FrameTick(float dt)
{
    if (!sInitialized) FrameInit();

    // ---- Main Menu ----
    float menuBarH = 0.0f;
    if (ImGui::BeginMainMenuBar())
    {
        menuBarH = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Map"))          { /* TODO */ }
            if (ImGui::MenuItem("Open Map"))         { /* TODO */ }
            if (ImGui::MenuItem("Save Map"))         { /* TODO */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Import Tileset"))   { /* TODO */ }
            if (ImGui::MenuItem("Export Tileset"))    { /* TODO */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Package GBA ROM", nullptr, false, !sPackaging))
            {
                sPackaging = true;
                sPackageDone = false;
                sPackageSuccess = false;
                sPackageMsg = "Building...";

                // Find gba_runtime relative to exe location
                namespace fs = std::filesystem;
                // Get exe directory (not cwd)
                char exeBuf[MAX_PATH] = {};
                GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
                fs::path exeDir = fs::path(exeBuf).parent_path();
                fs::path cwdDir = fs::current_path();
                // Try from both exe dir and cwd
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

                    // Build on background thread
                    std::thread([rtDirStr, outPath]() {
                        std::string err;
                        bool ok = PackageGBA(rtDirStr, outPath, err);
                        sPackageSuccess = ok;
                        sPackageMsg = ok
                            ? ("ROM saved: " + outPath + "\n\n" + err)
                            : err;
                        sPackageDone = true;
                        sPackaging = false;
                    }).detach();
                }
            }
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
        ImGui::EndMainMenuBar();
    }

    // ---- Camera Controls (only when no ImGui widget has focus) ----
    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        float moveSpeed = 80.0f * dt;
        float rotSpeed  = 2.0f * dt;

        // Rotate first so forward/back use the new heading
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
        if (ImGui::IsKeyDown(ImGuiKey_Q))
            sCamera.height = std::max(8.0f, sCamera.height - 40.0f * dt);
        if (ImGui::IsKeyDown(ImGuiKey_E))
            sCamera.height = std::min(256.0f, sCamera.height + 40.0f * dt);
    }

    // ---- Render Mode 7 ----
    Mode7::Render(sCamera);
    Mode7::UploadTexture();

    // ---- Layout: NEXXT-style fixed panels ----
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float totalW = vp->WorkSize.x;
    float totalH = vp->WorkSize.y;
    float topY   = vp->WorkPos.y + menuBarH;

    float tabH     = 28.0f;
    float statusH  = 24.0f;
    float bodyY    = topY + tabH;
    float bodyH    = totalH - menuBarH - tabH - statusH;

    // Right panel width: ~35% of window, min 280
    float rightW = std::max(280.0f, totalW * 0.35f);
    float leftW  = totalW - rightW;

    // Right panel split: tileset 35%, tilemap 40%, palette+props 25%
    float tilesetH  = bodyH * 0.35f;
    float tilemapH  = bodyH * 0.40f;
    float paletteH  = bodyH - tilesetH - tilemapH;

    // Draw everything
    DrawTabBar();

    // Tab bar positioned at topY
    // The tab bar positions itself via GetMainViewport WorkPos

    DrawViewport(
        ImVec2(vp->WorkPos.x, bodyY),
        ImVec2(leftW, bodyH));

    DrawTilesetPanel(
        ImVec2(vp->WorkPos.x + leftW, bodyY),
        ImVec2(rightW, tilesetH));

    DrawTilemapPanel(
        ImVec2(vp->WorkPos.x + leftW, bodyY + tilesetH),
        ImVec2(rightW, tilemapH));

    DrawPalettePanel(
        ImVec2(vp->WorkPos.x + leftW, bodyY + tilesetH + tilemapH),
        ImVec2(rightW, paletteH));

    DrawStatusBar(
        ImVec2(vp->WorkPos.x, bodyY + bodyH),
        ImVec2(totalW, statusH));

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
            ImGui::TextWrapped("%s", sPackageMsg.c_str());
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(80, 0)))
                sPackageDone = false;

            if (sPackageSuccess)
            {
                ImGui::SameLine();
                if (ImGui::Button("Open in mGBA", ImVec2(120, 0)))
                {
                    // Try to launch mGBA with the ROM
                    std::string cmd = "start \"\" \"" + sPackageOutputPath + "\"";
                    system(cmd.c_str());
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

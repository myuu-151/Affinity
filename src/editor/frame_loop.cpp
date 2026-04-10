#include "frame_loop.h"
#include "../viewport/mode7_preview.h"
#include "../map/map_types.h"
#include "../platform/gba/gba_package.h"
#include "../platform/nds/nds_package.h"
#include "imgui.h"

#include <array>
#include <cmath>
#include <algorithm>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <sstream>
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
enum class EditorTab { Map, Sprites, Tiles, Skybox, Player, ThreeD, Mode7, Tilemap, Events };
static EditorTab sActiveTab = EditorTab::Map;
static EditorTab sPlayTab = EditorTab::Map; // which tab Play was started on

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

// ---- Tilemap tab state ----
static Mode7Map sTilemapData;
static bool sTilemapDataInit = false;
static int sTmSelectedTile = 0;    // selected tile index in tileset
static int sTmPalColor = 1;        // selected palette paint color
static float sTmMapZoom = 1.0f;    // tilemap grid zoom
static float sTmMapPanX = 0.0f;
static float sTmMapPanY = 0.0f;
static int sTmTool = 0;            // 0 = draw, 1 = erase, 2 = pick
static int  sTmDragEdge = 0;       // 0=none, 1=top, 2=bottom, 3=left, 4=right

// ---- Tilemap sprite texture cache ----
static std::vector<unsigned int> sTmSpriteTextures; // GL texture IDs, indexed by sprite asset
static std::vector<bool> sTmSpriteTexOwned;          // true = we created it, false = borrowed from dir sprites
static int sTmSpriteTexCount = 0; // how many we've cached

// ---- Tilemap object system ----
enum class TmObjType { Player = 0, Enemy, NPC, Door, Chest, SavePoint, Tile, Teleport, COUNT };
static const char* sTmObjTypeNames[] = { "Player", "Enemy", "NPC", "Door", "Chest", "Save Point", "Tile", "Teleport" };
static const uint32_t sTmObjTypeColors[] = {
    0xFF44FF44, // Player - green
    0xFF4444FF, // Enemy - red
    0xFFFFAA44, // NPC - orange
    0xFF44AAFF, // Door - blue
    0xFFFFFF44, // Chest - yellow
    0xFFFF44FF, // Save Point - magenta
    0xFF888888, // Tile - grey
    0xFF44FFFF, // Teleport - cyan
};

struct TmObject {
    TmObjType type = TmObjType::Player;
    int tileX = 0, tileY = 0;       // position in tile coords (for non-Tile objects)
    char name[32] = "";
    int spriteAssetIdx = -1;         // index into sSpriteAssets (-1 = none)
    int teleportScene = -1;          // target scene index (-1 = none, only for Teleport type)
    // Tile objects: one object covers multiple cells
    std::vector<std::pair<int,int>> cells; // (tileX,tileY) list — only used when type==Tile
    float displayScale = 1.0f;      // visual scale multiplier (0.5 - 4.0)
    // Blueprint script attachment
    int blueprintIdx = -1;
    struct { int paramIdx; int value; } instanceParams[8] = {};
    int instanceParamCount = 0;
};
static std::vector<TmObject> sTmObjects;
static int sTmSelectedObj = -1;      // selected object index (-1 = none)
static int sTmDragObj = -1;          // object being dragged (-1 = none)
static int sTmStampAsset = -1;       // sprite asset index for stamp/paint mode (-1 = off)
static int sTmStampObj = -1;         // which Tile object we're painting into (-1 = none)
static int sTmPaintScale = 4;       // paint block size: 1=single sub-cell, 4=4x4 block, 8=8x8 block
static int sTmPlaceTX = 0, sTmPlaceTY = 0; // tile coords for popup placement
static float sTmObjPanelW = 200.0f;  // object panel width
static std::vector<TmObject> sSavedTmObjects; // saved tilemap objects before Play
static float sTmMoveAccum[4] = {};  // per-direction movement accumulator (Up/Down/Left/Right)
static std::vector<int> sTmObjFacing; // per-object facing direction (0=N..7=NW, index into dir sprites)
static std::vector<int> sTmObjAnimSet; // per-object animation set index (for directional sprites)
static std::vector<int> sTmObjStepCount; // per-object tile step counter (for step-based animation)
static std::vector<float> sTmObjMoveRate; // per-object tile move speed (tiles/sec), set by Walk/Sprint
static int sTmLastMoveDir = -1; // last direction pressed (0=Left,1=Right,2=Up,3=Down), -1=none
static bool sTmPrevDirHeld[4] = {}; // previous frame held state for edge detection
static bool sTmPrevKeyState[10] = {}; // previous frame key state for OnKeyReleased
static bool sTmOnStartRan = false; // tilemap OnStart fired this play session
static std::vector<float> sTmObjVisX; // per-object visual X position (smooth lerp)
static std::vector<float> sTmObjVisY; // per-object visual Y position (smooth lerp)

// ---- Scene instances ----
struct TmScene {
    char name[32] = "";
    int mapW = 1, mapH = 1;        // grid dimensions in tiles
    // Per-scene tilemap data
    std::vector<uint16_t> tileIndices;  // saved tile grid
    std::vector<TmObject> objects;      // saved objects
    // Scene-level blueprint attachment
    int blueprintIdx = -1;
    struct { int paramIdx; int value; } instanceParams[8] = {};
    int instanceParamCount = 0;
};
static std::vector<TmScene> sTmScenes;
static int sTmSelectedScene = 0;     // active scene index
static float sTmScenePanelW = 180.0f; // scene panel width

// Save current tilemap state into the given scene
static void SaveSceneState(TmScene& sc)
{
    sc.mapW = sTilemapData.floor.width;
    sc.mapH = sTilemapData.floor.height;
    sc.tileIndices = sTilemapData.floor.tileIndices;
    sc.objects = sTmObjects;
}

// Load a scene's tilemap state into the active tilemap
static void LoadSceneState(const TmScene& sc)
{
    sTilemapData.floor.width  = sc.mapW;
    sTilemapData.floor.height = sc.mapH;
    if (!sc.tileIndices.empty())
        sTilemapData.floor.tileIndices = sc.tileIndices;
    else
        sTilemapData.floor.tileIndices.assign(sc.mapW * sc.mapH, 0);
    sTmObjects = sc.objects;
    sTmSelectedObj = -1;
    sTmDragObj = -1;
    sTmStampObj = -1;
}

// ---- Visual Script Node System ----
enum class VsNodeType : int {
    // Events (triggers — green, no input exec pin)
    OnKeyPressed = 0, OnKeyReleased, OnKeyHeld,
    OnCollision,
    OnStart,
    // Logic (blue)
    Branch,         // if/else
    CompareVar,     // variable comparison
    // Actions (orange, has input exec pin)
    MovePlayer,
    LookDirection,
    ChangeScene,
    SetVariable,
    AddVariable,
    PlaySound,
    Wait,
    Jump,           // player jump
    Walk,           // set walk speed
    Sprint,         // set sprint speed
    OrbitCamera,    // rotate orbit camera
    PlayAnim,       // play animation on player sprite
    SetGravity,     // set gravity value
    SetMaxFall,     // set max fall speed
    DestroyObject,  // remove sprite/object from scene
    AutoOrbit,      // auto-orbit camera when strafing
    DampenJump,     // reduce upward velocity while A released (variable jump height)
    Integer,        // constant integer output
    Key,            // constant key output (A/B/L/R/etc)
    Direction,      // constant direction output (Left/Right/Up/Down)
    Animation,      // constant animation index output
    Float,          // constant float output
    OnUpdate,       // fires every frame
    Group,          // subgraph containing other nodes
    Object,         // constant object/sprite index output (dropdown)
    CustomCode,     // user-written C code (GBA only)
    COUNT
};

struct VsNodeTypeDef {
    const char* name;
    ImU32 color;        // header color (ABGR)
    int inExec;         // number of exec input pins (0 for events)
    int outExec;        // number of exec output pins
    int inData;         // number of data input pins
    int outData;        // number of data output pins
    const char* inDataNames[4];
    const char* outDataNames[4];
    const char* outExecNames[4];
};

static const char* sVsKeyNames[] = { "A", "B", "L", "R", "Start", "Select", "Up", "Down", "Left", "Right" };
static constexpr int kVsKeyCount = 10;
static const char* sVsAxisNames[] = { "Left", "Right", "Up", "Down" };
static constexpr int kVsAxisCount = 4;

static const VsNodeTypeDef sVsNodeDefs[] = {
    // Events (green)
    { "On Key Pressed",  0xFF338833, 0, 1, 1, 0, {"Key"}, {}, {} },
    { "On Key Released", 0xFF338833, 0, 1, 1, 0, {"Key"}, {}, {} },
    { "On Key Held",     0xFF338833, 0, 1, 1, 0, {"Key"}, {}, {} },
    { "On Collision",    0xFF338833, 0, 1, 0, 0, {}, {}, {} },
    { "On Start",        0xFF338833, 0, 1, 0, 0, {}, {}, {} },
    // Logic (blue)
    { "Branch",          0xFF885533, 1, 2, 1, 0, {"Condition"}, {}, {"True", "False"} },
    { "Compare Var",     0xFF885533, 0, 0, 2, 1, {"Var Slot", "Value"}, {"Result"}, {} },
    // Actions (orange)
    { "Move Player",     0xFF3355AA, 1, 1, 1, 0, {"Direction"}, {}, {} },
    { "Look Direction",  0xFF3355AA, 1, 1, 1, 0, {"Direction"}, {}, {} },
    { "Change Scene",    0xFF3355AA, 1, 1, 1, 0, {"Scene (int)"}, {}, {} },
    { "Set Variable",    0xFF3355AA, 1, 1, 2, 0, {"Var Slot (int)", "Value"}, {}, {} },
    { "Add Variable",    0xFF3355AA, 1, 1, 2, 0, {"Var Slot (int)", "Amount"}, {}, {} },
    { "Play Sound",      0xFF3355AA, 1, 1, 1, 0, {"Sound ID (int)"}, {}, {} },
    { "Wait",            0xFF3355AA, 1, 1, 1, 0, {"Frames (int)"}, {}, {} },
    { "Jump",            0xFF3355AA, 1, 1, 1, 0, {"Force (float)"}, {}, {} },
    { "Walk",            0xFF3355AA, 1, 1, 1, 0, {"Speed (int)"}, {}, {} },
    { "Sprint",          0xFF3355AA, 1, 1, 1, 0, {"Speed (int)"}, {}, {} },
    { "Orbit Camera",    0xFF3355AA, 1, 1, 2, 0, {"Direction", "Speed (int)"}, {}, {} },
    { "Play Animation",  0xFF3355AA, 1, 1, 1, 0, {"Anim"}, {}, {} },
    { "Set Gravity",     0xFF3355AA, 1, 1, 1, 0, {"Value (float)"}, {}, {} },
    { "Set Max Fall",    0xFF3355AA, 1, 1, 1, 0, {"Value (float)"}, {}, {} },
    { "Destroy Object", 0xFF3355AA, 1, 1, 1, 0, {"Object (int)"}, {}, {} },
    { "Auto Orbit",     0xFF3355AA, 1, 1, 1, 0, {"Speed (int)"}, {}, {} },
    { "Dampen",         0xFF3355AA, 1, 1, 1, 0, {"Factor (float)"}, {}, {} },
    { "Integer",         0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "Key",             0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "Direction",       0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "Animation",       0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "Float",           0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "On Update",       0xFF338833, 0, 1, 0, 0, {}, {}, {} },
    { "Group",           0xFF888844, 0, 0, 0, 0, {}, {}, {} },
    { "Object",          0xFF666688, 0, 0, 0, 1, {}, {"Out"}, {} },
    { "Custom Code",     0xFF993399, 1, 1, 0, 0, {}, {}, {} },
};

struct VsNode {
    int id = 0;
    VsNodeType type = VsNodeType::OnStart;
    float x = 0, y = 0;          // canvas position
    // Per-node param values (interpreted based on type)
    int paramInt[4] = {};         // e.g. key index, var slot, pixels, scene index
    bool selected = false;        // multi-select flag
    // Group support
    int groupId = 0;              // 0 = top-level; >0 = inside group with this node id
    char groupLabel[32] = {};     // label for Group-type nodes
    int grpInExec = 0, grpOutExec = 0;   // dynamic pin counts for Group nodes
    int grpInData = 0, grpOutData = 0;
    char customCode[512] = {};            // user-editable code override (empty = use default)
    char funcName[64] = {};               // custom function name (empty = use default afn_ name)
};

// Pin address: which node, which pin type, which pin index
struct VsPin {
    int nodeId = -1;
    int pinType = 0;              // 0=execOut, 1=execIn, 2=dataOut, 3=dataIn
    int pinIdx = 0;
};

struct VsLink {
    VsPin from, to;
};

// Annotation boxes (drawn behind nodes)
struct VsAnnotation {
    float x, y, w, h;            // canvas-space position and size
    char label[64] = {};
    ImU32 color = 0x44888888;     // fill color (translucent)
    bool selected = false;        // multi-select flag
};

static std::vector<VsNode> sVsNodes;
static std::vector<VsLink> sVsLinks;
static std::vector<VsAnnotation> sVsAnnotations;
static int sVsNextId = 1;
static int sVsSelected = -1;     // selected node index (-1 = none)
static float sVsPanX = 0, sVsPanY = 0;   // canvas pan offset
static float sVsZoom = 1.0f;
// Dragging state
static bool sVsDraggingNode = false;
static bool sVsDraggingCanvas = false;
static bool sVsDraggingLink = false;
static VsPin sVsLinkStart = {};
static ImVec2 sVsLinkEndPos = {};
// Box selection
static bool sVsBoxSelecting = false;
static ImVec2 sVsBoxStart = {};        // screen-space start of selection box
// Group navigation
static int sVsEditingGroup = 0;        // 0 = top-level; >0 = inside group node id
static float sVsParentPanX = 0, sVsParentPanY = 0;
static float sVsParentZoom = 1.0f;
// Group pin mappings: which internal pin maps to which group-node pin
struct VsGroupPinMap {
    int groupNodeId;
    int pinType, pinIdx;           // pin on the group node
    int innerNodeId;
    int innerPinType, innerPinIdx; // pin on the internal node
};
static std::vector<VsGroupPinMap> sVsGroupPins;

// ---- Blueprint Script Asset System ----
// Parameter slot exposed by a blueprint for per-instance override
struct BpParam {
    char name[32] = {};          // display name ("Patrol Speed")
    int  sourceNodeId = -1;      // which VsNode provides the default
    int  sourceParamIdx = 0;     // which paramInt[] slot (0-3)
    int  dataType = 0;           // 0=int, 1=float, 2=key, 3=direction, 4=animation
    int  defaultInt = 0;         // default value
};

struct BlueprintAsset {
    char name[32] = "Script";
    std::vector<VsNode>        nodes;
    std::vector<VsLink>        links;
    std::vector<VsAnnotation>  annotations;
    std::vector<VsGroupPinMap> groupPins;
    int nextId = 1;
    float panX = 0, panY = 0, zoom = 1.0f;
    BpParam params[8] = {};
    int paramCount = 0;
};

static std::vector<BlueprintAsset> sBlueprintAssets;
static int sSelectedBlueprint = -1;

// Track what the node editor is currently editing
enum class VsEditSource { Scene, Blueprint };
static VsEditSource sVsEditSource = VsEditSource::Scene;
static int sVsEditBlueprintIdx = -1;

// Per-instance override (on objects that reference blueprints)
struct BpInstanceParam {
    int paramIdx = 0;   // index into BlueprintAsset::params[]
    int value = 0;      // overridden value
};

// Annotation interaction
static int sVsSelectedAnnotation = -1;
static bool sVsDraggingAnnotation = false;
static bool sVsResizingAnnotation = false;
static bool sVsResizingAnnotationLeft = false;
static int sVsEditingAnnotation = -1;  // which annotation has active text input
// Context menu
static bool sVsShowContextMenu = false;
static ImVec2 sVsContextMenuPos = {};
// Auto-wire: when dropping a link on empty space, open context menu and wire to new node
static VsPin sVsPendingAutoWire = { -1, 0, 0 };
// Undo stack for node deletions
struct VsUndoSnapshot {
    std::vector<VsNode> nodes;
    std::vector<VsLink> links;
    std::vector<VsAnnotation> annotations;
    std::vector<VsGroupPinMap> groupPins;
    int nextId;
};
static std::vector<VsUndoSnapshot> sVsUndoStack;
static const int kVsMaxUndo = 32;
// Node info popup (right-click on node)
static int sVsNodeInfoIdx = -1;   // index of node showing info popup
static char sVsNodeCodeBuf[2048] = {}; // editable code buffer
static bool sVsNodeInfoJustOpened = false;
// Script code window (opened from node right-click "Edit")
static bool sVsCodeWindowOpen = false;
static bool sVsCodeWindowJustOpened = false;
static int sVsCodeWindowNodeIdx = -1;
static char sVsCodeWindowBuf[4096] = {}; // generated code (read-only display)
static char sVsCodeWindowEditBuf[2048] = {}; // editable override

static constexpr float kVsNodeW = 160.0f;
static constexpr float kVsHeaderH = 30.0f;
static constexpr float kVsPinSpacing = 20.0f;
static constexpr float kVsPinRadius = 5.0f;

// Get pin counts for a node (uses dynamic counts for Group nodes)
struct VsPinCounts { int inExec, outExec, inData, outData; };
static VsPinCounts VsGetPinCounts(const VsNode& n) {
    if (n.type == VsNodeType::Group)
        return { n.grpInExec, n.grpOutExec, n.grpInData, n.grpOutData };
    if ((int)n.type < 0 || (int)n.type >= (int)VsNodeType::COUNT)
        return { 0, 0, 0, 0 };
    const auto& def = sVsNodeDefs[(int)n.type];
    return { def.inExec, def.outExec, def.inData, def.outData };
}

static float VsNodeHeight(const VsNode& n) {
    if ((int)n.type < 0 || (int)n.type >= (int)VsNodeType::COUNT) return kVsHeaderH + kVsPinSpacing + 8.0f;
    auto pc = VsGetPinCounts(n);
    int rows = std::max({ pc.inExec + pc.inData, pc.outExec + pc.outData, 1 });
    return kVsHeaderH + rows * kVsPinSpacing + 8.0f;
}

static int VsFindNode(int id) {
    for (int i = 0; i < (int)sVsNodes.size(); i++)
        if (sVsNodes[i].id == id) return i;
    return -1;
}

// Get pin screen position given canvas origin and zoom
static ImVec2 VsPinPos(const VsNode& n, int pinType, int pinIdx, ImVec2 canvasOrig, float zoom) {
    float nx = canvasOrig.x + (n.x + sVsPanX) * zoom;
    float ny = canvasOrig.y + (n.y + sVsPanY) * zoom;
    float w = kVsNodeW * zoom;
    float h = kVsHeaderH * zoom;
    float sp = kVsPinSpacing * zoom;
    auto pc = VsGetPinCounts(n);
    float px, py;
    switch (pinType) {
    case 0: // execOut (right side, top)
        px = nx + w; py = ny + h + sp * (0.5f + pinIdx); break;
    case 1: // execIn (left side, top)
        px = nx; py = ny + h + sp * 0.5f; break;
    case 2: // dataOut (right side, below exec)
        px = nx + w; py = ny + h + sp * (0.5f + pc.outExec + pinIdx); break;
    case 3: // dataIn (left side, below exec)
        px = nx; py = ny + h + sp * (0.5f + pc.inExec + pinIdx); break;
    default: px = nx; py = ny; break;
    }
    return ImVec2(px, py);
}

// Recompute group node pins from boundary-crossing links
static void VsRecomputeGroupPins(int groupNodeId) {
    int gi = VsFindNode(groupNodeId);
    if (gi < 0) return;
    VsNode& gn = sVsNodes[gi];

    // Collect internal node ids
    std::vector<int> insideIds;
    for (auto& n : sVsNodes)
        if (n.groupId == groupNodeId) insideIds.push_back(n.id);

    auto isInside = [&](int id) {
        for (int x : insideIds) if (x == id) return true;
        return false;
    };

    // Remove old mappings for this group
    sVsGroupPins.erase(std::remove_if(sVsGroupPins.begin(), sVsGroupPins.end(),
        [&](const VsGroupPinMap& m) { return m.groupNodeId == groupNodeId; }), sVsGroupPins.end());

    int inExec = 0, outExec = 0, inData = 0, outData = 0;

    for (auto& lk : sVsLinks) {
        bool fromIn = isInside(lk.from.nodeId);
        bool toIn   = isInside(lk.to.nodeId);
        if (fromIn && !toIn) {
            // Outgoing: internal node's output becomes group's output
            int pt, pi;
            if (lk.from.pinType == 0) { pt = 0; pi = outExec++; }
            else { pt = 2; pi = outData++; }
            sVsGroupPins.push_back({ groupNodeId, pt, pi, lk.from.nodeId, lk.from.pinType, lk.from.pinIdx });
        }
        if (!fromIn && toIn) {
            // Incoming: internal node's input becomes group's input
            int pt, pi;
            if (lk.to.pinType == 1) { pt = 1; pi = inExec++; }
            else { pt = 3; pi = inData++; }
            sVsGroupPins.push_back({ groupNodeId, pt, pi, lk.to.nodeId, lk.to.pinType, lk.to.pinIdx });
        }
    }

    gn.grpInExec = inExec;
    gn.grpOutExec = outExec;
    gn.grpInData = inData;
    gn.grpOutData = outData;
}

// Resolve a pin to its visible representation at the current editing level
// If the pin's node is inside a group, map it to the group node's pin
static VsPin VsResolvePin(const VsPin& pin) {
    int ni = VsFindNode(pin.nodeId);
    if (ni < 0) return pin;
    VsNode& n = sVsNodes[ni];
    if (n.groupId == sVsEditingGroup) return pin; // visible at current level
    // Node is inside a group — find the mapping
    for (auto& m : sVsGroupPins) {
        if (m.innerNodeId == pin.nodeId && m.innerPinType == pin.pinType && m.innerPinIdx == pin.pinIdx)
            return { m.groupNodeId, m.pinType, m.pinIdx };
    }
    return { -1, 0, 0 }; // not visible
}

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

// Rebuild tilemap sprite texture cache from sprite assets
static void RebuildTmSpriteTextures()
{
    for (int j = 0; j < (int)sTmSpriteTextures.size(); j++)
        if (sTmSpriteTextures[j] && (j >= (int)sTmSpriteTexOwned.size() || sTmSpriteTexOwned[j]))
            glDeleteTextures(1, &sTmSpriteTextures[j]);
    sTmSpriteTextures.clear();
    sTmSpriteTextures.resize(sSpriteAssets.size(), 0);
    sTmSpriteTexOwned.clear();
    sTmSpriteTexOwned.resize(sSpriteAssets.size(), false);
    for (int i = 0; i < (int)sSpriteAssets.size(); i++)
    {
        const SpriteAsset& sa = sSpriteAssets[i];

        // Try frame 0 pixels first
        bool hasFramePixels = false;
        if (!sa.frames.empty())
        {
            const SpriteFrame& frame = sa.frames[0];
            for (int p = 0; p < frame.width * frame.height && !hasFramePixels; p++)
                if (frame.pixels[p / frame.width * kMaxFrameSize + p % frame.width] != 0)
                    hasFramePixels = true;
        }

        if (hasFramePixels)
        {
            const SpriteFrame& frame = sa.frames[0];
            int fw = frame.width, fh = frame.height;
            std::vector<uint32_t> rgba(fw * fh, 0);
            for (int py = 0; py < fh; py++)
                for (int px = 0; px < fw; px++)
                {
                    uint8_t palIdx = frame.pixels[py * kMaxFrameSize + px];
                    if (palIdx == 0) continue;
                    uint32_t c = sa.palette[palIdx & 0xF];
                    rgba[py * fw + px] = c | 0xFF000000;
                }
            unsigned int tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            sTmSpriteTextures[i] = tex;
            sTmSpriteTexOwned[i] = true;
        }
        // Direction sprite fallback is handled in a second pass below
        // (sAssetDirSprites is declared after this function)
    }
    sTmSpriteTexCount = (int)sSpriteAssets.size();
}

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

// Patch tilemap sprite textures: for assets with no frame pixels but direction sprites,
// use the South-facing (or first available) direction texture as the preview.
static void PatchTmSpriteTexturesFromDirs()
{
    for (int i = 0; i < (int)sTmSpriteTextures.size(); i++)
    {
        if (sTmSpriteTextures[i] != 0) continue; // already has a texture
        if (i >= (int)sAssetDirSprites.size() || sAssetDirSprites[i].empty()) continue;
        // Try South (4) first, then cycle through others
        for (int dir = 4, tries = 0; tries < 8; tries++, dir = (dir + 1) % 8)
        {
            GLuint tex = sAssetDirSprites[i][0][dir].texture;
            if (tex)
            {
                sTmSpriteTextures[i] = tex;
                break;
            }
        }
    }
}

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
static bool sScriptStartRan = false;  // script OnStart ran this Play session
static float sScriptMoveSpeed = -1.0f; // persists across frames (Walk/Sprint nodes)
static float sScriptAutoOrbitSpeed = 0.0f; // persists across frames (AutoOrbit node)
static int sPendingSceneSwitch = -1;   // scene index to switch to (-1 = none)
static int sPendingSceneMode  = 0;    // 0 = 3D/MapScene, 1 = Tilemap/TmScene
static int sActivePlayAnimNodeId = -1; // PlayAnim node currently driving animation
static int sPlayAnimIdle = -1;    // PlayAnim from OnStart (idle)
static int sPlayAnimHeld = -1;    // PlayAnim from OnKeyHeld (sprint anim)
static int sPlayAnimReleased = -1; // PlayAnim from OnKeyReleased (walk anim)
// Link surge animation: tracks which node IDs fired this frame
static std::vector<int> sVsFiredNodes;       // node IDs that fired this frame
static std::map<int, float> sVsLinkSurgeT;   // link index → surge position 0→1 (traveling dot)
static std::map<int, float> sVsLinkSurgeRevT; // link index → reverse surge (travels to→from)
static SelectedObjType sSelectedObjType = SelectedObjType::None;

// Active transform axis for viewport gizmo (0=none, 'X','Y','Z','S')
static char sTransformAxis = 0;

// Grab mode state (G key — Blender-style translate)
static bool  sGrabMode = false;
static char  sGrabAxis = 0;        // 0=free XZ, 'X','Y','Z'=constrained
static float sGrabOrigX, sGrabOrigY, sGrabOrigZ; // for Escape cancel
static float sGrabStartGbaX, sGrabStartGbaY; // GBA-space mouse pos at grab start
static float sGrabStartWorldX, sGrabStartWorldZ; // world pos under mouse at grab start

// Scale mode state (S key — mouse Y to scale)
static bool  sScaleMode = false;
static float sScaleOrig = 1.0f;
static float sScaleMouseStartY = 0.0f;

// R-drag undo tracking
static bool sRDragUndoPushed = false;

// ---- Undo system ----
struct UndoEntry
{
    int spriteIdx;
    float x, y, z, scale, rotation;
};
static constexpr int kMaxUndo = 64;
static UndoEntry sUndoStack[kMaxUndo];
static int sUndoCount = 0;
static int sUndoCursor = 0; // points to next write slot; redo goes forward

static void UndoPush(int idx, const FloorSprite& sp)
{
    if (sUndoCursor < kMaxUndo)
    {
        sUndoStack[sUndoCursor] = { idx, sp.x, sp.y, sp.z, sp.scale, sp.rotation };
        sUndoCursor++;
        sUndoCount = sUndoCursor; // discard redo history
    }
    else
    {
        // Shift stack left by 1
        for (int i = 0; i < kMaxUndo - 1; i++)
            sUndoStack[i] = sUndoStack[i + 1];
        sUndoStack[kMaxUndo - 1] = { idx, sp.x, sp.y, sp.z, sp.scale, sp.rotation };
        sUndoCount = kMaxUndo;
    }
}

static void UndoPop()
{
    if (sUndoCursor <= 0) return;
    sUndoCursor--;
    UndoEntry& e = sUndoStack[sUndoCursor];
    if (e.spriteIdx >= 0 && e.spriteIdx < sSpriteCount)
    {
        FloorSprite& sp = sSprites[e.spriteIdx];
        // Save current state for redo before restoring
        float cx = sp.x, cy = sp.y, cz = sp.z, cs = sp.scale, cr = sp.rotation;
        sp.x = e.x; sp.y = e.y; sp.z = e.z; sp.scale = e.scale; sp.rotation = e.rotation;
        // Overwrite the entry with what we just replaced (for redo)
        e = { e.spriteIdx, cx, cy, cz, cs, cr };
    }
}

static void RedoPush()
{
    if (sUndoCursor >= sUndoCount) return;
    UndoEntry& e = sUndoStack[sUndoCursor];
    if (e.spriteIdx >= 0 && e.spriteIdx < sSpriteCount)
    {
        FloorSprite& sp = sSprites[e.spriteIdx];
        float cx = sp.x, cy = sp.y, cz = sp.z, cs = sp.scale, cr = sp.rotation;
        sp.x = e.x; sp.y = e.y; sp.z = e.z; sp.scale = e.scale; sp.rotation = e.rotation;
        e = { e.spriteIdx, cx, cy, cz, cs, cr };
    }
    sUndoCursor++;
}

// Viewport image position/scale — updated each frame for grab mode
static ImVec2 sVPImgPos;
static float  sVPImgScale = 1.0f;
static CameraStartObject sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
static float sCamObjEditorScale = 0.05f; // editor-only visual size
static Mode7Camera sSavedEditorCam;

// ---- Map scenes (Scene tab) ----
struct MapScene {
    char name[32] = {};
    FloorSprite sprites[kMaxFloorSprites] = {};
    int spriteCount = 0;
    CameraStartObject camera = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
    float camEditorScale = 0.05f;
    std::vector<VsNode> vsNodes;
    std::vector<VsLink> vsLinks;
    std::vector<VsAnnotation> vsAnnotations;
    std::vector<VsGroupPinMap> vsGroupPins;
    int vsNextId = 1;
    float vsPanX = 0, vsPanY = 0;
    float vsZoom = 1.0f;
    // Scene-level blueprint attachment
    int blueprintIdx = -1;
    struct { int paramIdx; int value; } instanceParams[8] = {};
    int instanceParamCount = 0;
};
static std::vector<MapScene> sMapScenes;
static int sMapSelectedScene = 0;

static void SaveMapSceneState(MapScene& sc)
{
    memcpy(sc.sprites, sSprites, sizeof(sSprites));
    sc.spriteCount = sSpriteCount;
    sc.camera = sCamObj;
    sc.camEditorScale = sCamObjEditorScale;
    // Only save visual script state if we're editing the scene script (not a blueprint)
    if (sVsEditSource != VsEditSource::Blueprint) {
        sc.vsNodes = sVsNodes;
        sc.vsLinks = sVsLinks;
        sc.vsAnnotations = sVsAnnotations;
        sc.vsGroupPins = sVsGroupPins;
        sc.vsNextId = sVsNextId;
        sc.vsPanX = sVsPanX;
        sc.vsPanY = sVsPanY;
        sc.vsZoom = sVsZoom;
    }
}

static void LoadMapSceneState(const MapScene& sc)
{
    memcpy(sSprites, sc.sprites, sizeof(sSprites));
    sSpriteCount = sc.spriteCount;
    sSelectedSprite = -1;
    sCamObj = sc.camera;
    sCamObjEditorScale = sc.camEditorScale;
    sVsNodes = sc.vsNodes;
    sVsLinks = sc.vsLinks;
    sVsAnnotations = sc.vsAnnotations;
    sVsGroupPins = sc.vsGroupPins;
    sVsNextId = sc.vsNextId;
    sVsPanX = sc.vsPanX;
    sVsPanY = sc.vsPanY;
    sVsZoom = sc.vsZoom;
    sVsSelected = -1;
}

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

enum class BuildTarget { GBA = 0, NDS = 1 };
static BuildTarget sBuildTarget = BuildTarget::NDS; // default to NDS
static bool sBuildRequested = false; // set by toolbar Build button

// Project file
static std::string sProjectPath;  // empty = no project loaded
static bool sProjectDirty = false; // unsaved changes

// Mesh assets
static std::vector<MeshAsset> sMeshAssets;
static int sSelectedMesh = -1;
static std::set<int> sSelectedMeshes;       // multi-select set for mesh assets
static bool sMeshDragSelecting = false;     // true while drag-selecting in mesh list

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
    struct V2 { float u, v; };
    std::vector<V3> positions;
    std::vector<V3> normals;
    std::vector<V2> texcoords;
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idxs;
    std::vector<uint32_t> quadIdxs;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            V3 v;
            if (sscanf(line + 2, "%f %f %f", &v.x, &v.y, &v.z) == 3)
                positions.push_back(v);
        }
        else if (line[0] == 'v' && line[1] == 't')
        {
            V2 tc;
            if (sscanf(line + 3, "%f %f", &tc.u, &tc.v) >= 2)
                texcoords.push_back(tc);
        }
        else if (line[0] == 'v' && line[1] == 'n')
        {
            V3 n;
            if (sscanf(line + 3, "%f %f %f", &n.x, &n.y, &n.z) == 3)
                normals.push_back(n);
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            // Parse face — supports v, v/vt, v/vt/vn, v//vn (up to 16-gons)
            int vi[16] = {}, ti[16] = {}, ni[16] = {};
            int count = 0;
            char* p = line + 2;
            while (*p && count < 16)
            {
                int v = 0, vt = 0, vn = 0;
                int nread = 0;
                if (sscanf(p, "%d/%d/%d%n", &v, &vt, &vn, &nread) >= 3 && nread > 0) {}
                else if (sscanf(p, "%d//%d%n", &v, &vn, &nread) >= 2 && nread > 0) {}
                else if (sscanf(p, "%d/%d%n", &v, &vt, &nread) >= 2 && nread > 0) {}
                else if (sscanf(p, "%d%n", &v, &nread) >= 1 && nread > 0) {}
                else break;
                vi[count] = v;
                ti[count] = vt;
                ni[count] = vn;
                count++;
                p += nread;
                while (*p == ' ' || *p == '\t') p++;
            }

            // Build face vertices
            uint32_t faceBaseIdx = (uint32_t)verts.size();
            for (int fi = 0; fi < count; fi++)
            {
                int pi = vi[fi] - 1;
                int tci = ti[fi] - 1;
                int nni = ni[fi] - 1;
                MeshVertex mv = {};
                if (pi >= 0 && pi < (int)positions.size())
                { mv.px = positions[pi].x; mv.py = positions[pi].y; mv.pz = positions[pi].z; }
                if (tci >= 0 && tci < (int)texcoords.size())
                { mv.u = texcoords[tci].u; mv.v = texcoords[tci].v; }
                if (nni >= 0 && nni < (int)normals.size())
                { mv.nx = normals[nni].x; mv.ny = normals[nni].y; mv.nz = normals[nni].z; }
                mv.r = mv.g = mv.b = 1.0f;
                mv.objPosIdx = pi;
                verts.push_back(mv);
            }
            // Respect OBJ topology: quads stay quads, tris stay tris
            if (count == 4)
            {
                quadIdxs.push_back(faceBaseIdx);
                quadIdxs.push_back(faceBaseIdx + 1);
                quadIdxs.push_back(faceBaseIdx + 2);
                quadIdxs.push_back(faceBaseIdx + 3);
            }
            else if (count == 3)
            {
                idxs.push_back(faceBaseIdx);
                idxs.push_back(faceBaseIdx + 1);
                idxs.push_back(faceBaseIdx + 2);
            }
            else
            {
                // N-gons: fan-triangulate
                for (int t = 1; t + 1 < count; t++)
                {
                    idxs.push_back(faceBaseIdx);
                    idxs.push_back(faceBaseIdx + t);
                    idxs.push_back(faceBaseIdx + t + 1);
                }
            }
        }
    }
    fclose(f);

    if (verts.empty()) return false;

    out.vertices = std::move(verts);
    out.indices = std::move(idxs);
    out.quadIndices = std::move(quadIdxs);
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

// Load and quantize a texture PNG for a mesh (max 64x64, 16 colors)
static bool LoadMeshTexture(const std::string& path, MeshAsset& mesh)
{
    int w, h, ch;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!img) return false;

    // Resize to nearest power-of-2, max 256
    int tw = 1, th = 1;
    while (tw < w && tw < 256) tw <<= 1;
    while (th < h && th < 256) th <<= 1;
    if (tw > 256) tw = 256;
    if (th > 256) th = 256;

    // Simple nearest-neighbor resize
    std::vector<uint32_t> resized(tw * th);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
        {
            int sx = x * w / tw;
            int sy = y * h / th;
            unsigned char* p = img + (sy * w + sx) * 4;
            resized[y * tw + x] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        }
    stbi_image_free(img);

    // Median-cut quantize to 16 colors
    // Simple approach: collect unique colors, pick 16 most common
    struct ColorCount { uint32_t rgb; int count; };
    std::vector<ColorCount> hist;
    for (auto px : resized)
    {
        uint32_t rgb = px & 0x00FFFFFF;
        bool found = false;
        for (auto& hc : hist)
            if (hc.rgb == rgb) { hc.count++; found = true; break; }
        if (!found)
            hist.push_back({ rgb, 1 });
    }
    std::sort(hist.begin(), hist.end(), [](const ColorCount& a, const ColorCount& b) { return a.count > b.count; });

    // Build 16-color palette
    int palCount = (int)hist.size();
    if (palCount > 16) palCount = 16;
    uint32_t pal[16] = {};
    for (int i = 0; i < palCount; i++)
        pal[i] = hist[i].rgb | 0xFF000000;

    // Map each pixel to nearest palette entry
    std::vector<uint8_t> indexed(tw * th);
    for (int i = 0; i < tw * th; i++)
    {
        uint32_t px = resized[i] & 0x00FFFFFF;
        int bestIdx = 0, bestDist = 0x7FFFFFFF;
        for (int c = 0; c < palCount; c++)
        {
            int dr = (int)(px & 0xFF) - (int)(pal[c] & 0xFF);
            int dg = (int)((px >> 8) & 0xFF) - (int)((pal[c] >> 8) & 0xFF);
            int db = (int)((px >> 16) & 0xFF) - (int)((pal[c] >> 16) & 0xFF);
            int dist = dr * dr + dg * dg + db * db;
            if (dist < bestDist) { bestDist = dist; bestIdx = c; }
        }
        indexed[i] = (uint8_t)bestIdx;
    }

    mesh.texturePath = path;
    mesh.texW = tw;
    mesh.texH = th;
    mesh.texturePixels = std::move(indexed);
    memcpy(mesh.texturePalette, pal, sizeof(pal));
    mesh.textured = true;

    // Upload GL texture for editor preview (reconstruct RGBA from indexed + palette)
    std::vector<uint32_t> rgba(tw * th);
    for (int i = 0; i < tw * th; i++)
        rgba[i] = mesh.texturePalette[mesh.texturePixels[i]];
    if (mesh.glTexID) glDeleteTextures(1, &mesh.glTexID);
    glGenTextures(1, &mesh.glTexID);
    glBindTexture(GL_TEXTURE_2D, mesh.glTexID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

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
    fprintf(f, "jump_force=%.3f\n", sCamObj.jumpForce);
    fprintf(f, "gravity=%.4f\n", sCamObj.gravity);
    fprintf(f, "max_fall_speed=%.2f\n", sCamObj.maxFallSpeed);
    fprintf(f, "jump_cam_land=%.1f\n", sCamObj.jumpCamLand);
    fprintf(f, "jump_cam_air=%.1f\n", sCamObj.jumpCamAir);
    fprintf(f, "auto_orbit_speed=%.1f\n", sCamObj.autoOrbitSpeed);
    fprintf(f, "jump_dampen=%.2f\n", sCamObj.jumpDampen);
    fprintf(f, "draw_distance=%.1f\n", sCamObj.drawDistance);
    fprintf(f, "small_tri_cull=%d\n", sCamObj.smallTriCull);
    fprintf(f, "skip_floor=%d\n", sCamObj.skipFloor ? 1 : 0);
    fprintf(f, "coverage_buf=%d\n", sCamObj.coverageBuf ? 1 : 0);
    fprintf(f, "icon_scale=%.6f\n", sCamObjEditorScale);
    fprintf(f, "build_target=%d\n\n", (int)sBuildTarget);

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
        fprintf(f, "sprite=%d,%.6f,%.6f,%.6f,%.6f,%u,%d,%d,%d,%.6f,%d,%d,%d,%d",
                sp.spriteId, sp.x, sp.y, sp.z, sp.scale, sp.color,
                sp.assetIdx, sp.animIdx, (int)sp.type, sp.rotation, sp.animEnabled ? 1 : 0,
                sp.meshIdx, sp.blueprintIdx, sp.instanceParamCount);
        for (int ip = 0; ip < sp.instanceParamCount; ip++)
            fprintf(f, "|%d:%d", sp.instanceParams[ip].paramIdx, sp.instanceParams[ip].value);
        fprintf(f, "\n");
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
        if (!sa.sourceImagePath.empty())
            fprintf(f, "srcImg=%s\n", sa.sourceImagePath.c_str());
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
            fprintf(f, "anim=%s,%d,%d,%d,%d,%.2f,%d,%d\n",
                    an.name.c_str(), an.startFrame, an.endFrame, an.fps, an.loop ? 1 : 0, an.speed, (int)an.gameState, an.stepAnim ? 1 : 0);
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
        fprintf(f, "mesh=%s|%s|%d|%d|%d|%d|%d|%d|%d|%s|%d|%.1f|%d|%d|%d\n", ma.name.c_str(), ma.sourcePath.c_str(), (int)ma.cullMode, (int)ma.exportMode, ma.lit ? 1 : 0, ma.halfRes ? 1 : 0, ma.textured ? 1 : 0, ma.wireframe ? 1 : 0, ma.grayscale ? 1 : 0, ma.texturePath.c_str(), ma.useQuads ? 1 : 0, ma.drawDistance, ma.collision ? 1 : 0, ma.drawPriority, ma.visible ? 1 : 0);
    }
    fprintf(f, "\n");

    // Blueprint assets
    // Save current blueprint working set back if editing one
    if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
        BlueprintAsset& bp = sBlueprintAssets[sVsEditBlueprintIdx];
        bp.nodes = sVsNodes; bp.links = sVsLinks; bp.annotations = sVsAnnotations;
        bp.groupPins = sVsGroupPins; bp.nextId = sVsNextId;
        bp.panX = sVsPanX; bp.panY = sVsPanY; bp.zoom = sVsZoom;
    }
    fprintf(f, "[Blueprints]\n");
    fprintf(f, "bpCount=%d\n", (int)sBlueprintAssets.size());
    for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++)
    {
        const BlueprintAsset& bp = sBlueprintAssets[bi];
        fprintf(f, "bp_begin=%s\n", bp.name);
        fprintf(f, "bpParamCount=%d\n", bp.paramCount);
        for (int pi = 0; pi < bp.paramCount; pi++) {
            const BpParam& p = bp.params[pi];
            fprintf(f, "bpParam=%d|%s|%d|%d|%d|%d\n", pi, p.name, p.sourceNodeId, p.sourceParamIdx, p.dataType, p.defaultInt);
        }
        fprintf(f, "bpVsNextId=%d\n", bp.nextId);
        fprintf(f, "bpVsPan=%.1f,%.1f\n", bp.panX, bp.panY);
        fprintf(f, "bpVsZoom=%.3f\n", bp.zoom);
        fprintf(f, "bpVsNodeCount=%d\n", (int)bp.nodes.size());
        for (auto& n : bp.nodes) {
            fprintf(f, "bpVsNode=%d,%d,%.1f,%.1f,%d,%d,%d,%d,%d\n",
                n.id, (int)n.type, n.x, n.y,
                n.paramInt[0], n.paramInt[1], n.paramInt[2], n.paramInt[3], n.groupId);
            if (n.type == VsNodeType::Group)
                fprintf(f, "bpVsGroupDef=%d|%s|%d,%d,%d,%d\n", n.id, n.groupLabel,
                    n.grpInExec, n.grpOutExec, n.grpInData, n.grpOutData);
            if (n.customCode[0])
                fprintf(f, "bpVsNodeCode=%d|%s\n", n.id, n.customCode);
            if (n.funcName[0])
                fprintf(f, "bpVsNodeFunc=%d|%s\n", n.id, n.funcName);
        }
        fprintf(f, "bpVsLinkCount=%d\n", (int)bp.links.size());
        for (auto& lk : bp.links)
            fprintf(f, "bpVsLink=%d,%d,%d|%d,%d,%d\n",
                lk.from.nodeId, lk.from.pinType, lk.from.pinIdx,
                lk.to.nodeId, lk.to.pinType, lk.to.pinIdx);
        fprintf(f, "bpVsAnnotCount=%d\n", (int)bp.annotations.size());
        for (auto& ann : bp.annotations)
            fprintf(f, "bpVsAnnot=%.1f,%.1f,%.1f,%.1f|%s\n", ann.x, ann.y, ann.w, ann.h, ann.label);
        fprintf(f, "bpVsGroupPinCount=%d\n", (int)bp.groupPins.size());
        for (auto& m : bp.groupPins)
            fprintf(f, "bpVsGroupPin=%d,%d,%d,%d,%d,%d\n",
                m.groupNodeId, m.pinType, m.pinIdx, m.innerNodeId, m.innerPinType, m.innerPinIdx);
        fprintf(f, "bp_end\n");
    }
    fprintf(f, "\n");

    // Palette
    fprintf(f, "[Palette]\n");
    for (int i = 0; i < 16; i++)
        fprintf(f, "color=%d,%u\n", i, sPalette[i]);
    fprintf(f, "\n");

    // Tilemap tab
    fprintf(f, "[Tilemap]\n");
    fprintf(f, "width=%d\n", sTilemapData.floor.width);
    fprintf(f, "height=%d\n", sTilemapData.floor.height);
    fprintf(f, "subdivided=2\n");
    fprintf(f, "zoom=%.4f\n", sTmMapZoom);
    fprintf(f, "panX=%.2f\n", sTmMapPanX);
    fprintf(f, "panY=%.2f\n", sTmMapPanY);
    fprintf(f, "panelW=%.1f\n", sTmObjPanelW);
    fprintf(f, "scenePanelW=%.1f\n", sTmScenePanelW);
    // Tile indices
    fprintf(f, "tiles=");
    for (int i = 0; i < (int)sTilemapData.floor.tileIndices.size(); i++)
    {
        if (i > 0) fprintf(f, ",");
        fprintf(f, "%d", sTilemapData.floor.tileIndices[i]);
    }
    fprintf(f, "\n");
    // Objects
    fprintf(f, "objCount=%d\n", (int)sTmObjects.size());
    for (int i = 0; i < (int)sTmObjects.size(); i++)
    {
        const TmObject& obj = sTmObjects[i];
        fprintf(f, "obj=%d,%d,%d,%d,%d,%s\n",
            (int)obj.type, obj.tileX, obj.tileY, obj.spriteAssetIdx, obj.teleportScene, obj.name);
        if (obj.displayScale != 1.0f)
            fprintf(f, "objScale=%.2f\n", obj.displayScale);
        if (obj.blueprintIdx >= 0) {
            fprintf(f, "objBp=%d,%d", obj.blueprintIdx, obj.instanceParamCount);
            for (int ip = 0; ip < obj.instanceParamCount; ip++)
                fprintf(f, "|%d:%d", obj.instanceParams[ip].paramIdx, obj.instanceParams[ip].value);
            fprintf(f, "\n");
        }
        // Save cells for Tile objects
        if (obj.type == TmObjType::Tile && !obj.cells.empty())
        {
            fprintf(f, "cells=");
            for (int c = 0; c < (int)obj.cells.size(); c++)
            {
                if (c > 0) fprintf(f, ",");
                fprintf(f, "%d:%d", obj.cells[c].first, obj.cells[c].second);
            }
            fprintf(f, "\n");
        }
    }

    // Scenes — save current scene state first
    if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size())
        SaveSceneState(sTmScenes[sTmSelectedScene]);
    fprintf(f, "sceneCount=%d\n", (int)sTmScenes.size());
    fprintf(f, "selectedScene=%d\n", sTmSelectedScene);
    for (int i = 0; i < (int)sTmScenes.size(); i++)
    {
        const TmScene& sc = sTmScenes[i];
        fprintf(f, "scene=%d,%d,%s\n", sc.mapW, sc.mapH, sc.name);
        // Per-scene tile data
        fprintf(f, "scenetiles=");
        for (int t = 0; t < (int)sc.tileIndices.size(); t++)
        {
            if (t > 0) fprintf(f, ",");
            fprintf(f, "%d", sc.tileIndices[t]);
        }
        fprintf(f, "\n");
        // Per-scene objects
        fprintf(f, "sceneobjcount=%d\n", (int)sc.objects.size());
        for (int oi = 0; oi < (int)sc.objects.size(); oi++)
        {
            const TmObject& obj = sc.objects[oi];
            fprintf(f, "sceneobj=%d,%d,%d,%d,%d,%s\n",
                (int)obj.type, obj.tileX, obj.tileY, obj.spriteAssetIdx, obj.teleportScene, obj.name);
            if (obj.displayScale != 1.0f)
                fprintf(f, "sceneobjScale=%.2f\n", obj.displayScale);
            if (obj.blueprintIdx >= 0) {
                fprintf(f, "sceneobjBp=%d,%d", obj.blueprintIdx, obj.instanceParamCount);
                for (int ip = 0; ip < obj.instanceParamCount; ip++)
                    fprintf(f, "|%d:%d", obj.instanceParams[ip].paramIdx, obj.instanceParams[ip].value);
                fprintf(f, "\n");
            }
            if (obj.type == TmObjType::Tile && !obj.cells.empty())
            {
                fprintf(f, "sceneobjcells=");
                for (int c = 0; c < (int)obj.cells.size(); c++)
                {
                    if (c > 0) fprintf(f, ",");
                    fprintf(f, "%d:%d", obj.cells[c].first, obj.cells[c].second);
                }
                fprintf(f, "\n");
            }
        }
        // Scene-level blueprint
        if (sc.blueprintIdx >= 0) {
            fprintf(f, "tmSceneBp=%d,%d", sc.blueprintIdx, sc.instanceParamCount);
            for (int ip = 0; ip < sc.instanceParamCount; ip++)
                fprintf(f, "|%d:%d", sc.instanceParams[ip].paramIdx, sc.instanceParams[ip].value);
            fprintf(f, "\n");
        }
    }

    // Visual Script Nodes — if editing a blueprint, save scene nodes from MapScene stash
    fprintf(f, "\n[VisualScript]\n");
    const std::vector<VsNode>* saveNodes = &sVsNodes;
    const std::vector<VsLink>* saveLinks = &sVsLinks;
    const std::vector<VsAnnotation>* saveAnnotations = &sVsAnnotations;
    const std::vector<VsGroupPinMap>* saveGroupPins = &sVsGroupPins;
    int saveNextId = sVsNextId;
    float savePanX = sVsPanX, savePanY = sVsPanY, saveZoom = sVsZoom;
    if (sVsEditSource == VsEditSource::Blueprint && sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size()) {
        const MapScene& ms = sMapScenes[sMapSelectedScene];
        saveNodes = &ms.vsNodes;
        saveLinks = &ms.vsLinks;
        saveAnnotations = &ms.vsAnnotations;
        saveGroupPins = &ms.vsGroupPins;
        saveNextId = ms.vsNextId;
        savePanX = ms.vsPanX; savePanY = ms.vsPanY; saveZoom = ms.vsZoom;
    }
    fprintf(f, "vsNextId=%d\n", saveNextId);
    fprintf(f, "vsPan=%.1f,%.1f\n", savePanX, savePanY);
    fprintf(f, "vsZoom=%.3f\n", saveZoom);
    fprintf(f, "vsNodeCount=%d\n", (int)saveNodes->size());
    for (int i = 0; i < (int)saveNodes->size(); i++)
    {
        const VsNode& n = (*saveNodes)[i];
        fprintf(f, "vsNode=%d,%d,%.1f,%.1f,%d,%d,%d,%d,%d\n",
            n.id, (int)n.type, n.x, n.y,
            n.paramInt[0], n.paramInt[1], n.paramInt[2], n.paramInt[3], n.groupId);
        if (n.type == VsNodeType::Group)
            fprintf(f, "vsGroupDef=%d|%s|%d,%d,%d,%d\n", n.id, n.groupLabel,
                n.grpInExec, n.grpOutExec, n.grpInData, n.grpOutData);
        if (n.customCode[0])
            fprintf(f, "vsNodeCode=%d|%s\n", n.id, n.customCode);
        if (n.funcName[0])
            fprintf(f, "vsNodeFunc=%d|%s\n", n.id, n.funcName);
    }
    fprintf(f, "vsLinkCount=%d\n", (int)saveLinks->size());
    for (int i = 0; i < (int)saveLinks->size(); i++)
    {
        const VsLink& lk = (*saveLinks)[i];
        fprintf(f, "vsLink=%d,%d,%d|%d,%d,%d\n",
            lk.from.nodeId, lk.from.pinType, lk.from.pinIdx,
            lk.to.nodeId, lk.to.pinType, lk.to.pinIdx);
    }
    fprintf(f, "vsAnnotCount=%d\n", (int)saveAnnotations->size());
    for (int i = 0; i < (int)saveAnnotations->size(); i++)
    {
        const VsAnnotation& ann = (*saveAnnotations)[i];
        fprintf(f, "vsAnnot=%.1f,%.1f,%.1f,%.1f|%s\n", ann.x, ann.y, ann.w, ann.h, ann.label);
    }
    fprintf(f, "vsGroupPinCount=%d\n", (int)saveGroupPins->size());
    for (int i = 0; i < (int)saveGroupPins->size(); i++) {
        const auto& m = (*saveGroupPins)[i];
        fprintf(f, "vsGroupPin=%d,%d,%d,%d,%d,%d\n",
            m.groupNodeId, m.pinType, m.pinIdx, m.innerNodeId, m.innerPinType, m.innerPinIdx);
    }

    // ---- Map Scenes (3D Scene tab) ----
    // Save current state into active scene before writing
    if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
        SaveMapSceneState(sMapScenes[sMapSelectedScene]);
    fprintf(f, "\n[MapScenes]\n");
    fprintf(f, "mapSceneCount=%d\n", (int)sMapScenes.size());
    fprintf(f, "mapSelectedScene=%d\n", sMapSelectedScene);
    for (int si = 0; si < (int)sMapScenes.size(); si++)
    {
        const MapScene& ms = sMapScenes[si];
        fprintf(f, "mapScene=%s\n", ms.name);
        fprintf(f, "msSpriteCount=%d\n", ms.spriteCount);
        for (int i = 0; i < ms.spriteCount; i++)
        {
            const FloorSprite& sp = ms.sprites[i];
            fprintf(f, "msSprite=%d,%.6f,%.6f,%.6f,%.6f,%u,%d,%d,%d,%.6f,%d,%d,%d,%d",
                    sp.spriteId, sp.x, sp.y, sp.z, sp.scale, sp.color,
                    sp.assetIdx, sp.animIdx, (int)sp.type, sp.rotation, sp.animEnabled ? 1 : 0, sp.meshIdx,
                    sp.blueprintIdx, sp.instanceParamCount);
            for (int ip = 0; ip < sp.instanceParamCount; ip++)
                fprintf(f, "|%d:%d", sp.instanceParams[ip].paramIdx, sp.instanceParams[ip].value);
            fprintf(f, "\n");
        }
        // Camera
        fprintf(f, "msCam=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%.6f\n",
                ms.camera.x, ms.camera.z, ms.camera.height, ms.camera.angle, ms.camera.horizon,
                ms.camera.walkSpeed, ms.camera.sprintSpeed,
                ms.camera.walkEaseIn, ms.camera.walkEaseOut, ms.camera.sprintEaseIn, ms.camera.sprintEaseOut,
                ms.camera.jumpForce, ms.camera.gravity, ms.camera.maxFallSpeed,
                ms.camera.jumpCamLand, ms.camera.jumpCamAir, ms.camera.autoOrbitSpeed, ms.camera.jumpDampen,
                ms.camera.smallTriCull, ms.camera.skipFloor ? 1 : 0, ms.camera.coverageBuf ? 1 : 0,
                ms.camera.drawDistance);
        // Scene-level blueprint
        if (ms.blueprintIdx >= 0) {
            fprintf(f, "msSceneBp=%d,%d", ms.blueprintIdx, ms.instanceParamCount);
            for (int ip = 0; ip < ms.instanceParamCount; ip++)
                fprintf(f, "|%d:%d", ms.instanceParams[ip].paramIdx, ms.instanceParams[ip].value);
            fprintf(f, "\n");
        }
        // Visual script
        fprintf(f, "msVsNextId=%d\n", ms.vsNextId);
        fprintf(f, "msVsPan=%.1f,%.1f\n", ms.vsPanX, ms.vsPanY);
        fprintf(f, "msVsZoom=%.3f\n", ms.vsZoom);
        fprintf(f, "msVsNodeCount=%d\n", (int)ms.vsNodes.size());
        for (auto& n : ms.vsNodes)
        {
            fprintf(f, "msVsNode=%d,%d,%.1f,%.1f,%d,%d,%d,%d,%d\n",
                n.id, (int)n.type, n.x, n.y,
                n.paramInt[0], n.paramInt[1], n.paramInt[2], n.paramInt[3], n.groupId);
            if (n.type == VsNodeType::Group)
                fprintf(f, "msVsGroupDef=%d|%s|%d,%d,%d,%d\n", n.id, n.groupLabel,
                    n.grpInExec, n.grpOutExec, n.grpInData, n.grpOutData);
            if (n.customCode[0])
                fprintf(f, "msVsNodeCode=%d|%s\n", n.id, n.customCode);
        }
        fprintf(f, "msVsLinkCount=%d\n", (int)ms.vsLinks.size());
        for (auto& lk : ms.vsLinks)
            fprintf(f, "msVsLink=%d,%d,%d|%d,%d,%d\n",
                lk.from.nodeId, lk.from.pinType, lk.from.pinIdx,
                lk.to.nodeId, lk.to.pinType, lk.to.pinIdx);
        fprintf(f, "msVsAnnotCount=%d\n", (int)ms.vsAnnotations.size());
        for (auto& ann : ms.vsAnnotations)
            fprintf(f, "msVsAnnot=%.1f,%.1f,%.1f,%.1f|%s\n", ann.x, ann.y, ann.w, ann.h, ann.label);
        fprintf(f, "msVsGroupPinCount=%d\n", (int)ms.vsGroupPins.size());
        for (auto& m : ms.vsGroupPins)
            fprintf(f, "msVsGroupPin=%d,%d,%d,%d,%d,%d\n",
                m.groupNodeId, m.pinType, m.pinIdx, m.innerNodeId, m.innerPinType, m.innerPinIdx);
    }

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
    sVsUndoStack.clear();
    sMapScenes.clear();
    sMapSelectedScene = 0;
    sTmObjects.clear();
    sTmScenes.clear();
    sTmSelectedScene = 0;
    sTmSelectedObj = -1;
    sTmDragObj = -1;
    sTmStampAsset = -1;
    sTmStampObj = -1;
    sTilemapDataInit = false;
    sTilemapData.floor.width = 2;
    sTilemapData.floor.height = 2;
    sTilemapData.floor.tileIndices.assign(4, 0);
    sSpriteCount = 0;
    sSelectedSprite = -1;
    sSelectedObjType = SelectedObjType::None;
    sEditorMode = EditorMode::Edit;
    sSpriteAssets.clear();
    for (int i = 0; i < (int)sAssetDirSprites.size(); i++) FreeAssetDirSprites(i);
    sAssetDirSprites.clear();
    sSelectedAsset = -1;
    sMeshAssets.clear();
    sSelectedMesh = -1;
    sSelectedMeshes.clear();
    sBlueprintAssets.clear();
    sSelectedBlueprint = -1;
    sVsEditSource = VsEditSource::Scene;
    sVsEditBlueprintIdx = -1;
    sCamObj = { 0.0f, 0.0f, 14.0f, 0.0f, 60.0f };
    sCamObjEditorScale = 0.05f;

    char line[32768]; // large buffer for frame pixel data lines (64x64 = ~16KB worst case)
    char section[64] = {};
    int tmSubdivLevel = 0; // 0=old, 1=first 2x, 2=current 4x

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

        float fval, fval2;
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
            else if (sscanf(line, "jump_force=%f", &fval) == 1) sCamObj.jumpForce = fval;
            else if (sscanf(line, "gravity=%f", &fval) == 1) sCamObj.gravity = fval;
            else if (sscanf(line, "max_fall_speed=%f", &fval) == 1) sCamObj.maxFallSpeed = fval;
            else if (sscanf(line, "jump_cam_land=%f", &fval) == 1) sCamObj.jumpCamLand = fval;
            else if (sscanf(line, "jump_cam_air=%f", &fval) == 1) sCamObj.jumpCamAir = fval;
            else if (sscanf(line, "auto_orbit_speed=%f", &fval) == 1) sCamObj.autoOrbitSpeed = fval;
            else if (sscanf(line, "jump_dampen=%f", &fval) == 1) sCamObj.jumpDampen = fval;
            else if (sscanf(line, "draw_distance=%f", &fval) == 1) sCamObj.drawDistance = fval;
            else if (sscanf(line, "small_tri_cull=%d", &ival) == 1) sCamObj.smallTriCull = ival;
            else if (sscanf(line, "skip_floor=%d", &ival) == 1) sCamObj.skipFloor = (ival != 0);
            else if (sscanf(line, "coverage_buf=%d", &ival) == 1) sCamObj.coverageBuf = (ival != 0);
            else if (sscanf(line, "icon_scale=%f", &fval) == 1) sCamObjEditorScale = fval;
            else if (sscanf(line, "build_target=%d", &ival) == 1) sBuildTarget = (BuildTarget)ival;
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
                int bpIdx = -1, bpParamCnt = 0;
                int matched = sscanf(line, "sprite=%d,%f,%f,%f,%f,%u,%d,%d,%d,%f,%d,%d,%d,%d", &sid, &sx, &sy, &sz, &sc, &col, &aIdx, &anIdx, &typeVal, &rot, &animEn, &mIdx, &bpIdx, &bpParamCnt);
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
                    sp.blueprintIdx = (matched >= 13) ? bpIdx : -1;
                    sp.instanceParamCount = (matched >= 14) ? std::min(bpParamCnt, 8) : 0;
                    // Parse instance params from pipe-delimited suffix
                    if (sp.instanceParamCount > 0) {
                        const char* p = line;
                        for (int ip = 0; ip < sp.instanceParamCount; ip++) {
                            p = strchr(p, '|'); if (!p) break; p++;
                            int pIdx, pVal;
                            if (sscanf(p, "%d:%d", &pIdx, &pVal) == 2) {
                                sp.instanceParams[ip].paramIdx = pIdx;
                                sp.instanceParams[ip].value = pVal;
                            }
                        }
                    }
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
                    else if (strncmp(line, "srcImg=", 7) == 0) sa.sourceImagePath = line + 7;
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
                        int astep = 0;
                        int nread = sscanf(line + 5, "%63[^,],%d,%d,%d,%d,%f,%d,%d", aname, &sf, &ef, &afps, &aloop, &aspeed, &agstate, &astep);
                        if (nread >= 5)
                        {
                            an.name = aname;
                            an.startFrame = sf;
                            an.endFrame = ef;
                            an.fps = afps;
                            an.loop = (aloop != 0);
                            if (nread >= 6) an.speed = aspeed;
                            if (nread >= 7) an.gameState = (AnimState)agstate;
                            if (nread >= 8) an.stepAnim = (astep != 0);
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
            char mname[256], mpath[512], mtexpath[512] = {};
            int mcull = 0, mexport = 0, mlit = 1, mhalfres = 0, mtextured = 0, mwireframe = 0, mgrayscale = 0, musequads = 1, mcollision = 1, mdrawpri = 0, mvisible = 1;
            float mdrawdist = 0.0f;
            // Try newest format: name|path|cull|export|lit|halfres|textured|wireframe|grayscale|texpath|usequads|drawdist|collision|drawpriority
            int matched = sscanf(line, "mesh=%255[^|]|%511[^|]|%d|%d|%d|%d|%d|%d|%d|%511[^|\n]|%d|%f|%d|%d|%d", mname, mpath, &mcull, &mexport, &mlit, &mhalfres, &mtextured, &mwireframe, &mgrayscale, mtexpath, &musequads, &mdrawdist, &mcollision, &mdrawpri, &mvisible);
            if (matched < 2)
            {
                // Try format without usequads: name|path|cull|export|lit|halfres|textured|wireframe|grayscale|texpath
                matched = sscanf(line, "mesh=%255[^|]|%511[^|]|%d|%d|%d|%d|%d|%d|%d|%511[^\n]", mname, mpath, &mcull, &mexport, &mlit, &mhalfres, &mtextured, &mwireframe, &mgrayscale, mtexpath);
            }
            if (matched < 2)
            {
                // Try previous format: name|path|cull|export|lit|halfres|textured|wireframe|texpath
                matched = sscanf(line, "mesh=%255[^|]|%511[^|]|%d|%d|%d|%d|%d|%d|%511[^\n]", mname, mpath, &mcull, &mexport, &mlit, &mhalfres, &mtextured, &mwireframe, mtexpath);
            }
            if (matched < 2)
            {
                // Try old format: name|path|cull|export|lit|halfres|textured|texpath|wireframe
                matched = sscanf(line, "mesh=%255[^|]|%511[^|]|%d|%d|%d|%d|%d|%511[^|\n]|%d", mname, mpath, &mcull, &mexport, &mlit, &mhalfres, &mtextured, mtexpath, &mwireframe);
            }
            if (matched >= 2)
            {
                MeshAsset ma;
                ma.name = mname;
                ma.sourcePath = mpath;
                if (matched >= 3 && mcull >= 0 && mcull <= 2)
                    ma.cullMode = (CullMode)mcull;
                if (matched >= 4 && mexport >= 0 && mexport <= 2)
                    ma.exportMode = (MeshExportMode)mexport;
                if (matched >= 5)
                    ma.lit = (mlit != 0);
                if (matched >= 6)
                    ma.halfRes = (mhalfres != 0);
                if (matched >= 7)
                    ma.textured = (mtextured != 0);
                if (matched >= 8)
                    ma.wireframe = (mwireframe != 0);
                if (matched >= 9)
                    ma.grayscale = (mgrayscale != 0);
                if (matched >= 11)
                    ma.useQuads = (musequads != 0);
                if (matched >= 12)
                    ma.drawDistance = mdrawdist;
                if (matched >= 13)
                    ma.collision = (mcollision != 0);
                if (matched >= 14)
                    ma.drawPriority = mdrawpri;
                if (matched >= 15)
                    ma.visible = (mvisible != 0);
                // Reload from source OBJ
                if (!ma.sourcePath.empty())
                    LoadOBJ(ma.sourcePath, ma);
                ma.name = mname; // restore name in case LoadOBJ overwrote it
                // Reload texture if textured
                if (ma.textured && mtexpath[0])
                    LoadMeshTexture(std::string(mtexpath), ma);
                sMeshAssets.push_back(std::move(ma));
            }
        }
        else if (strcmp(section, "Blueprints") == 0)
        {
            if (sscanf(line, "bpCount=%d", &ival) == 1)
            {
                sBlueprintAssets.clear();
                sBlueprintAssets.reserve(ival);
            }
            else if (strncmp(line, "bp_begin=", 9) == 0)
            {
                BlueprintAsset bp;
                strncpy(bp.name, line + 9, sizeof(bp.name) - 1);
                sBlueprintAssets.push_back(bp);
            }
            else if (sscanf(line, "bpParamCount=%d", &ival) == 1 && !sBlueprintAssets.empty())
                sBlueprintAssets.back().paramCount = ival;
            else if (strncmp(line, "bpParam=", 8) == 0 && !sBlueprintAssets.empty())
            {
                int pi, srcNode, srcParam, dtype, defVal;
                char pname[32] = {};
                if (sscanf(line + 8, "%d|%31[^|]|%d|%d|%d|%d", &pi, pname, &srcNode, &srcParam, &dtype, &defVal) >= 6
                    && pi >= 0 && pi < 8) {
                    BpParam& p = sBlueprintAssets.back().params[pi];
                    strncpy(p.name, pname, sizeof(p.name) - 1);
                    p.sourceNodeId = srcNode; p.sourceParamIdx = srcParam;
                    p.dataType = dtype; p.defaultInt = defVal;
                }
            }
            else if (sscanf(line, "bpVsNextId=%d", &ival) == 1 && !sBlueprintAssets.empty())
                sBlueprintAssets.back().nextId = ival;
            else if (strncmp(line, "bpVsPan=", 8) == 0 && !sBlueprintAssets.empty())
            {
                float px, py;
                if (sscanf(line + 8, "%f,%f", &px, &py) == 2) {
                    sBlueprintAssets.back().panX = px; sBlueprintAssets.back().panY = py;
                }
            }
            else if (sscanf(line, "bpVsZoom=%f", &fval) == 1 && !sBlueprintAssets.empty())
                sBlueprintAssets.back().zoom = fval;
            else if (sscanf(line, "bpVsNodeCount=%d", &ival) == 1 && !sBlueprintAssets.empty())
            {
                sBlueprintAssets.back().nodes.clear();
                sBlueprintAssets.back().nodes.reserve(ival);
            }
            else if (strncmp(line, "bpVsNode=", 9) == 0 && !sBlueprintAssets.empty())
            {
                VsNode n;
                int typeInt, gid = 0;
                if (sscanf(line + 9, "%d,%d,%f,%f,%d,%d,%d,%d,%d",
                    &n.id, &typeInt, &n.x, &n.y,
                    &n.paramInt[0], &n.paramInt[1], &n.paramInt[2], &n.paramInt[3], &gid) >= 4) {
                    n.type = (VsNodeType)typeInt; n.groupId = gid;
                    sBlueprintAssets.back().nodes.push_back(n);
                }
            }
            else if (strncmp(line, "bpVsGroupDef=", 13) == 0 && !sBlueprintAssets.empty())
            {
                int nid, ie, oe, id2, od;
                char lbl[32] = {};
                if (sscanf(line + 13, "%d|%31[^|]|%d,%d,%d,%d", &nid, lbl, &ie, &oe, &id2, &od) >= 6) {
                    for (auto& n : sBlueprintAssets.back().nodes)
                        if (n.id == nid) {
                            strncpy(n.groupLabel, lbl, sizeof(n.groupLabel) - 1);
                            n.grpInExec = ie; n.grpOutExec = oe; n.grpInData = id2; n.grpOutData = od;
                            break;
                        }
                }
            }
            else if (strncmp(line, "bpVsNodeCode=", 13) == 0 && !sBlueprintAssets.empty())
            {
                int nid;
                char codeBuf[512] = {};
                if (sscanf(line + 13, "%d|%511[^\n]", &nid, codeBuf) >= 2) {
                    for (auto& n : sBlueprintAssets.back().nodes)
                        if (n.id == nid) { strncpy(n.customCode, codeBuf, sizeof(n.customCode) - 1); break; }
                }
            }
            else if (strncmp(line, "bpVsNodeFunc=", 13) == 0 && !sBlueprintAssets.empty())
            {
                int nid;
                char nameBuf[64] = {};
                if (sscanf(line + 13, "%d|%63[^\n]", &nid, nameBuf) >= 2) {
                    for (auto& n : sBlueprintAssets.back().nodes)
                        if (n.id == nid) { strncpy(n.funcName, nameBuf, sizeof(n.funcName) - 1); break; }
                }
            }
            else if (sscanf(line, "bpVsLinkCount=%d", &ival) == 1 && !sBlueprintAssets.empty())
            {
                sBlueprintAssets.back().links.clear();
                sBlueprintAssets.back().links.reserve(ival);
            }
            else if (strncmp(line, "bpVsLink=", 9) == 0 && !sBlueprintAssets.empty())
            {
                VsLink lk;
                if (sscanf(line + 9, "%d,%d,%d|%d,%d,%d",
                    &lk.from.nodeId, &lk.from.pinType, &lk.from.pinIdx,
                    &lk.to.nodeId, &lk.to.pinType, &lk.to.pinIdx) == 6)
                    sBlueprintAssets.back().links.push_back(lk);
            }
            else if (sscanf(line, "bpVsAnnotCount=%d", &ival) == 1 && !sBlueprintAssets.empty())
            {
                sBlueprintAssets.back().annotations.clear();
                sBlueprintAssets.back().annotations.reserve(ival);
            }
            else if (strncmp(line, "bpVsAnnot=", 10) == 0 && !sBlueprintAssets.empty())
            {
                VsAnnotation ann;
                float ax, ay, aw, ah;
                if (sscanf(line + 10, "%f,%f,%f,%f|", &ax, &ay, &aw, &ah) == 4) {
                    ann.x = ax; ann.y = ay; ann.w = aw; ann.h = ah;
                    const char* pipe = strchr(line + 10, '|');
                    if (pipe) strncpy(ann.label, pipe + 1, sizeof(ann.label) - 1);
                    sBlueprintAssets.back().annotations.push_back(ann);
                }
            }
            else if (sscanf(line, "bpVsGroupPinCount=%d", &ival) == 1 && !sBlueprintAssets.empty())
            {
                sBlueprintAssets.back().groupPins.clear();
                sBlueprintAssets.back().groupPins.reserve(ival);
            }
            else if (strncmp(line, "bpVsGroupPin=", 13) == 0 && !sBlueprintAssets.empty())
            {
                VsGroupPinMap m;
                if (sscanf(line + 13, "%d,%d,%d,%d,%d,%d",
                    &m.groupNodeId, &m.pinType, &m.pinIdx, &m.innerNodeId, &m.innerPinType, &m.innerPinIdx) == 6)
                    sBlueprintAssets.back().groupPins.push_back(m);
            }
        }
        else if (strcmp(section, "Palette") == 0)
        {
            int idx;
            if (sscanf(line, "color=%d,%u", &idx, &uval) == 2 && idx >= 0 && idx < 16)
                sPalette[idx] = uval;
        }
        else if (strcmp(section, "Tilemap") == 0)
        {
            if (sscanf(line, "width=%d", &ival) == 1) sTilemapData.floor.width = ival;
            else if (sscanf(line, "height=%d", &ival) == 1)
            {
                sTilemapData.floor.height = ival;
                sTilemapData.floor.tileIndices.resize(
                    sTilemapData.floor.width * sTilemapData.floor.height, 0);
            }
            else if (sscanf(line, "subdivided=%d", &ival) == 1) { tmSubdivLevel = ival; }
            else if (sscanf(line, "zoom=%f", &fval) == 1) sTmMapZoom = fval;
            else if (sscanf(line, "panX=%f", &fval) == 1) sTmMapPanX = fval;
            else if (sscanf(line, "panY=%f", &fval) == 1) sTmMapPanY = fval;
            else if (sscanf(line, "panelW=%f", &fval) == 1) sTmObjPanelW = fval;
            else if (sscanf(line, "scenePanelW=%f", &fval) == 1) sTmScenePanelW = fval;
            else if (strncmp(line, "tiles=", 6) == 0)
            {
                const char* p = line + 6;
                int total = sTilemapData.floor.width * sTilemapData.floor.height;
                for (int i = 0; i < total && *p; i++)
                {
                    sTilemapData.floor.tileIndices[i] = (uint16_t)atoi(p);
                    const char* comma = strchr(p, ',');
                    if (comma) p = comma + 1; else break;
                }
            }
            else if (sscanf(line, "objCount=%d", &ival) == 1)
            {
                sTmObjects.clear();
                sTmObjects.reserve(ival);
            }
            else if (strncmp(line, "obj=", 4) == 0)
            {
                TmObject obj;
                int typeInt = 0;
                // Parse: type,tileX,tileY,spriteAssetIdx,teleportScene,name
                char nameBuf[32] = {};
                if (sscanf(line + 4, "%d,%d,%d,%d,%d,%31[^\n]",
                    &typeInt, &obj.tileX, &obj.tileY, &obj.spriteAssetIdx, &obj.teleportScene, nameBuf) >= 4)
                {
                    obj.type = (TmObjType)typeInt;
                    strncpy(obj.name, nameBuf, sizeof(obj.name) - 1);
                    sTmObjects.push_back(obj);
                }
            }
            else if (sscanf(line, "objScale=%f", &fval) == 1 && !sTmObjects.empty())
            {
                sTmObjects.back().displayScale = fval;
            }
            else if (strncmp(line, "objBp=", 6) == 0 && !sTmObjects.empty())
            {
                TmObject& lastObj = sTmObjects.back();
                int bpIdx = -1, bpCnt = 0;
                sscanf(line + 6, "%d,%d", &bpIdx, &bpCnt);
                lastObj.blueprintIdx = bpIdx;
                lastObj.instanceParamCount = std::min(bpCnt, 8);
                const char* p = line + 6;
                for (int ip = 0; ip < lastObj.instanceParamCount; ip++) {
                    p = strchr(p, '|'); if (!p) break; p++;
                    int pIdx, pVal;
                    if (sscanf(p, "%d:%d", &pIdx, &pVal) == 2) {
                        lastObj.instanceParams[ip].paramIdx = pIdx;
                        lastObj.instanceParams[ip].value = pVal;
                    }
                }
            }
            else if (strncmp(line, "cells=", 6) == 0 && !sTmObjects.empty())
            {
                // Parse cells for the last-added Tile object: "x:y,x:y,..."
                TmObject& lastObj = sTmObjects.back();
                const char* p = line + 6;
                while (*p)
                {
                    int cx, cy;
                    if (sscanf(p, "%d:%d", &cx, &cy) == 2)
                        lastObj.cells.push_back({cx, cy});
                    const char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            }
            else if (sscanf(line, "sceneCount=%d", &ival) == 1)
            {
                sTmScenes.clear();
                sTmScenes.reserve(ival);
            }
            else if (sscanf(line, "selectedScene=%d", &ival) == 1)
            {
                sTmSelectedScene = ival;
            }
            else if (strncmp(line, "scene=", 6) == 0)
            {
                TmScene sc;
                char nameBuf[32] = {};
                if (sscanf(line + 6, "%d,%d,%31[^\n]", &sc.mapW, &sc.mapH, nameBuf) >= 2)
                {
                    strncpy(sc.name, nameBuf, sizeof(sc.name) - 1);
                    sTmScenes.push_back(sc);
                }
            }
            else if (strncmp(line, "scenetiles=", 11) == 0 && !sTmScenes.empty())
            {
                TmScene& sc = sTmScenes.back();
                sc.tileIndices.clear();
                const char* p = line + 11;
                while (*p)
                {
                    int val = 0;
                    if (sscanf(p, "%d", &val) == 1)
                        sc.tileIndices.push_back((uint16_t)val);
                    const char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            }
            else if (sscanf(line, "sceneobjcount=%d", &ival) == 1 && !sTmScenes.empty())
            {
                sTmScenes.back().objects.clear();
                sTmScenes.back().objects.reserve(ival);
            }
            else if (strncmp(line, "sceneobj=", 9) == 0 && !sTmScenes.empty())
            {
                TmObject obj;
                int typeInt = 0;
                char nameBuf[32] = {};
                if (sscanf(line + 9, "%d,%d,%d,%d,%d,%31[^\n]",
                    &typeInt, &obj.tileX, &obj.tileY, &obj.spriteAssetIdx, &obj.teleportScene, nameBuf) >= 4)
                {
                    obj.type = (TmObjType)typeInt;
                    strncpy(obj.name, nameBuf, sizeof(obj.name) - 1);
                    sTmScenes.back().objects.push_back(obj);
                }
            }
            else if (sscanf(line, "sceneobjScale=%f", &fval) == 1 && !sTmScenes.empty() && !sTmScenes.back().objects.empty())
            {
                sTmScenes.back().objects.back().displayScale = fval;
            }
            else if (strncmp(line, "sceneobjBp=", 11) == 0 && !sTmScenes.empty() && !sTmScenes.back().objects.empty())
            {
                TmObject& lastObj = sTmScenes.back().objects.back();
                int bpIdx = -1, bpCnt = 0;
                sscanf(line + 11, "%d,%d", &bpIdx, &bpCnt);
                lastObj.blueprintIdx = bpIdx;
                lastObj.instanceParamCount = std::min(bpCnt, 8);
                const char* p = line + 11;
                for (int ip = 0; ip < lastObj.instanceParamCount; ip++) {
                    p = strchr(p, '|'); if (!p) break; p++;
                    int pIdx, pVal;
                    if (sscanf(p, "%d:%d", &pIdx, &pVal) == 2) {
                        lastObj.instanceParams[ip].paramIdx = pIdx;
                        lastObj.instanceParams[ip].value = pVal;
                    }
                }
            }
            else if (strncmp(line, "sceneobjcells=", 14) == 0 && !sTmScenes.empty() && !sTmScenes.back().objects.empty())
            {
                TmObject& lastObj = sTmScenes.back().objects.back();
                const char* p = line + 14;
                while (*p)
                {
                    int cx, cy;
                    if (sscanf(p, "%d:%d", &cx, &cy) == 2)
                        lastObj.cells.push_back({cx, cy});
                    const char* comma = strchr(p, ',');
                    if (!comma) break;
                    p = comma + 1;
                }
            }
            else if (strncmp(line, "tmSceneBp=", 10) == 0 && !sTmScenes.empty())
            {
                TmScene& sc = sTmScenes.back();
                int bpIdx = -1, ipc = 0;
                sscanf(line + 10, "%d,%d", &bpIdx, &ipc);
                sc.blueprintIdx = bpIdx;
                sc.instanceParamCount = 0;
                const char* pipe = strchr(line + 10, '|');
                while (pipe && sc.instanceParamCount < 8) {
                    int pi = 0, pv = 0;
                    if (sscanf(pipe + 1, "%d:%d", &pi, &pv) == 2) {
                        sc.instanceParams[sc.instanceParamCount].paramIdx = pi;
                        sc.instanceParams[sc.instanceParamCount].value = pv;
                        sc.instanceParamCount++;
                    }
                    pipe = strchr(pipe + 1, '|');
                }
            }
        }
        else if (strcmp(section, "VisualScript") == 0)
        {
            if (sscanf(line, "vsNextId=%d", &ival) == 1) sVsNextId = ival;
            else if (sscanf(line, "vsPan=%f,%f", &fval, &fval2) == 2) { sVsPanX = fval; sVsPanY = fval2; }
            else if (sscanf(line, "vsZoom=%f", &fval) == 1) sVsZoom = fval;
            else if (sscanf(line, "vsNodeCount=%d", &ival) == 1) { sVsNodes.clear(); sVsNodes.reserve(ival); }
            else if (strncmp(line, "vsNode=", 7) == 0)
            {
                VsNode n;
                int typeInt, gid = 0;
                int nread = sscanf(line + 7, "%d,%d,%f,%f,%d,%d,%d,%d,%d",
                    &n.id, &typeInt, &n.x, &n.y,
                    &n.paramInt[0], &n.paramInt[1], &n.paramInt[2], &n.paramInt[3], &gid);
                if (nread >= 4)
                {
                    n.type = (VsNodeType)typeInt;
                    n.groupId = gid;
                    sVsNodes.push_back(n);
                }
            }
            else if (strncmp(line, "vsGroupDef=", 11) == 0)
            {
                int nid, ie, oe, id2, od;
                char lbl[32] = {};
                if (sscanf(line + 11, "%d|%31[^|]|%d,%d,%d,%d", &nid, lbl, &ie, &oe, &id2, &od) >= 6) {
                    int gi = VsFindNode(nid);
                    if (gi >= 0) {
                        strncpy(sVsNodes[gi].groupLabel, lbl, sizeof(sVsNodes[gi].groupLabel) - 1);
                        sVsNodes[gi].grpInExec = ie;
                        sVsNodes[gi].grpOutExec = oe;
                        sVsNodes[gi].grpInData = id2;
                        sVsNodes[gi].grpOutData = od;
                    }
                }
            }
            else if (strncmp(line, "vsNodeCode=", 11) == 0)
            {
                int nid;
                char codeBuf[512] = {};
                if (sscanf(line + 11, "%d|%511[^\n]", &nid, codeBuf) >= 2) {
                    int gi = VsFindNode(nid);
                    if (gi >= 0)
                        strncpy(sVsNodes[gi].customCode, codeBuf, sizeof(sVsNodes[gi].customCode) - 1);
                }
            }
            else if (strncmp(line, "vsNodeFunc=", 11) == 0)
            {
                int nid;
                char nameBuf[64] = {};
                if (sscanf(line + 11, "%d|%63[^\n]", &nid, nameBuf) >= 2) {
                    int gi = VsFindNode(nid);
                    if (gi >= 0)
                        strncpy(sVsNodes[gi].funcName, nameBuf, sizeof(sVsNodes[gi].funcName) - 1);
                }
            }
            else if (sscanf(line, "vsLinkCount=%d", &ival) == 1) { sVsLinks.clear(); sVsLinks.reserve(ival); }
            else if (strncmp(line, "vsLink=", 7) == 0)
            {
                VsLink lk;
                if (sscanf(line + 7, "%d,%d,%d|%d,%d,%d",
                    &lk.from.nodeId, &lk.from.pinType, &lk.from.pinIdx,
                    &lk.to.nodeId, &lk.to.pinType, &lk.to.pinIdx) == 6)
                {
                    sVsLinks.push_back(lk);
                }
            }
            else if (sscanf(line, "vsGroupPinCount=%d", &ival) == 1) { sVsGroupPins.clear(); sVsGroupPins.reserve(ival); }
            else if (strncmp(line, "vsGroupPin=", 11) == 0) {
                VsGroupPinMap m;
                if (sscanf(line + 11, "%d,%d,%d,%d,%d,%d",
                    &m.groupNodeId, &m.pinType, &m.pinIdx, &m.innerNodeId, &m.innerPinType, &m.innerPinIdx) == 6)
                    sVsGroupPins.push_back(m);
            }
            else if (sscanf(line, "vsAnnotCount=%d", &ival) == 1) { sVsAnnotations.clear(); sVsAnnotations.reserve(ival); }
            else if (strncmp(line, "vsAnnot=", 8) == 0)
            {
                VsAnnotation ann;
                float ax, ay, aw, ah;
                // Parse "x,y,w,h|label"
                if (sscanf(line + 8, "%f,%f,%f,%f|", &ax, &ay, &aw, &ah) == 4) {
                    ann.x = ax; ann.y = ay; ann.w = aw; ann.h = ah;
                    const char* pipe = strchr(line + 8, '|');
                    if (pipe) strncpy(ann.label, pipe + 1, sizeof(ann.label) - 1);
                    sVsAnnotations.push_back(ann);
                }
            }
        }
        else if (strcmp(section, "MapScenes") == 0)
        {
            // ---- Map Scenes (3D Scene tab) ----
            if (sscanf(line, "mapSceneCount=%d", &ival) == 1) { sMapScenes.clear(); sMapScenes.reserve(ival); }
            else if (sscanf(line, "mapSelectedScene=%d", &ival) == 1) sMapSelectedScene = ival;
            else if (strncmp(line, "mapScene=", 9) == 0)
            {
                MapScene ms;
                strncpy(ms.name, line + 9, sizeof(ms.name) - 1);
                // Strip newline
                char* nl = strchr(ms.name, '\n'); if (nl) *nl = '\0';
                char* cr = strchr(ms.name, '\r'); if (cr) *cr = '\0';
                sMapScenes.push_back(ms);
            }
            else if (sscanf(line, "msSpriteCount=%d", &ival) == 1 && !sMapScenes.empty())
            {
                sMapScenes.back().spriteCount = 0; // reset, will increment as we parse sprites
            }
            else if (strncmp(line, "msSprite=", 9) == 0 && !sMapScenes.empty())
            {
                MapScene& ms = sMapScenes.back();
                FloorSprite sp = {};
                int typeVal = 0, animEn = 0, bpIdx = -1, bpParamCnt = 0;
                int matched = sscanf(line + 9, "%d,%f,%f,%f,%f,%u,%d,%d,%d,%f,%d,%d,%d,%d",
                    &sp.spriteId, &sp.x, &sp.y, &sp.z, &sp.scale, &sp.color,
                    &sp.assetIdx, &sp.animIdx, &typeVal, &sp.rotation, &animEn, &sp.meshIdx,
                    &bpIdx, &bpParamCnt);
                if (matched >= 6)
                {
                    sp.type = (SpriteType)typeVal;
                    sp.animEnabled = (animEn != 0);
                    sp.blueprintIdx = (matched >= 13) ? bpIdx : -1;
                    sp.instanceParamCount = (matched >= 14) ? std::min(bpParamCnt, 8) : 0;
                    // Parse instance params from pipe-delimited suffix
                    if (sp.instanceParamCount > 0) {
                        const char* p = line + 9;
                        for (int ip = 0; ip < sp.instanceParamCount; ip++) {
                            p = strchr(p, '|');
                            if (!p) break;
                            p++;
                            int pIdx, pVal;
                            if (sscanf(p, "%d:%d", &pIdx, &pVal) == 2) {
                                sp.instanceParams[ip].paramIdx = pIdx;
                                sp.instanceParams[ip].value = pVal;
                            }
                        }
                    }
                    if (ms.spriteCount < kMaxFloorSprites)
                        ms.sprites[ms.spriteCount++] = sp;
                }
            }
            else if (strncmp(line, "msCam=", 6) == 0 && !sMapScenes.empty())
            {
                MapScene& ms = sMapScenes.back();
                CameraStartObject& c = ms.camera;
                int sti = 0, sf = 0, cb = 0;
                sscanf(line + 6, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%d,%d,%f",
                    &c.x, &c.z, &c.height, &c.angle, &c.horizon,
                    &c.walkSpeed, &c.sprintSpeed,
                    &c.walkEaseIn, &c.walkEaseOut, &c.sprintEaseIn, &c.sprintEaseOut,
                    &c.jumpForce, &c.gravity, &c.maxFallSpeed,
                    &c.jumpCamLand, &c.jumpCamAir, &c.autoOrbitSpeed, &c.jumpDampen,
                    &sti, &sf, &cb, &c.drawDistance);
                c.smallTriCull = sti;
                c.skipFloor = (sf != 0);
                c.coverageBuf = (cb != 0);
            }
            else if (strncmp(line, "msSceneBp=", 10) == 0 && !sMapScenes.empty())
            {
                MapScene& ms = sMapScenes.back();
                int bpIdx = -1, ipc = 0;
                sscanf(line + 10, "%d,%d", &bpIdx, &ipc);
                ms.blueprintIdx = bpIdx;
                ms.instanceParamCount = 0;
                const char* pipe = strchr(line + 10, '|');
                while (pipe && ms.instanceParamCount < 8) {
                    int pi = 0, pv = 0;
                    if (sscanf(pipe + 1, "%d:%d", &pi, &pv) == 2) {
                        ms.instanceParams[ms.instanceParamCount].paramIdx = pi;
                        ms.instanceParams[ms.instanceParamCount].value = pv;
                        ms.instanceParamCount++;
                    }
                    pipe = strchr(pipe + 1, '|');
                }
            }
            else if (sscanf(line, "msVsNextId=%d", &ival) == 1 && !sMapScenes.empty())
                sMapScenes.back().vsNextId = ival;
            else if (!sMapScenes.empty() && strncmp(line, "msVsPan=", 8) == 0)
            {
                float px, py;
                if (sscanf(line + 8, "%f,%f", &px, &py) == 2) { sMapScenes.back().vsPanX = px; sMapScenes.back().vsPanY = py; }
            }
            else if (!sMapScenes.empty() && sscanf(line, "msVsZoom=%f", &fval) == 1)
                sMapScenes.back().vsZoom = fval;
            else if (sscanf(line, "msVsNodeCount=%d", &ival) == 1 && !sMapScenes.empty())
            {
                sMapScenes.back().vsNodes.clear();
                sMapScenes.back().vsNodes.reserve(ival);
            }
            else if (strncmp(line, "msVsNode=", 9) == 0 && !sMapScenes.empty())
            {
                VsNode n;
                int typeInt, gid = 0;
                if (sscanf(line + 9, "%d,%d,%f,%f,%d,%d,%d,%d,%d",
                    &n.id, &typeInt, &n.x, &n.y,
                    &n.paramInt[0], &n.paramInt[1], &n.paramInt[2], &n.paramInt[3], &gid) >= 4)
                {
                    n.type = (VsNodeType)typeInt;
                    n.groupId = gid;
                    sMapScenes.back().vsNodes.push_back(n);
                }
            }
            else if (strncmp(line, "msVsGroupDef=", 13) == 0 && !sMapScenes.empty())
            {
                int nid, ie, oe, id2, od;
                char lbl[32] = {};
                if (sscanf(line + 13, "%d|%31[^|]|%d,%d,%d,%d", &nid, lbl, &ie, &oe, &id2, &od) >= 6) {
                    for (auto& n : sMapScenes.back().vsNodes)
                        if (n.id == nid) {
                            strncpy(n.groupLabel, lbl, sizeof(n.groupLabel) - 1);
                            n.grpInExec = ie; n.grpOutExec = oe; n.grpInData = id2; n.grpOutData = od;
                            break;
                        }
                }
            }
            else if (strncmp(line, "msVsNodeCode=", 13) == 0 && !sMapScenes.empty())
            {
                int nid;
                char codeBuf[512] = {};
                if (sscanf(line + 13, "%d|%511[^\n]", &nid, codeBuf) >= 2) {
                    for (auto& n : sMapScenes.back().vsNodes)
                        if (n.id == nid) { strncpy(n.customCode, codeBuf, sizeof(n.customCode) - 1); break; }
                }
            }
            else if (sscanf(line, "msVsLinkCount=%d", &ival) == 1 && !sMapScenes.empty())
            {
                sMapScenes.back().vsLinks.clear();
                sMapScenes.back().vsLinks.reserve(ival);
            }
            else if (strncmp(line, "msVsLink=", 9) == 0 && !sMapScenes.empty())
            {
                VsLink lk;
                if (sscanf(line + 9, "%d,%d,%d|%d,%d,%d",
                    &lk.from.nodeId, &lk.from.pinType, &lk.from.pinIdx,
                    &lk.to.nodeId, &lk.to.pinType, &lk.to.pinIdx) == 6)
                    sMapScenes.back().vsLinks.push_back(lk);
            }
            else if (sscanf(line, "msVsAnnotCount=%d", &ival) == 1 && !sMapScenes.empty())
            {
                sMapScenes.back().vsAnnotations.clear();
                sMapScenes.back().vsAnnotations.reserve(ival);
            }
            else if (strncmp(line, "msVsAnnot=", 10) == 0 && !sMapScenes.empty())
            {
                VsAnnotation ann;
                float ax, ay, aw, ah;
                if (sscanf(line + 10, "%f,%f,%f,%f|", &ax, &ay, &aw, &ah) == 4) {
                    ann.x = ax; ann.y = ay; ann.w = aw; ann.h = ah;
                    const char* pipe = strchr(line + 10, '|');
                    if (pipe) strncpy(ann.label, pipe + 1, sizeof(ann.label) - 1);
                    sMapScenes.back().vsAnnotations.push_back(ann);
                }
            }
            else if (sscanf(line, "msVsGroupPinCount=%d", &ival) == 1 && !sMapScenes.empty())
            {
                sMapScenes.back().vsGroupPins.clear();
                sMapScenes.back().vsGroupPins.reserve(ival);
            }
            else if (strncmp(line, "msVsGroupPin=", 13) == 0 && !sMapScenes.empty())
            {
                VsGroupPinMap m;
                if (sscanf(line + 13, "%d,%d,%d,%d,%d,%d",
                    &m.groupNodeId, &m.pinType, &m.pinIdx, &m.innerNodeId, &m.innerPinType, &m.innerPinIdx) == 6)
                    sMapScenes.back().vsGroupPins.push_back(m);
            }
        }
    }

    // --- Migrate tilemap data to 4x4 sub-grid (subdivided=2) ---
    // subdivided=0: old format, needs 4x expansion
    // subdivided=1: first 2x format, needs 2x more expansion
    // subdivided=2: current format, no migration needed
    if (tmSubdivLevel < 2 && sTilemapData.floor.width > 0 && sTilemapData.floor.height > 0)
    {
        int passes = (tmSubdivLevel == 0) ? 2 : 1; // 0→2 passes, 1→1 pass

        auto migrateFloor = [](int& w, int& h, std::vector<uint16_t>& tiles, std::vector<TmObject>& objs)
        {
            int oldW = w, oldH = h;
            int newW = oldW * 2, newH = oldH * 2;
            std::vector<uint16_t> newTiles(newW * newH, 0);
            for (int y = 0; y < oldH; y++)
            {
                for (int x = 0; x < oldW; x++)
                {
                    uint16_t v = tiles[y * oldW + x];
                    newTiles[(2 * y)     * newW + (2 * x)]     = v;
                    newTiles[(2 * y)     * newW + (2 * x + 1)] = v;
                    newTiles[(2 * y + 1) * newW + (2 * x)]     = v;
                    newTiles[(2 * y + 1) * newW + (2 * x + 1)] = v;
                }
            }
            tiles = std::move(newTiles);
            w = newW;
            h = newH;

            for (auto& obj : objs)
            {
                if (obj.type == TmObjType::Tile)
                {
                    std::vector<std::pair<int,int>> newCells;
                    newCells.reserve(obj.cells.size() * 4);
                    for (auto& c : obj.cells)
                    {
                        int nx = c.first * 2, ny = c.second * 2;
                        newCells.push_back({nx, ny});
                        newCells.push_back({nx + 1, ny});
                        newCells.push_back({nx, ny + 1});
                        newCells.push_back({nx + 1, ny + 1});
                    }
                    obj.cells = std::move(newCells);
                }
                else
                {
                    obj.tileX *= 2;
                    obj.tileY *= 2;
                }
            }
        };

        for (int p = 0; p < passes; p++)
        {
            migrateFloor(sTilemapData.floor.width, sTilemapData.floor.height,
                         sTilemapData.floor.tileIndices, sTmObjects);
            for (auto& sc : sTmScenes)
                migrateFloor(sc.mapW, sc.mapH, sc.tileIndices, sc.objects);
        }
    }

    // Mark tilemap as initialized if we loaded data
    if (sTilemapData.floor.width > 0 && sTilemapData.floor.height > 0)
        sTilemapDataInit = true;

    // Load selected scene's data into active tilemap
    if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size())
    {
        const TmScene& sc = sTmScenes[sTmSelectedScene];
        if (!sc.tileIndices.empty())
            LoadSceneState(sc);
    }

    // Post-load cleanup: remove blueprint nodes that leaked into MapScene scripts
    // (caused by a bug where SaveMapSceneState saved blueprint nodes as scene data)
    for (int si = 0; si < (int)sMapScenes.size(); si++) {
        MapScene& ms = sMapScenes[si];
        for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
            const auto& bp = sBlueprintAssets[bi];
            if (!ms.vsNodes.empty() && ms.vsNodes.size() == bp.nodes.size()) {
                bool match = true;
                for (int ni = 0; ni < (int)ms.vsNodes.size() && match; ni++) {
                    if (ms.vsNodes[ni].id != bp.nodes[ni].id ||
                        ms.vsNodes[ni].type != bp.nodes[ni].type)
                        match = false;
                }
                if (match) {
                    ms.vsNodes.clear();
                    ms.vsLinks.clear();
                    ms.vsAnnotations.clear();
                    ms.vsGroupPins.clear();
                    ms.vsNextId = 1;
                    break;
                }
            }
        }
    }

    // Load selected map scene into active editor state
    if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
        LoadMapSceneState(sMapScenes[sMapSelectedScene]);

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

    sBlueprintAssets.clear();
    sSelectedBlueprint = -1;
    sVsEditSource = VsEditSource::Scene;
    sVsEditBlueprintIdx = -1;

    sMapScenes.clear();
    sMapSelectedScene = 0;

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
        bool inSet = sSelectedMeshes.count(mi) > 0;
        bool sel = (sSelectedMesh == mi) || inSet;
        if (ImGui::Selectable(sMeshAssets[mi].name.c_str(), sel))
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && sSelectedMesh >= 0) {
                // Range select from sSelectedMesh to mi
                int lo = std::min(sSelectedMesh, mi);
                int hi = std::max(sSelectedMesh, mi);
                if (!io.KeyCtrl) sSelectedMeshes.clear();
                for (int k = lo; k <= hi; k++) sSelectedMeshes.insert(k);
            } else if (io.KeyCtrl) {
                // Toggle individual
                if (inSet) sSelectedMeshes.erase(mi); else sSelectedMeshes.insert(mi);
            } else {
                sSelectedMeshes.clear();
                sSelectedMeshes.insert(mi);
            }
            sSelectedMesh = mi;
        }
        // Drag-select: if mouse is down and hovering over this item, add it
        if (ImGui::IsItemHovered() && ImGui::IsMouseDown(0) && ImGui::IsMouseDragging(0, 2.0f))
        {
            if (!sMeshDragSelecting) { sSelectedMeshes.clear(); sMeshDragSelecting = true; }
            sSelectedMeshes.insert(mi);
            sSelectedMesh = mi;
        }
    }
    if (sMeshDragSelecting && !ImGui::IsMouseDown(0))
        sMeshDragSelecting = false;
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
    if (!sSelectedMeshes.empty() || (sSelectedMesh >= 0 && sSelectedMesh < (int)sMeshAssets.size()))
    {
        bool doDelete = ImGui::Button("Delete##meshDel");
        // Also delete on Delete key when mesh list child was recently active
        if (!doDelete && ImGui::IsKeyPressed(ImGuiKey_Delete) && !sSelectedMeshes.empty())
            doDelete = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete All##meshDelAll"))
        {
            for (int i = 0; i < sSpriteCount; i++)
                sSprites[i].meshIdx = -1;
            sMeshAssets.clear();
            sSelectedMesh = -1;
            sSelectedMeshes.clear();
            sProjectDirty = true;
            doDelete = false;
        }
        if (doDelete)
        {
            // Ensure sSelectedMesh is in the set
            if (sSelectedMeshes.empty() && sSelectedMesh >= 0)
                sSelectedMeshes.insert(sSelectedMesh);
            // Delete from highest index to lowest so indices stay valid
            std::vector<int> toDelete(sSelectedMeshes.begin(), sSelectedMeshes.end());
            std::sort(toDelete.rbegin(), toDelete.rend());
            for (int delIdx : toDelete)
            {
                if (delIdx < 0 || delIdx >= (int)sMeshAssets.size()) continue;
                for (int i = 0; i < sSpriteCount; i++)
                {
                    if (sSprites[i].meshIdx == delIdx)
                        sSprites[i].meshIdx = -1;
                    else if (sSprites[i].meshIdx > delIdx)
                        sSprites[i].meshIdx--;
                }
                sMeshAssets.erase(sMeshAssets.begin() + delIdx);
            }
            sSelectedMesh = -1;
            sSelectedMeshes.clear();
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
        { int nTri = (int)ma.indices.size() / 3, nQuad = (int)ma.quadIndices.size() / 4;
          if (nQuad > 0)
              ImGui::Text("Verts: %d  Tris: %d  Quads: %d", (int)ma.vertices.size(), nTri, nQuad);
          else
              ImGui::Text("Verts: %d  Tris: %d", (int)ma.vertices.size(), nTri);
        }

        int cullInt = (int)ma.cullMode;
        ImGui::PushItemWidth(-1);
        if (ImGui::Combo("Cull##cullMode", &cullInt, kCullModeNames, 3))
            ma.cullMode = (CullMode)cullInt;
        ImGui::PopItemWidth();

        int exportInt = (int)ma.exportMode;
        ImGui::PushItemWidth(-1);
        if (ImGui::Combo("Export##exportMode", &exportInt, kMeshExportModeNames, 3))
            ma.exportMode = (MeshExportMode)exportInt;
        ImGui::PopItemWidth();

        ImGui::Checkbox("Lit##meshLit", &ma.lit);
        ImGui::SameLine();
        ImGui::Checkbox("Half-Res##meshHalfRes", &ma.halfRes);
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe##meshWire", &ma.wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Grayscale##meshGray", &ma.grayscale);
        if (!ma.quadIndices.empty())
            ImGui::Checkbox("Use Quads##meshQuad", &ma.useQuads);
        ImGui::DragFloat("Draw Distance##mesh", &ma.drawDistance, 1.0f, 0.0f, 2000.0f, "%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = use global draw distance");
        ImGui::Checkbox("Visible##meshVis", &ma.visible);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uncheck for invisible collision-only mesh (saves GPU/CPU)");
        ImGui::Checkbox("Collision##meshCol", &ma.collision);
        ImGui::DragInt("Draw Priority##meshPri", &ma.drawPriority, 0.1f, 0, 10);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = draws on top, higher = draws behind");

        ImGui::Separator();
        ImGui::Checkbox("Textured##meshTex", &ma.textured);
        if (ma.textured)
        {
            if (ma.texW > 0)
                ImGui::Text("Tex: %dx%d (%s)", ma.texW, ma.texH, ma.texturePath.c_str());
            else
                ImGui::TextDisabled("No texture loaded");
            if (ImGui::Button("Import Texture##meshTexBtn"))
            {
                std::string texPath = OpenFileDialog("PNG Files\0*.png\0All Files\0*.*\0", "png");
                if (!texPath.empty())
                    LoadMeshTexture(texPath, ma);
            }
            if (ma.glTexID)
            {
                if (ImGui::Checkbox("Filtered##meshTexFilter", &ma.texFiltered))
                {
                    glBindTexture(GL_TEXTURE_2D, ma.glTexID);
                    GLint filter = ma.texFiltered ? GL_LINEAR : GL_NEAREST;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }
        }
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
            if (sSprites[i].meshIdx >= 0 && sSprites[i].meshIdx < (int)sMeshAssets.size())
                sSelectedMesh = sSprites[i].meshIdx;
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
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Y##m3d", &sp.y, 0.5f, -200.0f, 200.0f);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Z##m3d", &sp.z, 1.0f, -kWorldHalf, kWorldHalf);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Scale##m3d", &sp.scale, 0.1f, 0.01f, 100.0f);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Rotation##m3d", &sp.rotation, 1.0f, 0.0f, 360.0f, "%.0f deg");
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
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
    TabButton("Mode 7",  EditorTab::Mode7);
    TabButton("Tilemap", EditorTab::Tilemap);
    TabButton("Sprites", EditorTab::Sprites);
    TabButton("Skybox",  EditorTab::Skybox);
    TabButton("3D",      EditorTab::ThreeD);
    TabButton("Nodes",   EditorTab::Events);

    ImGui::SameLine(0, Scaled(20));
    // Play/Stop button
    if (sEditorMode == EditorMode::Edit)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.45f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Play", ImVec2(btnW, btnH)))
        {
            // Save blueprint working set back before playing so bp.nodes/links are current
            if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                sBlueprintAssets[sVsEditBlueprintIdx].nodes = sVsNodes;
                sBlueprintAssets[sVsEditBlueprintIdx].links = sVsLinks;
            }
            sEditorMode = EditorMode::Play;
            sPlayTab = sActiveTab;
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
            // Save tilemap object state for restore on Stop
            sSavedTmObjects = sTmObjects;
            memset(sTmMoveAccum, 0, sizeof(sTmMoveAccum));
            memset(sTmPrevDirHeld, 0, sizeof(sTmPrevDirHeld));
            memset(sTmPrevKeyState, 0, sizeof(sTmPrevKeyState));
            sTmLastMoveDir = -1;
            sTmObjFacing.assign(sTmObjects.size(), 4); // default facing South
            sTmObjAnimSet.assign(sTmObjects.size(), 0); // default animation set 0
            sTmObjStepCount.assign(sTmObjects.size(), 0);
            sTmObjMoveRate.assign(sTmObjects.size(), 6.0f); // default move speed
            sTmObjVisX.resize(sTmObjects.size());
            sTmObjVisY.resize(sTmObjects.size());
            for (int i = 0; i < (int)sTmObjects.size(); i++) {
                sTmObjVisX[i] = (float)sTmObjects[i].tileX;
                sTmObjVisY[i] = (float)sTmObjects[i].tileY;
            }
            sTmOnStartRan = false;
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
            sScriptStartRan = false;
            sScriptMoveSpeed = -1.0f;
            sScriptAutoOrbitSpeed = 0.0f;
            sPendingSceneSwitch = -1;
            sPendingSceneMode = 0;
            sActivePlayAnimNodeId = -1;
            sPlayAnimIdle = -1;
            sPlayAnimHeld = -1;
            sPlayAnimReleased = -1;
            sVsFiredNodes.clear();
            sVsLinkSurgeT.clear();
            sVsLinkSurgeRevT.clear();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();

    // Build button (triggers GBA/NDS build)
    {
        ImVec4 buildCol = sPackaging ? ImVec4(0.4f, 0.4f, 0.1f, 1.0f) : ImVec4(0.15f, 0.3f, 0.55f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, buildCol);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        const char* buildLabel = sPackaging ? "Building..." : "Build";
        if (ImGui::Button(buildLabel, ImVec2(btnW * 1.2f, btnH)) && !sPackaging)
            sBuildRequested = true;
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();

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
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
    sVPImgPos = imgPos;
    sVPImgScale = scale;
    ImGui::Image((ImTextureID)(intptr_t)Mode7::GetTexture(), ImVec2(w, h));

    // Click on sprite in viewport to select it
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsKeyDown(ImGuiKey_R) && !sGrabMode)
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
                sSelectedObjType = SelectedObjType::Sprite;
                if (sSprites[sSelectedSprite].type == SpriteType::Mesh &&
                    sSprites[sSelectedSprite].meshIdx >= 0 &&
                    sSprites[sSelectedSprite].meshIdx < (int)sMeshAssets.size())
                    sSelectedMesh = sSprites[sSelectedSprite].meshIdx;
                ImGui::SetWindowFocus(nullptr); // release widget focus so keys work
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
        0x80FFFFFF, "Viewport (WASD + Q/E + I/K)");

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
        ImGui::PushItemWidth(Scaled(80));
        {
            const char* sizes[] = { "8x8", "16x16", "32x32", "64x64" };
            int sizeVals[] = { 8, 16, 32, 64 };
            int curIdx = 2; // default 32x32
            for (int si = 0; si < 4; si++)
                if (sizeVals[si] == asset.baseSize) { curIdx = si; break; }
            if (ImGui::Combo("##baseSize", &curIdx, sizes, 4))
            {
                int newSize = sizeVals[curIdx];
                int oldSize = asset.baseSize;
                if (newSize != oldSize && !asset.frames.empty())
                {
                    if (newSize < oldSize)
                    {
                        // Downscale: split each frame into sub-frames
                        // e.g., 32x32 → 16x16 = 4 sub-frames per original frame
                        int tilesX = oldSize / newSize;
                        int tilesY = oldSize / newSize;
                        std::vector<SpriteFrame> newFrames;
                        for (auto& fr : asset.frames)
                        {
                            for (int ty = 0; ty < tilesY; ty++)
                                for (int tx = 0; tx < tilesX; tx++)
                                {
                                    SpriteFrame sub;
                                    sub.width = newSize;
                                    sub.height = newSize;
                                    memset(sub.pixels, 0, sizeof(sub.pixels));
                                    for (int py = 0; py < newSize; py++)
                                        for (int px = 0; px < newSize; px++)
                                        {
                                            int sx = tx * newSize + px;
                                            int sy = ty * newSize + py;
                                            if (sx < fr.width && sy < fr.height)
                                                sub.pixels[py * kMaxFrameSize + px] = fr.pixels[sy * kMaxFrameSize + sx];
                                        }
                                    newFrames.push_back(sub);
                                }
                        }
                        asset.frames = std::move(newFrames);
                    }
                    else
                    {
                        // Upscale: merge consecutive sub-frames back into larger frames
                        // e.g., 16x16 → 32x32 = merge 4 sub-frames into 1
                        int tilesX = newSize / oldSize;
                        int tilesY = newSize / oldSize;
                        int subsPerFrame = tilesX * tilesY;
                        std::vector<SpriteFrame> newFrames;
                        for (int i = 0; i < (int)asset.frames.size(); i += subsPerFrame)
                        {
                            SpriteFrame big;
                            big.width = newSize;
                            big.height = newSize;
                            memset(big.pixels, 0, sizeof(big.pixels));
                            for (int s = 0; s < subsPerFrame && (i + s) < (int)asset.frames.size(); s++)
                            {
                                int tx = s % tilesX;
                                int ty = s / tilesX;
                                const SpriteFrame& sub = asset.frames[i + s];
                                for (int py = 0; py < oldSize; py++)
                                    for (int px = 0; px < oldSize; px++)
                                    {
                                        int dx = tx * oldSize + px;
                                        int dy = ty * oldSize + py;
                                        if (dx < newSize && dy < newSize)
                                            big.pixels[dy * kMaxFrameSize + dx] = sub.pixels[py * kMaxFrameSize + px];
                                    }
                            }
                            newFrames.push_back(big);
                        }
                        asset.frames = std::move(newFrames);
                    }
                    sTmSpriteTexCount = -1;
                }
                asset.baseSize = newSize;
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
            int fw = frame.width, fh = frame.height;

            // Draw pixel grid
            float gridAvail = std::min(colW2 - Scaled(20), size.y * 0.5f);
            float cellSize = std::min(gridAvail / (float)fw, gridAvail / (float)fh);
            cellSize = std::max(cellSize, 4.0f);
            float gridW = cellSize * fw;
            float gridH = cellSize * fh;

            ImVec2 gridStart = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw pixels
            for (int py = 0; py < fh; py++)
            {
                for (int px = 0; px < fw; px++)
                {
                    uint8_t palIdx = frame.pixels[py * kMaxFrameSize + px];
                    uint32_t col = asset.palette[palIdx & 0xF];
                    uint32_t r = (col >> 0) & 0xFF;
                    uint32_t g = (col >> 8) & 0xFF;
                    uint32_t b = (col >> 16) & 0xFF;
                    uint32_t a = (col >> 24) & 0xFF;
                    if (palIdx == 0) a = 0;
                    uint32_t imCol = (a << 24) | (b << 16) | (g << 8) | r;
                    if (palIdx == 0)
                        imCol = 0xFF1A1A1A;

                    ImVec2 p0(gridStart.x + px * cellSize, gridStart.y + py * cellSize);
                    ImVec2 p1(p0.x + cellSize, p0.y + cellSize);
                    dl->AddRectFilled(p0, p1, imCol);
                }
            }

            // Grid lines
            for (int i = 0; i <= fw; i++)
            {
                float x = gridStart.x + i * cellSize;
                dl->AddLine(ImVec2(x, gridStart.y), ImVec2(x, gridStart.y + gridH), 0x40FFFFFF);
            }
            for (int i = 0; i <= fh; i++)
            {
                float y = gridStart.y + i * cellSize;
                dl->AddLine(ImVec2(gridStart.x, y), ImVec2(gridStart.x + gridW, y), 0x40FFFFFF);
            }

            // Click to paint
            ImGui::SetCursorScreenPos(gridStart);
            ImGui::InvisibleButton("##PixelGrid", ImVec2(gridW, gridH));
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 mouse = ImGui::GetMousePos();
                int px = (int)((mouse.x - gridStart.x) / cellSize);
                int py = (int)((mouse.y - gridStart.y) / cellSize);
                if (px >= 0 && px < fw && py >= 0 && py < fh)
                    frame.pixels[py * kMaxFrameSize + px] = (uint8_t)sSpriteEditorPalColor;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                ImVec2 mouse = ImGui::GetMousePos();
                int px = (int)((mouse.x - gridStart.x) / cellSize);
                int py = (int)((mouse.y - gridStart.y) / cellSize);
                if (px >= 0 && px < fw && py >= 0 && py < fh)
                    frame.pixels[py * kMaxFrameSize + px] = 0;
            }

            ImGui::SetCursorScreenPos(ImVec2(gridStart.x, gridStart.y + gridH + 4));

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

            // Thumbnail strip — aspect ratio matches frame dimensions
            float thumbBase = Scaled(32);
            for (int fi = 0; fi < (int)asset.frames.size(); fi++)
            {
                ImGui::PushID(fi + 2000);
                bool fsel = (sSelectedFrame == fi);

                int tfw = asset.frames[fi].width, tfh = asset.frames[fi].height;
                float thumbCell = std::min(thumbBase / (float)tfw, thumbBase / (float)tfh);
                float thumbW = thumbCell * tfw, thumbH = thumbCell * tfh;

                ImVec2 thumbStart = ImGui::GetCursorScreenPos();
                uint32_t borderCol = fsel ? 0xFFFFFFFF : 0xFF444444;
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                tdl->AddRect(thumbStart, ImVec2(thumbStart.x + thumbW, thumbStart.y + thumbH), borderCol);

                // Mini preview of frame
                for (int ty = 0; ty < tfh; ty++)
                    for (int tx = 0; tx < tfw; tx++)
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
                if (ImGui::InvisibleButton("##fthumb", ImVec2(thumbW, thumbH)))
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
            ImGui::Checkbox("Step##astep", &anim.stepAnim);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Advance frame per tile step");
            ImGui::SameLine();
            ImGui::PushItemWidth(Scaled(50));
            ImGui::DragFloat("##aspeed", &anim.speed, 0.05f, 0.0f, 10.0f, "%.1f");
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed");
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

                    // Auto-detect frame size from baseSize
                    asset.stripFrameW = asset.baseSize;
                    asset.stripFrameH = asset.baseSize;
                    int fw = asset.baseSize;
                    int fh = asset.baseSize;
                    if (fw > kMaxFrameSize) fw = kMaxFrameSize;
                    if (fh > kMaxFrameSize) fh = kMaxFrameSize;

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
                            frame.width = fw;
                            frame.height = fh;
                            memset(frame.pixels, 0, sizeof(frame.pixels));

                            for (int py = 0; py < fh && py < frame.height; py++)
                            {
                                for (int px = 0; px < fw && px < frame.width; px++)
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
            float dirPreviewSz = Scaled(44);
            float dirCellW = dirPreviewSz + 8.0f;
            float dirCellH = dirPreviewSz + 22.0f;  // label text + square image
            float dirPreviewW = dirPreviewSz;
            float dirPreviewH = dirPreviewSz;
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
                    ImVec2 cmax = ImVec2(cpos.x + dirCellW - 4, cpos.y + dirCellH - 4);
                    ImDrawList* dl2 = ImGui::GetWindowDrawList();

                    bool clicked = ImGui::InvisibleButton("##adirbtn", ImVec2(dirCellW - 4, dirCellH - 4));
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
                    dl2->AddText(ImVec2(cmin.x + (dirCellW - 4 - lsz2.x) * 0.5f, cmin.y + 2), 0xFFAAAACC, dirLabel);

                    AssetDirSprite& ads = sAssetDirSprites[sSelectedAsset][dirSetIdx][d];
                    if (ads.texture)
                    {
                        // Maintain aspect ratio within the square preview area
                        float aspect = (ads.width > 0 && ads.height > 0) ? (float)ads.width / (float)ads.height : 1.0f;
                        float drawW = dirPreviewW, drawH = dirPreviewH;
                        if (aspect > 1.0f) drawH = dirPreviewH / aspect;
                        else drawW = dirPreviewW * aspect;
                        float imgX = cmin.x + (dirCellW - 4 - drawW) * 0.5f;
                        float imgY = cmin.y + 16.0f + (dirPreviewH - drawH) * 0.5f;
                        dl2->AddImage((ImTextureID)(uintptr_t)ads.texture,
                            ImVec2(imgX, imgY), ImVec2(imgX + drawW, imgY + drawH));
                    }
                    else
                    {
                        const char* emptyTxt = "Click";
                        ImVec2 esz2 = ImGui::CalcTextSize(emptyTxt);
                        dl2->AddText(
                            ImVec2(cmin.x + (dirCellW - 4 - esz2.x) * 0.5f,
                                   cmin.y + 16.0f + (dirPreviewH - esz2.y) * 0.5f),
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
            if (sSprites[i].type == SpriteType::Mesh &&
                sSprites[i].meshIdx >= 0 && sSprites[i].meshIdx < (int)sMeshAssets.size())
                sSelectedMesh = sSprites[i].meshIdx;
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
        ImGui::Text("Camera Delay");
        ImGui::DragFloat("Walk Ease In##cam",  &sCamObj.walkEaseIn,  0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Walk Ease Out##cam", &sCamObj.walkEaseOut, 0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Sprint Ease In##cam",  &sCamObj.sprintEaseIn,  0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::DragFloat("Sprint Ease Out##cam", &sCamObj.sprintEaseOut, 0.5f, 1.0f, 50.0f, "%.0f%%");
        ImGui::Separator();
        ImGui::Text("Jump Camera Delay");
        ImGui::DragFloat("Cam Delay (Land)##cam", &sCamObj.jumpCamLand, 0.5f, 1.0f, 100.0f, "%.0f%%");
        ImGui::DragFloat("Cam Delay (Air)##cam",  &sCamObj.jumpCamAir,  0.5f, 1.0f, 100.0f, "%.0f%%");
        ImGui::Separator();
        ImGui::Separator();
        ImGui::Text("Rendering");
        ImGui::DragFloat("Draw Distance##cam", &sCamObj.drawDistance, 1.0f, 0.0f, 2000.0f, "%.0f");
        ImGui::DragInt("Small Tri Cull##cam", &sCamObj.smallTriCull, 1.0f, 0, 500, "%d");
        if (sCamObj.smallTriCull > 0)
            ImGui::TextDisabled("Skip tris with screen area < %d", sCamObj.smallTriCull);
        ImGui::Checkbox("Skip Floor##cam", &sCamObj.skipFloor);
        ImGui::Checkbox("Coverage Buffer##cam", &sCamObj.coverageBuf);
        if (sCamObj.coverageBuf)
            ImGui::TextDisabled("Front-to-back render, skip covered pixels");
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
                {
                    sp.type = (SpriteType)t;
                    if (sp.type != SpriteType::Mesh)
                        sp.meshIdx = -1; // clear mesh link when changing away from Mesh type
                }
            }
            ImGui::EndCombo();
        }
        ImGui::DragFloat("X##spr", &sp.x, 1.0f, -kWorldHalf, kWorldHalf);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Y##spr", &sp.y, 0.5f, 0.0f, 200.0f);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Z##spr", &sp.z, 1.0f, -kWorldHalf, kWorldHalf);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Scale##spr", &sp.scale, 0.1f, 0.1f, 50.0f);
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);
        ImGui::DragFloat("Rotation##spr", &sp.rotation, 1.0f, 0.0f, 360.0f, "%.0f deg");
        if (ImGui::IsItemActivated()) UndoPush(sSelectedSprite, sp);

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

        // Blueprint attachment
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Blueprint");
            ImGui::PushItemWidth(size.x * 0.5f);
            const char* bpPreview = (sp.blueprintIdx >= 0 && sp.blueprintIdx < (int)sBlueprintAssets.size())
                ? sBlueprintAssets[sp.blueprintIdx].name : "(none)";
            if (ImGui::BeginCombo("Script##sprbp", bpPreview)) {
                if (ImGui::Selectable("(none)##sprbpnone", sp.blueprintIdx < 0)) {
                    sp.blueprintIdx = -1;
                    sp.instanceParamCount = 0;
                    sProjectDirty = true;
                }
                for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                    bool sel = (sp.blueprintIdx == bi);
                    if (ImGui::Selectable(sBlueprintAssets[bi].name, sel)) {
                        sp.blueprintIdx = bi;
                        sp.instanceParamCount = 0; // reset overrides on change
                        sProjectDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            // Show exposed parameters with override fields
            if (sp.blueprintIdx >= 0 && sp.blueprintIdx < (int)sBlueprintAssets.size()) {
                const BlueprintAsset& bp = sBlueprintAssets[sp.blueprintIdx];
                for (int pi = 0; pi < bp.paramCount; pi++) {
                    const BpParam& param = bp.params[pi];
                    // Find if this param has an instance override
                    int overrideIdx = -1;
                    for (int oi = 0; oi < sp.instanceParamCount; oi++)
                        if (sp.instanceParams[oi].paramIdx == pi) { overrideIdx = oi; break; }
                    int val = (overrideIdx >= 0) ? sp.instanceParams[overrideIdx].value : param.defaultInt;
                    ImGui::PushID(pi + 5000);
                    char label[48]; snprintf(label, sizeof(label), "%s##bp%d", param.name, pi);
                    if (ImGui::DragInt(label, &val, 1.0f)) {
                        if (overrideIdx >= 0) {
                            sp.instanceParams[overrideIdx].value = val;
                        } else if (sp.instanceParamCount < 8) {
                            sp.instanceParams[sp.instanceParamCount].paramIdx = pi;
                            sp.instanceParams[sp.instanceParamCount].value = val;
                            sp.instanceParamCount++;
                        }
                        sProjectDirty = true;
                    }
                    if (overrideIdx >= 0) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset")) {
                            for (int oi = overrideIdx; oi < sp.instanceParamCount - 1; oi++)
                                sp.instanceParams[oi] = sp.instanceParams[oi + 1];
                            sp.instanceParamCount--;
                            sProjectDirty = true;
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopItemWidth();
        }

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

// ---- Tilemap Tab: tile editor + tileset + tilemap grid + palette ----
static void DrawTilemapTab(ImVec2 pos, ImVec2 size)
{
    // Init tilemap data on first use
    if (!sTilemapDataInit)
    {
        sTilemapDataInit = true;
        // Default tileset: 128 blank tiles + default palette
        sTilemapData.tileset.tiles.resize(kTilesetCols * kTilesetRows);
        for (auto& t : sTilemapData.tileset.tiles)
            memset(t.pixels, 0, sizeof(t.pixels));
        for (int i = 0; i < 16; i++)
            sTilemapData.tileset.palette[i] = sPalette[i];
        // Default tilemap: 4x4 sub-cells (one full tile) — drag edges to expand
        sTilemapData.floor.width  = 4;
        sTilemapData.floor.height = 4;
        sTilemapData.floor.tileIndices.resize(16, 0);
    }

    // Create default Scene 0 if none exist (tilemap scenes)
    if (sTmScenes.empty())
    {
        TmScene sc;
        snprintf(sc.name, sizeof(sc.name), "Scene 0");
        sc.mapW = sTilemapData.floor.width;
        sc.mapH = sTilemapData.floor.height;
        sTmScenes.push_back(sc);
        sTmSelectedScene = 0;
    }

    Tileset& ts = sTilemapData.tileset;
    TilemapLayer& tm = sTilemapData.floor;
    int tileCount = (int)ts.tiles.size();

    // Rebuild sprite textures if asset count changed or textures are stale
    {
        bool needRebuild = ((int)sSpriteAssets.size() != sTmSpriteTexCount);
        if (!needRebuild)
        {
            // Check if any texture is missing (e.g. after project load)
            for (int i = 0; i < (int)sSpriteAssets.size(); i++)
            {
                if (!sSpriteAssets[i].frames.empty() &&
                    (i >= (int)sTmSpriteTextures.size() || sTmSpriteTextures[i] == 0))
                { needRebuild = true; break; }
            }
        }
        if (needRebuild)
        {
            RebuildTmSpriteTextures();
            PatchTmSpriteTexturesFromDirs();
        }
    }

    const ImGuiWindowFlags panelFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoTitleBar;

    float objPanelW = sTmObjPanelW;

    // ======== LEFT PANEL: Objects ========
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(objPanelW, size.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##TmObjPanel", nullptr, panelFlags);
    {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Objects");

        // Add object button + type selector
        static int sAddObjType = 0;
        ImGui::PushItemWidth(objPanelW - 80);
        ImGui::Combo("##ObjType", &sAddObjType, sTmObjTypeNames, (int)TmObjType::COUNT);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Add"))
        {
            TmObject obj;
            obj.type = (TmObjType)sAddObjType;
            obj.tileX = tm.width / 2;
            obj.tileY = tm.height / 2;
            snprintf(obj.name, sizeof(obj.name), "%s %d",
                sTmObjTypeNames[sAddObjType], (int)sTmObjects.size());
            sTmObjects.push_back(obj);
            sTmSelectedObj = (int)sTmObjects.size() - 1;
        }

        ImGui::Separator();

        // Object list (exclude Tile-type objects — those live in the Tiles panel)
        ImGui::BeginChild("##ObjList", ImVec2(0, size.y * 0.45f), true);
        for (int i = 0; i < (int)sTmObjects.size(); i++)
        {
            TmObject& obj = sTmObjects[i];
            if (obj.type == TmObjType::Tile) continue;
            uint32_t col = sTmObjTypeColors[(int)obj.type];
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
            bool selected = (i == sTmSelectedObj);
            char label[64];
            snprintf(label, sizeof(label), "%s##obj%d", obj.name, i);
            if (ImGui::Selectable(label, selected))
                sTmSelectedObj = i;
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Selected object properties
        if (sTmSelectedObj >= 0 && sTmSelectedObj < (int)sTmObjects.size())
        {
            TmObject& obj = sTmObjects[sTmSelectedObj];
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Properties");

            ImGui::PushItemWidth(objPanelW - 16);
            ImGui::InputText("Name", obj.name, sizeof(obj.name));

            int typeInt = (int)obj.type;
            if (ImGui::Combo("Type", &typeInt, sTmObjTypeNames, (int)TmObjType::COUNT))
                obj.type = (TmObjType)typeInt;

            // Sprite asset link
            {
                const char* preview = (obj.spriteAssetIdx >= 0 && obj.spriteAssetIdx < (int)sSpriteAssets.size())
                    ? sSpriteAssets[obj.spriteAssetIdx].name.c_str() : "None";
                if (ImGui::BeginCombo("Sprite", preview))
                {
                    if (ImGui::Selectable("None", obj.spriteAssetIdx < 0))
                        obj.spriteAssetIdx = -1;
                    for (int si = 0; si < (int)sSpriteAssets.size(); si++)
                    {
                        bool sel = (obj.spriteAssetIdx == si);
                        if (ImGui::Selectable(sSpriteAssets[si].name.c_str(), sel))
                            obj.spriteAssetIdx = si;
                    }
                    ImGui::EndCombo();
                }
            }

            // Teleport: target scene selector
            if (obj.type == TmObjType::Teleport)
            {
                const char* tpPreview = (obj.teleportScene >= 0 && obj.teleportScene < (int)sTmScenes.size())
                    ? sTmScenes[obj.teleportScene].name : "None";
                if (ImGui::BeginCombo("To Scene", tpPreview))
                {
                    if (ImGui::Selectable("None##tpnone", obj.teleportScene < 0))
                        obj.teleportScene = -1;
                    for (int si = 0; si < (int)sTmScenes.size(); si++)
                    {
                        if (si == sTmSelectedScene) continue;
                        bool tpsel = (obj.teleportScene == si);
                        if (ImGui::Selectable(sTmScenes[si].name, tpsel))
                            obj.teleportScene = si;
                    }
                    ImGui::EndCombo();
                }
            }

            if (obj.type == TmObjType::Tile)
            {
                ImGui::Text("Cells: %d", (int)obj.cells.size());
                if (ImGui::Button("Paint"))
                {
                    sTmStampAsset = obj.spriteAssetIdx;
                    sTmStampObj = sTmSelectedObj;
                }
                ImGui::SameLine();
            }
            else
            {
                ImGui::DragInt("Tile X", &obj.tileX, 0.5f, 0, tm.width - 1);
                ImGui::DragInt("Tile Y", &obj.tileY, 0.5f, 0, tm.height - 1);
                ImGui::SliderFloat("Scale", &obj.displayScale, 0.5f, 4.0f, "%.1f");
            }
            // Blueprint attachment
            {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Blueprint");
                const char* bpPreview = (obj.blueprintIdx >= 0 && obj.blueprintIdx < (int)sBlueprintAssets.size())
                    ? sBlueprintAssets[obj.blueprintIdx].name : "(none)";
                if (ImGui::BeginCombo("Script##tmbp", bpPreview)) {
                    if (ImGui::Selectable("(none)##tmbpnone", obj.blueprintIdx < 0)) {
                        obj.blueprintIdx = -1;
                        obj.instanceParamCount = 0;
                        sProjectDirty = true;
                    }
                    for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                        bool sel = (obj.blueprintIdx == bi);
                        if (ImGui::Selectable(sBlueprintAssets[bi].name, sel)) {
                            obj.blueprintIdx = bi;
                            obj.instanceParamCount = 0;
                            sProjectDirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (obj.blueprintIdx >= 0 && obj.blueprintIdx < (int)sBlueprintAssets.size()) {
                    const BlueprintAsset& bp = sBlueprintAssets[obj.blueprintIdx];
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        const BpParam& param = bp.params[pi];
                        int overrideIdx = -1;
                        for (int oi = 0; oi < obj.instanceParamCount; oi++)
                            if (obj.instanceParams[oi].paramIdx == pi) { overrideIdx = oi; break; }
                        int val = (overrideIdx >= 0) ? obj.instanceParams[overrideIdx].value : param.defaultInt;
                        ImGui::PushID(pi + 6000);
                        char label[48]; snprintf(label, sizeof(label), "%s##tmbp%d", param.name, pi);
                        if (ImGui::DragInt(label, &val, 1.0f)) {
                            if (overrideIdx >= 0) {
                                obj.instanceParams[overrideIdx].value = val;
                            } else if (obj.instanceParamCount < 8) {
                                obj.instanceParams[obj.instanceParamCount].paramIdx = pi;
                                obj.instanceParams[obj.instanceParamCount].value = val;
                                obj.instanceParamCount++;
                            }
                            sProjectDirty = true;
                        }
                        if (overrideIdx >= 0) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Reset")) {
                                for (int oi = overrideIdx; oi < obj.instanceParamCount - 1; oi++)
                                    obj.instanceParams[oi] = obj.instanceParams[oi + 1];
                                obj.instanceParamCount--;
                                sProjectDirty = true;
                            }
                        }
                        ImGui::PopID();
                    }
                }
            }

            ImGui::PopItemWidth();

            ImGui::Spacing();
            if (ImGui::Button("Delete"))
            {
                sTmObjects.erase(sTmObjects.begin() + sTmSelectedObj);
                sTmSelectedObj = std::min(sTmSelectedObj, (int)sTmObjects.size() - 1);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);

    // ======== SPLITTER between object panel and grid ========
    {
        float splitterW = 6.0f;
        float splitterX = pos.x + objPanelW - splitterW * 0.5f;
        ImVec2 splitterMin(splitterX, pos.y);
        ImVec2 splitterMax(splitterX + splitterW, pos.y + size.y);
        ImVec2 mouse = ImGui::GetMousePos();
        bool hovered = (mouse.x >= splitterMin.x && mouse.x <= splitterMax.x &&
                        mouse.y >= splitterMin.y && mouse.y <= splitterMax.y);
        static bool sDraggingSplitter = false;

        if (hovered || sDraggingSplitter)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        if (hovered && ImGui::IsMouseClicked(0))
            sDraggingSplitter = true;

        if (sDraggingSplitter)
        {
            if (ImGui::IsMouseDown(0))
            {
                sTmObjPanelW += ImGui::GetIO().MouseDelta.x;
                sTmObjPanelW = std::clamp(sTmObjPanelW, 120.0f, size.x * 0.5f);
                objPanelW = sTmObjPanelW;
            }
            else
                sDraggingSplitter = false;
        }

        // Draw splitter line
        ImDrawList* fgDl = ImGui::GetForegroundDrawList();
        uint32_t splCol = (hovered || sDraggingSplitter) ? 0xFF44AAFF : 0x40FFFFFF;
        fgDl->AddLine(ImVec2(pos.x + objPanelW, pos.y),
                       ImVec2(pos.x + objPanelW, pos.y + size.y), splCol, 2.0f);
    }

    // ======== SCENE PANEL (top right) ========
    float scenePanelW = sTmScenePanelW;
    float tilePanelH  = size.y * 0.4f;
    float scenePanelH = size.y - tilePanelH;
    {
        ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - scenePanelW, pos.y));
        ImGui::SetNextWindowSize(ImVec2(scenePanelW, scenePanelH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
        ImGui::Begin("##TmScenePanel", nullptr, panelFlags);

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Scenes");

        // Add scene button
        if (ImGui::Button("Add##addscene"))
        {
            // Save current scene state before switching
            if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size())
                SaveSceneState(sTmScenes[sTmSelectedScene]);
            // Create fresh 1x1 scene
            TmScene sc;
            snprintf(sc.name, sizeof(sc.name), "Scene %d", (int)sTmScenes.size());
            sc.mapW = 1;
            sc.mapH = 1;
            sTmScenes.push_back(sc);
            sTmSelectedScene = (int)sTmScenes.size() - 1;
            // Load the fresh scene (1x1 empty grid, no objects)
            LoadSceneState(sTmScenes[sTmSelectedScene]);
        }

        ImGui::Separator();

        // Scene list (compact)
        ImGui::BeginChild("##SceneList", ImVec2(0, 80), true);
        for (int i = 0; i < (int)sTmScenes.size(); i++)
        {
            bool sel = (i == sTmSelectedScene);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s##sc%d", sTmScenes[i].name, i);
            if (ImGui::Selectable(lbl, sel) && i != sTmSelectedScene)
            {
                // Save current scene before switching
                if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size())
                    SaveSceneState(sTmScenes[sTmSelectedScene]);
                sTmSelectedScene = i;
                LoadSceneState(sTmScenes[i]);
            }
        }
        ImGui::EndChild();

        // Selected scene properties (right below scene list)
        if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size())
        {
            TmScene& sc = sTmScenes[sTmSelectedScene];
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Properties");
            ImGui::PushItemWidth(scenePanelW - 16);
            ImGui::InputText("Name##scname", sc.name, sizeof(sc.name));
            // Sync scene dimensions with tilemap — bidirectional
            sc.mapW = sTilemapData.floor.width;
            sc.mapH = sTilemapData.floor.height;
            int prevW = sc.mapW, prevH = sc.mapH;
            ImGui::DragInt("Width##scw", &sc.mapW, 0.5f, 1, 128);
            ImGui::DragInt("Height##sch", &sc.mapH, 0.5f, 1, 128);
            if (sc.mapW != prevW || sc.mapH != prevH)
            {
                // User changed dimensions via properties — resize tilemap
                std::vector<uint16_t> old = sTilemapData.floor.tileIndices;
                int oldW = prevW, oldH = prevH;
                sTilemapData.floor.width  = sc.mapW;
                sTilemapData.floor.height = sc.mapH;
                sTilemapData.floor.tileIndices.assign(sc.mapW * sc.mapH, 0);
                for (int y = 0; y < std::min(oldH, sc.mapH); y++)
                    for (int x = 0; x < std::min(oldW, sc.mapW); x++)
                        sTilemapData.floor.tileIndices[y * sc.mapW + x] = old[y * oldW + x];
            }

            ImGui::PopItemWidth();

            // Scene-level blueprint
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Scene Blueprint");
            ImGui::PushItemWidth(scenePanelW - 16);
            {
                const char* bpPreview = (sc.blueprintIdx >= 0 && sc.blueprintIdx < (int)sBlueprintAssets.size())
                    ? sBlueprintAssets[sc.blueprintIdx].name : "(none)";
                if (ImGui::BeginCombo("Script##tmscbp", bpPreview)) {
                    if (ImGui::Selectable("(none)##tmscbpnone", sc.blueprintIdx < 0)) {
                        sc.blueprintIdx = -1;
                        sc.instanceParamCount = 0;
                        sProjectDirty = true;
                    }
                    for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                        bool sel2 = (sc.blueprintIdx == bi);
                        if (ImGui::Selectable(sBlueprintAssets[bi].name, sel2)) {
                            sc.blueprintIdx = bi;
                            sc.instanceParamCount = 0;
                            sProjectDirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (sc.blueprintIdx >= 0 && sc.blueprintIdx < (int)sBlueprintAssets.size()) {
                    const BlueprintAsset& bp = sBlueprintAssets[sc.blueprintIdx];
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        const BpParam& param = bp.params[pi];
                        int overrideIdx = -1;
                        for (int oi = 0; oi < sc.instanceParamCount; oi++)
                            if (sc.instanceParams[oi].paramIdx == pi) { overrideIdx = oi; break; }
                        int val = (overrideIdx >= 0) ? sc.instanceParams[overrideIdx].value : param.defaultInt;
                        ImGui::PushID(pi + 8000);
                        char label[48]; snprintf(label, sizeof(label), "%s##tmscbp%d", param.name, pi);
                        if (ImGui::DragInt(label, &val, 1.0f)) {
                            if (overrideIdx >= 0) {
                                sc.instanceParams[overrideIdx].value = val;
                            } else if (sc.instanceParamCount < 8) {
                                sc.instanceParams[sc.instanceParamCount].paramIdx = pi;
                                sc.instanceParams[sc.instanceParamCount].value = val;
                                sc.instanceParamCount++;
                            }
                            sProjectDirty = true;
                        }
                        if (overrideIdx >= 0) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Reset")) {
                                for (int oi = overrideIdx; oi < sc.instanceParamCount - 1; oi++)
                                    sc.instanceParams[oi] = sc.instanceParams[oi + 1];
                                sc.instanceParamCount--;
                                sProjectDirty = true;
                            }
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::PopItemWidth();

            ImGui::Spacing();
            if (ImGui::Button("Delete##delscene") && sTmScenes.size() > 1)
            {
                sTmScenes.erase(sTmScenes.begin() + sTmSelectedScene);
                if (sTmSelectedScene >= (int)sTmScenes.size())
                    sTmSelectedScene = (int)sTmScenes.size() - 1;
                LoadSceneState(sTmScenes[sTmSelectedScene]);
            }
        }

        ImGui::End();
        ImGui::PopStyleColor(2);
    }

    // ======== TILES PANEL (bottom right — Tile objects only) ========
    {
        ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - scenePanelW, pos.y + scenePanelH));
        ImGui::SetNextWindowSize(ImVec2(scenePanelW, tilePanelH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
        ImGui::Begin("##TmTilesPanel", nullptr, panelFlags);

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Tiles");
        ImGui::SameLine();
        if (ImGui::SmallButton("Add##addtile"))
        {
            TmObject obj;
            obj.type = TmObjType::Tile;
            snprintf(obj.name, sizeof(obj.name), "Tile %d", (int)sTmObjects.size());
            obj.tileX = tm.width / 2;
            obj.tileY = tm.height / 2;
            sTmObjects.push_back(obj);
            sTmSelectedObj = (int)sTmObjects.size() - 1;
        }

        ImGui::Separator();

        // Tile object list
        ImGui::BeginChild("##TileObjList", ImVec2(0, tilePanelH * 0.35f), true);
        for (int i = 0; i < (int)sTmObjects.size(); i++)
        {
            TmObject& obj = sTmObjects[i];
            if (obj.type != TmObjType::Tile) continue;
            bool selected = (i == sTmSelectedObj);
            char label[64];
            snprintf(label, sizeof(label), "%s##tile%d", obj.name, i);
            if (ImGui::Selectable(label, selected))
                sTmSelectedObj = i;
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Selected tile properties
        if (sTmSelectedObj >= 0 && sTmSelectedObj < (int)sTmObjects.size() &&
            sTmObjects[sTmSelectedObj].type == TmObjType::Tile)
        {
            TmObject& obj = sTmObjects[sTmSelectedObj];
            ImGui::PushItemWidth(scenePanelW - 16);
            ImGui::InputText("Name##tilename", obj.name, sizeof(obj.name));

            // Sprite asset link
            {
                const char* preview = (obj.spriteAssetIdx >= 0 && obj.spriteAssetIdx < (int)sSpriteAssets.size())
                    ? sSpriteAssets[obj.spriteAssetIdx].name.c_str() : "None";
                if (ImGui::BeginCombo("Sprite##tilesprite", preview))
                {
                    if (ImGui::Selectable("None##tsnone", obj.spriteAssetIdx < 0))
                        obj.spriteAssetIdx = -1;
                    for (int si = 0; si < (int)sSpriteAssets.size(); si++)
                    {
                        bool sel = (obj.spriteAssetIdx == si);
                        if (ImGui::Selectable(sSpriteAssets[si].name.c_str(), sel))
                            obj.spriteAssetIdx = si;
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::Text("Cells: %d", (int)obj.cells.size());
            if (ImGui::Button("Paint##tilepaint"))
            {
                sTmStampAsset = obj.spriteAssetIdx;
                sTmStampObj = sTmSelectedObj;
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete##tiledel"))
            {
                sTmObjects.erase(sTmObjects.begin() + sTmSelectedObj);
                sTmSelectedObj = std::min(sTmSelectedObj, (int)sTmObjects.size() - 1);
            }
            // Draw mode: 1x, 4x, 8x
            {
                ImGui::Text("Draw:");
                ImGui::SameLine();
                if (ImGui::RadioButton("1x", sTmPaintScale == 1)) sTmPaintScale = 1;
                ImGui::SameLine();
                if (ImGui::RadioButton("4x", sTmPaintScale == 4)) sTmPaintScale = 4;
                ImGui::SameLine();
                if (ImGui::RadioButton("8x", sTmPaintScale == 8)) sTmPaintScale = 8;
            }
            ImGui::PopItemWidth();
        }

        ImGui::End();
        ImGui::PopStyleColor(2);
    }

    // ======== SPLITTER between grid and scene panel ========
    {
        float splitterW = 6.0f;
        float splitterX = pos.x + size.x - scenePanelW - splitterW * 0.5f;
        ImVec2 splitterMin(splitterX, pos.y);
        ImVec2 splitterMax(splitterX + splitterW, pos.y + size.y);
        ImVec2 mouse = ImGui::GetMousePos();
        bool hovered = (mouse.x >= splitterMin.x && mouse.x <= splitterMax.x &&
                        mouse.y >= splitterMin.y && mouse.y <= splitterMax.y);
        static bool sDraggingSceneSplitter = false;

        if (hovered || sDraggingSceneSplitter)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        if (hovered && ImGui::IsMouseClicked(0))
            sDraggingSceneSplitter = true;

        if (sDraggingSceneSplitter)
        {
            if (ImGui::IsMouseDown(0))
            {
                sTmScenePanelW -= ImGui::GetIO().MouseDelta.x;
                sTmScenePanelW = std::clamp(sTmScenePanelW, 120.0f, size.x * 0.5f);
                scenePanelW = sTmScenePanelW;
            }
            else
                sDraggingSceneSplitter = false;
        }

        ImDrawList* fgDl = ImGui::GetForegroundDrawList();
        uint32_t splCol = (hovered || sDraggingSceneSplitter) ? 0xFF44AAFF : 0x40FFFFFF;
        fgDl->AddLine(ImVec2(pos.x + size.x - scenePanelW, pos.y),
                       ImVec2(pos.x + size.x - scenePanelW, pos.y + size.y), splCol, 2.0f);
    }

    // ======== CENTER: tilemap grid with draggable edges ========
    float gridPosX = pos.x + objPanelW;
    float gridW2 = size.x - objPanelW - scenePanelW;
    ImGui::SetNextWindowPos(ImVec2(gridPosX, pos.y));
    ImGui::SetNextWindowSize(ImVec2(gridW2, size.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.30f, 1.0f));
    ImGui::Begin("##TmRight", nullptr, panelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImVec2 cursor = ImVec2(gridPosX, pos.y);
        float availW = gridW2 - 16.0f;
        float availH = size.y - 8.0f;

        // Zoom with scroll wheel
        if (ImGui::IsWindowHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                float oldZoom = sTmMapZoom;
                sTmMapZoom *= (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
                sTmMapZoom = std::clamp(sTmMapZoom, 0.5f, 16.0f);
                // Zoom toward mouse
                ImVec2 mouse = ImGui::GetMousePos();
                float mx = mouse.x - cursor.x - sTmMapPanX;
                float mz = mouse.y - cursor.y - sTmMapPanY;
                float ratio = 1.0f - sTmMapZoom / oldZoom;
                sTmMapPanX += mx * ratio;
                sTmMapPanY += mz * ratio;
            }
            // Middle-mouse pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                sTmMapPanX += delta.x;
                sTmMapPanY += delta.y;
            }
        }

        // Cell size: fixed base of 48px * zoom
        float cellSz = 48.0f * sTmMapZoom;

        // Center the grid in the available area
        float gridW = tm.width * cellSz;
        float gridH = tm.height * cellSz;
        float ox = cursor.x + (availW - gridW) * 0.5f + sTmMapPanX;
        float oy = cursor.y + (availH - gridH) * 0.5f + sTmMapPanY;

        // Clip to panel
        ImVec2 clipMin = cursor;
        ImVec2 clipMax(pos.x + size.x, pos.y + size.y);
        dl->PushClipRect(clipMin, clipMax, true);

        // Draw tilemap cells — match window bg, subtle grey grid lines
        uint32_t gridLineCol = 0x55FFFFFF;   // semi-transparent white cell border
        for (int ty = 0; ty < tm.height; ty++)
        {
            for (int tx = 0; tx < tm.width; tx++)
            {
                float x0 = ox + tx * cellSz;
                float y0 = oy + ty * cellSz;
                float x1 = x0 + cellSz;
                float y1 = y0 + cellSz;

                // Skip if off-screen
                if (x1 < clipMin.x || x0 > clipMax.x ||
                    y1 < clipMin.y || y0 > clipMax.y)
                    continue;


                // Always draw dark grey background first
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), 0xFF1A1A1A);

                // Draw tile content on top (skip transparent/index-0 pixels)
                int ti = tm.tileIndices[ty * tm.width + tx];
                if (ti >= 0 && ti < (int)ts.tiles.size())
                {
                    const Tile8& tile = ts.tiles[ti];
                    float pxSz = cellSz / (float)kTileSize;
                    if (pxSz >= 2.0f)
                    {
                        for (int py = 0; py < kTileSize; py++)
                        {
                            for (int px = 0; px < kTileSize; px++)
                            {
                                uint8_t ci = tile.pixels[py * kTileSize + px];
                                if (ci == 0) continue; // transparent
                                uint32_t col = ts.palette[ci & 0xF];
                                ImVec2 pp0(x0 + px * pxSz, y0 + py * pxSz);
                                ImVec2 pp1(pp0.x + pxSz, pp0.y + pxSz);
                                dl->AddRectFilled(pp0, pp1, col);
                            }
                        }
                    }
                    else
                    {
                        uint8_t ci = tile.pixels[4 * kTileSize + 4];
                        if (ci != 0)
                            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ts.palette[ci & 0xF]);
                    }
                }

                // Draw sprite asset frame if any object with a sprite is on this cell
                bool cellPainted = false;
                for (const auto& obj : sTmObjects)
                {
                    // Tile objects use cells list; other objects use tileX/tileY
                    if (obj.type == TmObjType::Tile)
                    {
                        if (std::find(obj.cells.begin(), obj.cells.end(), std::make_pair(tx, ty)) == obj.cells.end())
                            continue;
                    }
                    else
                    {
                        // Non-Tile objects are drawn separately in the object loop below
                        // (with scaling, directional sprites, etc.) — skip them here
                        if (obj.type != TmObjType::Tile) continue;
                        if (obj.tileX != tx || obj.tileY != ty) continue;
                    }
                    if (obj.spriteAssetIdx < 0 || obj.spriteAssetIdx >= (int)sSpriteAssets.size()) continue;
                    // Use cached GL texture — one quad per cell
                    if (obj.spriteAssetIdx < (int)sTmSpriteTextures.size() && sTmSpriteTextures[obj.spriteAssetIdx])
                    {
                        dl->AddImage((ImTextureID)(uintptr_t)sTmSpriteTextures[obj.spriteAssetIdx],
                            ImVec2(x0, y0), ImVec2(x1, y1));
                        cellPainted = true;
                    }
                    else
                    {
                        // Fallback: colored label if no texture
                        uint32_t col = sTmObjTypeColors[(int)obj.type];
                        dl->AddRectFilled(ImVec2(x0 + 2, y0 + 2), ImVec2(x1 - 2, y1 - 2),
                            (col & 0x00FFFFFF) | 0x40000000);
                        if (cellSz >= 20.0f)
                        {
                            const char* lbl = obj.name[0] ? obj.name : "?";
                            ImVec2 tsz = ImGui::CalcTextSize(lbl);
                            dl->AddText(ImVec2(x0 + (cellSz - tsz.x) * 0.5f, y0 + (cellSz - tsz.y) * 0.5f), col, lbl);
                        }
                        cellPainted = true;
                    }
                    break;
                }

                // Border on unpainted cells only — lighter for sub-cells, medium every 2, heavy every 4
                if (!cellPainted)
                {
                    // Faint sub-cell border
                    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), 0x22FFFFFF);
                    // Medium lines every 2 cells
                    uint32_t medCol = 0x44FFFFFF;
                    if (tx % 2 == 0)
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y1), medCol, 1.0f);
                    if (ty % 2 == 0)
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), medCol, 1.0f);
                    if (tx == tm.width - 1 || (tx + 1) % 2 == 0)
                        dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y1), medCol, 1.0f);
                    if (ty == tm.height - 1 || (ty + 1) % 2 == 0)
                        dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), medCol, 1.0f);
                    // Heavy lines every 4 cells (original tile boundaries)
                    if (tx % 4 == 0)
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y1), gridLineCol, 1.0f);
                    if (ty % 4 == 0)
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y0), gridLineCol, 1.0f);
                    if (tx == tm.width - 1 || (tx + 1) % 4 == 0)
                        dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y1), gridLineCol, 1.0f);
                    if (ty == tm.height - 1 || (ty + 1) % 4 == 0)
                        dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), gridLineCol, 1.0f);
                }

            }
        }
        // Outer border around entire grid (foreground so it draws on top of textures)
        ImGui::GetForegroundDrawList()->AddRect(ImVec2(ox, oy), ImVec2(ox + gridW, oy + gridH), 0x80FFFFFF);

        // --- Draggable edges to expand/shrink the tilemap ---
        // Edge hit zones (wider than visual for easy grabbing)
        float grab = std::max(8.0f, cellSz * 0.3f);
        float edgeTop    = oy;
        float edgeBottom = oy + gridH;
        float edgeLeft   = ox;
        float edgeRight  = ox + gridW;

        ImVec2 mouse = ImGui::GetMousePos();
        bool inPanel = (mouse.x >= clipMin.x && mouse.x <= clipMax.x &&
                        mouse.y >= clipMin.y && mouse.y <= clipMax.y);

        // Detect which edge the mouse is near (for cursor + drag start) — edit mode only
        int hoverEdge = 0;
        if (sEditorMode != EditorMode::Play && inPanel && sTmDragEdge == 0)
        {
            // Top edge zone
            if (mouse.y >= edgeTop - grab && mouse.y <= edgeTop + grab &&
                mouse.x >= edgeLeft - grab && mouse.x <= edgeRight + grab)
                hoverEdge = 1;
            // Bottom edge zone
            else if (mouse.y >= edgeBottom - grab && mouse.y <= edgeBottom + grab &&
                     mouse.x >= edgeLeft - grab && mouse.x <= edgeRight + grab)
                hoverEdge = 2;
            // Left edge zone
            else if (mouse.x >= edgeLeft - grab && mouse.x <= edgeLeft + grab &&
                     mouse.y >= edgeTop - grab && mouse.y <= edgeBottom + grab)
                hoverEdge = 3;
            // Right edge zone
            else if (mouse.x >= edgeRight - grab && mouse.x <= edgeRight + grab &&
                     mouse.y >= edgeTop - grab && mouse.y <= edgeBottom + grab)
                hoverEdge = 4;
        }

        // Set cursor shape + highlight edges (edit mode only)
        if (sEditorMode != EditorMode::Play)
        {
            if (hoverEdge == 1 || hoverEdge == 2 || sTmDragEdge == 1 || sTmDragEdge == 2)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            else if (hoverEdge == 3 || hoverEdge == 4 || sTmDragEdge == 3 || sTmDragEdge == 4)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            if (hoverEdge == 1 || sTmDragEdge == 1)
                dl->AddLine(ImVec2(edgeLeft, edgeTop), ImVec2(edgeRight, edgeTop), 0xFF44AAFF, 3.0f);
            if (hoverEdge == 2 || sTmDragEdge == 2)
                dl->AddLine(ImVec2(edgeLeft, edgeBottom), ImVec2(edgeRight, edgeBottom), 0xFF44AAFF, 3.0f);
            if (hoverEdge == 3 || sTmDragEdge == 3)
                dl->AddLine(ImVec2(edgeLeft, edgeTop), ImVec2(edgeLeft, edgeBottom), 0xFF44AAFF, 3.0f);
            if (hoverEdge == 4 || sTmDragEdge == 4)
                dl->AddLine(ImVec2(edgeRight, edgeTop), ImVec2(edgeRight, edgeBottom), 0xFF44AAFF, 3.0f);
        }

        // Draw non-Tile objects on the grid
        for (int i = 0; i < (int)sTmObjects.size(); i++)
        {
            const TmObject& obj = sTmObjects[i];
            if (obj.type == TmObjType::Tile) continue; // tiles already rendered in cells

            float visTileX = (sEditorMode == EditorMode::Play && i < (int)sTmObjVisX.size())
                ? sTmObjVisX[i] : (float)obj.tileX;
            float visTileY = (sEditorMode == EditorMode::Play && i < (int)sTmObjVisY.size())
                ? sTmObjVisY[i] : (float)obj.tileY;
            float objX = ox + visTileX * cellSz;
            float objY = oy + visTileY * cellSz;
            float dscale = obj.displayScale > 0.0f ? obj.displayScale : 1.0f;
            float objSz = cellSz * dscale;
            // Center the scaled sprite on the object's tile
            float objDrawX = objX - (objSz - cellSz) * 0.5f;
            float objDrawY = objY - (objSz - cellSz) * 0.5f;
            uint32_t col = sTmObjTypeColors[(int)obj.type];
            bool isSel = (i == sTmSelectedObj);

            // Draw sprite texture if available, otherwise diamond marker
            bool drewSprite = false;
            GLuint drawTex = 0;
            // During play mode, use direction sprite if available
            if (sEditorMode == EditorMode::Play && i < (int)sTmObjFacing.size() &&
                obj.spriteAssetIdx >= 0 && obj.spriteAssetIdx < (int)sAssetDirSprites.size() &&
                !sAssetDirSprites[obj.spriteAssetIdx].empty())
            {
                int facing = sTmObjFacing[i];
                int animIdx = (i < (int)sTmObjAnimSet.size()) ? sTmObjAnimSet[i] : 0;
                auto& dirSets = sAssetDirSprites[obj.spriteAssetIdx];
                int dirSetIdx = 0;
                // Convert animIdx to sAssetDirSprites set index with frame cycling
                if (obj.spriteAssetIdx < (int)sSpriteAssets.size()) {
                    auto& asset = sSpriteAssets[obj.spriteAssetIdx];
                    if (animIdx >= 0 && animIdx < (int)asset.anims.size()) {
                        int base = GetAnimDirBase(asset, animIdx);
                        int frameCount = asset.anims[animIdx].endFrame;
                        if (frameCount > 0) {
                            int frame;
                            if (asset.anims[animIdx].stepAnim) {
                                // Step-based: advance one frame per tile step
                                int stepCount = (i < (int)sTmObjStepCount.size()) ? sTmObjStepCount[i] : 0;
                                frame = stepCount % frameCount;
                            } else {
                                // Time-based: continuous cycling
                                int fps = asset.anims[animIdx].fps > 0 ? asset.anims[animIdx].fps : 8;
                                float effectiveFps = fps * asset.anims[animIdx].speed;
                                frame = effectiveFps > 0.0f ? ((int)(sViewportAnimTime * effectiveFps)) % frameCount : 0;
                            }
                            dirSetIdx = base + frame;
                        }
                    }
                }
                if (dirSetIdx >= (int)dirSets.size()) dirSetIdx = 0;
                if (facing >= 0 && facing < 8)
                    drawTex = dirSets[dirSetIdx][facing].texture;
            }
            // Fallback to static sprite texture
            if (!drawTex && obj.spriteAssetIdx >= 0 && obj.spriteAssetIdx < (int)sTmSpriteTextures.size())
                drawTex = sTmSpriteTextures[obj.spriteAssetIdx];
            if (drawTex)
            {
                dl->AddImage((ImTextureID)(uintptr_t)drawTex,
                    ImVec2(objDrawX, objDrawY), ImVec2(objDrawX + objSz, objDrawY + objSz));
                if (isSel)
                    dl->AddRect(ImVec2(objDrawX, objDrawY), ImVec2(objDrawX + objSz, objDrawY + objSz),
                        0xFFFFFFFF, 0.0f, 0, 2.0f);
                drewSprite = true;
            }
            if (!drewSprite)
            {
                float cx = objDrawX + objSz * 0.5f;
                float cy = objDrawY + objSz * 0.5f;
                float r = objSz * 0.35f;
                ImVec2 diamond[4] = {
                    ImVec2(cx, cy - r), ImVec2(cx + r, cy),
                    ImVec2(cx, cy + r), ImVec2(cx - r, cy)
                };
                dl->AddConvexPolyFilled(diamond, 4, (col & 0x00FFFFFF) | 0x80000000);
                dl->AddPolyline(diamond, 4, isSel ? 0xFFFFFFFF : col, ImDrawFlags_Closed, isSel ? 2.5f : 1.5f);
            }

            // Label
            if (cellSz >= 20.0f)
            {
                const char* lbl = obj.name[0] ? obj.name : sTmObjTypeNames[(int)obj.type];
                ImVec2 textSz = ImGui::CalcTextSize(lbl);
                float lcx = objX + cellSz * 0.5f;
                float lcy = objY + cellSz;
                dl->AddText(ImVec2(lcx - textSz.x * 0.5f, lcy + 2), col, lbl);
            }
        }

        // Stamp mode indicator (edit mode only)
        if (sEditorMode != EditorMode::Play && sTmStampAsset >= 0 && sTmStampAsset < (int)sSpriteAssets.size())
        {
            char stampLbl[96];
            snprintf(stampLbl, sizeof(stampLbl), "Painting: %s  [%dx]  (Esc to stop, RMB erase, Tab cycle)",
                sSpriteAssets[sTmStampAsset].name.c_str(), sTmPaintScale);
            ImVec2 txtSz = ImGui::CalcTextSize(stampLbl);
            float lx = clipMin.x + (clipMax.x - clipMin.x - txtSz.x) * 0.5f;
            float ly = clipMax.y - txtSz.y - 8;
            dl->AddRectFilled(ImVec2(lx - 6, ly - 2), ImVec2(lx + txtSz.x + 6, ly + txtSz.y + 2), 0xCC000000);
            dl->AddText(ImVec2(lx, ly), 0xFF44AAFF, stampLbl);
        }

        dl->PopClipRect();

        // Invisible button for mouse interaction
        ImGui::SetCursorScreenPos(cursor);
        ImGui::InvisibleButton("##TmMapGrid", ImVec2(availW, availH));

        // --- Edge dragging logic ---
        // Track cumulative drag distance in pixels to know when to add/remove a row/col
        static float sDragAccum = 0.0f;

        if (ImGui::IsMouseClicked(0) && hoverEdge != 0)
        {
            sTmDragEdge = hoverEdge;
            sDragAccum = 0.0f;
        }

        if (sTmDragEdge != 0 && ImGui::IsMouseDown(0))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            float d = (sTmDragEdge <= 2) ? delta.y : delta.x;
            sDragAccum += d;

            // Helper lambda to resize tilemap, preserving content
            auto resizeTilemap = [&](int newW, int newH, int shiftX, int shiftY)
            {
                if (newW < 1 || newH < 1 || newW > 128 || newH > 128) return;
                std::vector<uint16_t> newMap(newW * newH, 0);
                for (int y = 0; y < tm.height && (y + shiftY) < newH; y++)
                {
                    if (y + shiftY < 0) continue;
                    for (int x = 0; x < tm.width && (x + shiftX) < newW; x++)
                    {
                        if (x + shiftX < 0) continue;
                        newMap[(y + shiftY) * newW + (x + shiftX)] =
                            tm.tileIndices[y * tm.width + x];
                    }
                }
                tm.tileIndices = std::move(newMap);
                tm.width = newW;
                tm.height = newH;
            };

            float threshold = cellSz;
            if (threshold < 8.0f) threshold = 8.0f;
            int step = sTmPaintScale > 1 ? sTmPaintScale : 1;
            int minDim = step;

            if (sTmDragEdge == 2) // bottom: drag down = add row, drag up = remove
            {
                while (sDragAccum >= threshold)
                {
                    resizeTilemap(tm.width, tm.height + step, 0, 0);
                    sDragAccum -= threshold;
                }
                while (sDragAccum <= -threshold && tm.height > minDim)
                {
                    resizeTilemap(tm.width, tm.height - step, 0, 0);
                    sDragAccum += threshold;
                }
            }
            else if (sTmDragEdge == 1) // top: drag up = add row at top, drag down = remove
            {
                while (sDragAccum <= -threshold)
                {
                    resizeTilemap(tm.width, tm.height + step, 0, step);
                    sTmMapPanY -= cellSz * 0.5f;
                    sDragAccum += threshold;
                }
                while (sDragAccum >= threshold && tm.height > minDim)
                {
                    resizeTilemap(tm.width, tm.height - step, 0, -step);
                    sTmMapPanY += cellSz * 0.5f;
                    sDragAccum -= threshold;
                }
            }
            else if (sTmDragEdge == 4) // right: drag right = add col, drag left = remove
            {
                while (sDragAccum >= threshold)
                {
                    resizeTilemap(tm.width + step, tm.height, 0, 0);
                    sDragAccum -= threshold;
                }
                while (sDragAccum <= -threshold && tm.width > minDim)
                {
                    resizeTilemap(tm.width - step, tm.height, 0, 0);
                    sDragAccum += threshold;
                }
            }
            else if (sTmDragEdge == 3) // left: drag left = add col at left, drag right = remove
            {
                while (sDragAccum <= -threshold)
                {
                    resizeTilemap(tm.width + step, tm.height, step, 0);
                    sTmMapPanX -= cellSz * 0.5f;
                    sDragAccum += threshold;
                }
                while (sDragAccum >= threshold && tm.width > minDim)
                {
                    resizeTilemap(tm.width - step, tm.height, -step, 0);
                    sTmMapPanX += cellSz * 0.5f;
                    sDragAccum -= threshold;
                }
            }
        }
        else
        {
            sTmDragEdge = 0;
        }

        // --- Stamp mode / Object dragging / Tile popup ---
        bool popupOpen = ImGui::IsPopupOpen("##TmPlaceObj");
        if (sTmDragEdge == 0 && !popupOpen)
        {
            int hoverTX = (int)std::floor((mouse.x - ox) / cellSz);
            int hoverTY = (int)std::floor((mouse.y - oy) / cellSz);
            bool inGrid = (hoverTX >= 0 && hoverTX < tm.width &&
                           hoverTY >= 0 && hoverTY < tm.height);

            // Cancel stamp mode with Escape
            if (sTmStampAsset >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                sTmStampAsset = -1;
                sTmStampObj = -1;
            }

            // Cycle paint scale with Tab while stamping (1→4→8→1)
            if (sTmStampAsset >= 0 && ImGui::IsKeyPressed(ImGuiKey_Tab))
            {
                if (sTmPaintScale <= 1) sTmPaintScale = 4;
                else if (sTmPaintScale <= 4) sTmPaintScale = 8;
                else sTmPaintScale = 1;
            }

            // Stamp paint mode: left-click adds cells, right-click removes cells
            if (sTmStampAsset >= 0 && sTmStampObj >= 0 && sTmStampObj < (int)sTmObjects.size() && inGrid)
            {
                TmObject& tileObj = sTmObjects[sTmStampObj];
                int ps = sTmPaintScale > 1 ? sTmPaintScale : 1;
                int mask = ~(ps - 1); // alignment mask (works for powers of 2)
                int bx = hoverTX & mask;
                int by = hoverTY & mask;
                if (ImGui::IsMouseDown(0) && ImGui::IsItemActive())
                {
                    for (int dy = 0; dy < ps; dy++)
                        for (int dx = 0; dx < ps; dx++)
                        {
                            std::pair<int,int> bc = {bx + dx, by + dy};
                            if (bc.first >= 0 && bc.first < tm.width && bc.second >= 0 && bc.second < tm.height)
                                if (std::find(tileObj.cells.begin(), tileObj.cells.end(), bc) == tileObj.cells.end())
                                    tileObj.cells.push_back(bc);
                        }
                }
                else if (ImGui::IsMouseDown(1))
                {
                    for (int dy = 0; dy < ps; dy++)
                        for (int dx = 0; dx < ps; dx++)
                        {
                            auto it = std::find(tileObj.cells.begin(), tileObj.cells.end(), std::make_pair(bx + dx, by + dy));
                            if (it != tileObj.cells.end())
                                tileObj.cells.erase(it);
                        }
                }
            }

            // Show crosshair cursor in stamp mode
            if (sTmStampAsset >= 0)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            // Check if mouse is over any object (for click-to-grab)
            int hoveredObj = -1;
            if (sTmStampAsset < 0 && inGrid && sTmDragObj < 0)
            {
                for (int i = (int)sTmObjects.size() - 1; i >= 0; i--)
                {
                    if (sTmObjects[i].type == TmObjType::Tile) continue; // tiles aren't dragged
                    float objCX = ox + sTmObjects[i].tileX * cellSz + cellSz * 0.5f;
                    float objCY = oy + sTmObjects[i].tileY * cellSz + cellSz * 0.5f;
                    float r = cellSz * 0.4f;
                    float dx = mouse.x - objCX, dy = mouse.y - objCY;
                    if (dx * dx + dy * dy <= r * r) { hoveredObj = i; break; }
                }
            }

            // Grab cursor when hovering an object
            if (sTmStampAsset < 0 && (hoveredObj >= 0 || sTmDragObj >= 0))
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            // Start dragging on click
            if (sTmStampAsset < 0 && ImGui::IsMouseClicked(0) && hoveredObj >= 0)
            {
                sTmDragObj = hoveredObj;
                sTmSelectedObj = hoveredObj;
            }

            // Continue dragging
            if (sTmDragObj >= 0)
            {
                if (ImGui::IsMouseDown(0))
                {
                    TmObject& obj = sTmObjects[sTmDragObj];
                    int snapX = std::clamp(hoverTX, 0, tm.width - 1);
                    int snapY = std::clamp(hoverTY, 0, tm.height - 1);
                    if (sTmPaintScale > 1) { int m = ~(sTmPaintScale - 1); snapX &= m; snapY &= m; }
                    obj.tileX = snapX;
                    obj.tileY = snapY;
                }
                else
                    sTmDragObj = -1;
            }
            // Click empty cell to open sprite instance picker (only when not stamping)
            else if (sTmStampAsset < 0 && hoveredObj < 0 && inGrid && ImGui::IsMouseClicked(0))
            {
                if (sTmPaintScale > 1) { int m = ~(sTmPaintScale - 1); sTmPlaceTX = hoverTX & m; sTmPlaceTY = hoverTY & m; }
                else { sTmPlaceTX = hoverTX; sTmPlaceTY = hoverTY; }
                ImGui::OpenPopup("##TmPlaceObj");
            }
        }

        if (ImGui::BeginPopup("##TmPlaceObj"))
        {
            if (sSpriteAssets.empty())
            {
                ImGui::TextDisabled("No sprite assets — add some in the Sprites tab");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Select Sprite");
                ImGui::Separator();
                for (int i = 0; i < (int)sSpriteAssets.size(); i++)
                {
                    char label[64];
                    snprintf(label, sizeof(label), "%s##sa%d", sSpriteAssets[i].name.c_str(), i);
                    if (ImGui::Selectable(label))
                    {
                        // Create Tile object with initial cells and enter stamp mode
                        TmObject obj;
                        obj.type = TmObjType::Tile;
                        obj.spriteAssetIdx = i;
                        {
                            int ps = sTmPaintScale > 1 ? sTmPaintScale : 1;
                            int m = ~(ps - 1);
                            int bx = sTmPlaceTX & m, by = sTmPlaceTY & m;
                            for (int dy = 0; dy < ps; dy++)
                                for (int dx = 0; dx < ps; dx++)
                                    obj.cells.push_back({bx + dx, by + dy});
                        }
                        snprintf(obj.name, sizeof(obj.name), "%s", sSpriteAssets[i].name.c_str());
                        sTmObjects.push_back(obj);
                        sTmSelectedObj = (int)sTmObjects.size() - 1;
                        sTmStampAsset = i;
                        sTmStampObj = sTmSelectedObj;
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
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

    // FPS counter (right-aligned)
    {
        static float fpsTimer = 0.0f;
        static int fpsFrames = 0;
        static float fpsDisplay = 0.0f;
        fpsTimer += ImGui::GetIO().DeltaTime;
        fpsFrames++;
        if (fpsTimer >= 0.5f) {
            fpsDisplay = fpsFrames / fpsTimer;
            fpsFrames = 0;
            fpsTimer = 0.0f;
        }
        char fpsBuf[32];
        snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", fpsDisplay);
        float textW = ImGui::CalcTextSize(fpsBuf).x;
        ImGui::SameLine(size.x - textW - Scaled(8));
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.5f, 1.0f), "%s", fpsBuf);
    }

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
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, sUndoCursor > 0))  UndoPop();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, sUndoCursor < sUndoCount))  RedoPush();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Grid Overlay", nullptr, nullptr);
            ImGui::MenuItem("Camera Bounds", nullptr, nullptr);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::RadioButton("NDS", sBuildTarget == BuildTarget::NDS))
            sBuildTarget = BuildTarget::NDS;
        ImGui::SameLine();
        if (ImGui::RadioButton("GBA", sBuildTarget == BuildTarget::GBA))
            sBuildTarget = BuildTarget::GBA;
        bool buildFromMenu = false;
        if (buildFromMenu || sBuildRequested)
        {
            sBuildRequested = false;
            // Save blueprint working set so export reads current nodes/links
            if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                sBlueprintAssets[sVsEditBlueprintIdx].nodes = sVsNodes;
                sBlueprintAssets[sVsEditBlueprintIdx].links = sVsLinks;
            }
            sPackaging = true;
            sPackageDone = false;
            sPackageSuccess = false;
            sPackageMsg = "Building...";

            namespace fs = std::filesystem;
            char exeBuf[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
            fs::path exeDir = fs::path(exeBuf).parent_path();
            fs::path cwdDir = fs::current_path();

            const char* rtName = (sBuildTarget == BuildTarget::NDS) ? "nds_runtime" : "gba_runtime";
            const char* rtExt  = (sBuildTarget == BuildTarget::NDS) ? "affinity.nds" : "affinity.gba";

            fs::path rtDir;
            for (auto& base : { exeDir, exeDir / "..", exeDir / ".." / "..", exeDir / ".." / ".." / "..", cwdDir, cwdDir / ".." })
            {
                fs::path candidate = base / rtName;
                if (fs::exists(candidate / "Makefile"))
                { rtDir = fs::canonical(candidate); break; }
            }

            if (rtDir.empty())
            {
                sPackaging = false;
                sPackageDone = true;
                sPackageSuccess = false;
                sPackageMsg = std::string("Cannot find ") + rtName + "/Makefile\n\nSearched from:\n  exe: " + exeDir.string() + "\n  cwd: " + cwdDir.string();
            }
            else
            {
                sPackageOutputPath = (rtDir / rtExt).string();
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
                    // Only Mesh-type sprites get a mesh reference; others force -1
                    se.meshIdx = (sSprites[i].type == SpriteType::Mesh) ? sSprites[i].meshIdx : -1;
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
                exportCam.jumpForce = sCamObj.jumpForce;
                exportCam.gravity = sCamObj.gravity;
                exportCam.maxFallSpeed = sCamObj.maxFallSpeed;
                exportCam.jumpCamLand = sCamObj.jumpCamLand;
                exportCam.jumpCamAir = sCamObj.jumpCamAir;
                exportCam.autoOrbitSpeed = sCamObj.autoOrbitSpeed;
                exportCam.jumpDampen = sCamObj.jumpDampen;
                exportCam.drawDistance = sCamObj.drawDistance;
                exportCam.smallTriCull = sCamObj.smallTriCull;
                exportCam.skipFloor = sCamObj.skipFloor;
                exportCam.coverageBuf = sCamObj.coverageBuf;

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

                // Collect mesh assets for export (skip for Mode 7 tab — uses hardware affine floor)
                std::vector<GBAMeshExport> exportMeshes;
                bool exportMode7 = (sActiveTab == EditorTab::Mode7);
                if (!exportMode7) for (const auto& ma : sMeshAssets)
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
                        me.objPosIdx.push_back(v.objPosIdx);
                        me.uvs.push_back(v.u);
                        me.uvs.push_back(v.v);
                    }
                    me.indices = ma.indices;
                    if (ma.useQuads)
                    {
                        me.quadIndices = ma.quadIndices;
                    }
                    else
                    {
                        // Fan-triangulate quads into the triangle index buffer
                        for (size_t qi = 0; qi + 4 <= ma.quadIndices.size(); qi += 4)
                        {
                            me.indices.push_back(ma.quadIndices[qi + 0]);
                            me.indices.push_back(ma.quadIndices[qi + 1]);
                            me.indices.push_back(ma.quadIndices[qi + 2]);
                            me.indices.push_back(ma.quadIndices[qi + 0]);
                            me.indices.push_back(ma.quadIndices[qi + 2]);
                            me.indices.push_back(ma.quadIndices[qi + 3]);
                        }
                    }
                    // Convert sprite color to RGB15 (use magenta as default)
                    me.colorRGB15 = 0x7C1F; // magenta
                    me.cullMode = (int)ma.cullMode;
                    me.exportMode = (int)ma.exportMode;
                    me.lit = ma.lit ? 1 : 0;
                    me.halfRes = ma.halfRes ? 1 : 0;
                    me.wireframe = ma.wireframe ? 1 : 0;
                    me.grayscale = ma.grayscale ? 1 : 0;
                    me.drawDistance = ma.drawDistance;
                    me.collision = ma.collision ? 1 : 0;
                    me.drawPriority = ma.drawPriority;
                    me.visible = ma.visible ? 1 : 0;
                    me.textured = ma.textured ? 1 : 0;
                    me.texW = ma.texW;
                    me.texH = ma.texH;
                    me.texPixels = ma.texturePixels;
                    // Convert texture palette RGBA8 -> RGB15
                    for (int pi = 0; pi < 16; pi++)
                    {
                        uint32_t c = ma.texturePalette[pi];
                        int r = (c & 0xFF) >> 3;
                        int g = ((c >> 8) & 0xFF) >> 3;
                        int b = ((c >> 16) & 0xFF) >> 3;
                        me.texPalette[pi] = (uint16_t)(r | (g << 5) | (b << 10));
                    }
                    exportMeshes.push_back(me);
                }

                float exportOrbitDist = sOrbitDist;
                BuildTarget target = sBuildTarget;

                // Build visual script export from node graph
                GBAScriptExport exportScript;
                for (auto& n : sVsNodes)
                {
                    GBAScriptNodeExport sn;
                    sn.id = n.id;
                    sn.type = (GBAScriptNodeType)(int)n.type;
                    for (int pi = 0; pi < 4; pi++) sn.paramInt[pi] = n.paramInt[pi];
                    memcpy(sn.customCode, n.customCode, sizeof(sn.customCode));
                    memcpy(sn.funcName, n.funcName, sizeof(sn.funcName));
                    exportScript.nodes.push_back(sn);
                }
                for (auto& l : sVsLinks)
                {
                    GBAScriptLinkExport sl;
                    sl.fromNodeId  = l.from.nodeId;
                    sl.fromPinType = l.from.pinType;
                    sl.fromPinIdx  = l.from.pinIdx;
                    sl.toNodeId    = l.to.nodeId;
                    sl.toPinType   = l.to.pinType;
                    sl.toPinIdx    = l.to.pinIdx;
                    exportScript.links.push_back(sl);
                }

                // Build blueprint exports
                std::vector<GBABlueprintExport> exportBlueprints;
                std::vector<GBABlueprintInstanceExport> exportBpInstances;
                for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                    const BlueprintAsset& bp = sBlueprintAssets[bi];
                    GBABlueprintExport bpEx;
                    bpEx.name = bp.name;
                    for (auto& n : bp.nodes) {
                        GBAScriptNodeExport sn;
                        sn.id = n.id;
                        sn.type = (GBAScriptNodeType)(int)n.type;
                        for (int pi = 0; pi < 4; pi++) sn.paramInt[pi] = n.paramInt[pi];
                        memcpy(sn.customCode, n.customCode, sizeof(sn.customCode));
                    memcpy(sn.funcName, n.funcName, sizeof(sn.funcName));
                        // Mark parameter-exposed nodes with sentinel in paramInt[3]
                        for (int pi = 0; pi < bp.paramCount; pi++)
                            if (bp.params[pi].sourceNodeId == n.id)
                                sn.paramInt[3] = -(pi + 1);
                        bpEx.script.nodes.push_back(sn);
                    }
                    for (auto& l : bp.links) {
                        GBAScriptLinkExport sl;
                        sl.fromNodeId = l.from.nodeId; sl.fromPinType = l.from.pinType; sl.fromPinIdx = l.from.pinIdx;
                        sl.toNodeId = l.to.nodeId; sl.toPinType = l.to.pinType; sl.toPinIdx = l.to.pinIdx;
                        bpEx.script.links.push_back(sl);
                    }
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        GBABlueprintParamExport pe;
                        pe.dataType = bp.params[pi].dataType;
                        pe.defaultValue = bp.params[pi].defaultInt;
                        bpEx.params.push_back(pe);
                    }
                    exportBlueprints.push_back(std::move(bpEx));
                }
                // Collect instances from sprites
                for (int si = 0; si < sSpriteCount; si++) {
                    const FloorSprite& sp = sSprites[si];
                    if (sp.blueprintIdx < 0 || sp.blueprintIdx >= (int)sBlueprintAssets.size()) continue;
                    const BlueprintAsset& bp = sBlueprintAssets[sp.blueprintIdx];
                    GBABlueprintInstanceExport inst;
                    inst.blueprintIdx = sp.blueprintIdx;
                    inst.spriteIdx = si;
                    inst.tmObjIdx = -1;
                    inst.paramCount = bp.paramCount;
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        inst.paramValues[pi] = bp.params[pi].defaultInt;
                        for (int oi = 0; oi < sp.instanceParamCount; oi++)
                            if (sp.instanceParams[oi].paramIdx == pi)
                                inst.paramValues[pi] = sp.instanceParams[oi].value;
                    }
                    exportBpInstances.push_back(inst);
                }
                // Skip TmObject blueprint instances — GBA runtime is 3D/Mode7 only;
                // tilemap blueprints would double-execute Walk/Sprint nodes and cause
                // the player to move too fast.
                // Collect scene-level blueprint instances (MapScenes)
                for (int si = 0; si < (int)sMapScenes.size(); si++) {
                    const MapScene& ms = sMapScenes[si];
                    if (ms.blueprintIdx < 0 || ms.blueprintIdx >= (int)sBlueprintAssets.size()) continue;
                    const BlueprintAsset& bp = sBlueprintAssets[ms.blueprintIdx];
                    GBABlueprintInstanceExport inst;
                    inst.blueprintIdx = ms.blueprintIdx;
                    inst.spriteIdx = -1;
                    inst.tmObjIdx = -1;
                    inst.paramCount = bp.paramCount;
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        inst.paramValues[pi] = bp.params[pi].defaultInt;
                        for (int oi = 0; oi < ms.instanceParamCount; oi++)
                            if (ms.instanceParams[oi].paramIdx == pi)
                                inst.paramValues[pi] = ms.instanceParams[oi].value;
                    }
                    exportBpInstances.push_back(inst);
                }
                // Skip TmScene blueprint instances — same reason as TmObjects above.

                std::thread([rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                             exportMeshes, exportOrbitDist, exportScript, exportBlueprints, exportBpInstances, target]() {
                    std::string err;
                    bool ok;
                    if (target == BuildTarget::NDS)
                        ok = PackageNDS(rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                                        exportMeshes, exportOrbitDist, err);
                    else
                        ok = PackageGBA(rtDirStr, outPath, exportSprites, exportAssets, exportCam,
                                        exportMeshes, exportOrbitDist, exportScript, exportBlueprints, exportBpInstances, err);
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
            sScriptStartRan = false;
            sScriptMoveSpeed = -1.0f;
            sScriptAutoOrbitSpeed = 0.0f;
            sPendingSceneSwitch = -1;
            sPendingSceneMode = 0;
            sActivePlayAnimNodeId = -1;
            sPlayAnimIdle = -1;
            sPlayAnimHeld = -1;
            sPlayAnimReleased = -1;
            sVsFiredNodes.clear();
            sVsLinkSurgeT.clear();
            sVsLinkSurgeRevT.clear();
            // Restore tilemap objects
            if (!sSavedTmObjects.empty()) {
                sTmObjects = sSavedTmObjects;
                sSavedTmObjects.clear();
            }
            // sOnStartFrames reset handled by sScriptStartRan = false
        }

        if (sEditorMode == EditorMode::Edit)
        {
            // ---- EDIT MODE: free camera ----
            float moveSpeed = 80.0f * dt;
            float rotSpeed  = 2.0f * dt;

            if (!sGrabMode && !sScaleMode)
            {
                if (ImGui::IsKeyDown(ImGuiKey_A))
                    sCamera.angle += rotSpeed;
                if (ImGui::IsKeyDown(ImGuiKey_D))
                    sCamera.angle -= rotSpeed;

                if (ImGui::IsKeyDown(ImGuiKey_W))
                {
                    sCamera.x -= sinf(sCamera.angle) * moveSpeed;
                    sCamera.z -= cosf(sCamera.angle) * moveSpeed;
                }
                bool sKeyUsedForScale = ImGui::IsKeyDown(ImGuiKey_S)
                    && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount;
                if (ImGui::IsKeyDown(ImGuiKey_S) && !sKeyUsedForScale)
                {
                    sCamera.x += sinf(sCamera.angle) * moveSpeed;
                    sCamera.z += cosf(sCamera.angle) * moveSpeed;
                }
            }

            if (ImGui::IsKeyDown(ImGuiKey_I))
                sCamera.horizon = std::min(120.0f, sCamera.horizon + 60.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_K))
                sCamera.horizon = std::max(10.0f, sCamera.horizon - 60.0f * dt);

            if (ImGui::IsKeyDown(ImGuiKey_Q))
                sCamera.height = std::max(4.0f, sCamera.height - 40.0f * dt);
            if (ImGui::IsKeyDown(ImGuiKey_E))
                sCamera.height = std::min(256.0f, sCamera.height + 40.0f * dt);

            // Cancel grab if sprite deselected
            if (sGrabMode && (sSelectedSprite < 0 || sSelectedSprite >= sSpriteCount))
                sGrabMode = false;

            bool wantKeys = !ImGui::GetIO().WantCaptureKeyboard || sGrabMode;

            // Ctrl+Z / Ctrl+Y: undo/redo
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
                UndoPop();
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
                RedoPush();

            // G key: enter grab mode (Blender-style translate)
            if (wantKeys && ImGui::IsKeyPressed(ImGuiKey_G) && !sGrabMode && !sScaleMode
                && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                UndoPush(sSelectedSprite, sSprites[sSelectedSprite]);
                sGrabMode = true;
                sGrabAxis = 0;
                sGrabOrigX = sSprites[sSelectedSprite].x;
                sGrabOrigY = sSprites[sSelectedSprite].y;
                sGrabOrigZ = sSprites[sSelectedSprite].z;

                // Store GBA-space mouse position at grab start
                ImVec2 mouse = ImGui::GetMousePos();
                sGrabStartGbaX = (mouse.x - sVPImgPos.x) / sVPImgScale;
                sGrabStartGbaY = (mouse.y - sVPImgPos.y) / sVPImgScale;
            }

            // Grab mode active — handle axis constraint, mouse movement, confirm/cancel
            sTransformAxis = 0;
            if (sGrabMode && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                FloorSprite& gsp = sSprites[sSelectedSprite];

                // X/Y/Z to constrain axis (resets mouse anchor)
                auto resetGrabMouse = [&]() {
                    gsp.x = sGrabOrigX; gsp.y = sGrabOrigY; gsp.z = sGrabOrigZ;
                    ImVec2 m = ImGui::GetMousePos();
                    sGrabStartGbaX = (m.x - sVPImgPos.x) / sVPImgScale;
                    sGrabStartGbaY = (m.y - sVPImgPos.y) / sVPImgScale;
                };
                if (ImGui::IsKeyPressed(ImGuiKey_X)) {
                    sGrabAxis = (sGrabAxis == 'X') ? (char)0 : 'X'; resetGrabMouse();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
                    sGrabAxis = (sGrabAxis == 'Y') ? (char)0 : 'Y'; resetGrabMouse();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
                    sGrabAxis = (sGrabAxis == 'Z') ? (char)0 : 'Z'; resetGrabMouse();
                }

                // Show axis gizmo
                if (sGrabAxis) sTransformAxis = sGrabAxis;

                // Mouse position in GBA coordinates
                ImVec2 mpos = ImGui::GetMousePos();
                float gbaX = (mpos.x - sVPImgPos.x) / sVPImgScale;
                float gbaY = (mpos.y - sVPImgPos.y) / sVPImgScale;
                float dgbaX = gbaX - sGrabStartGbaX;
                float dgbaY = gbaY - sGrabStartGbaY;

                // Inverse Mode 7: compute world-space delta from GBA-pixel delta
                // At the sprite's depth, 1 GBA pixel = (depth / fov) world units laterally
                float dxw = sGrabOrigX - sCamera.x;
                float dzw = sGrabOrigZ - sCamera.z;
                float cosA = cosf(-sCamera.angle), sinA = sinf(-sCamera.angle);
                float depth = dxw * sinA - dzw * cosA;
                if (depth < 1.0f) depth = 1.0f;
                float lateralScale = depth / sCamera.fov;  // world per GBA pixel (horizontal)
                float verticalScale = lateralScale;         // same ratio for vertical

                // For single-axis constraints, use both mouse axes projected onto that world axis
                // Camera right vector = (cosA, sinA), camera forward = (-sinA, cosA) in XZ
                if (sGrabAxis == 'X')
                    gsp.x = sGrabOrigX + (dgbaX * cosA - dgbaY * sinA) * lateralScale;
                else if (sGrabAxis == 'Y')
                    gsp.y = std::max(0.0f, sGrabOrigY - dgbaY * verticalScale);
                else if (sGrabAxis == 'Z')
                    gsp.z = sGrabOrigZ + (dgbaX * sinA + dgbaY * cosA) * lateralScale;
                else
                {
                    // Free XZ: GBA X maps to world side, GBA Y maps to world forward
                    gsp.x = sGrabOrigX + dgbaX * cosA * lateralScale - dgbaY * sinA * lateralScale;
                    gsp.z = sGrabOrigZ + dgbaX * sinA * lateralScale + dgbaY * cosA * lateralScale;
                }

                // Left click confirms
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    sGrabMode = false;

                // Escape cancels
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    gsp.x = sGrabOrigX;
                    gsp.y = sGrabOrigY;
                    gsp.z = sGrabOrigZ;
                    sGrabMode = false;
                }
            }

            // S key: enter scale mode (mouse Y to scale, like G for grab)
            if (wantKeys && ImGui::IsKeyPressed(ImGuiKey_S) && !sGrabMode && !sScaleMode
                && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                UndoPush(sSelectedSprite, sSprites[sSelectedSprite]);
                sScaleMode = true;
                sScaleOrig = sSprites[sSelectedSprite].scale;
                sScaleMouseStartY = ImGui::GetMousePos().y;
            }

            if (sScaleMode && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                sTransformAxis = 'S';
                float dy = sScaleMouseStartY - ImGui::GetMousePos().y; // up = bigger
                sSprites[sSelectedSprite].scale = std::clamp(
                    sScaleOrig + dy * 0.02f, 0.1f, 50.0f);

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    sScaleMode = false;

                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    sSprites[sSelectedSprite].scale = sScaleOrig;
                    sScaleMode = false;
                }
            }

            // R + mouse drag up/down to resize selected sprite
            if (ImGui::IsKeyDown(ImGuiKey_R) && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
            {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    if (!sRDragUndoPushed)
                    {
                        UndoPush(sSelectedSprite, sSprites[sSelectedSprite]);
                        sRDragUndoPushed = true;
                    }
                    float dragY = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y;
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                    sSprites[sSelectedSprite].scale = std::clamp(
                        sSprites[sSelectedSprite].scale - dragY * 0.1f, 0.1f, 50.0f);
                }
            }
            if (!ImGui::IsKeyDown(ImGuiKey_R) || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                sRDragUndoPushed = false;

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
            // ---- PLAY MODE: node-graph-driven third-person orbit camera ----
            // Interpret the visual script node graph to drive gameplay, matching
            // the C code generated for the GBA runtime.

            if (sPlayTab != EditorTab::Tilemap)
            {
            // Key mapping: Editor keyboard → GBA button indices
            // 0=A, 1=B, 2=L, 3=R, 4=Start, 5=Select, 6=Up, 7=Down, 8=Left, 9=Right
            auto editorKeyDown = [](int gbaKey) -> bool {
                switch (gbaKey) {
                case 0: return ImGui::IsKeyDown(ImGuiKey_Space);
                case 1: return ImGui::IsKeyDown(ImGuiKey_LeftShift);
                case 2: return ImGui::IsKeyDown(ImGuiKey_J);
                case 3: return ImGui::IsKeyDown(ImGuiKey_L);
                case 4: return ImGui::IsKeyDown(ImGuiKey_Enter);
                case 5: return ImGui::IsKeyDown(ImGuiKey_Backspace);
                case 6: return ImGui::IsKeyDown(ImGuiKey_W);
                case 7: return ImGui::IsKeyDown(ImGuiKey_S);
                case 8: return ImGui::IsKeyDown(ImGuiKey_A);
                case 9: return ImGui::IsKeyDown(ImGuiKey_D);
                default: return false;
                }
            };
            auto editorKeyHit = [](int gbaKey) -> bool {
                switch (gbaKey) {
                case 0: return ImGui::IsKeyPressed(ImGuiKey_Space);
                case 1: return ImGui::IsKeyPressed(ImGuiKey_LeftShift);
                case 2: return ImGui::IsKeyPressed(ImGuiKey_J);
                case 3: return ImGui::IsKeyPressed(ImGuiKey_L);
                case 4: return ImGui::IsKeyPressed(ImGuiKey_Enter);
                case 5: return ImGui::IsKeyPressed(ImGuiKey_Backspace);
                case 6: return ImGui::IsKeyPressed(ImGuiKey_W);
                case 7: return ImGui::IsKeyPressed(ImGuiKey_S);
                case 8: return ImGui::IsKeyPressed(ImGuiKey_A);
                case 9: return ImGui::IsKeyPressed(ImGuiKey_D);
                default: return false;
                }
            };

            float rotSpeed = 3.0f * dt;

            // --- Node graph interpreter helpers ---
            auto findNodePlay = [&](int id) -> const VsNode* {
                for (auto& n : sVsNodes) if (n.id == id) return &n;
                return nullptr;
            };
            auto findDataInPlay = [&](int nodeId, int pinIdx) -> const VsNode* {
                for (auto& lk : sVsLinks)
                    if (lk.to.nodeId == nodeId && lk.to.pinType == 3 && lk.to.pinIdx == pinIdx)
                        return findNodePlay(lk.from.nodeId);
                return nullptr;
            };
            auto resolveIntPlay = [](const VsNode* dn) -> int {
                return dn ? dn->paramInt[0] : 0;
            };
            auto resolveFloatPlay = [](const VsNode* dn) -> float {
                if (!dn) return 0.0f;
                float fv; memcpy(&fv, &dn->paramInt[0], sizeof(float)); return fv;
            };
            auto resolveEventKeyPlay = [&](const VsNode& ev) -> int {
                int count = 0; int keyVal = -1;
                for (auto& lk : sVsLinks)
                    if (lk.to.nodeId == ev.id && lk.to.pinType == 3 && lk.to.pinIdx == 0) {
                        auto* dn = findNodePlay(lk.from.nodeId);
                        if (dn) { keyVal = dn->paramInt[0]; count++; }
                    }
                if (count == 1) return keyVal;
                if (count == 0) return ev.paramInt[0];
                return -1;
            };
            auto collectActionsPlay = [&](int startNodeId) -> std::vector<const VsNode*> {
                std::vector<const VsNode*> acts;
                std::vector<int> front;
                std::vector<bool> vis(10000, false);
                for (auto& lk : sVsLinks)
                    if (lk.from.nodeId == startNodeId && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                        front.push_back(lk.to.nodeId);
                int safety = 0;
                while (!front.empty() && safety < 256) {
                    int nid = front.front(); front.erase(front.begin());
                    if (nid < 0 || nid >= (int)vis.size() || vis[nid]) continue;
                    vis[nid] = true; safety++;
                    auto* an = findNodePlay(nid); if (!an) continue;
                    acts.push_back(an);
                    for (auto& lk : sVsLinks)
                        if (lk.from.nodeId == an->id && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                            front.push_back(lk.to.nodeId);
                }
                return acts;
            };

            // Script state for this frame
            float scInputFwd = 0.0f, scInputRight = 0.0f;
            float scOrbitDelta = 0.0f;

            // Action interpreter
            // execEventType tracks which event type is currently driving actions
            VsNodeType execEventType = VsNodeType::Group; // sentinel
            auto execActionPlay = [&](const VsNode* action) {
                VsNodeType t = action->type;
                if (t == VsNodeType::MovePlayer) {
                    auto* dd = findDataInPlay(action->id, 0);
                    int dir = dd ? dd->paramInt[0] : 0;
                    int dirKeys[] = { 8, 9, 6, 7 }; // Left,Right,Up,Down
                    if (dir >= 0 && dir < 4 && editorKeyDown(dirKeys[dir])) {
                        if (dir == 0) scInputRight -= 1.0f;
                        if (dir == 1) scInputRight += 1.0f;
                        if (dir == 2) scInputFwd   += 1.0f;
                        if (dir == 3) scInputFwd   -= 1.0f;
                    }
                }
                else if (t == VsNodeType::Walk) {
                    auto* sd = findDataInPlay(action->id, 0);
                    sScriptMoveSpeed = sd ? (float)resolveIntPlay(sd) : sCamObj.walkSpeed;
                }
                else if (t == VsNodeType::Sprint) {
                    auto* sd = findDataInPlay(action->id, 0);
                    sScriptMoveSpeed = sd ? (float)resolveIntPlay(sd) : sCamObj.sprintSpeed;
                }
                else if (t == VsNodeType::OrbitCamera) {
                    auto* dd = findDataInPlay(action->id, 0);
                    auto* sd = findDataInPlay(action->id, 1);
                    int dir = dd ? dd->paramInt[0] : 1;
                    int speed = sd ? resolveIntPlay(sd) : 512;
                    int key = (dir == 0) ? 2 : 3;
                    if (editorKeyDown(key)) {
                        float radPerFrame = (float)speed / 65536.0f * 6.28318f;
                        scOrbitDelta += (dir == 0) ? -radPerFrame : radPerFrame;
                    }
                }
                else if (t == VsNodeType::AutoOrbit) {
                    auto* sd = findDataInPlay(action->id, 0);
                    sScriptAutoOrbitSpeed = sd ? (float)resolveIntPlay(sd) : 205.0f;
                }
                else if (t == VsNodeType::ChangeScene) {
                    auto* sd = findDataInPlay(action->id, 0);
                    int scIdx = sd ? resolveIntPlay(sd) : action->paramInt[0];
                    sPendingSceneSwitch = scIdx;
                    sPendingSceneMode = action->paramInt[1];
                }
                // Jump, DampenJump, SetGravity, SetMaxFall — editor has no vertical physics
                else if (t == VsNodeType::PlayAnim) {
                    if (execEventType == VsNodeType::OnStart)
                        sPlayAnimIdle = action->id;
                    else if (execEventType == VsNodeType::OnKeyHeld)
                        sPlayAnimHeld = action->id;
                    else if (execEventType == VsNodeType::OnKeyReleased)
                        sPlayAnimReleased = action->id;
                }
            };

            // OnStart: run once when entering Play mode, keep firing for a few frames
            // so the surge animation has time to be picked up by drawing code
            static int sOnStartFrames = 0;
            if (!sScriptStartRan) {
                sScriptStartRan = true;
                sOnStartFrames = 5; // keep OnStart surges alive for 5 frames
                sScriptMoveSpeed = sCamObj.walkSpeed;
                execEventType = VsNodeType::OnStart;
                for (auto& ev : sVsNodes) {
                    if (ev.type != VsNodeType::OnStart) continue;
                    auto acts = collectActionsPlay(ev.id);
                    for (auto* a : acts) execActionPlay(a);
                }
                execEventType = VsNodeType::Group;
            }
            sVsFiredNodes.clear();
            // Re-inject OnStart node IDs for a few frames to guarantee surge pickup
            if (sOnStartFrames > 0) {
                sOnStartFrames--;
                for (auto& ev : sVsNodes) {
                    if (ev.type != VsNodeType::OnStart) continue;
                    auto acts = collectActionsPlay(ev.id);
                    sVsFiredNodes.push_back(ev.id);
                    for (auto* a : acts) {
                        sVsFiredNodes.push_back(a->id);
                        // Directly surge exec links from event to actions
                        for (int li = 0; li < (int)sVsLinks.size(); li++)
                            if (sVsLinks[li].to.nodeId == a->id && sVsLinks[li].to.pinType == 1) {
                                auto it = sVsLinkSurgeT.find(li);
                                if (it == sVsLinkSurgeT.end() || it->second > 1.0f)
                                    sVsLinkSurgeT[li] = 0.0f;
                            }
                        // Data inputs to PlayAnim nodes get reverse surge
                        if (a->type == VsNodeType::PlayAnim) {
                            for (int li = 0; li < (int)sVsLinks.size(); li++)
                                if (sVsLinks[li].to.nodeId == a->id && sVsLinks[li].to.pinType == 3) {
                                    auto it = sVsLinkSurgeRevT.find(li);
                                    if (it == sVsLinkSurgeRevT.end() || it->second > 1.0f)
                                        sVsLinkSurgeRevT[li] = 0.0f;
                                }
                        } else {
                            for (auto& lk : sVsLinks)
                                if (lk.to.nodeId == a->id && lk.to.pinType == 3)
                                    sVsFiredNodes.push_back(lk.from.nodeId);
                        }
                    }
                }
            }

            // Continuously surge the active PlayAnim node and its entire chain
            if (sActivePlayAnimNodeId >= 0) {
                sVsFiredNodes.push_back(sActivePlayAnimNodeId);
                // Continuously reverse-surge data inputs (PlayAnim → Animation)
                for (int li = 0; li < (int)sVsLinks.size(); li++)
                    if (sVsLinks[li].to.nodeId == sActivePlayAnimNodeId && sVsLinks[li].to.pinType == 3) {
                        auto it = sVsLinkSurgeRevT.find(li);
                        if (it == sVsLinkSurgeRevT.end() || it->second > 1.0f)
                            sVsLinkSurgeRevT[li] = 0.0f;
                    }
                // Walk backwards through exec links to find the chain that triggered it
                // Stop before event nodes (OnStart, OnKeyPressed, etc.) — they pulse on their own
                int cur = sActivePlayAnimNodeId;
                for (int safety = 0; safety < 20; safety++) {
                    int prev = -1;
                    for (auto& lk : sVsLinks) {
                        if (lk.to.nodeId == cur && lk.to.pinType == 1) {
                            prev = lk.from.nodeId;
                            break;
                        }
                    }
                    if (prev < 0) break;
                    // Don't continuously surge event nodes — they fire once
                    VsNodeType prevType = VsNodeType::Group;
                    for (auto& n : sVsNodes)
                        if (n.id == prev) { prevType = n.type; break; }
                    if (prevType == VsNodeType::OnStart || prevType == VsNodeType::OnKeyPressed ||
                        prevType == VsNodeType::OnKeyReleased)
                        break;
                    sVsFiredNodes.push_back(prev);
                    // Also fire data nodes feeding into this predecessor
                    for (auto& lk : sVsLinks)
                        if (lk.to.nodeId == prev && lk.to.pinType == 3)
                            sVsFiredNodes.push_back(lk.from.nodeId);
                    cur = prev;
                }
            }

            // OnUpdate: fires every frame
            for (auto& ev : sVsNodes) {
                if (ev.type != VsNodeType::OnUpdate) continue;
                auto acts = collectActionsPlay(ev.id);
                execEventType = VsNodeType::OnUpdate;
                for (auto* a : acts) execActionPlay(a);
                execEventType = VsNodeType::Group;
                // Surge
                sVsFiredNodes.push_back(ev.id);
                for (auto* a : acts) {
                    sVsFiredNodes.push_back(a->id);
                    for (auto& lk : sVsLinks)
                        if (lk.to.nodeId == a->id && lk.to.pinType == 3)
                            sVsFiredNodes.push_back(lk.from.nodeId);
                }
            }

            // Track previous key state for release edge detection (10 GBA keys)
            static bool sPrevKeyState[10] = {};

            // Walk all event nodes, track which fire for surge animation
            for (auto& ev : sVsNodes)
            {
                if (ev.type != VsNodeType::OnKeyHeld &&
                    ev.type != VsNodeType::OnKeyPressed &&
                    ev.type != VsNodeType::OnKeyReleased)
                    continue;
                auto acts = collectActionsPlay(ev.id);
                if (acts.empty()) continue;

                int key = resolveEventKeyPlay(ev);
                bool shouldFire = false;
                bool shouldSurge = false;
                if (ev.type == VsNodeType::OnKeyHeld) {
                    if (key >= 0) {
                        shouldFire = editorKeyDown(key);
                        shouldSurge = shouldFire;
                    } else {
                        // Ambiguous key — always run actions (they have own checks),
                        // but only surge if any d-pad or shoulder key is actually pressed
                        shouldFire = true;
                        shouldSurge = editorKeyDown(6) || editorKeyDown(7) ||
                                      editorKeyDown(8) || editorKeyDown(9) ||
                                      editorKeyDown(2) || editorKeyDown(3);
                    }
                }
                else if (ev.type == VsNodeType::OnKeyPressed) {
                    shouldFire = (key >= 0) && editorKeyHit(key);
                    shouldSurge = shouldFire;
                }
                else if (ev.type == VsNodeType::OnKeyReleased) {
                    if (key >= 0 && key < 10) {
                        // Single key: edge-triggered release
                        shouldFire = sPrevKeyState[key] && !editorKeyDown(key);
                        shouldSurge = shouldFire;
                    } else {
                        // Multiple keys connected — fire if ANY was just released
                        for (auto& lk : sVsLinks) {
                            if (lk.to.nodeId == ev.id && lk.to.pinType == 3) {
                                auto* kn = findNodePlay(lk.from.nodeId);
                                if (kn && kn->type == VsNodeType::Key) {
                                    int k = kn->paramInt[0];
                                    if (k >= 0 && k < 10 && sPrevKeyState[k] && !editorKeyDown(k)) {
                                        shouldFire = true;
                                        shouldSurge = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                if (shouldFire) {
                    execEventType = ev.type;
                    for (auto* a : acts) execActionPlay(a);
                    execEventType = VsNodeType::Group;
                }
                if (shouldSurge) {
                    bool anyActionSurged = false;
                    for (auto* a : acts) {
                        // For actions with built-in key checks, only surge if their key is held
                        bool actionSurge = true;
                        if (a->type == VsNodeType::MovePlayer) {
                            auto* dd = findDataInPlay(a->id, 0);
                            int dir = dd ? dd->paramInt[0] : 0;
                            int dirKeys[] = { 8, 9, 6, 7 }; // Left,Right,Up,Down
                            actionSurge = (dir >= 0 && dir < 4 && editorKeyDown(dirKeys[dir]));
                        } else if (a->type == VsNodeType::OrbitCamera) {
                            auto* dd = findDataInPlay(a->id, 0);
                            int dir = dd ? dd->paramInt[0] : 1;
                            actionSurge = editorKeyDown((dir == 0) ? 2 : 3);
                        }
                        if (!actionSurge) continue;
                        anyActionSurged = true;
                        sVsFiredNodes.push_back(a->id);
                        // Directly surge the exec link(s) leading to this action
                        for (int li = 0; li < (int)sVsLinks.size(); li++)
                            if (sVsLinks[li].to.nodeId == a->id && sVsLinks[li].to.pinType == 1) {
                                auto it = sVsLinkSurgeT.find(li);
                                if (it == sVsLinkSurgeT.end() || it->second > 1.0f)
                                    sVsLinkSurgeT[li] = 0.0f;
                            }
                        // PlayAnim data inputs get reverse surge; others forward
                        if (a->type == VsNodeType::PlayAnim) {
                            for (int li = 0; li < (int)sVsLinks.size(); li++)
                                if (sVsLinks[li].to.nodeId == a->id && sVsLinks[li].to.pinType == 3) {
                                    auto it = sVsLinkSurgeRevT.find(li);
                                    if (it == sVsLinkSurgeRevT.end() || it->second > 1.0f)
                                        sVsLinkSurgeRevT[li] = 0.0f;
                                }
                        } else {
                            for (auto& lk : sVsLinks)
                                if (lk.to.nodeId == a->id && lk.to.pinType == 3)
                                    sVsFiredNodes.push_back(lk.from.nodeId);
                        }
                    }
                    // Only surge Key data nodes if any action actually surged
                    // (don't add event node to sVsFiredNodes — we manually surged specific exec links above)
                    if (anyActionSurged) {
                        // Only surge Key nodes whose key is actually pressed
                        for (auto& lk : sVsLinks) {
                            if (lk.to.nodeId == ev.id && lk.to.pinType == 3) {
                                auto* kn = findNodePlay(lk.from.nodeId);
                                if (kn && kn->type == VsNodeType::Key) {
                                    if (editorKeyDown(kn->paramInt[0]))
                                        sVsFiredNodes.push_back(lk.from.nodeId);
                                } else {
                                    sVsFiredNodes.push_back(lk.from.nodeId);
                                }
                            }
                        }
                    }
                }
            }

            // --- Collision detection ---
            int collidedSprite = -1;
            {
                int pIdx = -1;
                for (int i = 0; i < sSpriteCount; i++)
                    if (sSprites[i].type == SpriteType::Player) { pIdx = i; break; }
                if (pIdx >= 0) {
                    float px = sSprites[pIdx].x, pz = sSprites[pIdx].z;
                    float collisionRadius = 1.5f; // world units
                    float cr2 = collisionRadius * collisionRadius;
                    for (int ci = 0; ci < sSpriteCount; ci++) {
                        if (ci == pIdx) continue;
                        float dx = px - sSprites[ci].x;
                        float dz = pz - sSprites[ci].z;
                        if (dx * dx + dz * dz < cr2) {
                            collidedSprite = ci;
                            break;
                        }
                    }
                }
            }

            // --- OnCollision scene script events ---
            if (collidedSprite >= 0) {
                for (auto& ev : sVsNodes) {
                    if (ev.type != VsNodeType::OnCollision) continue;
                    auto acts = collectActionsPlay(ev.id);
                    execEventType = VsNodeType::OnCollision;
                    for (auto* a : acts) execActionPlay(a);
                    execEventType = VsNodeType::Group;
                }
            }

            // --- Blueprint instance scripts ---
            // For each sprite with a blueprint, run the blueprint's events using instance params
            for (int si = 0; si < sSpriteCount; si++) {
                const FloorSprite& sp = sSprites[si];
                if (sp.blueprintIdx < 0 || sp.blueprintIdx >= (int)sBlueprintAssets.size()) continue;
                const BlueprintAsset& bp = sBlueprintAssets[sp.blueprintIdx];
                if (bp.nodes.empty()) continue;

                // Build a temporary copy of nodes with parameter overrides applied
                std::vector<VsNode> bpNodes = bp.nodes;
                for (int pi = 0; pi < bp.paramCount; pi++) {
                    int overrideVal = bp.params[pi].defaultInt;
                    for (int oi = 0; oi < sp.instanceParamCount; oi++)
                        if (sp.instanceParams[oi].paramIdx == pi)
                            overrideVal = sp.instanceParams[oi].value;
                    // Apply override to the source data node
                    for (auto& n : bpNodes)
                        if (n.id == bp.params[pi].sourceNodeId)
                            n.paramInt[bp.params[pi].sourceParamIdx] = overrideVal;
                }

                // Lambdas operating on blueprint nodes/links
                auto bpFindNode = [&](int id) -> const VsNode* {
                    for (auto& n : bpNodes) if (n.id == id) return &n;
                    return nullptr;
                };
                auto bpFindDataIn = [&](int nodeId, int pinIdx) -> const VsNode* {
                    for (auto& lk : bp.links)
                        if (lk.to.nodeId == nodeId && lk.to.pinType == 3 && lk.to.pinIdx == pinIdx)
                            return bpFindNode(lk.from.nodeId);
                    return nullptr;
                };
                auto bpCollectActions = [&](int startNodeId) -> std::vector<const VsNode*> {
                    std::vector<const VsNode*> acts;
                    std::vector<int> front;
                    std::vector<bool> vis(10000, false);
                    for (auto& lk : bp.links)
                        if (lk.from.nodeId == startNodeId && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                            front.push_back(lk.to.nodeId);
                    int safety = 0;
                    while (!front.empty() && safety < 256) {
                        int nid = front.front(); front.erase(front.begin());
                        if (nid < 0 || nid >= (int)vis.size() || vis[nid]) continue;
                        vis[nid] = true; safety++;
                        auto* an = bpFindNode(nid); if (!an) continue;
                        acts.push_back(an);
                        for (auto& lk : bp.links)
                            if (lk.from.nodeId == an->id && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                                front.push_back(lk.to.nodeId);
                    }
                    return acts;
                };
                auto bpResolveEventKey = [&](const VsNode& ev) -> int {
                    int count = 0; int keyVal = -1;
                    for (auto& lk : bp.links)
                        if (lk.to.nodeId == ev.id && lk.to.pinType == 3 && lk.to.pinIdx == 0) {
                            auto* dn = bpFindNode(lk.from.nodeId);
                            if (dn) { keyVal = dn->paramInt[0]; count++; }
                        }
                    if (count == 1) return keyVal;
                    if (count == 0) return ev.paramInt[0];
                    return -1;
                };
                auto bpExecAction = [&](const VsNode* action) {
                    VsNodeType t = action->type;
                    if (t == VsNodeType::MovePlayer) {
                        auto* dd = bpFindDataIn(action->id, 0);
                        int dir = dd ? dd->paramInt[0] : 0;
                        int dirKeys[] = { 8, 9, 6, 7 };
                        if (dir >= 0 && dir < 4 && editorKeyDown(dirKeys[dir])) {
                            if (dir == 0) scInputRight -= 1.0f;
                            if (dir == 1) scInputRight += 1.0f;
                            if (dir == 2) scInputFwd   += 1.0f;
                            if (dir == 3) scInputFwd   -= 1.0f;
                        }
                    }
                    else if (t == VsNodeType::Walk) {
                        auto* sd = bpFindDataIn(action->id, 0);
                        sScriptMoveSpeed = sd ? (float)sd->paramInt[0] : sCamObj.walkSpeed;
                    }
                    else if (t == VsNodeType::Sprint) {
                        auto* sd = bpFindDataIn(action->id, 0);
                        sScriptMoveSpeed = sd ? (float)sd->paramInt[0] : sCamObj.sprintSpeed;
                    }
                    else if (t == VsNodeType::OrbitCamera) {
                        auto* dd = bpFindDataIn(action->id, 0);
                        auto* sd = bpFindDataIn(action->id, 1);
                        int dir = dd ? dd->paramInt[0] : 1;
                        int speed = sd ? sd->paramInt[0] : 512;
                        int key = (dir == 0) ? 2 : 3;
                        if (editorKeyDown(key)) {
                            float radPerFrame = (float)speed / 65536.0f * 6.28318f;
                            scOrbitDelta += (dir == 0) ? -radPerFrame : radPerFrame;
                        }
                    }
                    else if (t == VsNodeType::AutoOrbit) {
                        auto* sd = bpFindDataIn(action->id, 0);
                        sScriptAutoOrbitSpeed = sd ? (float)sd->paramInt[0] : 205.0f;
                    }
                    else if (t == VsNodeType::ChangeScene) {
                        auto* sd = bpFindDataIn(action->id, 0);
                        int scIdx = sd ? sd->paramInt[0] : action->paramInt[0];
                        sPendingSceneSwitch = scIdx;
                        sPendingSceneMode = action->paramInt[1];
                    }
                };

                // Run blueprint events
                for (auto& ev : bpNodes) {
                    if (ev.type == VsNodeType::OnStart && !sScriptStartRan) {
                        // OnStart already ran above
                    }
                    if (ev.type == VsNodeType::OnUpdate) {
                        auto acts = bpCollectActions(ev.id);
                        for (auto* a : acts) bpExecAction(a);
                    }
                    if (ev.type == VsNodeType::OnKeyHeld) {
                        int key = bpResolveEventKey(ev);
                        bool fire = (key >= 0) ? editorKeyDown(key) : true;
                        if (fire) {
                            auto acts = bpCollectActions(ev.id);
                            for (auto* a : acts) bpExecAction(a);
                        }
                    }
                    if (ev.type == VsNodeType::OnKeyPressed) {
                        int key = bpResolveEventKey(ev);
                        if (key >= 0 && editorKeyHit(key)) {
                            auto acts = bpCollectActions(ev.id);
                            for (auto* a : acts) bpExecAction(a);
                        }
                    }
                    if (ev.type == VsNodeType::OnKeyReleased) {
                        int key = bpResolveEventKey(ev);
                        if (key >= 0 && key < 10 && sPrevKeyState[key] && !editorKeyDown(key)) {
                            auto acts = bpCollectActions(ev.id);
                            for (auto* a : acts) bpExecAction(a);
                        }
                    }
                    if (ev.type == VsNodeType::OnCollision && collidedSprite >= 0) {
                        auto acts = bpCollectActions(ev.id);
                        for (auto* a : acts) bpExecAction(a);
                    }
                }
            }

            // --- Scene-level blueprint ---
            if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size()) {
                const MapScene& ms = sMapScenes[sMapSelectedScene];
                if (ms.blueprintIdx >= 0 && ms.blueprintIdx < (int)sBlueprintAssets.size()) {
                    const BlueprintAsset& bp = sBlueprintAssets[ms.blueprintIdx];
                    if (!bp.nodes.empty()) {
                        std::vector<VsNode> bpNodes = bp.nodes;
                        for (int pi = 0; pi < bp.paramCount; pi++) {
                            int overrideVal = bp.params[pi].defaultInt;
                            for (int oi = 0; oi < ms.instanceParamCount; oi++)
                                if (ms.instanceParams[oi].paramIdx == pi)
                                    overrideVal = ms.instanceParams[oi].value;
                            for (auto& n : bpNodes)
                                if (n.id == bp.params[pi].sourceNodeId)
                                    n.paramInt[bp.params[pi].sourceParamIdx] = overrideVal;
                        }
                        auto bpFindNode = [&](int id) -> const VsNode* {
                            for (auto& n : bpNodes) if (n.id == id) return &n;
                            return nullptr;
                        };
                        auto bpFindDataIn = [&](int nodeId, int pinIdx) -> const VsNode* {
                            for (auto& lk : bp.links)
                                if (lk.to.nodeId == nodeId && lk.to.pinType == 3 && lk.to.pinIdx == pinIdx)
                                    return bpFindNode(lk.from.nodeId);
                            return nullptr;
                        };
                        auto bpCollectActions = [&](int startNodeId) -> std::vector<const VsNode*> {
                            std::vector<const VsNode*> acts;
                            std::vector<int> front;
                            std::vector<bool> vis(10000, false);
                            for (auto& lk : bp.links)
                                if (lk.from.nodeId == startNodeId && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                                    front.push_back(lk.to.nodeId);
                            int safety = 0;
                            while (!front.empty() && safety < 256) {
                                int nid = front.front(); front.erase(front.begin());
                                if (nid < 0 || nid >= (int)vis.size() || vis[nid]) continue;
                                vis[nid] = true; safety++;
                                auto* an = bpFindNode(nid); if (!an) continue;
                                acts.push_back(an);
                                for (auto& lk : bp.links)
                                    if (lk.from.nodeId == an->id && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                                        front.push_back(lk.to.nodeId);
                            }
                            return acts;
                        };
                        auto bpResolveEventKey = [&](const VsNode& ev) -> int {
                            int count = 0; int keyVal = -1;
                            for (auto& lk : bp.links)
                                if (lk.to.nodeId == ev.id && lk.to.pinType == 3 && lk.to.pinIdx == 0) {
                                    auto* dn = bpFindNode(lk.from.nodeId);
                                    if (dn) { keyVal = dn->paramInt[0]; count++; }
                                }
                            if (count == 1) return keyVal;
                            if (count == 0) return ev.paramInt[0];
                            return -1;
                        };
                        auto bpExecAction = [&](const VsNode* action) {
                            VsNodeType t = action->type;
                            if (t == VsNodeType::MovePlayer) {
                                auto* dd = bpFindDataIn(action->id, 0);
                                int dir = dd ? dd->paramInt[0] : 0;
                                int dirKeys[] = { 8, 9, 6, 7 };
                                if (dir >= 0 && dir < 4 && editorKeyDown(dirKeys[dir])) {
                                    if (dir == 0) scInputRight -= 1.0f;
                                    if (dir == 1) scInputRight += 1.0f;
                                    if (dir == 2) scInputFwd   += 1.0f;
                                    if (dir == 3) scInputFwd   -= 1.0f;
                                }
                            }
                            else if (t == VsNodeType::Walk) {
                                auto* sd = bpFindDataIn(action->id, 0);
                                sScriptMoveSpeed = sd ? (float)sd->paramInt[0] : sCamObj.walkSpeed;
                            }
                            else if (t == VsNodeType::Sprint) {
                                auto* sd = bpFindDataIn(action->id, 0);
                                sScriptMoveSpeed = sd ? (float)sd->paramInt[0] : sCamObj.sprintSpeed;
                            }
                            else if (t == VsNodeType::OrbitCamera) {
                                auto* dd = bpFindDataIn(action->id, 0);
                                auto* sd = bpFindDataIn(action->id, 1);
                                int dir = dd ? dd->paramInt[0] : 1;
                                int speed = sd ? sd->paramInt[0] : 512;
                                int key = (dir == 0) ? 2 : 3;
                                if (editorKeyDown(key)) {
                                    float radPerFrame = (float)speed / 65536.0f * 6.28318f;
                                    scOrbitDelta += (dir == 0) ? -radPerFrame : radPerFrame;
                                }
                            }
                            else if (t == VsNodeType::AutoOrbit) {
                                auto* sd = bpFindDataIn(action->id, 0);
                                sScriptAutoOrbitSpeed = sd ? (float)sd->paramInt[0] : 205.0f;
                            }
                            else if (t == VsNodeType::ChangeScene) {
                                auto* sd = bpFindDataIn(action->id, 0);
                                int scIdx = sd ? sd->paramInt[0] : action->paramInt[0];
                                sPendingSceneSwitch = scIdx;
                                sPendingSceneMode = action->paramInt[1];
                            }
                        };
                        for (auto& ev : bpNodes) {
                            if (ev.type == VsNodeType::OnUpdate) {
                                auto acts = bpCollectActions(ev.id);
                                for (auto* a : acts) bpExecAction(a);
                            }
                            if (ev.type == VsNodeType::OnKeyHeld) {
                                int key = bpResolveEventKey(ev);
                                bool fire = (key >= 0) ? editorKeyDown(key) : true;
                                if (fire) { auto acts = bpCollectActions(ev.id); for (auto* a : acts) bpExecAction(a); }
                            }
                            if (ev.type == VsNodeType::OnKeyPressed) {
                                int key = bpResolveEventKey(ev);
                                if (key >= 0 && editorKeyHit(key)) { auto acts = bpCollectActions(ev.id); for (auto* a : acts) bpExecAction(a); }
                            }
                            if (ev.type == VsNodeType::OnKeyReleased) {
                                int key = bpResolveEventKey(ev);
                                if (key >= 0 && key < 10 && sPrevKeyState[key] && !editorKeyDown(key)) { auto acts = bpCollectActions(ev.id); for (auto* a : acts) bpExecAction(a); }
                            }
                            if (ev.type == VsNodeType::OnCollision && collidedSprite >= 0) {
                                auto acts = bpCollectActions(ev.id);
                                for (auto* a : acts) bpExecAction(a);
                            }
                        }
                    }
                }
            }

            // Handle pending scene switch
            if (sPendingSceneSwitch >= 0) {
                if (sPendingSceneMode == 0 && sPendingSceneSwitch < (int)sMapScenes.size()) {
                    // Switch to a 3D/MapScene
                    if (sPendingSceneSwitch != sMapSelectedScene || sActiveTab == EditorTab::Tilemap) {
                        SaveMapSceneState(sMapScenes[sMapSelectedScene]);
                        sMapSelectedScene = sPendingSceneSwitch;
                        LoadMapSceneState(sMapScenes[sMapSelectedScene]);
                        sActiveTab = EditorTab::Mode7;
                        sScriptStartRan = false;
                    }
                } else if (sPendingSceneMode == 1 && sPendingSceneSwitch < (int)sTmScenes.size()) {
                    // Switch to a Tilemap/TmScene
                    if (sPendingSceneSwitch != sTmSelectedScene || sActiveTab != EditorTab::Tilemap) {
                        SaveSceneState(sTmScenes[sTmSelectedScene]);
                        sTmSelectedScene = sPendingSceneSwitch;
                        LoadSceneState(sTmScenes[sTmSelectedScene]);
                        sActiveTab = EditorTab::Tilemap;
                        sScriptStartRan = false;
                    }
                }
                sPendingSceneSwitch = -1;
            }

            // Update previous key state
            for (int ki = 0; ki < 10; ki++) sPrevKeyState[ki] = editorKeyDown(ki);

            // --- Apply results ---
            float moveSpeed = (sScriptMoveSpeed > 0.0f ? sScriptMoveSpeed : sCamObj.walkSpeed) * dt;

            int playerIdx = -1;
            for (int i = 0; i < sSpriteCount; i++)
                if (sSprites[i].type == SpriteType::Player) { playerIdx = i; break; }

            if (playerIdx >= 0)
            {
                FloorSprite& player = sSprites[playerIdx];
                float viewAngle = sOrbitAngle + 3.14159265f;

                float inputX = scInputFwd, inputZ = scInputRight;
                if (inputX != 0.0f && inputZ != 0.0f) {
                    float len = sqrtf(inputX * inputX + inputZ * inputZ);
                    inputX /= len; inputZ /= len;
                }

                bool wasMoving = sPlayerMoving;
                sPlayerMoving = (inputX != 0.0f || inputZ != 0.0f);
                sPlayerSprinting = sPlayerMoving && editorKeyDown(1);

                // Pick active PlayAnim based on movement state
                if (sPlayerSprinting && sPlayAnimHeld >= 0)
                    sActivePlayAnimNodeId = sPlayAnimHeld;
                else if (sPlayAnimReleased >= 0)
                    sActivePlayAnimNodeId = sPlayAnimReleased;
                else if (sPlayAnimIdle >= 0)
                    sActivePlayAnimNodeId = sPlayAnimIdle;

                // Manual orbit from OrbitCamera nodes
                {
                    if (fabsf(scOrbitDelta) > 0.0001f)
                        sManualOrbitCurrent += (scOrbitDelta - sManualOrbitCurrent) * std::min(1.0f, 6.0f * dt);
                    else
                        sManualOrbitCurrent *= 0.85f;
                    if (fabsf(sManualOrbitCurrent) < 0.0001f)
                        sManualOrbitCurrent = 0.0f;
                    sOrbitAngle += sManualOrbitCurrent;
                }

                // Auto-orbit when strafing
                {
                    float autoOrbitTarget = 0.0f;
                    if (sScriptAutoOrbitSpeed > 0.0f && scInputRight != 0.0f) {
                        autoOrbitTarget = (sScriptAutoOrbitSpeed / 65536.0f * 6.28318f) * scInputRight;
                        if (editorKeyDown(2) || editorKeyDown(3))
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

                if (sPlayerMoving) {
                    sPlayerMoveAngle = atan2f(inputZ, inputX);
                    float fwdX = sinf(viewAngle), fwdZ = cosf(viewAngle);
                    float rightX = -cosf(viewAngle), rightZ = sinf(viewAngle);
                    player.x += (fwdX * inputX + rightX * inputZ) * moveSpeed;
                    player.z += (fwdZ * inputX + rightZ * inputZ) * moveSpeed;
                }
                else if (wasMoving)
                    sPlayerMoveAngle = sPlayerMoveAngle - sOrbitAngle;

                player.x = std::clamp(player.x, -kWorldHalf, kWorldHalf);
                player.z = std::clamp(player.z, -kWorldHalf, kWorldHalf);

                // Camera follow with smooth ease
                {
                    float targetX = player.x + sinf(sOrbitAngle) * sOrbitDist;
                    float targetZ = player.z + cosf(sOrbitAngle) * sOrbitDist;
                    bool orbiting = fabsf(sManualOrbitCurrent) > 0.0f;
                    float followPct;
                    if (orbiting)
                        followPct = 50.0f;
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
                if (ImGui::IsKeyDown(ImGuiKey_W)) {
                    sCamera.x -= sinf(sCamera.angle) * moveSpeed;
                    sCamera.z -= cosf(sCamera.angle) * moveSpeed;
                }
                if (ImGui::IsKeyDown(ImGuiKey_S)) {
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
            } // end Mode7/3D play mode

            // ---- TILEMAP PLAY MODE: blueprint execution for TmObjects ----
            if (sPlayTab == EditorTab::Tilemap)
            {
                // Key mapping (reuse same GBA key indices)
                auto tmKeyDown = [](int gbaKey) -> bool {
                    switch (gbaKey) {
                    case 0: return ImGui::IsKeyDown(ImGuiKey_Space);
                    case 1: return ImGui::IsKeyDown(ImGuiKey_LeftShift);
                    case 2: return ImGui::IsKeyDown(ImGuiKey_J);
                    case 3: return ImGui::IsKeyDown(ImGuiKey_L);
                    case 4: return ImGui::IsKeyDown(ImGuiKey_Enter);
                    case 5: return ImGui::IsKeyDown(ImGuiKey_Backspace);
                    case 6: return ImGui::IsKeyDown(ImGuiKey_W);
                    case 7: return ImGui::IsKeyDown(ImGuiKey_S);
                    case 8: return ImGui::IsKeyDown(ImGuiKey_A);
                    case 9: return ImGui::IsKeyDown(ImGuiKey_D);
                    default: return false;
                    }
                };

                // Get current tilemap dimensions for clamping
                int tmMapW = 1, tmMapH = 1;
                if (sTmSelectedScene >= 0 && sTmSelectedScene < (int)sTmScenes.size()) {
                    tmMapW = sTmScenes[sTmSelectedScene].mapW;
                    tmMapH = sTmScenes[sTmSelectedScene].mapH;
                }

                // Track most-recently-pressed direction to prevent diagonal movement
                // dir mapping: 0=Left(A), 1=Right(D), 2=Up(W), 3=Down(S)
                bool curHeld[4] = {
                    ImGui::IsKeyDown(ImGuiKey_A), ImGui::IsKeyDown(ImGuiKey_D),
                    ImGui::IsKeyDown(ImGuiKey_W), ImGui::IsKeyDown(ImGuiKey_S)
                };
                // Newly pressed direction takes priority
                for (int d = 0; d < 4; d++)
                    if (curHeld[d] && !sTmPrevDirHeld[d])
                        sTmLastMoveDir = d;
                // If current priority dir released, fall back to any still-held dir
                if (sTmLastMoveDir >= 0 && !curHeld[sTmLastMoveDir]) {
                    sTmLastMoveDir = -1;
                    for (int d = 0; d < 4; d++)
                        if (curHeld[d]) { sTmLastMoveDir = d; break; }
                }
                for (int d = 0; d < 4; d++) sTmPrevDirHeld[d] = curHeld[d];

                // Per-object flag: prevent any MovePlayer from firing twice in one frame
                std::vector<bool> tmObjMovedThisFrame(sTmObjects.size(), false);

                for (int oi = 0; oi < (int)sTmObjects.size(); oi++)
                {
                    TmObject& obj = sTmObjects[oi];
                    if (obj.blueprintIdx < 0 || obj.blueprintIdx >= (int)sBlueprintAssets.size()) continue;
                    const BlueprintAsset& bp = sBlueprintAssets[obj.blueprintIdx];
                    if (bp.nodes.empty()) continue;

                    // Reset to default each frame; Walk/Sprint node overrides it
                    if (oi >= (int)sTmObjMoveRate.size()) sTmObjMoveRate.resize(oi + 1, 6.0f);
                    sTmObjMoveRate[oi] = 6.0f;
                    float& tmMoveRate = sTmObjMoveRate[oi];

                    // Build temporary copy with param overrides
                    std::vector<VsNode> bpNodes = bp.nodes;
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        int overrideVal = bp.params[pi].defaultInt;
                        for (int oi2 = 0; oi2 < obj.instanceParamCount; oi2++)
                            if (obj.instanceParams[oi2].paramIdx == pi)
                                overrideVal = obj.instanceParams[oi2].value;
                        for (auto& n : bpNodes)
                            if (n.id == bp.params[pi].sourceNodeId)
                                n.paramInt[bp.params[pi].sourceParamIdx] = overrideVal;
                    }

                    auto tmFindNode = [&](int id) -> const VsNode* {
                        for (auto& n : bpNodes) if (n.id == id) return &n;
                        return nullptr;
                    };
                    auto tmFindDataIn = [&](int nodeId, int pinIdx) -> const VsNode* {
                        for (auto& lk : bp.links)
                            if (lk.to.nodeId == nodeId && lk.to.pinType == 3 && lk.to.pinIdx == pinIdx)
                                return tmFindNode(lk.from.nodeId);
                        return nullptr;
                    };
                    auto tmCollectActions = [&](int startNodeId) -> std::vector<const VsNode*> {
                        std::vector<const VsNode*> acts;
                        std::vector<int> front;
                        std::vector<bool> vis(10000, false);
                        for (auto& lk : bp.links)
                            if (lk.from.nodeId == startNodeId && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                                front.push_back(lk.to.nodeId);
                        int safety = 0;
                        while (!front.empty() && safety < 256) {
                            int nid = front.front(); front.erase(front.begin());
                            if (nid < 0 || nid >= (int)vis.size() || vis[nid]) continue;
                            vis[nid] = true; safety++;
                            auto* an = tmFindNode(nid); if (!an) continue;
                            acts.push_back(an);
                            for (auto& lk : bp.links)
                                if (lk.from.nodeId == an->id && lk.from.pinType == 0 && lk.from.pinIdx == 0)
                                    front.push_back(lk.to.nodeId);
                        }
                        return acts;
                    };
                    auto tmResolveEventKey = [&](const VsNode& ev) -> int {
                        int count = 0; int keyVal = -1;
                        for (auto& lk : bp.links)
                            if (lk.to.nodeId == ev.id && lk.to.pinType == 3 && lk.to.pinIdx == 0) {
                                auto* dn = tmFindNode(lk.from.nodeId);
                                if (dn) { keyVal = dn->paramInt[0]; count++; }
                            }
                        if (count == 1) return keyVal;
                        if (count == 0) return ev.paramInt[0];
                        return -1;
                    };
                    bool tmInstantMove = false; // set true for OnKeyPressed events
                    auto tmExecAction = [&](const VsNode* action) {
                        VsNodeType t = action->type;
                        if (t == VsNodeType::MovePlayer) {
                            if (tmObjMovedThisFrame[oi]) return;
                            auto* dd = tmFindDataIn(action->id, 0);
                            int dir = dd ? dd->paramInt[0] : 0;
                            // dir: 0=Left, 1=Right, 2=Up, 3=Down
                            // facing: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
                            int dirKeys[] = { 8, 9, 6, 7 }; // A, D, W, S
                            int dirToFacing[] = { 6, 2, 0, 4 }; // Left=W, Right=E, Up=N, Down=S
                            if (dir >= 0 && dir < 4) {
                                if (tmInstantMove) {
                                    // OnKeyPressed: immediate single-tile move, no repeat
                                    ImGuiKey dirImKeys[] = { ImGuiKey_A, ImGuiKey_D, ImGuiKey_W, ImGuiKey_S };
                                    if (ImGui::IsKeyPressed(dirImKeys[dir], false)) {
                                        if (oi < (int)sTmObjFacing.size())
                                            sTmObjFacing[oi] = dirToFacing[dir];
                                        if (dir == 0) obj.tileX -= 1;
                                        else if (dir == 1) obj.tileX += 1;
                                        else if (dir == 2) obj.tileY -= 1;
                                        else if (dir == 3) obj.tileY += 1;
                                        if (oi < (int)sTmObjStepCount.size())
                                            sTmObjStepCount[oi]++;
                                        tmObjMovedThisFrame[oi] = true;
                                    }
                                } else if (tmKeyDown(dirKeys[dir]) && sTmLastMoveDir == dir) {
                                    if (oi < (int)sTmObjFacing.size())
                                        sTmObjFacing[oi] = dirToFacing[dir];
                                    ImGuiKey dirImKeys[] = { ImGuiKey_A, ImGuiKey_D, ImGuiKey_W, ImGuiKey_S };
                                    if (ImGui::IsKeyPressed(dirImKeys[dir], false)) {
                                        // First frame: just face, pre-fill accumulator (lower = longer before move)
                                        sTmMoveAccum[dir] = 0.45f;
                                    } else {
                                        // Held: accumulator movement
                                        sTmMoveAccum[dir] += tmMoveRate * dt;
                                        if (sTmMoveAccum[dir] >= 1.0f) {
                                            int steps = (int)sTmMoveAccum[dir];
                                            sTmMoveAccum[dir] -= steps;
                                            if (dir == 0) obj.tileX -= steps;
                                            else if (dir == 1) obj.tileX += steps;
                                            else if (dir == 2) obj.tileY -= steps;
                                            else if (dir == 3) obj.tileY += steps;
                                            if (oi < (int)sTmObjStepCount.size())
                                                sTmObjStepCount[oi] += steps;
                                            tmObjMovedThisFrame[oi] = true;
                                        }
                                    }
                                }
                            }
                        }
                        else if (t == VsNodeType::Walk) {
                            auto* sd = tmFindDataIn(action->id, 0);
                            tmMoveRate = sd ? (float)sd->paramInt[0] : 6.0f;
                        }
                        else if (t == VsNodeType::Sprint) {
                            auto* sd = tmFindDataIn(action->id, 0);
                            tmMoveRate = sd ? (float)sd->paramInt[0] : 12.0f;
                        }
                        else if (t == VsNodeType::ChangeScene) {
                            auto* sd = tmFindDataIn(action->id, 0);
                            int scIdx = sd ? sd->paramInt[0] : action->paramInt[0];
                            sPendingSceneSwitch = scIdx;
                            sPendingSceneMode = action->paramInt[1];
                        }
                        else if (t == VsNodeType::PlayAnim) {
                            auto* sd = tmFindDataIn(action->id, 0);
                            int assetIdx = sd ? sd->paramInt[0] : -1;
                            int animIdx = sd ? sd->paramInt[1] : 0;
                            // Check animation gameState vs movement
                            bool apply = true;
                            if (assetIdx >= 0 && assetIdx < (int)sSpriteAssets.size() &&
                                animIdx >= 0 && animIdx < (int)sSpriteAssets[assetIdx].anims.size()) {
                                AnimState gs = sSpriteAssets[assetIdx].anims[animIdx].gameState;
                                bool moving = ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A) ||
                                              ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D);
                                if (gs == AnimState::Idle && moving) apply = false;
                                if ((gs == AnimState::Walk || gs == AnimState::Run) && !moving) apply = false;
                            }
                            if (apply && oi >= 0 && oi < (int)sTmObjAnimSet.size())
                                sTmObjAnimSet[oi] = animIdx;
                        }
                    };

                    // Run blueprint events — OnStart once, then OnUpdate each frame
                    if (!sTmOnStartRan) {
                        for (auto& ev : bpNodes)
                            if (ev.type == VsNodeType::OnStart) {
                                auto acts = tmCollectActions(ev.id);
                                for (auto* a : acts) tmExecAction(a);
                            }
                    }
                    for (auto& ev : bpNodes)
                        if (ev.type == VsNodeType::OnUpdate) {
                            auto acts = tmCollectActions(ev.id);
                            for (auto* a : acts) tmExecAction(a);
                        }
                    for (auto& ev : bpNodes) {
                        if (ev.type == VsNodeType::OnKeyHeld) {
                            int key = tmResolveEventKey(ev);
                            bool fire = (key >= 0) ? tmKeyDown(key) : true;
                            if (fire) {
                                auto acts = tmCollectActions(ev.id);
                                for (auto* a : acts) tmExecAction(a);
                            }
                        }
                        if (ev.type == VsNodeType::OnKeyPressed) {
                            int key = tmResolveEventKey(ev);
                            auto tmKeyHit = [](int gbaKey) -> bool {
                                switch (gbaKey) {
                                case 0: return ImGui::IsKeyPressed(ImGuiKey_Space, false);
                                case 1: return ImGui::IsKeyPressed(ImGuiKey_LeftShift, false);
                                case 2: return ImGui::IsKeyPressed(ImGuiKey_J, false);
                                case 3: return ImGui::IsKeyPressed(ImGuiKey_L, false);
                                case 4: return ImGui::IsKeyPressed(ImGuiKey_Enter, false);
                                case 5: return ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
                                case 6: return ImGui::IsKeyPressed(ImGuiKey_W, false);
                                case 7: return ImGui::IsKeyPressed(ImGuiKey_S, false);
                                case 8: return ImGui::IsKeyPressed(ImGuiKey_A, false);
                                case 9: return ImGui::IsKeyPressed(ImGuiKey_D, false);
                                default: return false;
                                }
                            };
                            // key < 0 means multiple keys connected — fire if ANY key was just pressed
                            bool fire = false;
                            if (key >= 0) { fire = tmKeyHit(key); }
                            else { for (int k = 0; k < 10; k++) if (tmKeyHit(k)) { fire = true; break; } }
                            if (fire) {
                                tmInstantMove = true;
                                auto acts = tmCollectActions(ev.id);
                                for (auto* a : acts) tmExecAction(a);
                                tmInstantMove = false;
                            }
                        }
                        if (ev.type == VsNodeType::OnKeyReleased) {
                            int key = tmResolveEventKey(ev);
                            if (key >= 0 && key < 10 && sTmPrevKeyState[key] && !tmKeyDown(key)) {
                                auto acts = tmCollectActions(ev.id);
                                for (auto* a : acts) tmExecAction(a);
                            }
                        }
                    }

                    // Clamp object to tilemap bounds
                    obj.tileX = std::clamp(obj.tileX, 0, tmMapW - 1);
                    obj.tileY = std::clamp(obj.tileY, 0, tmMapH - 1);
                }

                sTmOnStartRan = true;

                // Smooth visual position lerp toward logical tile position
                bool anyDirHeld = ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A) ||
                                  ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D);
                for (int i = 0; i < (int)sTmObjects.size(); i++) {
                    if (i >= (int)sTmObjVisX.size()) break;
                    float tx = (float)sTmObjects[i].tileX;
                    float ty = (float)sTmObjects[i].tileY;
                    float speed = 10.0f;
                    if (i < (int)sTmObjMoveRate.size() && sTmObjMoveRate[i] > 0.0f)
                        speed = sTmObjMoveRate[i];
                    float maxStep = speed * dt;
                    float dx = tx - sTmObjVisX[i];
                    float dy = ty - sTmObjVisY[i];
                    if (dx > maxStep) dx = maxStep; else if (dx < -maxStep) dx = -maxStep;
                    if (dy > maxStep) dy = maxStep; else if (dy < -maxStep) dy = -maxStep;
                    sTmObjVisX[i] += dx;
                    sTmObjVisY[i] += dy;
                }

                // Reset accumulators for directions not held
                if (!ImGui::IsKeyDown(ImGuiKey_A)) sTmMoveAccum[0] = 0.0f;
                if (!ImGui::IsKeyDown(ImGuiKey_D)) sTmMoveAccum[1] = 0.0f;
                if (!ImGui::IsKeyDown(ImGuiKey_W)) sTmMoveAccum[2] = 0.0f;
                if (!ImGui::IsKeyDown(ImGuiKey_S)) sTmMoveAccum[3] = 0.0f;

                // Update previous key state for OnKeyReleased
                for (int ki = 0; ki < 10; ki++) sTmPrevKeyState[ki] = tmKeyDown(ki);
            }
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
    bool useMode7Floor = (sActiveTab == EditorTab::Mode7);
    Mode7::Render(sCamera, nullptr, sSprites, sSpriteCount, camObjPtr, sCamObjEditorScale,
                  assetsPtr, (int)sSpriteAssets.size(), sViewportAnimTime, isPlaying,
                  nullptr, spriteAngle,
                  assetDirImgs.empty() ? nullptr : assetDirImgs.data(), (int)assetDirImgs.size(),
                  spriteDirImgs.empty() ? nullptr : spriteDirImgs.data(), (int)spriteDirImgs.size(),
                  (useMode7Floor || sMeshAssets.empty()) ? nullptr : sMeshAssets.data(),
                  useMode7Floor ? 0 : (int)sMeshAssets.size(),
                  useMode7Floor);

    // Draw axis guide line when transforming a selected sprite
    if (sTransformAxis && sTransformAxis != 'S'
        && sSelectedSprite >= 0 && sSelectedSprite < sSpriteCount)
    {
        const FloorSprite& sp = sSprites[sSelectedSprite];
        Mode7::DrawAxisGuide(sCamera, sp.x, sp.y, sp.z, sTransformAxis);
    }

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

    // Create default map scene if none exist (3D scene tab)
    if (sMapScenes.empty())
    {
        MapScene ms;
        snprintf(ms.name, sizeof(ms.name), "Scene 0");
        SaveMapSceneState(ms);
        sMapScenes.push_back(ms);
        sMapSelectedScene = 0;
    }

    // Draw everything
    DrawTabBar();

    if (sActiveTab == EditorTab::Tilemap)
    {
        DrawTilemapTab(
            ImVec2(vp->WorkPos.x, bodyY),
            ImVec2(totalW, bodyH));
    }
    else if (sActiveTab == EditorTab::Skybox)
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
    else if (sActiveTab == EditorTab::Events)
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;

        // ---- Expose as Parameter helpers ----
        auto isDataNodeType = [](VsNodeType t) {
            return t == VsNodeType::Integer || t == VsNodeType::Float ||
                   t == VsNodeType::Key || t == VsNodeType::Direction || t == VsNodeType::Animation;
        };
        auto dataTypeFromNode = [](VsNodeType t) -> int {
            switch (t) {
                case VsNodeType::Integer:   return 0;
                case VsNodeType::Float:     return 1;
                case VsNodeType::Key:       return 2;
                case VsNodeType::Direction: return 3;
                case VsNodeType::Animation: return 4;
                default: return 0;
            }
        };

        // ---- NODE GRAPH CANVAS ----
        float browserH = Scaled(120);
        float canvasH = bodyH - browserH;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, bodyY));
        ImGui::SetNextWindowSize(ImVec2(totalW, canvasH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
        ImGui::Begin("##NodeCanvas", nullptr, flags);

        // Edit source indicator (small label at top-left of canvas)
        {
            ImDrawList* barDl = ImGui::GetWindowDrawList();
            if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                char lbl[64]; snprintf(lbl, sizeof(lbl), " Blueprint: %s ", sBlueprintAssets[sVsEditBlueprintIdx].name);
                ImVec2 tsz = ImGui::CalcTextSize(lbl);
                ImVec2 p0(vp->WorkPos.x + 4, bodyY + 4);
                ImVec2 p1(p0.x + tsz.x + 8, p0.y + tsz.y + 6);
                barDl->AddRectFilled(p0, p1, 0xCC1A3A5A, 4.0f);
                barDl->AddText(ImVec2(p0.x + 4, p0.y + 3), 0xFF66CCFF, lbl);
                // Back button
                ImGui::SetCursorScreenPos(ImVec2(p1.x + 4, p0.y));
                if (ImGui::SmallButton("Back##bpback")) {
                    BlueprintAsset& bp = sBlueprintAssets[sVsEditBlueprintIdx];
                    bp.nodes = sVsNodes; bp.links = sVsLinks; bp.annotations = sVsAnnotations;
                    bp.groupPins = sVsGroupPins; bp.nextId = sVsNextId;
                    bp.panX = sVsPanX; bp.panY = sVsPanY; bp.zoom = sVsZoom;
                    if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                        LoadMapSceneState(sMapScenes[sMapSelectedScene]);
                    sVsEditSource = VsEditSource::Scene;
                    sVsEditBlueprintIdx = -1;
                    sVsSelected = -1;
                    sVsUndoStack.clear();
                }
            } else {
                ImVec2 p0(vp->WorkPos.x + 4, bodyY + 4);
                barDl->AddText(ImVec2(p0.x + 4, p0.y + 3), 0xFF99CC99, "Scene Script");
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 canvasOrig = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImVec2(totalW, canvasH - 8);
        float zoom = sVsZoom;

        // Clip to canvas
        dl->PushClipRect(canvasOrig, ImVec2(canvasOrig.x + canvasSize.x, canvasOrig.y + canvasSize.y), true);

        // Grid
        float gridStep = 32.0f * zoom;
        float offX = fmodf(sVsPanX * zoom, gridStep);
        float offY = fmodf(sVsPanY * zoom, gridStep);
        for (float gx = offX; gx < canvasSize.x; gx += gridStep)
            dl->AddLine(ImVec2(canvasOrig.x + gx, canvasOrig.y),
                        ImVec2(canvasOrig.x + gx, canvasOrig.y + canvasSize.y), 0xFF1A1A1E);
        for (float gy = offY; gy < canvasSize.y; gy += gridStep)
            dl->AddLine(ImVec2(canvasOrig.x, canvasOrig.y + gy),
                        ImVec2(canvasOrig.x + canvasSize.x, canvasOrig.y + gy), 0xFF1A1A1E);

        // Draw annotations (behind everything)
        for (int ai = 0; ai < (int)sVsAnnotations.size(); ai++) {
            auto& ann = sVsAnnotations[ai];
            float ax = canvasOrig.x + (ann.x + sVsPanX) * zoom;
            float ay = canvasOrig.y + (ann.y + sVsPanY) * zoom;
            float aw = ann.w * zoom;
            float ah = ann.h * zoom;
            float headerH = 22.0f * zoom;
            ImVec2 aMin(ax, ay);
            ImVec2 aMax(ax + aw, ay + ah);
            // Body fill
            dl->AddRectFilled(aMin, aMax, ann.color, 4.0f * zoom);
            // Header bar
            dl->AddRectFilled(aMin, ImVec2(aMax.x, ay + headerH), 0x66AAAAAA, 4.0f * zoom, ImDrawFlags_RoundCornersTop);
            // Border (highlight if selected)
            bool aSel = (ai == sVsSelectedAnnotation) || ann.selected;
            dl->AddRect(aMin, aMax, aSel ? 0xAAFFAA44 : 0x66666666, 4.0f * zoom, 0, aSel ? 1.5f : 1.0f);
            // Label text
            if (ann.label[0])
                dl->AddText(ImVec2(ax + 6 * zoom, ay + 3 * zoom), 0xFFDDDDDD, ann.label);
            // Resize grips (both bottom corners)
            float grip = 22.0f * zoom;
            ImU32 gripCol = aSel ? 0xAAFFAA44 : 0x66888888;
            // Bottom-right
            dl->AddTriangleFilled(
                ImVec2(aMax.x, aMax.y - grip), ImVec2(aMax.x - grip, aMax.y), aMax,
                gripCol);
            // Bottom-left
            dl->AddTriangleFilled(
                ImVec2(aMin.x, aMax.y - grip), ImVec2(aMin.x + grip, aMax.y), ImVec2(aMin.x, aMax.y),
                gripCol);
        }

        // Update link surge positions — blue dot travels from source to dest
        if (sEditorMode == EditorMode::Play)
        {
            float surgeDt = ImGui::GetIO().DeltaTime;
            for (int li = 0; li < (int)sVsLinks.size(); li++) {
                int srcId = sVsLinks[li].from.nodeId;
                bool firing = false;
                for (int fid : sVsFiredNodes)
                    if (fid == srcId) { firing = true; break; }
                // Start new surge when source fires and no surge active (or past halfway for held keys)
                if (firing) {
                    auto it = sVsLinkSurgeT.find(li);
                    if (it == sVsLinkSurgeT.end() || it->second > 1.0f)
                        sVsLinkSurgeT[li] = 0.0f;
                }
            }
            // Advance surges
            std::vector<int> done;
            for (auto& [li, t] : sVsLinkSurgeT) {
                t += surgeDt * 2.5f; // ~0.4s to travel full length
                if (t > 1.5f) done.push_back(li);
            }
            for (int li : done) sVsLinkSurgeT.erase(li);
            // Advance reverse surges
            done.clear();
            for (auto& [li, t] : sVsLinkSurgeRevT) {
                t += surgeDt * 2.5f;
                if (t > 1.5f) done.push_back(li);
            }
            for (int li : done) sVsLinkSurgeRevT.erase(li);
        }
        else {
            sVsLinkSurgeT.clear();
            sVsLinkSurgeRevT.clear();
        }

        // Draw links (bezier curves) — resolve pins to current editing level
        auto bezEval = [](ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float u) -> ImVec2 {
            float v = 1.0f - u;
            return ImVec2(
                v*v*v*a.x + 3*v*v*u*b.x + 3*v*u*u*c.x + u*u*u*d.x,
                v*v*v*a.y + 3*v*v*u*b.y + 3*v*u*u*c.y + u*u*u*d.y);
        };
        for (int li = 0; li < (int)sVsLinks.size(); li++) {
            auto& lk = sVsLinks[li];
            VsPin fromP = VsResolvePin(lk.from);
            VsPin toP   = VsResolvePin(lk.to);
            if (fromP.nodeId < 0 || toP.nodeId < 0) continue;
            int fi = VsFindNode(fromP.nodeId);
            int ti = VsFindNode(toP.nodeId);
            if (fi < 0 || ti < 0) continue;
            if (sVsNodes[fi].groupId != sVsEditingGroup || sVsNodes[ti].groupId != sVsEditingGroup) continue;
            ImVec2 p1 = VsPinPos(sVsNodes[fi], fromP.pinType, fromP.pinIdx, canvasOrig, zoom);
            ImVec2 p2 = VsPinPos(sVsNodes[ti], toP.pinType, toP.pinIdx, canvasOrig, zoom);
            float dx = std::max(50.0f * zoom, fabsf(p2.x - p1.x) * 0.5f);
            ImVec2 cp1(p1.x + dx, p1.y), cp2(p2.x - dx, p2.y);
            bool isExec = (lk.from.pinType == 0);
            ImU32 wireCol = isExec ? 0xFFFFFFFF : 0xFF44CCAA;
            dl->AddBezierCubic(p1, cp1, cp2, p2, wireCol, 2.0f * zoom);

            // Draw blue traveling surge (forward: from → to)
            auto surgeIt = sVsLinkSurgeT.find(li);
            if (surgeIt != sVsLinkSurgeT.end()) {
                float st = surgeIt->second;
                float center = std::clamp(st, 0.0f, 1.0f);
                float fadeAlpha = (st > 1.0f) ? std::max(0.0f, 1.0f - (st - 1.0f) * 2.0f) : 1.0f;
                int segs = 8;
                float halfLen = 0.12f;
                for (int si = 0; si <= segs; si++) {
                    float u = center - halfLen + (halfLen * 2.0f * si / segs);
                    u = std::clamp(u, 0.0f, 1.0f);
                    ImVec2 pt = bezEval(p1, cp1, cp2, p2, u);
                    float dist = fabsf(u - center) / halfLen;
                    float bright = std::max(0.0f, 1.0f - dist * dist) * fadeAlpha;
                    ImU32 col = ImGui::GetColorU32(ImVec4(0.3f * bright, 0.6f * bright, 1.0f * bright, bright));
                    float rad = (2.0f + 3.0f * bright) * zoom;
                    dl->AddCircleFilled(pt, rad, col);
                }
            }
            // Draw reverse surge (to → from, for PlayAnim data links)
            auto revIt = sVsLinkSurgeRevT.find(li);
            if (revIt != sVsLinkSurgeRevT.end()) {
                float st = revIt->second;
                float center = 1.0f - std::clamp(st, 0.0f, 1.0f); // reversed direction
                float fadeAlpha = (st > 1.0f) ? std::max(0.0f, 1.0f - (st - 1.0f) * 2.0f) : 1.0f;
                int segs = 8;
                float halfLen = 0.12f;
                for (int si = 0; si <= segs; si++) {
                    float u = center - halfLen + (halfLen * 2.0f * si / segs);
                    u = std::clamp(u, 0.0f, 1.0f);
                    ImVec2 pt = bezEval(p1, cp1, cp2, p2, u);
                    float dist = fabsf(u - center) / halfLen;
                    float bright = std::max(0.0f, 1.0f - dist * dist) * fadeAlpha;
                    ImU32 col = ImGui::GetColorU32(ImVec4(0.3f * bright, 0.6f * bright, 1.0f * bright, bright));
                    float rad = (2.0f + 3.0f * bright) * zoom;
                    dl->AddCircleFilled(pt, rad, col);
                }
            }
        }

        // Draw in-progress link drag
        if (sVsDraggingLink) {
            int si = VsFindNode(sVsLinkStart.nodeId);
            if (si >= 0) {
                ImVec2 p1 = VsPinPos(sVsNodes[si], sVsLinkStart.pinType, sVsLinkStart.pinIdx, canvasOrig, zoom);
                ImVec2 p2 = sVsLinkEndPos;
                float dx = std::max(50.0f * zoom, fabsf(p2.x - p1.x) * 0.5f);
                dl->AddBezierCubic(p1, ImVec2(p1.x + dx, p1.y), ImVec2(p2.x - dx, p2.y), p2, 0x88FFFFFF, 2.0f * zoom);
            }
        }

        // Draw nodes
        ImGuiIO& io = ImGui::GetIO();
        int hoveredNode = -1;
        VsPin hoveredPin = { -1, 0, 0 };

        for (int ni = 0; ni < (int)sVsNodes.size(); ni++) {
            VsNode& n = sVsNodes[ni];
            if ((int)n.type < 0 || (int)n.type >= (int)VsNodeType::COUNT) continue;
            if (n.groupId != sVsEditingGroup) continue; // filter by editing level
            const auto& def = sVsNodeDefs[(int)n.type];
            float nw = kVsNodeW * zoom;
            float nh = VsNodeHeight(n) * zoom;
            float hh = kVsHeaderH * zoom;
            float sp = kVsPinSpacing * zoom;
            float pr = kVsPinRadius * zoom;

            float nx = canvasOrig.x + (n.x + sVsPanX) * zoom;
            float ny = canvasOrig.y + (n.y + sVsPanY) * zoom;

            ImVec2 nMin(nx, ny);
            ImVec2 nMax(nx + nw, ny + nh);
            bool isSel = (ni == sVsSelected) || n.selected;

            // Node body
            dl->AddRectFilled(nMin, nMax, 0xDD222228, 6.0f * zoom);
            // Header
            dl->AddRectFilled(nMin, ImVec2(nMax.x, nMin.y + hh), def.color, 6.0f * zoom, ImDrawFlags_RoundCornersTop);
            // Border
            dl->AddRect(nMin, nMax, isSel ? 0xFFFFAA44 : 0xFF555566, 6.0f * zoom, 0, isSel ? 2.0f : 1.0f);
            // Title
            const char* title = ((n.type == VsNodeType::Group || n.type == VsNodeType::CustomCode) && n.groupLabel[0]) ? n.groupLabel : def.name;
            dl->AddText(ImVec2(nx + 6 * zoom, ny + 2 * zoom), 0xFFFFFFFF, title);
            // Custom code indicator
            if (n.customCode[0])
                dl->AddText(ImVec2(nMax.x - 22 * zoom, ny + 2 * zoom), 0xAAFFCC66, "<>");
            // Blueprint parameter badge "P"
            if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0
                && sVsEditBlueprintIdx < (int)sBlueprintAssets.size() && isDataNodeType(n.type)) {
                const BlueprintAsset& bp = sBlueprintAssets[sVsEditBlueprintIdx];
                for (int pi = 0; pi < bp.paramCount; pi++) {
                    if (bp.params[pi].sourceNodeId == n.id) {
                        float badgeX = n.customCode[0] ? (nMax.x - 40 * zoom) : (nMax.x - 22 * zoom);
                        dl->AddText(ImVec2(badgeX, ny + 2 * zoom), 0xAA44CCFF, "P");
                        break;
                    }
                }
            }

            // Subtitle — show key/param value on the header
            {
                const char* sub = nullptr;
                char subBuf[32] = {};
                switch (n.type) {
                case VsNodeType::Integer:
                    snprintf(subBuf, sizeof(subBuf), "%d", n.paramInt[0]);
                    sub = subBuf;
                    break;
                case VsNodeType::Float: {
                    float fv;
                    memcpy(&fv, &n.paramInt[0], sizeof(float));
                    snprintf(subBuf, sizeof(subBuf), "%.3g", fv);
                    sub = subBuf;
                    break;
                }
                case VsNodeType::Key:
                    if (n.paramInt[0] >= 0 && n.paramInt[0] < kVsKeyCount)
                        sub = sVsKeyNames[n.paramInt[0]];
                    break;
                case VsNodeType::Direction:
                    if (n.paramInt[0] >= 0 && n.paramInt[0] < kVsAxisCount)
                        sub = sVsAxisNames[n.paramInt[0]];
                    break;
                case VsNodeType::Animation: {
                    int si = n.paramInt[0], ai = n.paramInt[1];
                    if (si >= 0 && si < (int)sSpriteAssets.size() &&
                        ai >= 0 && ai < (int)sSpriteAssets[si].anims.size())
                        sub = sSpriteAssets[si].anims[ai].name.c_str();
                    break;
                }
                case VsNodeType::ChangeScene: {
                    sub = (n.paramInt[1] == 1) ? "Mode 0" : "Mode 4";
                    break;
                }
                case VsNodeType::Object: {
                    int oi = n.paramInt[0];
                    int kind = n.paramInt[1];
                    if (kind == 0 && oi >= 0 && oi < (int)sSpriteAssets.size())
                        sub = sSpriteAssets[oi].name.c_str();
                    else if (kind == 1 && oi >= 0 && oi < (int)sMeshAssets.size())
                        sub = sMeshAssets[oi].name.c_str();
                    else if (kind == 2 && oi >= 0 && oi < sSpriteCount) {
                        int ai2 = sSprites[oi].assetIdx;
                        if (ai2 >= 0 && ai2 < (int)sSpriteAssets.size())
                            sub = sSpriteAssets[ai2].name.c_str();
                        else { snprintf(subBuf, sizeof(subBuf), "[%d]", oi); sub = subBuf; }
                    }
                    break;
                }
                default: break;
                }
                if (sub)
                    dl->AddText(ImVec2(nx + 6 * zoom, ny + 16 * zoom), 0xFFAADDFF, sub);
            }

            // Draw pins — use dynamic counts for Group nodes
            auto pc = VsGetPinCounts(n);
            // exec in
            for (int p = 0; p < pc.inExec; p++) {
                ImVec2 pp = VsPinPos(n, 1, p, canvasOrig, zoom);
                dl->AddTriangleFilled(
                    ImVec2(pp.x - pr, pp.y - pr), ImVec2(pp.x + pr, pp.y), ImVec2(pp.x - pr, pp.y + pr),
                    0xFFFFFFFF);
                if (((io.MousePos.x - pp.x) * (io.MousePos.x - pp.x) + (io.MousePos.y - pp.y) * (io.MousePos.y - pp.y)) < pr * pr * 4)
                    hoveredPin = { n.id, 1, p };
            }
            // exec out
            for (int p = 0; p < pc.outExec; p++) {
                ImVec2 pp = VsPinPos(n, 0, p, canvasOrig, zoom);
                dl->AddTriangleFilled(
                    ImVec2(pp.x - pr, pp.y - pr), ImVec2(pp.x + pr, pp.y), ImVec2(pp.x - pr, pp.y + pr),
                    0xFFFFFFFF);
                // Label (from def if available)
                if (n.type != VsNodeType::Group && def.outExecNames[p])
                    dl->AddText(ImVec2(pp.x - 8 * zoom - ImGui::CalcTextSize(def.outExecNames[p]).x * zoom, pp.y - 6 * zoom),
                        0xFFCCCCCC, def.outExecNames[p]);
                if (((io.MousePos.x - pp.x) * (io.MousePos.x - pp.x) + (io.MousePos.y - pp.y) * (io.MousePos.y - pp.y)) < pr * pr * 4)
                    hoveredPin = { n.id, 0, p };
            }
            // data in
            for (int p = 0; p < pc.inData; p++) {
                ImVec2 pp = VsPinPos(n, 3, p, canvasOrig, zoom);
                dl->AddCircleFilled(pp, pr, 0xFF44CCAA);
                if (n.type != VsNodeType::Group && def.inDataNames[p])
                    dl->AddText(ImVec2(pp.x + pr + 4 * zoom, pp.y - 6 * zoom), 0xFFCCCCCC, def.inDataNames[p]);
                if (((io.MousePos.x - pp.x) * (io.MousePos.x - pp.x) + (io.MousePos.y - pp.y) * (io.MousePos.y - pp.y)) < pr * pr * 4)
                    hoveredPin = { n.id, 3, p };
            }
            // data out
            for (int p = 0; p < pc.outData; p++) {
                ImVec2 pp = VsPinPos(n, 2, p, canvasOrig, zoom);
                dl->AddCircleFilled(pp, pr, 0xFF44CCAA);
                if (n.type != VsNodeType::Group && def.outDataNames[p])
                    dl->AddText(ImVec2(pp.x - pr - 4 * zoom - ImGui::CalcTextSize(def.outDataNames[p]).x, pp.y - 6 * zoom),
                        0xFFCCCCCC, def.outDataNames[p]);
                if (((io.MousePos.x - pp.x) * (io.MousePos.x - pp.x) + (io.MousePos.y - pp.y) * (io.MousePos.y - pp.y)) < pr * pr * 4)
                    hoveredPin = { n.id, 2, p };
            }

            // Hit test node body for hover
            if (io.MousePos.x >= nMin.x && io.MousePos.x <= nMax.x &&
                io.MousePos.y >= nMin.y && io.MousePos.y <= nMax.y)
                hoveredNode = ni;
        }

        // Invisible button for canvas interaction
        ImGui::SetCursorScreenPos(canvasOrig);
        ImGui::InvisibleButton("##canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        // Manual bounds check — IsItemHovered fails when ##NodeProps window overlaps
        bool canvasHovered = (io.MousePos.x >= canvasOrig.x && io.MousePos.x <= canvasOrig.x + canvasSize.x &&
                              io.MousePos.y >= canvasOrig.y && io.MousePos.y <= canvasOrig.y + canvasSize.y);

        // Mouse interactions
        if (canvasHovered) {
            // Zoom with scroll
            if (fabsf(io.MouseWheel) > 0.01f) {
                float oldZoom = sVsZoom;
                sVsZoom = std::clamp(sVsZoom + io.MouseWheel * 0.1f, 0.3f, 3.0f);
                // Zoom toward mouse
                float mx = (io.MousePos.x - canvasOrig.x) / oldZoom - sVsPanX;
                float my = (io.MousePos.y - canvasOrig.y) / oldZoom - sVsPanY;
                sVsPanX = (io.MousePos.x - canvasOrig.x) / sVsZoom - mx;
                sVsPanY = (io.MousePos.y - canvasOrig.y) / sVsZoom - my;
            }

            // Don't process canvas clicks if a widget is active (e.g. combo dropdown open)
            // or if mouse is over the properties panel child window
            bool skipCanvasClick = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
            if (!skipCanvasClick && sVsSelected >= 0) {
                // Check props panel bounds (positioned below selected node)
                const VsNode& sn = sVsNodes[sVsSelected];
                float propW = 260, propH = 180;
                float pnx = canvasOrig.x + (sn.x + sVsPanX) * zoom;
                float pny = canvasOrig.y + (sn.y + sVsPanY) * zoom + VsNodeHeight(sn) * zoom + 4;
                if (pnx + propW > canvasOrig.x + canvasSize.x) pnx = canvasOrig.x + canvasSize.x - propW - 4;
                if (pny + propH > canvasOrig.y + canvasSize.y) pny = canvasOrig.y + canvasSize.y - propH - 4;
                if (pnx < canvasOrig.x) pnx = canvasOrig.x + 4;
                if (pny < canvasOrig.y) pny = canvasOrig.y + 4;
                if (io.MousePos.x >= pnx && io.MousePos.x <= pnx + propW &&
                    io.MousePos.y >= pny && io.MousePos.y <= pny + propH)
                    skipCanvasClick = true;
            }

            // Alt + left click — create annotation
            if (io.MouseClicked[0] && !skipCanvasClick && io.KeyAlt) {
                VsAnnotation ann;
                ann.x = (io.MousePos.x - canvasOrig.x) / zoom - sVsPanX;
                ann.y = (io.MousePos.y - canvasOrig.y) / zoom - sVsPanY;
                ann.w = 300; ann.h = 200;
                snprintf(ann.label, sizeof(ann.label), "Note");
                sVsAnnotations.push_back(ann);
                sVsSelectedAnnotation = (int)sVsAnnotations.size() - 1;
                sVsEditingAnnotation = sVsSelectedAnnotation;
                sVsSelected = -1;
            }
            // Left click (use raw input — ImGui::IsMouseClicked may be eaten by other windows)
            else if (io.MouseClicked[0] && !skipCanvasClick) {
                // Check annotation hit (header drag, resize grip, body click)
                int hitAnnotation = -1;
                bool hitResize = false;
                bool hitResizeLeft = false;
                bool hitHeader = false;
                for (int ai = (int)sVsAnnotations.size() - 1; ai >= 0; ai--) {
                    auto& ann = sVsAnnotations[ai];
                    float ax = canvasOrig.x + (ann.x + sVsPanX) * zoom;
                    float ay = canvasOrig.y + (ann.y + sVsPanY) * zoom;
                    float aw = ann.w * zoom, ah = ann.h * zoom;
                    float headerH = 22.0f * zoom;
                    float grip = 24.0f * zoom;
                    // Resize grip (bottom-right corner)
                    if (io.MousePos.x >= ax + aw - grip && io.MousePos.x <= ax + aw &&
                        io.MousePos.y >= ay + ah - grip && io.MousePos.y <= ay + ah) {
                        hitAnnotation = ai; hitResize = true; break;
                    }
                    // Resize grip (bottom-left corner)
                    if (io.MousePos.x >= ax && io.MousePos.x <= ax + grip &&
                        io.MousePos.y >= ay + ah - grip && io.MousePos.y <= ay + ah) {
                        hitAnnotation = ai; hitResize = true; hitResizeLeft = true; break;
                    }
                    // Header bar
                    if (io.MousePos.x >= ax && io.MousePos.x <= ax + aw &&
                        io.MousePos.y >= ay && io.MousePos.y <= ay + headerH) {
                        hitAnnotation = ai; hitHeader = true; break;
                    }
                }

                if (hoveredPin.nodeId >= 0) {
                    // Start link drag from pin
                    sVsDraggingLink = true;
                    sVsLinkStart = hoveredPin;
                    sVsLinkEndPos = io.MousePos;
                    sVsSelectedAnnotation = -1;
                    sVsEditingAnnotation = -1;
                } else if (hoveredNode >= 0) {
                    // If clicking a node that's already part of multi-selection, keep the group
                    if (!sVsNodes[hoveredNode].selected) {
                        // Clear previous multi-selection
                        for (auto& nd : sVsNodes) nd.selected = false;
                        for (auto& ann : sVsAnnotations) ann.selected = false;
                        sVsNodes[hoveredNode].selected = true;
                    }
                    sVsSelected = hoveredNode;
                    sVsDraggingNode = true;
                    sVsSelectedAnnotation = -1;
                    sVsEditingAnnotation = -1;
                } else if (hitAnnotation >= 0) {
                    sVsSelectedAnnotation = hitAnnotation;
                    sVsSelected = -1;
                    for (auto& nd : sVsNodes) nd.selected = false;
                    for (auto& ann : sVsAnnotations) ann.selected = false;
                    if (hitResize) {
                        sVsResizingAnnotation = true;
                        sVsResizingAnnotationLeft = hitResizeLeft;
                    } else if (hitHeader) {
                        sVsDraggingAnnotation = true;
                    }
                    // Double-click header to edit label
                    if (hitHeader && io.MouseDoubleClicked[0])
                        sVsEditingAnnotation = hitAnnotation;
                    else if (!hitHeader)
                        sVsEditingAnnotation = -1;
                } else {
                    // Start box selection on empty canvas
                    sVsSelected = -1;
                    sVsSelectedAnnotation = -1;
                    sVsEditingAnnotation = -1;
                    for (auto& nd : sVsNodes) nd.selected = false;
                    for (auto& ann : sVsAnnotations) ann.selected = false;
                    sVsBoxSelecting = true;
                    sVsBoxStart = io.MousePos;
                }
            }

            // Right click — node info, disconnect pin, or open context menu
            if (io.MouseClicked[1]) {
                if (hoveredPin.nodeId >= 0) {
                    // Remove all links connected to this pin
                    sVsLinks.erase(std::remove_if(sVsLinks.begin(), sVsLinks.end(),
                        [&](const VsLink& l) {
                            return (l.from.nodeId == hoveredPin.nodeId && l.from.pinType == hoveredPin.pinType && l.from.pinIdx == hoveredPin.pinIdx) ||
                                   (l.to.nodeId == hoveredPin.nodeId && l.to.pinType == hoveredPin.pinType && l.to.pinIdx == hoveredPin.pinIdx);
                        }), sVsLinks.end());
                } else if (hoveredNode >= 0) {
                    // Open node info popup
                    sVsNodeInfoIdx = hoveredNode;
                    memcpy(sVsNodeCodeBuf, sVsNodes[hoveredNode].customCode, sizeof(sVsNodeCodeBuf));
                    sVsNodeInfoJustOpened = true;
                    ImGui::OpenPopup("##NodeInfo");
                } else {
                    sVsShowContextMenu = true;
                    sVsContextMenuPos = io.MousePos;
                }
            }

            // Middle drag — pan canvas
            if (io.MouseClicked[2])
                sVsDraggingCanvas = true;
        }

        // Drag node (move all selected nodes together)
        if (sVsDraggingNode && ImGui::IsMouseDragging(0) && sVsSelected >= 0) {
            float dx = io.MouseDelta.x / zoom;
            float dy = io.MouseDelta.y / zoom;
            for (auto& nd : sVsNodes) {
                if (nd.selected) { nd.x += dx; nd.y += dy; }
            }
            for (auto& ann : sVsAnnotations) {
                if (ann.selected) { ann.x += dx; ann.y += dy; }
            }
        }
        if (!ImGui::IsMouseDown(0))
            sVsDraggingNode = false;

        // Drag / resize annotation
        if (sVsDraggingAnnotation && ImGui::IsMouseDragging(0) && sVsSelectedAnnotation >= 0) {
            sVsAnnotations[sVsSelectedAnnotation].x += io.MouseDelta.x / zoom;
            sVsAnnotations[sVsSelectedAnnotation].y += io.MouseDelta.y / zoom;
        }
        if (!ImGui::IsMouseDown(0))
            sVsDraggingAnnotation = false;
        if (sVsResizingAnnotation && ImGui::IsMouseDragging(0) && sVsSelectedAnnotation >= 0) {
            auto& ra = sVsAnnotations[sVsSelectedAnnotation];
            float dx = io.MouseDelta.x / zoom;
            float dy = io.MouseDelta.y / zoom;
            if (sVsResizingAnnotationLeft) {
                // Left grip: move x, shrink w
                if (ra.w - dx >= 100) { ra.x += dx; ra.w -= dx; }
            } else {
                ra.w += dx;
            }
            ra.h += dy;
            if (ra.w < 100) ra.w = 100;
            if (ra.h < 60) ra.h = 60;
        }
        if (!ImGui::IsMouseDown(0)) {
            sVsResizingAnnotation = false;
            sVsResizingAnnotationLeft = false;
        }

        // Delete annotation with Delete key
        if (sVsSelectedAnnotation >= 0 && sVsEditingAnnotation < 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            sVsAnnotations.erase(sVsAnnotations.begin() + sVsSelectedAnnotation);
            sVsSelectedAnnotation = -1;
        }

        // Box selection
        if (sVsBoxSelecting) {
            if (ImGui::IsMouseDragging(0)) {
                // Draw selection rectangle
                ImVec2 bMin(std::min(sVsBoxStart.x, io.MousePos.x), std::min(sVsBoxStart.y, io.MousePos.y));
                ImVec2 bMax(std::max(sVsBoxStart.x, io.MousePos.x), std::max(sVsBoxStart.y, io.MousePos.y));
                dl->AddRectFilled(bMin, bMax, 0x22FFAA44);
                dl->AddRect(bMin, bMax, 0xAAFFAA44, 0, 0, 1.0f);
                // Live-select nodes within box (only at current editing level)
                for (auto& nd : sVsNodes) {
                    if ((int)nd.type < 0 || (int)nd.type >= (int)VsNodeType::COUNT) continue;
                    if (nd.groupId != sVsEditingGroup) { nd.selected = false; continue; }
                    float nx = canvasOrig.x + (nd.x + sVsPanX) * zoom;
                    float ny = canvasOrig.y + (nd.y + sVsPanY) * zoom;
                    float nw = kVsNodeW * zoom;
                    float nh = VsNodeHeight(nd) * zoom;
                    nd.selected = (nx + nw > bMin.x && nx < bMax.x && ny + nh > bMin.y && ny < bMax.y);
                }
                // Live-select annotations — only when box crosses an edge (not fully inside)
                for (auto& ann : sVsAnnotations) {
                    float ax = canvasOrig.x + (ann.x + sVsPanX) * zoom;
                    float ay = canvasOrig.y + (ann.y + sVsPanY) * zoom;
                    float aw = ann.w * zoom;
                    float ah = ann.h * zoom;
                    bool overlaps = (ax + aw > bMin.x && ax < bMax.x && ay + ah > bMin.y && ay < bMax.y);
                    bool boxInsideAnn = (bMin.x >= ax && bMax.x <= ax + aw && bMin.y >= ay && bMax.y <= ay + ah);
                    ann.selected = overlaps && !boxInsideAnn;
                }
            }
            if (!ImGui::IsMouseDown(0))
                sVsBoxSelecting = false;
        }

        // Annotation label editing (inline text input on header)
        if (sVsEditingAnnotation >= 0 && sVsEditingAnnotation < (int)sVsAnnotations.size()) {
            auto& ann = sVsAnnotations[sVsEditingAnnotation];
            float ax = canvasOrig.x + (ann.x + sVsPanX) * zoom;
            float ay = canvasOrig.y + (ann.y + sVsPanY) * zoom;
            float aw = ann.w * zoom;
            ImGui::SetCursorScreenPos(ImVec2(ax + 2, ay + 1));
            ImGui::PushItemWidth(aw - 4);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 0.9f));
            if (ImGui::InputText("##annlabel", ann.label, sizeof(ann.label),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                sVsEditingAnnotation = -1;
            // Auto-focus the input on first frame
            if (!ImGui::IsItemActive() && ImGui::IsWindowFocused())
                ImGui::SetKeyboardFocusHere(-1);
            ImGui::PopStyleColor();
            ImGui::PopItemWidth();
        }

        // Drag canvas (middle mouse)
        if (sVsDraggingCanvas && ImGui::IsMouseDragging(2)) {
            sVsPanX += io.MouseDelta.x / zoom;
            sVsPanY += io.MouseDelta.y / zoom;
        }
        if (!ImGui::IsMouseDown(2))
            sVsDraggingCanvas = false;

        // Drag link
        if (sVsDraggingLink) {
            sVsLinkEndPos = io.MousePos;
            if (!ImGui::IsMouseDown(0)) {
                sVsDraggingLink = false;
                // Complete link if hovering a compatible pin
                if (hoveredPin.nodeId >= 0 && hoveredPin.nodeId != sVsLinkStart.nodeId) {
                    // Exec->Exec or Data->Data
                    bool execLink = (sVsLinkStart.pinType == 0 && hoveredPin.pinType == 1);
                    bool dataLink = (sVsLinkStart.pinType == 2 && hoveredPin.pinType == 3);
                    bool execLinkRev = (sVsLinkStart.pinType == 1 && hoveredPin.pinType == 0);
                    bool dataLinkRev = (sVsLinkStart.pinType == 3 && hoveredPin.pinType == 2);
                    if (execLink || dataLink) {
                        sVsLinks.push_back({ sVsLinkStart, hoveredPin });
                    } else if (execLinkRev || dataLinkRev) {
                        sVsLinks.push_back({ hoveredPin, sVsLinkStart });
                    }
                } else if (hoveredPin.nodeId < 0 && hoveredNode < 0) {
                    // Dropped on empty space — open context menu and auto-wire
                    sVsShowContextMenu = true;
                    sVsContextMenuPos = io.MousePos;
                    sVsPendingAutoWire = sVsLinkStart;
                }
            }
        }

        // Delete selected nodes with Delete key (multi-select or single)
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            // Collect all node IDs to delete (selected or sVsSelected)
            std::set<int> delIds;
            for (auto& nd : sVsNodes)
                if (nd.selected) delIds.insert(nd.id);
            if (sVsSelected >= 0 && sVsSelected < (int)sVsNodes.size())
                delIds.insert(sVsNodes[sVsSelected].id);

            bool anyAnnotDel = false;
            for (auto& ann : sVsAnnotations)
                if (ann.selected) { anyAnnotDel = true; break; }

            if (!delIds.empty() || anyAnnotDel) {
                // Save undo snapshot before deleting
                VsUndoSnapshot snap;
                snap.nodes = sVsNodes;
                snap.links = sVsLinks;
                snap.annotations = sVsAnnotations;
                snap.groupPins = sVsGroupPins;
                snap.nextId = sVsNextId;
                sVsUndoStack.push_back(snap);
                if ((int)sVsUndoStack.size() > kVsMaxUndo)
                    sVsUndoStack.erase(sVsUndoStack.begin());
            }

            if (!delIds.empty()) {
                // For each group being deleted, also mark children for deletion
                std::set<int> extraDel;
                for (int did : delIds) {
                    int idx = VsFindNode(did);
                    if (idx >= 0 && sVsNodes[idx].type == VsNodeType::Group) {
                        for (auto& nd : sVsNodes)
                            if (nd.groupId == did) extraDel.insert(nd.id);
                    }
                }
                delIds.insert(extraDel.begin(), extraDel.end());

                // Remove all links connected to any deleted node
                sVsLinks.erase(std::remove_if(sVsLinks.begin(), sVsLinks.end(),
                    [&delIds](const VsLink& l) { return delIds.count(l.from.nodeId) || delIds.count(l.to.nodeId); }),
                    sVsLinks.end());
                // Remove pin mappings for deleted groups
                sVsGroupPins.erase(std::remove_if(sVsGroupPins.begin(), sVsGroupPins.end(),
                    [&delIds](const VsGroupPinMap& m) { return delIds.count(m.groupNodeId); }), sVsGroupPins.end());
                // Remove the nodes
                sVsNodes.erase(std::remove_if(sVsNodes.begin(), sVsNodes.end(),
                    [&delIds](const VsNode& nd) { return delIds.count(nd.id); }), sVsNodes.end());
                sVsSelected = -1;
            }

            // Delete selected annotations
            sVsAnnotations.erase(std::remove_if(sVsAnnotations.begin(), sVsAnnotations.end(),
                [](const VsAnnotation& a) { return a.selected; }), sVsAnnotations.end());
            sVsSelectedAnnotation = -1;
        }

        // Ctrl+Z — undo last delete
        if (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyCtrl && !io.KeyShift && !sVsUndoStack.empty()) {
            auto& snap = sVsUndoStack.back();
            sVsNodes = snap.nodes;
            sVsLinks = snap.links;
            sVsAnnotations = snap.annotations;
            sVsGroupPins = snap.groupPins;
            sVsNextId = snap.nextId;
            sVsUndoStack.pop_back();
            sVsSelected = -1;
            sVsSelectedAnnotation = -1;
        }

        // Ctrl+G — group selected nodes
        if (ImGui::IsKeyPressed(ImGuiKey_G) && io.KeyCtrl && !io.KeyShift) {
            std::vector<int> selIds;
            for (auto& nd : sVsNodes)
                if (nd.selected && nd.groupId == sVsEditingGroup)
                    selIds.push_back(nd.id);
            if (selIds.size() >= 2) {
                // Compute centroid
                float cx = 0, cy = 0;
                for (int id : selIds) { int i = VsFindNode(id); cx += sVsNodes[i].x; cy += sVsNodes[i].y; }
                cx /= selIds.size(); cy /= selIds.size();

                VsNode gn;
                gn.id = sVsNextId++;
                gn.type = VsNodeType::Group;
                gn.x = cx; gn.y = cy;
                gn.groupId = sVsEditingGroup;
                snprintf(gn.groupLabel, sizeof(gn.groupLabel), "Group %d", gn.id);

                // Reparent selected nodes into group
                for (int id : selIds) {
                    int i = VsFindNode(id);
                    sVsNodes[i].groupId = gn.id;
                    sVsNodes[i].selected = false;
                }

                sVsNodes.push_back(gn);
                VsRecomputeGroupPins(gn.id);
                sVsSelected = (int)sVsNodes.size() - 1;
                for (auto& nd : sVsNodes) nd.selected = false;
            }
        }

        // Ctrl+Shift+G — ungroup selected group node
        if (ImGui::IsKeyPressed(ImGuiKey_G) && io.KeyCtrl && io.KeyShift && sVsSelected >= 0) {
            VsNode& sel = sVsNodes[sVsSelected];
            if (sel.type == VsNodeType::Group) {
                int gid = sel.id;
                // Reparent children back to current level
                for (auto& nd : sVsNodes)
                    if (nd.groupId == gid) nd.groupId = sVsEditingGroup;
                // Remove pin mappings
                sVsGroupPins.erase(std::remove_if(sVsGroupPins.begin(), sVsGroupPins.end(),
                    [gid](const VsGroupPinMap& m) { return m.groupNodeId == gid; }), sVsGroupPins.end());
                // Remove links that connected to the group node itself
                sVsLinks.erase(std::remove_if(sVsLinks.begin(), sVsLinks.end(),
                    [gid](const VsLink& l) { return l.from.nodeId == gid || l.to.nodeId == gid; }),
                    sVsLinks.end());
                // Delete the group node
                int gi = VsFindNode(gid);
                if (gi >= 0) sVsNodes.erase(sVsNodes.begin() + gi);
                sVsSelected = -1;
            }
        }

        // Double-click to enter group node
        if (sVsSelected >= 0 && io.MouseDoubleClicked[0]) {
            VsNode& sel = sVsNodes[sVsSelected];
            if (sel.type == VsNodeType::Group) {
                sVsParentPanX = sVsPanX;
                sVsParentPanY = sVsPanY;
                sVsParentZoom = sVsZoom;
                sVsEditingGroup = sel.id;
                sVsPanX = 0; sVsPanY = 0; sVsZoom = 1.0f;
                sVsSelected = -1;
                for (auto& nd : sVsNodes) nd.selected = false;
            }
        }

        // Escape to exit group editing
        if (sVsEditingGroup != 0 && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            VsRecomputeGroupPins(sVsEditingGroup);
            sVsEditingGroup = 0;
            sVsPanX = sVsParentPanX;
            sVsPanY = sVsParentPanY;
            sVsZoom = sVsParentZoom;
            sVsSelected = -1;
        }

        // Ctrl+A — select all nodes + annotations at current level
        if (ImGui::IsKeyPressed(ImGuiKey_A) && io.KeyCtrl) {
            for (auto& nd : sVsNodes)
                if (nd.groupId == sVsEditingGroup) nd.selected = true;
            for (auto& ann : sVsAnnotations) ann.selected = true;
        }

        // Ctrl+C — copy selected nodes/links/annotations to system clipboard
        if (ImGui::IsKeyPressed(ImGuiKey_C) && io.KeyCtrl) {
            std::string cb = "[AffinityNodes]\n";
            // Collect selected node IDs
            std::vector<int> selIds;
            for (auto& nd : sVsNodes)
                if (nd.selected || (&nd - &sVsNodes[0]) == sVsSelected)
                    selIds.push_back(nd.id);
            // Nodes
            for (int id : selIds) {
                int ni = VsFindNode(id);
                if (ni < 0) continue;
                auto& n = sVsNodes[ni];
                char buf[1024];
                snprintf(buf, sizeof(buf), "vsNode=%d,%d,%.1f,%.1f,%d,%d,%d,%d,%d\n",
                    n.id, (int)n.type, n.x, n.y,
                    n.paramInt[0], n.paramInt[1], n.paramInt[2], n.paramInt[3], n.groupId);
                cb += buf;
                if (n.type == VsNodeType::Group) {
                    snprintf(buf, sizeof(buf), "vsGroupDef=%d|%s|%d,%d,%d,%d\n", n.id, n.groupLabel,
                        n.grpInExec, n.grpOutExec, n.grpInData, n.grpOutData);
                    cb += buf;
                }
                if (n.customCode[0]) {
                    snprintf(buf, sizeof(buf), "vsNodeCode=%d|%s\n", n.id, n.customCode);
                    cb += buf;
                }
            }
            // Links (only between selected nodes)
            std::set<int> selSet(selIds.begin(), selIds.end());
            for (auto& lk : sVsLinks) {
                if (selSet.count(lk.from.nodeId) && selSet.count(lk.to.nodeId)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "vsLink=%d,%d,%d|%d,%d,%d\n",
                        lk.from.nodeId, lk.from.pinType, lk.from.pinIdx,
                        lk.to.nodeId, lk.to.pinType, lk.to.pinIdx);
                    cb += buf;
                }
            }
            // Annotations
            for (auto& ann : sVsAnnotations) {
                if (ann.selected) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "vsAnnot=%.1f,%.1f,%.1f,%.1f|%s\n",
                        ann.x, ann.y, ann.w, ann.h, ann.label);
                    cb += buf;
                }
            }
            // Group pin mappings for selected groups
            for (auto& m : sVsGroupPins) {
                if (selSet.count(m.groupNodeId)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "vsGroupPin=%d,%d,%d,%d,%d,%d\n",
                        m.groupNodeId, m.pinType, m.pinIdx, m.innerNodeId, m.innerPinType, m.innerPinIdx);
                    cb += buf;
                }
            }
            ImGui::SetClipboardText(cb.c_str());
        }

        // Ctrl+V — paste nodes from system clipboard
        if (ImGui::IsKeyPressed(ImGuiKey_V) && io.KeyCtrl) {
            const char* clipText = ImGui::GetClipboardText();
            if (clipText && strstr(clipText, "[AffinityNodes]")) {
                // Parse clipboard into temp collections
                std::vector<VsNode> pasteNodes;
                std::vector<VsLink> pasteLinks;
                std::vector<VsAnnotation> pasteAnnots;
                std::vector<VsGroupPinMap> pastePins;
                std::map<int, int> idRemap; // old ID -> new ID

                // Parse line by line
                std::istringstream ss(clipText);
                std::string line;
                while (std::getline(ss, line)) {
                    if (line.empty() || line[0] == '[') continue;
                    const char* l = line.c_str();

                    VsNode n;
                    int typeInt, gid = 0;
                    if (sscanf(l, "vsNode=%d,%d,%f,%f,%d,%d,%d,%d,%d",
                        &n.id, &typeInt, &n.x, &n.y,
                        &n.paramInt[0], &n.paramInt[1], &n.paramInt[2], &n.paramInt[3], &gid) >= 4)
                    {
                        n.type = (VsNodeType)typeInt;
                        n.groupId = gid;
                        pasteNodes.push_back(n);
                        continue;
                    }

                    int nid;
                    char codeBuf[512] = {};
                    if (sscanf(l, "vsNodeCode=%d|%511[^\n]", &nid, codeBuf) >= 2) {
                        for (auto& pn : pasteNodes)
                            if (pn.id == nid) { strncpy(pn.customCode, codeBuf, sizeof(pn.customCode) - 1); break; }
                        continue;
                    }

                    int ie, oe, id2, od;
                    char lbl[32] = {};
                    if (sscanf(l, "vsGroupDef=%d|%31[^|]|%d,%d,%d,%d", &nid, lbl, &ie, &oe, &id2, &od) >= 6) {
                        for (auto& pn : pasteNodes)
                            if (pn.id == nid) {
                                strncpy(pn.groupLabel, lbl, sizeof(pn.groupLabel) - 1);
                                pn.grpInExec = ie; pn.grpOutExec = oe;
                                pn.grpInData = id2; pn.grpOutData = od;
                                break;
                            }
                        continue;
                    }

                    VsLink lk;
                    if (sscanf(l, "vsLink=%d,%d,%d|%d,%d,%d",
                        &lk.from.nodeId, &lk.from.pinType, &lk.from.pinIdx,
                        &lk.to.nodeId, &lk.to.pinType, &lk.to.pinIdx) == 6)
                    {
                        pasteLinks.push_back(lk);
                        continue;
                    }

                    VsAnnotation ann;
                    float ax, ay, aw, ah;
                    if (sscanf(l, "vsAnnot=%f,%f,%f,%f|", &ax, &ay, &aw, &ah) == 4) {
                        ann.x = ax; ann.y = ay; ann.w = aw; ann.h = ah;
                        const char* pipe = strchr(l + 8, '|');
                        if (pipe) strncpy(ann.label, pipe + 1, sizeof(ann.label) - 1);
                        pasteAnnots.push_back(ann);
                        continue;
                    }

                    VsGroupPinMap m;
                    if (sscanf(l, "vsGroupPin=%d,%d,%d,%d,%d,%d",
                        &m.groupNodeId, &m.pinType, &m.pinIdx, &m.innerNodeId, &m.innerPinType, &m.innerPinIdx) == 6)
                    {
                        pastePins.push_back(m);
                        continue;
                    }
                }

                if (!pasteNodes.empty()) {
                    // Assign new IDs and build remap
                    for (auto& pn : pasteNodes) {
                        int oldId = pn.id;
                        pn.id = sVsNextId++;
                        idRemap[oldId] = pn.id;
                    }

                    // Offset positions to paste near center of view
                    float cx = 0, cy = 0;
                    for (auto& pn : pasteNodes) { cx += pn.x; cy += pn.y; }
                    cx /= pasteNodes.size(); cy /= pasteNodes.size();
                    float targetX = (-sVsPanX + canvasSize.x * 0.5f) / zoom;
                    float targetY = (-sVsPanY + canvasSize.y * 0.5f) / zoom;
                    float offX = targetX - cx, offY = targetY - cy;
                    for (auto& pn : pasteNodes) { pn.x += offX; pn.y += offY; }
                    for (auto& pa : pasteAnnots) { pa.x += offX; pa.y += offY; }

                    // Remap group IDs
                    for (auto& pn : pasteNodes) {
                        if (pn.groupId != 0 && idRemap.count(pn.groupId))
                            pn.groupId = idRemap[pn.groupId];
                        else
                            pn.groupId = sVsEditingGroup;
                    }

                    // Remap links
                    for (auto& lk : pasteLinks) {
                        if (idRemap.count(lk.from.nodeId)) lk.from.nodeId = idRemap[lk.from.nodeId];
                        if (idRemap.count(lk.to.nodeId))   lk.to.nodeId = idRemap[lk.to.nodeId];
                    }

                    // Remap group pin mappings
                    for (auto& m : pastePins) {
                        if (idRemap.count(m.groupNodeId)) m.groupNodeId = idRemap[m.groupNodeId];
                        if (idRemap.count(m.innerNodeId)) m.innerNodeId = idRemap[m.innerNodeId];
                    }

                    // Deselect existing, select pasted
                    for (auto& nd : sVsNodes) nd.selected = false;
                    for (auto& ann : sVsAnnotations) ann.selected = false;
                    for (auto& pn : pasteNodes) pn.selected = true;
                    for (auto& pa : pasteAnnots) pa.selected = true;

                    // Add to editor state
                    sVsNodes.insert(sVsNodes.end(), pasteNodes.begin(), pasteNodes.end());
                    sVsLinks.insert(sVsLinks.end(), pasteLinks.begin(), pasteLinks.end());
                    sVsAnnotations.insert(sVsAnnotations.end(), pasteAnnots.begin(), pasteAnnots.end());
                    sVsGroupPins.insert(sVsGroupPins.end(), pastePins.begin(), pastePins.end());
                    sVsSelected = -1;
                    sProjectDirty = true;
                }
            }
        }

        // Breadcrumb when inside a group
        if (sVsEditingGroup != 0) {
            int gi = VsFindNode(sVsEditingGroup);
            const char* lbl = (gi >= 0 && sVsNodes[gi].groupLabel[0]) ? sVsNodes[gi].groupLabel : "Group";
            char bc[64]; snprintf(bc, sizeof(bc), "< Esc | Editing: %s", lbl);
            dl->AddText(ImVec2(canvasOrig.x + 10, canvasOrig.y + 10), 0xFFFFFF88, bc);
        }

        dl->PopClipRect();

        // Space — open add-node menu at mouse cursor (Blender-style)
        if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::IsPopupOpen("##AddNode")) {
            sVsShowContextMenu = true;
            sVsContextMenuPos = io.MousePos;
        }

        // Context menu (right-click or Space to add node)
        static char sVsNodeSearch[64] = {};
        static bool sVsSearchFocused = false;
        // Node info popup (right-click on node)
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(300, 500));
        if (ImGui::BeginPopup("##NodeInfo")) {
            if (sVsNodeInfoIdx >= 0 && sVsNodeInfoIdx < (int)sVsNodes.size()) {
                VsNode& infoNode = sVsNodes[sVsNodeInfoIdx];
                const auto& infoDef = sVsNodeDefs[(int)infoNode.type];

                // Header
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", infoDef.name);
                ImGui::Separator();

                // Description
                const char* desc = "";
                switch (infoNode.type) {
                case VsNodeType::OnKeyPressed:  desc = "Fires once when a key is pressed down."; break;
                case VsNodeType::OnKeyReleased: desc = "Fires once when a key is released."; break;
                case VsNodeType::OnKeyHeld:     desc = "Fires every frame while a key is held."; break;
                case VsNodeType::OnCollision:   desc = "Fires when the player collides with an object."; break;
                case VsNodeType::OnStart:       desc = "Fires once when the scene starts."; break;
                case VsNodeType::OnUpdate:      desc = "Fires every frame."; break;
                case VsNodeType::Branch:        desc = "If condition is true, execute True path; otherwise False."; break;
                case VsNodeType::CompareVar:    desc = "Compares a variable slot against a value. Outputs 1 or 0."; break;
                case VsNodeType::MovePlayer:    desc = "Moves the player in a direction while its key is held."; break;
                case VsNodeType::LookDirection: desc = "Sets the player's facing direction."; break;
                case VsNodeType::ChangeScene:   desc = "Loads a different scene by index."; break;
                case VsNodeType::SetVariable:   desc = "Sets a variable slot to a value."; break;
                case VsNodeType::AddVariable:   desc = "Adds an amount to a variable slot."; break;
                case VsNodeType::PlaySound:     desc = "Plays a sound effect by ID."; break;
                case VsNodeType::Wait:          desc = "Pauses execution for a number of frames."; break;
                case VsNodeType::Jump:          desc = "Makes the player jump with the given force. Only works when grounded."; break;
                case VsNodeType::Walk:          desc = "Sets the player's movement speed (walk)."; break;
                case VsNodeType::Sprint:        desc = "Sets the player's movement speed (sprint)."; break;
                case VsNodeType::OrbitCamera:   desc = "Rotates the orbit camera in a direction at a speed."; break;
                case VsNodeType::PlayAnim:      desc = "Plays an animation on the player sprite."; break;
                case VsNodeType::SetGravity:    desc = "Sets gravity strength (pixels per frame^2)."; break;
                case VsNodeType::SetMaxFall:    desc = "Sets the maximum fall speed (terminal velocity)."; break;
                case VsNodeType::DestroyObject: desc = "Removes a sprite/object from the scene."; break;
                case VsNodeType::AutoOrbit:     desc = "Enables auto-orbit camera when strafing. 0 = disabled."; break;
                case VsNodeType::DampenJump:    desc = "Multiplies upward velocity by factor when fired. Use with On Key Released for variable jump height."; break;
                case VsNodeType::Integer:       desc = "Outputs a constant integer value."; break;
                case VsNodeType::Key:           desc = "Outputs a key constant (A, B, L, R, etc)."; break;
                case VsNodeType::Direction:     desc = "Outputs a direction (Left, Right, Up, Down)."; break;
                case VsNodeType::Animation:     desc = "Outputs an animation index."; break;
                case VsNodeType::Float:         desc = "Outputs a constant float value."; break;
                case VsNodeType::Group:         desc = "Groups nodes into a reusable subgraph."; break;
                default: desc = "No description."; break;
                }
                ImGui::TextWrapped("%s", desc);
                ImGui::Spacing();

                // Resolve data input values by following links
                auto resolveDataIn = [&](int nodeId, int pinIdx) -> VsNode* {
                    for (auto& lk : sVsLinks)
                        if (lk.to.nodeId == nodeId && lk.to.pinType == 3 && lk.to.pinIdx == pinIdx)
                            for (auto& src : sVsNodes)
                                if (src.id == lk.from.nodeId) return &src;
                    return nullptr;
                };
                auto resolveFloat = [&](int nodeId, int pinIdx, float def) -> float {
                    VsNode* src = resolveDataIn(nodeId, pinIdx);
                    if (src && src->type == VsNodeType::Float) { float v; memcpy(&v, &src->paramInt[0], sizeof(float)); return v; }
                    return def;
                };
                auto resolveInt = [&](int nodeId, int pinIdx, int def) -> int {
                    VsNode* src = resolveDataIn(nodeId, pinIdx);
                    if (src && src->type == VsNodeType::Integer) return src->paramInt[0];
                    if (src && src->type == VsNodeType::Float) { float v; memcpy(&v, &src->paramInt[0], sizeof(float)); return (int)(v * 256.0f); }
                    return def;
                };
                auto resolveDir = [&](int nodeId, int pinIdx) -> int {
                    VsNode* src = resolveDataIn(nodeId, pinIdx);
                    return src ? src->paramInt[0] : 0;
                };

                // Helper: format a resolved float value or placeholder
                auto fmtFloat = [&](int nodeId, int pinIdx, const char* placeholder) -> const char* {
                    static char buf[32];
                    VsNode* src = resolveDataIn(nodeId, pinIdx);
                    if (!src) return placeholder;
                    float v; memcpy(&v, &src->paramInt[0], sizeof(float));
                    snprintf(buf, sizeof(buf), "%d", (int)(v * 256.0f));
                    return buf;
                };
                auto fmtInt = [&](int nodeId, int pinIdx, const char* placeholder) -> const char* {
                    static char buf[32];
                    VsNode* src = resolveDataIn(nodeId, pinIdx);
                    if (!src) return placeholder;
                    if (src->type == VsNodeType::Integer) { snprintf(buf, sizeof(buf), "%d", src->paramInt[0]); return buf; }
                    if (src->type == VsNodeType::Float) { float v; memcpy(&v, &src->paramInt[0], sizeof(float)); snprintf(buf, sizeof(buf), "%d", (int)(v * 256.0f)); return buf; }
                    return placeholder;
                };

                // Show both editor Play-mode and GBA runtime code
                char defaultCodeBuf[4096] = {};
                char gbaCodeBuf[1024] = {};
                const char* defaultCode = "";
                const char* editorCode = "";
                const char* gbaCode = "";

                // Helper: build function signature and GBA body for action node preview
                char funcSigBuf[128] = {};  // function signature line (empty = not an action node)
                char gbaBodyBuf[512] = {};  // GBA body line(s) without wrapper
                auto setActionFunc = [&](const VsNode& node, const char* suffix, const char* body) {
                    bool isBp = (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0);
                    if (node.funcName[0])
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void %s(void)", node.funcName);
                    else if (isBp)
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void afn_bp%d%s_%d(...)", sVsEditBlueprintIdx, suffix, node.id);
                    else
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void afn_script%s_%d(void)", suffix, node.id);
                    strncpy(gbaBodyBuf, body, sizeof(gbaBodyBuf) - 1);
                };

                // Resolve event key name for GBA code display
                auto resolveEventKeyForDisplay = [&](const VsNode& ev) -> int {
                    int count = 0; int keyVal = -1;
                    for (auto& lk : sVsLinks)
                        if (lk.to.nodeId == ev.id && lk.to.pinType == 3 && lk.to.pinIdx == 0) {
                            auto* dn = resolveDataIn(lk.to.nodeId, 0);
                            if (dn) { keyVal = dn->paramInt[0]; count++; }
                        }
                    if (count == 1) return keyVal;
                    return -1; // no key or multiple keys
                };
                auto gbaKeyName = [](int key) -> const char* {
                    const char* names[] = { "KEY_A","KEY_B","KEY_L","KEY_R","KEY_START","KEY_SELECT",
                                            "KEY_UP","KEY_DOWN","KEY_LEFT","KEY_RIGHT" };
                    return (key >= 0 && key < 10) ? names[key] : "0";
                };

                // Build function prefix for blueprint vs scene
                bool isBp = (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0);
                const char* bpPrefix = "";
                char bpFuncBuf[64] = {};
                char bpParamSig[128] = {};
                if (isBp && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                    snprintf(bpFuncBuf, sizeof(bpFuncBuf), "afn_bp%d", sVsEditBlueprintIdx);
                    bpPrefix = bpFuncBuf;
                    auto& bp = sBlueprintAssets[sVsEditBlueprintIdx];
                    bpParamSig[0] = '\0';
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        char tmp[32]; snprintf(tmp, sizeof(tmp), "%sint p%d", pi > 0 ? ", " : "", pi);
                        strncat(bpParamSig, tmp, sizeof(bpParamSig) - strlen(bpParamSig) - 1);
                    }
                } else {
                    bpPrefix = "afn_script";
                    bpParamSig[0] = '\0';
                }

                // Helper: build function signature for event node preview
                auto setEventFunc = [&](const VsNode& node, const char* defaultName) {
                    if (node.funcName[0])
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void %s(void)", node.funcName);
                    else if (isBp)
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void afn_bp%d_%s(%s)", sVsEditBlueprintIdx, defaultName,
                            bpParamSig[0] ? bpParamSig : "void");
                    else
                        snprintf(funcSigBuf, sizeof(funcSigBuf),
                            "static inline void %s(void)", defaultName);
                };

                // Helper: find action nodes connected via exec-out from an event node
                // and build their GBA call names as a string
                auto buildActionCalls = [&](const VsNode& evNode, const char* indent) -> std::string {
                    char callsBuf[1024] = {};
                    // Suffix map for VsNodeType -> GBA function suffix
                    auto editorActionSuffix = [](VsNodeType t) -> const char* {
                        switch (t) {
                        case VsNodeType::MovePlayer:    return "_move";
                        case VsNodeType::Jump:          return "_jump";
                        case VsNodeType::Walk:          return "_walk";
                        case VsNodeType::Sprint:        return "_sprint";
                        case VsNodeType::OrbitCamera:   return "_orbit";
                        case VsNodeType::PlayAnim:      return "_play_anim";
                        case VsNodeType::SetGravity:    return "_set_gravity";
                        case VsNodeType::SetMaxFall:    return "_set_max_fall";
                        case VsNodeType::DestroyObject: return "_destroy";
                        case VsNodeType::AutoOrbit:     return "_auto_orbit";
                        case VsNodeType::DampenJump:    return "_dampen_jump";
                        case VsNodeType::ChangeScene:   return "_change_scene";
                        case VsNodeType::CustomCode:    return "_custom";
                        case VsNodeType::SetVariable:   return "_set_var";
                        case VsNodeType::AddVariable:   return "_add_var";
                        default: return "";
                        }
                    };
                    // Walk exec-out links from the event node
                    std::vector<int> actionIds;
                    std::vector<int> frontier;
                    frontier.push_back(evNode.id);
                    while (!frontier.empty()) {
                        int curId = frontier.back(); frontier.pop_back();
                        for (auto& lk : sVsLinks) {
                            if (lk.from.nodeId == curId && lk.from.pinType == 0) {
                                int toId = lk.to.nodeId;
                                // Find the target node
                                for (auto& n : sVsNodes) {
                                    if (n.id == toId) {
                                        const char* suf = editorActionSuffix(n.type);
                                        if (suf[0]) {
                                            actionIds.push_back(toId);
                                            char line[128];
                                            if (n.funcName[0])
                                                snprintf(line, sizeof(line), "%s%s();\n", indent, n.funcName);
                                            else if (isBp)
                                                snprintf(line, sizeof(line), "%safn_bp%d%s_%d(...);\n", indent, sVsEditBlueprintIdx, suf, n.id);
                                            else
                                                snprintf(line, sizeof(line), "%safn_script%s_%d();\n", indent, suf, n.id);
                                            strncat(callsBuf, line, sizeof(callsBuf) - strlen(callsBuf) - 1);
                                        }
                                        // Follow chain further
                                        frontier.push_back(toId);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (!callsBuf[0])
                        snprintf(callsBuf, sizeof(callsBuf), "%s// (no actions connected)\n", indent);
                    return std::string(callsBuf);
                };

                switch (infoNode.type) {
                case VsNodeType::OnKeyPressed: {
                    setEventFunc(infoNode, "afn_script_key_pressed");
                    editorCode =
                        "    // ---- 2D Tilemap ----\n"
                        "    // ImGui::IsKeyPressed(dirKey, false) — no repeat\n"
                        "    if (keyJustPressed(dir)) {\n"
                        "        instantMove = true;\n"
                        "        for (action : actions) execAction(action);\n"
                        "        instantMove = false;\n"
                        "    }\n"
                        "\n"
                        "    // ---- 3D Scene ----\n"
                        "    if (editorKeyHit(key)) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "    }";
                    int ek = resolveEventKeyForDisplay(infoNode);
                    std::string calls = buildActionCalls(infoNode, "        ");
                    if (ek >= 0)
                        snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                            "    if (key_hit(%s)) {\n%s    }", gbaKeyName(ek), calls.c_str());
                    else
                        snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                            "    { // (all d-pad)\n%s    }", calls.c_str());
                    break;
                }
                case VsNodeType::OnKeyReleased: {
                    setEventFunc(infoNode, "afn_script_key_released");
                    editorCode =
                        "    // ---- 2D Tilemap ----\n"
                        "    if (prevKeys[key] && !keyDown(key)) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "    }\n"
                        "\n"
                        "    // ---- 3D Scene ----\n"
                        "    if (prevKeyState[key] && !editorKeyDown(key)) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "    }";
                    int ek = resolveEventKeyForDisplay(infoNode);
                    std::string calls = buildActionCalls(infoNode, "        ");
                    snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                        "    if (key_released(%s)) {\n%s    }",
                        ek >= 0 ? gbaKeyName(ek) : "KEY_xxx", calls.c_str());
                    break;
                }
                case VsNodeType::OnKeyHeld: {
                    setEventFunc(infoNode, "afn_script_key_held");
                    editorCode =
                        "    // ---- 2D Tilemap ----\n"
                        "    // lastMoveDir = resolveHeldDir(WASD);\n"
                        "    if (keyDown(key)) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "        // MovePlayer gates on lastMoveDir\n"
                        "    }\n"
                        "    // reset accum for released dirs\n"
                        "\n"
                        "    // ---- 3D Scene ----\n"
                        "    // fires every frame while held\n"
                        "    if (editorKeyDown(key)) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "    }";
                    int ek = resolveEventKeyForDisplay(infoNode);
                    std::string calls = buildActionCalls(infoNode, "        ");
                    snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                        "    if (key_is_down(%s)) {\n%s    }",
                        ek >= 0 ? gbaKeyName(ek) : "KEY_xxx", calls.c_str());
                    break;
                }
                case VsNodeType::OnStart: {
                    setEventFunc(infoNode, "afn_script_start");
                    editorCode =
                        "    // ---- 2D Tilemap ----\n"
                        "    if (!onStartRan) {\n"
                        "        for (action : actions) execAction(action);\n"
                        "        onStartRan = true;\n"
                        "    }\n"
                        "\n"
                        "    // ---- 3D Scene ----\n"
                        "    if (!sScriptStartRan) {\n"
                        "        sScriptMoveSpeed = walkSpeed; // default\n"
                        "        for (action : actions) execAction(action);\n"
                        "        sScriptStartRan = true;\n"
                        "    }";
                    std::string calls = buildActionCalls(infoNode, "    ");
                    snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                        "    // Called once at boot after scene load\n%s", calls.c_str());
                    break;
                }
                case VsNodeType::OnUpdate: {
                    setEventFunc(infoNode, "afn_script_update");
                    editorCode =
                        "    // ---- 2D Tilemap ----\n"
                        "    for (action : actions) execAction(action);\n"
                        "    // runs every frame\n"
                        "\n"
                        "    // ---- 3D Scene ----\n"
                        "    for (action : actions) execAction(action);\n"
                        "    // runs every frame";
                    std::string calls = buildActionCalls(infoNode, "    ");
                    snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                        "    // Called every frame\n%s", calls.c_str());
                    break;
                }
                case VsNodeType::OnCollision: {
                    setEventFunc(infoNode, "afn_script_collision");
                    editorCode =
                        "    // ---- 3D Scene only ----\n"
                        "    if (collidedSprite >= 0) {\n"
                        "        execActions();\n"
                        "    }";
                    std::string calls = buildActionCalls(infoNode, "    ");
                    snprintf(gbaBodyBuf, sizeof(gbaBodyBuf),
                        "    // Called when player collides with sprite\n%s", calls.c_str());
                    break;
                }
                case VsNodeType::MovePlayer: {
                    editorCode =
                        "// ---- 2D Tilemap ----\n"
                        "if (instantMove) {\n"
                        "    facing = dirToFacing[dir];\n"
                        "    tilePos += dirDelta[dir];\n"
                        "    stepCount++;\n"
                        "} else if (keyDown && lastMoveDir == dir) {\n"
                        "    facing = dirToFacing[dir];\n"
                        "    if (justPressed) accum[dir] = 0.45f;\n"
                        "    else {\n"
                        "        accum[dir] += moveRate * dt;\n"
                        "        if (accum >= 1.0) { move; stepCount++; }\n"
                        "    }\n"
                        "}\n"
                        "// visX/visY lerp toward tileX/tileY at moveRate\n"
                        "\n"
                        "// ---- 3D Scene ----\n"
                        "if (editorKeyDown(dirKey)) {\n"
                        "    scInputFwd/scInputRight += direction;\n"
                        "}\n"
                        "// Consumed: normalize diagonal, then:\n"
                        "// moveSpeed = sScriptMoveSpeed * dt;\n"
                        "// fwdX = sinf(viewAngle); fwdZ = cosf(viewAngle);\n"
                        "// player.x += (fwdX*inputX + rightX*inputZ) * moveSpeed;\n"
                        "// player.z += (fwdZ*inputX + rightZ*inputZ) * moveSpeed;";
                    auto* dirData = resolveDataIn(infoNode.id, 0);
                    int dir = dirData ? dirData->paramInt[0] : 0;
                    const char* dirKeysGba[] = { "KEY_LEFT", "KEY_RIGHT", "KEY_UP", "KEY_DOWN" };
                    const char* dirVarsGba[] = { "afn_input_right -= 256", "afn_input_right += 256",
                                                 "afn_input_fwd += 256", "afn_input_fwd -= 256" };
                    char bodyBuf[512];
                    if (dir >= 0 && dir < 4)
                        snprintf(bodyBuf, sizeof(bodyBuf),
                            "    if (key_is_down(%s)) %s;\n"
                            "    // Runtime consumption:\n"
                            "    // inputFwd/inputRight -> normalize diagonal\n"
                            "    // moveFwd = (inputFwd * moveSpeed) >> 8;\n"
                            "    // moveRight = (inputRight * moveSpeed) >> 8;\n"
                            "    // player_x += (viewSin*moveFwd + rightSin*moveRight) >> 8;\n"
                            "    // player_z -= (viewCos*moveFwd + rightCos*moveRight) >> 8;",
                            dirKeysGba[dir], dirVarsGba[dir]);
                    else
                        snprintf(bodyBuf, sizeof(bodyBuf), "    // MovePlayer (no direction set)");
                    setActionFunc(infoNode, "_move", bodyBuf);
                    break;
                }
                case VsNodeType::Walk: {
                    editorCode =
                        "// ---- 2D Tilemap ----\n"
                        "tmMoveRate = speed;\n"
                        "// accum[dir] += tmMoveRate * dt;\n"
                        "// visX/visY lerp speed = tmMoveRate\n"
                        "\n"
                        "// ---- 3D Scene ----\n"
                        "sScriptMoveSpeed = speed;\n"
                        "// moveSpeed = sScriptMoveSpeed * dt;\n"
                        "// player.x += fwd * inputFwd * moveSpeed;";
                    auto* sd = resolveDataIn(infoNode.id, 0);
                    int speed = sd ? sd->paramInt[0] : 37;
                    int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_move_speed = %d;\n"
                        "    // Runtime: moveSpeed = afn_move_speed;\n"
                        "    // player_x += (viewSin * moveFwd * moveSpeed) >> 16;",
                        gbaSpeed);
                    setActionFunc(infoNode, "_walk", bodyBuf);
                    break;
                }
                case VsNodeType::Sprint: {
                    editorCode =
                        "// ---- 2D Tilemap ----\n"
                        "tmMoveRate = speed;\n"
                        "// accum[dir] += tmMoveRate * dt;\n"
                        "// visX/visY lerp speed = tmMoveRate\n"
                        "\n"
                        "// ---- 3D Scene ----\n"
                        "sScriptMoveSpeed = speed;\n"
                        "// moveSpeed = sScriptMoveSpeed * dt;\n"
                        "// player.x += fwd * inputFwd * moveSpeed;";
                    auto* sd = resolveDataIn(infoNode.id, 0);
                    int speed = sd ? sd->paramInt[0] : 56;
                    int gbaSpeed = (int)(speed * 37.0f / 35.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_move_speed = %d;\n"
                        "    // Runtime: moveSpeed = afn_move_speed;\n"
                        "    // player_x += (viewSin * moveFwd * moveSpeed) >> 16;",
                        gbaSpeed);
                    setActionFunc(infoNode, "_sprint", bodyBuf);
                    break;
                }
                case VsNodeType::Jump: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "// (no editor vertical physics)";
                    auto* fd = resolveDataIn(infoNode.id, 0);
                    float force = 2.0f;
                    if (fd) { memcpy(&force, &fd->paramInt[0], sizeof(float)); }
                    int forceFixed = (int)(force * 256.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    if (player_on_ground) player_vy = %d;\n"
                        "    // Runtime: player_y += player_vy;\n"
                        "    // player_vy -= afn_gravity;\n"
                        "    // if (player_vy < -afn_terminal_vel)\n"
                        "    //     player_vy = -afn_terminal_vel;",
                        forceFixed);
                    setActionFunc(infoNode, "_jump", bodyBuf);
                    break;
                }
                case VsNodeType::DampenJump: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "// (no editor vertical physics)";
                    auto* fd = resolveDataIn(infoNode.id, 0);
                    float factor = 0.75f;
                    if (fd) { memcpy(&factor, &fd->paramInt[0], sizeof(float)); }
                    int factorFixed = (int)(factor * 256.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    if (player_vy > 0) player_vy = (player_vy * %d) >> 8;\n"
                        "    // Applied when A released while rising\n"
                        "    // Reduces upward velocity for variable jump height",
                        factorFixed);
                    setActionFunc(infoNode, "_dampen_jump", bodyBuf);
                    break;
                }
                case VsNodeType::OrbitCamera: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "if (editorKeyDown(key)) {\n"
                        "    float radPerFrame = speed / 65536.0f * 6.28318f;\n"
                        "    scOrbitDelta += (dir == 0) ? -radPerFrame : radPerFrame;\n"
                        "}\n"
                        "// Consumed: sManualOrbitCurrent += (scOrbitDelta - current) * 6*dt;\n"
                        "// sOrbitAngle += sManualOrbitCurrent;";
                    auto* dd = resolveDataIn(infoNode.id, 0);
                    auto* sd = resolveDataIn(infoNode.id, 1);
                    int odir = dd ? dd->paramInt[0] : 1;
                    int ospeed = sd ? sd->paramInt[0] : 512;
                    const char* okey = (odir == 0) ? "KEY_L" : "KEY_R";
                    const char* osign = (odir == 0) ? "-" : "+";
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    if (key_is_down(%s)) orbit_angle %s= %d;\n"
                        "    // orbit_angle drives camera rotation around player\n"
                        "    // viewAngle = orbit_angle + player facing",
                        okey, osign, ospeed);
                    setActionFunc(infoNode, "_orbit", bodyBuf);
                    break;
                }
                case VsNodeType::ChangeScene:
                    editorCode =
                        "// ---- 2D Tilemap / 3D Scene ----\n"
                        "sPendingSceneSwitch = scIdx;\n"
                        "sPendingSceneMode = mode; // 0=3D, 1=Tilemap\n"
                        "// Consumed next frame:\n"
                        "//   SaveState -> switch scene index -> LoadState\n"
                        "//   sScriptStartRan = false; // re-run OnStart";
                    setActionFunc(infoNode, "_change_scene",
                        "    afn_pending_scene = <scIdx>;\n"
                        "    afn_pending_scene_mode = <mode>;\n"
                        "    // Runtime: triggers scene reload next frame");
                    break;
                case VsNodeType::SetGravity: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "// (no editor vertical physics)";
                    auto* vd = resolveDataIn(infoNode.id, 0);
                    float val = 0.09f;
                    if (vd) { memcpy(&val, &vd->paramInt[0], sizeof(float)); }
                    int valFixed = (int)(val * 256.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_gravity = %d;\n"
                        "    // Runtime: player_vy -= afn_gravity; // each frame",
                        valFixed);
                    setActionFunc(infoNode, "_set_gravity", bodyBuf);
                    break;
                }
                case VsNodeType::SetMaxFall: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "// (no editor vertical physics)";
                    auto* vd = resolveDataIn(infoNode.id, 0);
                    float val = 6.0f;
                    if (vd) { memcpy(&val, &vd->paramInt[0], sizeof(float)); }
                    int valFixed = (int)(val * 256.0f);
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_terminal_vel = %d;\n"
                        "    // Runtime: if (player_vy < -afn_terminal_vel)\n"
                        "    //     player_vy = -afn_terminal_vel;",
                        valFixed);
                    setActionFunc(infoNode, "_set_max_fall", bodyBuf);
                    break;
                }
                case VsNodeType::AutoOrbit: {
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "sScriptAutoOrbitSpeed = speed;\n"
                        "// if strafing: target = (speed/65536 * 2pi) * inputRight;\n"
                        "// sAutoOrbitCurrent += (target - current) * 6*dt;\n"
                        "// sOrbitAngle -= sAutoOrbitCurrent;";
                    auto* sd = resolveDataIn(infoNode.id, 0);
                    int aspeed = sd ? sd->paramInt[0] : 205;
                    char bodyBuf[256];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_auto_orbit_speed = %d;\n"
                        "    // Runtime: if strafing && speed > 0:\n"
                        "    //   auto_orbit_smooth += (target - smooth) >> 2;\n"
                        "    //   orbit_angle += auto_orbit_smooth;",
                        aspeed);
                    setActionFunc(infoNode, "_auto_orbit", bodyBuf);
                    break;
                }
                case VsNodeType::PlayAnim: {
                    editorCode =
                        "// ---- 2D Tilemap ----\n"
                        "// gameState check: Idle vs Walk vs Sprint\n"
                        "if (apply) sTmObjAnimSet[oi] = animIdx;\n"
                        "\n"
                        "// ---- 3D Scene ----\n"
                        "// Registers which PlayAnim node drives each state:\n"
                        "if (OnStart)       sPlayAnimIdle = id;\n"
                        "if (OnKeyHeld)     sPlayAnimHeld = id;\n"
                        "if (OnKeyReleased) sPlayAnimReleased = id;\n"
                        "// Consumed: pick active based on movement:\n"
                        "//   sprinting -> sPlayAnimHeld\n"
                        "//   moving    -> sPlayAnimReleased\n"
                        "//   idle      -> sPlayAnimIdle\n"
                        "// Then reads Animation data node -> dirSetIdx\n"
                        "// baseSet + (animTime * fps) % frameCount";
                    auto* sd = resolveDataIn(infoNode.id, 0);
                    int animIdx = sd ? sd->paramInt[0] : 0;
                    char bodyBuf[512];
                    snprintf(bodyBuf, sizeof(bodyBuf),
                        "    afn_play_anim = %d;\n"
                        "    // Runtime consumption:\n"
                        "    // int targetAnim = afn_play_anim;\n"
                        "    // if (targetAnim != g_current_anim[ai]) {\n"
                        "    //     g_current_anim[ai] = targetAnim;\n"
                        "    //     g_anim_frame_counter = 0;\n"
                        "    // }\n"
                        "    // int baseSet = afn_anim_desc[ai][targetAnim][0];\n"
                        "    // int fc = afn_anim_desc[ai][targetAnim][1];\n"
                        "    // int fps = afn_anim_desc[ai][targetAnim][2];\n"
                        "    // int frame = (g_anim_frame_counter / (60/fps)) %% fc;\n"
                        "    // switch_dir_anim_set(ai, baseSet + frame);", animIdx);
                    setActionFunc(infoNode, "_play_anim", bodyBuf);
                    break;
                }
                case VsNodeType::SetVariable:
                    editorCode =
                        "// ---- 2D Tilemap / 3D Scene ----\n"
                        "// (not implemented in editor)";
                    setActionFunc(infoNode, "_set_var",
                        "    afn_vars[<slot>] = <value>;");
                    break;
                case VsNodeType::AddVariable:
                    editorCode =
                        "// ---- 2D Tilemap / 3D Scene ----\n"
                        "// (not implemented in editor)";
                    setActionFunc(infoNode, "_add_var",
                        "    afn_vars[<slot>] += <amount>;");
                    break;
                case VsNodeType::DestroyObject:
                    editorCode =
                        "// ---- 3D Scene ----\n"
                        "// (not implemented in editor)";
                    setActionFunc(infoNode, "_destroy",
                        "    // DestroyObject: hide sprite at index");
                    break;
                case VsNodeType::CustomCode:
                    editorCode = "// (runs only on GBA runtime)";
                    {
                        char bodyBuf[512];
                        if (infoNode.customCode[0])
                            snprintf(bodyBuf, sizeof(bodyBuf), "    %s", infoNode.customCode);
                        else
                            snprintf(bodyBuf, sizeof(bodyBuf), "    // (empty)");
                        setActionFunc(infoNode, "_custom", bodyBuf);
                    }
                    break;
                // Data nodes
                case VsNodeType::Integer:
                    snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "%d", infoNode.paramInt[0]);
                    defaultCode = defaultCodeBuf; break;
                case VsNodeType::Float: {
                    float fv; memcpy(&fv, &infoNode.paramInt[0], sizeof(float));
                    snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "%.3g  (fixed: %d)", fv, (int)(fv * 256.0f));
                    defaultCode = defaultCodeBuf; break;
                }
                case VsNodeType::Key:
                    if (infoNode.paramInt[0] >= 0 && infoNode.paramInt[0] < kVsKeyCount)
                        snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "KEY_%s", sVsKeyNames[infoNode.paramInt[0]]);
                    else snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "<unset>");
                    defaultCode = defaultCodeBuf; break;
                case VsNodeType::Direction:
                    if (infoNode.paramInt[0] >= 0 && infoNode.paramInt[0] < kVsAxisCount)
                        snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "%s", sVsAxisNames[infoNode.paramInt[0]]);
                    else snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "<unset>");
                    defaultCode = defaultCodeBuf; break;
                case VsNodeType::Animation:
                    snprintf(defaultCodeBuf, sizeof(defaultCodeBuf), "anim index %d", infoNode.paramInt[0]);
                    defaultCode = defaultCodeBuf; break;
                // Logic
                case VsNodeType::Branch:
                    defaultCode =
                        "// Branch — conditional execution\n"
                        "int cond = findDataIn(action, 0)->paramInt[0];\n"
                        "if (cond) execChain(truePin);\n"
                        "else execChain(falsePin);";
                    break;
                case VsNodeType::CompareVar:
                    defaultCode =
                        "// CompareVar — compare variable slot\n"
                        "int slot = findDataIn(action, 0)->paramInt[0];\n"
                        "int value = findDataIn(action, 1)->paramInt[0];\n"
                        "result = (vars[slot] == value) ? 1 : 0;";
                    break;
                case VsNodeType::LookDirection:
                    defaultCode =
                        "// LookDirection — set player facing\n"
                        "int dir = findDataIn(action, 0)->paramInt[0];\n"
                        "playerFacing = dir;";
                    break;
                case VsNodeType::PlaySound:
                    defaultCode =
                        "// PlaySound — trigger sound effect\n"
                        "int soundId = findDataIn(action, 0)->paramInt[0];\n"
                        "// (sound playback not yet implemented in editor)";
                    break;
                case VsNodeType::Wait:
                    defaultCode =
                        "// Wait — pause execution for N frames\n"
                        "int frames = findDataIn(action, 0)->paramInt[0];\n"
                        "waitCounter = frames;";
                    break;
                case VsNodeType::Group:
                    defaultCode = "// Group — contains a subgraph of nodes"; break;
                default: break;
                }

                // Combine editor + GBA code for event/action nodes
                if (editorCode[0]) {
                    char combinedBuf[4096];
                    if (funcSigBuf[0]) {
                        // Event/action node: all sections inside function block
                        snprintf(combinedBuf, sizeof(combinedBuf),
                            "%s {\n\n%s\n\n    // ---- GBA Runtime (mapdata.h) ----\n%s\n}",
                            funcSigBuf, editorCode, gbaBodyBuf);
                    } else {
                        // Other node: flat sections
                        snprintf(combinedBuf, sizeof(combinedBuf),
                            "%s\n\n// ---- GBA Runtime (mapdata.h) ----\n%s",
                            editorCode, gbaCode[0] ? gbaCode : "// (no GBA codegen for this node)");
                    }
                    strncpy(defaultCodeBuf, combinedBuf, sizeof(defaultCodeBuf) - 1);
                    defaultCodeBuf[sizeof(defaultCodeBuf) - 1] = '\0';
                    defaultCode = defaultCodeBuf;
                }

                // "View Code" button opens standalone script window
                if (defaultCode[0]) {
                    ImGui::Separator();
                    if (ImGui::Button("View Code")) {
                        sVsCodeWindowOpen = true;
                        sVsCodeWindowJustOpened = true;
                        sVsCodeWindowNodeIdx = sVsNodeInfoIdx;
                        strncpy(sVsCodeWindowBuf, defaultCode, sizeof(sVsCodeWindowBuf) - 1);
                        sVsCodeWindowBuf[sizeof(sVsCodeWindowBuf) - 1] = '\0';
                        strncpy(sVsCodeWindowEditBuf, infoNode.customCode, sizeof(sVsCodeWindowEditBuf) - 1);
                        sVsCodeWindowEditBuf[sizeof(sVsCodeWindowEditBuf) - 1] = '\0';
                    }
                    if (infoNode.customCode[0]) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "(has custom code)");
                    }
                }

                // "Expose as Parameter" — only in blueprint editing mode, only for data nodes
                if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0
                    && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()
                    && isDataNodeType(infoNode.type))
                {
                    ImGui::Separator();
                    BlueprintAsset& bp = sBlueprintAssets[sVsEditBlueprintIdx];
                    // Check if already exposed
                    int existingParam = -1;
                    for (int pi = 0; pi < bp.paramCount; pi++)
                        if (bp.params[pi].sourceNodeId == infoNode.id) { existingParam = pi; break; }
                    bool exposed = (existingParam >= 0);
                    if (ImGui::Checkbox("Expose as Parameter", &exposed)) {
                        if (exposed && existingParam < 0 && bp.paramCount < 8) {
                            BpParam& p = bp.params[bp.paramCount];
                            snprintf(p.name, sizeof(p.name), "Param%d", bp.paramCount);
                            p.sourceNodeId = infoNode.id;
                            p.sourceParamIdx = 0;
                            p.dataType = dataTypeFromNode(infoNode.type);
                            p.defaultInt = infoNode.paramInt[0];
                            bp.paramCount++;
                            sProjectDirty = true;
                        } else if (!exposed && existingParam >= 0) {
                            // Remove param, shift others down
                            for (int pi = existingParam; pi < bp.paramCount - 1; pi++)
                                bp.params[pi] = bp.params[pi + 1];
                            bp.paramCount--;
                            bp.params[bp.paramCount] = {};
                            sProjectDirty = true;
                        }
                    }
                    if (exposed && existingParam >= 0) {
                        ImGui::SetNextItemWidth(120);
                        ImGui::InputText("##PName", bp.params[existingParam].name, 32);
                    }
                }
            }
            ImGui::EndPopup();
        }

        // ---- Script Code Window (standalone, resizable) ----
        if (sVsCodeWindowJustOpened) {
            ImGui::SetNextWindowFocus();
            sVsCodeWindowJustOpened = false;
        }
        if (sVsCodeWindowOpen && sVsCodeWindowNodeIdx >= 0 && sVsCodeWindowNodeIdx < (int)sVsNodes.size()) {
            VsNode& cwNode = sVsNodes[sVsCodeWindowNodeIdx];
            const char* nodeName = sVsNodeDefs[(int)cwNode.type].name;
            char winTitle[64];
            snprintf(winTitle, sizeof(winTitle), "Code: %s###ScriptCodeWin", nodeName);
            ImGui::SetNextWindowSize(ImVec2(500, 550), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(winTitle, &sVsCodeWindowOpen)) {
                // Generated code (read-only, monospace)
                ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Generated Code");
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.9f, 0.6f, 1.0f));
                float genHeight = cwNode.customCode[0] ? ImGui::GetContentRegionAvail().y * 0.40f : ImGui::GetContentRegionAvail().y - 80;
                ImGui::InputTextMultiline("##GenCode", sVsCodeWindowBuf, sizeof(sVsCodeWindowBuf),
                    ImVec2(-1, genHeight), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopStyleColor(2);

                // Function declaration (editable)
                {
                    bool isBpNode = (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0);
                    const char* suffix = "";
                    switch (cwNode.type) {
                    case VsNodeType::OnKeyPressed:  suffix = "_key_pressed"; break;
                    case VsNodeType::OnKeyReleased: suffix = "_key_released"; break;
                    case VsNodeType::OnKeyHeld:     suffix = "_key_held"; break;
                    case VsNodeType::OnStart:       suffix = "_start"; break;
                    case VsNodeType::OnUpdate:      suffix = "_update"; break;
                    case VsNodeType::OnCollision:   suffix = "_collision"; break;
                    case VsNodeType::MovePlayer:    suffix = "_move"; break;
                    case VsNodeType::Jump:          suffix = "_jump"; break;
                    case VsNodeType::Walk:          suffix = "_walk"; break;
                    case VsNodeType::Sprint:        suffix = "_sprint"; break;
                    case VsNodeType::OrbitCamera:   suffix = "_orbit"; break;
                    case VsNodeType::PlayAnim:      suffix = "_play_anim"; break;
                    case VsNodeType::SetGravity:    suffix = "_set_gravity"; break;
                    case VsNodeType::SetMaxFall:    suffix = "_set_max_fall"; break;
                    case VsNodeType::DestroyObject: suffix = "_destroy"; break;
                    case VsNodeType::AutoOrbit:     suffix = "_auto_orbit"; break;
                    case VsNodeType::DampenJump:    suffix = "_dampen_jump"; break;
                    case VsNodeType::ChangeScene:   suffix = "_change_scene"; break;
                    case VsNodeType::CustomCode:    suffix = "_custom"; break;
                    default: suffix = ""; break;
                    }
                    // Show default name as placeholder (action nodes include node ID to disambiguate)
                    bool isEventNode = (cwNode.type == VsNodeType::OnKeyPressed || cwNode.type == VsNodeType::OnKeyReleased ||
                                        cwNode.type == VsNodeType::OnKeyHeld || cwNode.type == VsNodeType::OnStart ||
                                        cwNode.type == VsNodeType::OnUpdate || cwNode.type == VsNodeType::OnCollision);
                    char defaultName[64] = {};
                    if (isBpNode && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                        if (isEventNode)
                            snprintf(defaultName, sizeof(defaultName), "afn_bp%d%s", sVsEditBlueprintIdx, suffix);
                        else
                            snprintf(defaultName, sizeof(defaultName), "afn_bp%d%s_%d", sVsEditBlueprintIdx, suffix, cwNode.id);
                    } else {
                        if (isEventNode)
                            snprintf(defaultName, sizeof(defaultName), "afn_script%s", suffix);
                        else
                            snprintf(defaultName, sizeof(defaultName), "afn_script%s_%d", suffix, cwNode.id);
                    }
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Function Declaration");
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 1.0f, 1.0f));
                    if (ImGui::InputTextWithHint("##FuncDecl", defaultName, cwNode.funcName, sizeof(cwNode.funcName)))
                        sProjectDirty = true;
                    ImGui::PopStyleColor(2);
                }

                // Custom override section
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Custom Override");
                ImGui::SameLine();
                if (!cwNode.customCode[0]) {
                    if (ImGui::SmallButton("Enable")) {
                        strncpy(cwNode.customCode, sVsCodeWindowBuf, sizeof(cwNode.customCode) - 1);
                        strncpy(sVsCodeWindowEditBuf, cwNode.customCode, sizeof(sVsCodeWindowEditBuf) - 1);
                        sProjectDirty = true;
                    }
                } else {
                    if (ImGui::SmallButton("Reset")) {
                        cwNode.customCode[0] = '\0';
                        sVsCodeWindowEditBuf[0] = '\0';
                        sProjectDirty = true;
                    }
                }
                if (cwNode.customCode[0]) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.1f, 0.08f, 1.0f));
                    ImGui::InputTextMultiline("##EditCode", sVsCodeWindowEditBuf, sizeof(sVsCodeWindowEditBuf),
                        ImVec2(-1, -30), ImGuiInputTextFlags_AllowTabInput);
                    ImGui::PopStyleColor();
                    if (ImGui::Button("Save")) {
                        strncpy(cwNode.customCode, sVsCodeWindowEditBuf, sizeof(cwNode.customCode) - 1);
                        sProjectDirty = true;
                    }
                }
            }
            ImGui::End();
        }

        if (sVsShowContextMenu) {
            ImGui::OpenPopup("##AddNode");
            sVsShowContextMenu = false;
            sVsNodeSearch[0] = '\0';
            sVsSearchFocused = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(300, 400));
        if (ImGui::BeginPopup("##AddNode")) {
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Add");
            ImGui::Separator();

            // Helper lambda to create a node at context menu position
            auto addNodeAt = [&](VsNodeType t) {
                VsNode n;
                n.id = sVsNextId++;
                n.type = t;
                n.x = (sVsContextMenuPos.x - canvasOrig.x) / zoom - sVsPanX;
                n.y = (sVsContextMenuPos.y - canvasOrig.y) / zoom - sVsPanY;
                n.groupId = sVsEditingGroup;
                sVsNodes.push_back(n);
                sVsSelected = (int)sVsNodes.size() - 1;
                // Auto-wire if we have a pending pin from link drag
                if (sVsPendingAutoWire.nodeId >= 0) {
                    auto pc = VsGetPinCounts(n);
                    int srcType = sVsPendingAutoWire.pinType;
                    if (srcType == 0 && pc.inExec > 0)
                        sVsLinks.push_back({ sVsPendingAutoWire, { n.id, 1, 0 } });
                    else if (srcType == 1 && pc.outExec > 0)
                        sVsLinks.push_back({ { n.id, 0, 0 }, sVsPendingAutoWire });
                    else if (srcType == 2 && pc.inData > 0)
                        sVsLinks.push_back({ sVsPendingAutoWire, { n.id, 3, 0 } });
                    else if (srcType == 3 && pc.outData > 0)
                        sVsLinks.push_back({ { n.id, 2, 0 }, sVsPendingAutoWire });
                    sVsPendingAutoWire = { -1, 0, 0 };
                }
                // Auto-open code window for Custom Code nodes
                if (t == VsNodeType::CustomCode) {
                    VsNode& cn = sVsNodes.back();
                    strncpy(cn.customCode, "// write your GBA C code here\n", sizeof(cn.customCode) - 1);
                    sVsCodeWindowOpen = true;
                    sVsCodeWindowNodeIdx = (int)sVsNodes.size() - 1;
                    snprintf(sVsCodeWindowBuf, sizeof(sVsCodeWindowBuf),
                        "// ---- Editor Play Mode ----\n// Custom code — runs only on GBA\n\n// ---- GBA Runtime (mapdata.h) ----\n%s", cn.customCode);
                    strncpy(sVsCodeWindowEditBuf, cn.customCode, sizeof(sVsCodeWindowEditBuf) - 1);
                }
            };

            // Search bar — auto-focus on open
            if (!sVsSearchFocused) {
                ImGui::SetKeyboardFocusHere();
                sVsSearchFocused = true;
            }
            ImGui::PushItemWidth(-1);
            ImGui::InputTextWithHint("##NodeSearch", "Search...", sVsNodeSearch, sizeof(sVsNodeSearch));
            ImGui::PopItemWidth();
            ImGui::Separator();

            bool hasSearch = sVsNodeSearch[0] != '\0';

            // Case-insensitive substring match helper
            auto matchesSearch = [&](const char* name) -> bool {
                if (!hasSearch) return true;
                const char* s = sVsNodeSearch;
                const char* n2 = name;
                // Simple case-insensitive find
                for (; *n2; n2++) {
                    const char* si = s;
                    const char* ni = n2;
                    while (*si && *ni && (tolower(*si) == tolower(*ni))) { si++; ni++; }
                    if (!*si) return true;
                }
                return false;
            };

            if (hasSearch) {
                // Flat filtered list when searching
                for (int t = 0; t < (int)VsNodeType::COUNT; t++) {
                    if ((VsNodeType)t == VsNodeType::Group) continue;
                    if (matchesSearch(sVsNodeDefs[t].name))
                        if (ImGui::MenuItem(sVsNodeDefs[t].name)) addNodeAt((VsNodeType)t);
                }
            } else {
                // Categorized submenus
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
                if (ImGui::BeginMenu("Events")) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
                    for (int t = (int)VsNodeType::OnKeyPressed; t <= (int)VsNodeType::OnStart; t++)
                        if (ImGui::MenuItem(sVsNodeDefs[t].name)) addNodeAt((VsNodeType)t);
                    if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::OnUpdate].name)) addNodeAt(VsNodeType::OnUpdate);
                    ImGui::PopStyleColor();
                    ImGui::EndMenu();
                }
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.5f, 0.9f, 1.0f));
                if (ImGui::BeginMenu("Logic")) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
                    for (int t = (int)VsNodeType::Branch; t <= (int)VsNodeType::CompareVar; t++)
                        if (ImGui::MenuItem(sVsNodeDefs[t].name)) addNodeAt((VsNodeType)t);
                    ImGui::PopStyleColor();
                    ImGui::EndMenu();
                }
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.3f, 1.0f));
                if (ImGui::BeginMenu("Actions")) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
                    for (int t = (int)VsNodeType::MovePlayer; t <= (int)VsNodeType::DampenJump; t++)
                        if (ImGui::MenuItem(sVsNodeDefs[t].name)) addNodeAt((VsNodeType)t);
                    ImGui::Separator();
                    if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::CustomCode].name)) addNodeAt(VsNodeType::CustomCode);
                    ImGui::PopStyleColor();
                    ImGui::EndMenu();
                }
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.8f, 1.0f));
                if (ImGui::BeginMenu("Data")) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
                    for (int t = (int)VsNodeType::Integer; t <= (int)VsNodeType::Float; t++)
                        if (ImGui::MenuItem(sVsNodeDefs[t].name)) addNodeAt((VsNodeType)t);
                    if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::Object].name)) addNodeAt(VsNodeType::Object);
                    ImGui::PopStyleColor();
                    ImGui::EndMenu();
                }
                ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        } else {
            // Context menu closed without selection — clear pending auto-wire
            sVsPendingAutoWire = { -1, 0, 0 };
        }

        // Properties panel overlay — as child window inside canvas (data nodes only)
        if (sVsSelected >= 0 && sVsSelected < (int)sVsNodes.size()) {
            VsNode& n = sVsNodes[sVsSelected];
            if (n.type == VsNodeType::Integer || n.type == VsNodeType::Key || n.type == VsNodeType::Direction || n.type == VsNodeType::Animation || n.type == VsNodeType::Float || n.type == VsNodeType::Group || n.type == VsNodeType::Object || n.type == VsNodeType::ChangeScene || n.type == VsNodeType::CustomCode) {
            const auto& def = sVsNodeDefs[(int)n.type];
            float propW = 260, propH = 180;
            float nodeScreenX = canvasOrig.x + (n.x + sVsPanX) * zoom;
            float nodeScreenY = canvasOrig.y + (n.y + sVsPanY) * zoom + VsNodeHeight(n) * zoom + 4;
            // Clamp to stay within canvas
            if (nodeScreenX + propW > canvasOrig.x + canvasSize.x)
                nodeScreenX = canvasOrig.x + canvasSize.x - propW - 4;
            if (nodeScreenY + propH > canvasOrig.y + canvasSize.y)
                nodeScreenY = canvasOrig.y + canvasSize.y - propH - 4;
            if (nodeScreenX < canvasOrig.x) nodeScreenX = canvasOrig.x + 4;
            if (nodeScreenY < canvasOrig.y) nodeScreenY = canvasOrig.y + 4;
            ImVec2 propPos(nodeScreenX, nodeScreenY);
            ImGui::SetCursorScreenPos(propPos);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.16f, 0.2f, 0.95f));
            ImGui::BeginChild("##NodeProps", ImVec2(propW, propH), true);
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%s", def.name);
            ImGui::Separator();
            ImGui::PushItemWidth(-1);

            switch (n.type) {
            // Data nodes — these have editable properties
            case VsNodeType::Direction:
                ImGui::Text("Direction");
                ImGui::Combo("##Dir", &n.paramInt[0], sVsAxisNames, kVsAxisCount);
                break;
            case VsNodeType::Integer:
                ImGui::Text("Integer");
                ImGui::DragInt("##Val", &n.paramInt[0], 0.5f);
                break;
            case VsNodeType::Float: {
                ImGui::Text("Float");
                float fv;
                memcpy(&fv, &n.paramInt[0], sizeof(float));
                if (ImGui::DragFloat("##Flt", &fv, 0.001f, 0.0f, 0.0f, "%.3f"))
                    memcpy(&n.paramInt[0], &fv, sizeof(float));
                break;
            }
            case VsNodeType::Key:
                ImGui::Text("Key");
                ImGui::Combo("##Key2", &n.paramInt[0], sVsKeyNames, kVsKeyCount);
                break;
            case VsNodeType::Animation: {
                // Sprite asset selector
                ImGui::Text("Sprite Asset");
                if (sSpriteAssets.empty()) {
                    ImGui::Text("(no sprite assets)");
                    break;
                }
                {
                    const char* assetPreview = (n.paramInt[0] >= 0 && n.paramInt[0] < (int)sSpriteAssets.size())
                        ? sSpriteAssets[n.paramInt[0]].name.c_str() : "None";
                    if (ImGui::BeginCombo("##AnimAsset", assetPreview)) {
                        for (int si = 0; si < (int)sSpriteAssets.size(); si++) {
                            bool sel = (si == n.paramInt[0]);
                            if (ImGui::Selectable(sSpriteAssets[si].name.c_str(), sel)) {
                                n.paramInt[0] = si;
                                n.paramInt[1] = 0; // reset anim when asset changes
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    // Animation selector (from chosen asset)
                    ImGui::Text("Animation");
                    int ai = n.paramInt[0];
                    if (ai >= 0 && ai < (int)sSpriteAssets.size() && !sSpriteAssets[ai].anims.empty()) {
                        const auto& anims = sSpriteAssets[ai].anims;
                        const char* animPreview = (n.paramInt[1] >= 0 && n.paramInt[1] < (int)anims.size())
                            ? anims[n.paramInt[1]].name.c_str() : "None";
                        if (ImGui::BeginCombo("##AnimName", animPreview)) {
                            for (int a = 0; a < (int)anims.size(); a++) {
                                bool sel2 = (a == n.paramInt[1]);
                                if (ImGui::Selectable(anims[a].name.c_str(), sel2))
                                    n.paramInt[1] = a;
                                if (sel2) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    } else {
                        ImGui::Text("(no animations)");
                    }
                }
                break;
            }
            case VsNodeType::ChangeScene: {
                ImGui::Text("Mode");
                const char* modeNames[] = { "Mode 4 (3D)", "Mode 0 (Tilemap)" };
                int mode = (n.paramInt[1] == 1) ? 1 : 0;
                if (ImGui::BeginCombo("##ModeSel", modeNames[mode])) {
                    if (ImGui::Selectable("Mode 4 (3D)", mode == 0)) { n.paramInt[1] = 0; }
                    if (ImGui::Selectable("Mode 0 (Tilemap)", mode == 1)) { n.paramInt[1] = 1; }
                    ImGui::EndCombo();
                }
                break;
            }
            case VsNodeType::Object: {
                // paramInt[1]: 0=Sprite Asset, 1=Mesh Asset, 2=Scene Sprite
                ImGui::Text("Object");
                const char* objKinds[] = { "Sprite", "Mesh", "Instance" };
                int objKind = std::clamp(n.paramInt[1], 0, 2);
                ImGui::PushItemWidth(Scaled(70));
                if (ImGui::Combo("##ObjKind", &objKind, objKinds, 3))
                    n.paramInt[1] = objKind;
                ImGui::PopItemWidth();

                if (objKind == 0) {
                    // Sprite assets
                    const char* preview = (n.paramInt[0] >= 0 && n.paramInt[0] < (int)sSpriteAssets.size())
                        ? sSpriteAssets[n.paramInt[0]].name.c_str() : "None";
                    if (ImGui::BeginCombo("##ObjSpr", preview)) {
                        for (int si = 0; si < (int)sSpriteAssets.size(); si++) {
                            char itemLabel[64];
                            snprintf(itemLabel, sizeof(itemLabel), "[%d] %s", si, sSpriteAssets[si].name.c_str());
                            if (ImGui::Selectable(itemLabel, si == n.paramInt[0]))
                                n.paramInt[0] = si;
                            if (si == n.paramInt[0]) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else if (objKind == 1) {
                    // Mesh assets
                    const char* preview = (n.paramInt[0] >= 0 && n.paramInt[0] < (int)sMeshAssets.size())
                        ? sMeshAssets[n.paramInt[0]].name.c_str() : "None";
                    if (ImGui::BeginCombo("##ObjMesh", preview)) {
                        for (int mi = 0; mi < (int)sMeshAssets.size(); mi++) {
                            char itemLabel[64];
                            snprintf(itemLabel, sizeof(itemLabel), "[%d] %s", mi, sMeshAssets[mi].name.c_str());
                            if (ImGui::Selectable(itemLabel, mi == n.paramInt[0]))
                                n.paramInt[0] = mi;
                            if (mi == n.paramInt[0]) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    // Scene sprite instances (original behavior)
                    const char* preview = (n.paramInt[0] >= 0 && n.paramInt[0] < sSpriteCount)
                        ? (sSprites[n.paramInt[0]].assetIdx >= 0 && sSprites[n.paramInt[0]].assetIdx < (int)sSpriteAssets.size()
                            ? sSpriteAssets[sSprites[n.paramInt[0]].assetIdx].name.c_str() : "(no asset)")
                        : "None";
                    char objLabel[64];
                    if (n.paramInt[0] >= 0 && n.paramInt[0] < sSpriteCount)
                        snprintf(objLabel, sizeof(objLabel), "[%d] %s", n.paramInt[0], preview);
                    else snprintf(objLabel, sizeof(objLabel), "None");
                    if (ImGui::BeginCombo("##ObjInst", objLabel)) {
                        for (int si = 0; si < sSpriteCount; si++) {
                            const char* assetName = (sSprites[si].assetIdx >= 0 && sSprites[si].assetIdx < (int)sSpriteAssets.size())
                                ? sSpriteAssets[sSprites[si].assetIdx].name.c_str() : "(no asset)";
                            char itemLabel[64];
                            snprintf(itemLabel, sizeof(itemLabel), "[%d] %s", si, assetName);
                            if (ImGui::Selectable(itemLabel, si == n.paramInt[0]))
                                n.paramInt[0] = si;
                            if (si == n.paramInt[0]) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                break;
            }
            case VsNodeType::Group:
                ImGui::Text("Name");
                ImGui::InputText("##GrpName", n.groupLabel, sizeof(n.groupLabel));
                break;
            case VsNodeType::CustomCode:
                ImGui::Text("Name");
                ImGui::InputText("##CcName", n.groupLabel, sizeof(n.groupLabel));
                break;
            default: break;
            }

            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            }
        }

        ImGui::End();       // ##NodeCanvas
        ImGui::PopStyleColor();

        // ---- BLUEPRINT ASSET BROWSER (bottom panel) ----
        {
            float browY = bodyY + canvasH;
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, browY));
            ImGui::SetNextWindowSize(ImVec2(totalW, browserH));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.14f, 0.16f, 1.0f));
            ImGui::Begin("##BpBrowser", nullptr, flags);

            // Header row: title + action buttons
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.8f, 1.0f), "Blueprints");
            ImGui::SameLine();
            if (ImGui::SmallButton("+ New##bpnew")) {
                BlueprintAsset bp;
                snprintf(bp.name, sizeof(bp.name), "Blueprint%d", (int)sBlueprintAssets.size());
                sBlueprintAssets.push_back(bp);
                sSelectedBlueprint = (int)sBlueprintAssets.size() - 1;
                sProjectDirty = true;
            }
            if (sSelectedBlueprint >= 0 && sSelectedBlueprint < (int)sBlueprintAssets.size()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Edit##bpedit")) {
                    if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                        BlueprintAsset& old = sBlueprintAssets[sVsEditBlueprintIdx];
                        old.nodes = sVsNodes; old.links = sVsLinks; old.annotations = sVsAnnotations;
                        old.groupPins = sVsGroupPins; old.nextId = sVsNextId;
                        old.panX = sVsPanX; old.panY = sVsPanY; old.zoom = sVsZoom;
                    } else if (sVsEditSource == VsEditSource::Scene) {
                        if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                            SaveMapSceneState(sMapScenes[sMapSelectedScene]);
                    }
                    BlueprintAsset& bp = sBlueprintAssets[sSelectedBlueprint];
                    sVsNodes = bp.nodes; sVsLinks = bp.links; sVsAnnotations = bp.annotations;
                    sVsGroupPins = bp.groupPins; sVsNextId = bp.nextId;
                    sVsPanX = bp.panX; sVsPanY = bp.panY; sVsZoom = bp.zoom;
                    sVsEditSource = VsEditSource::Blueprint;
                    sVsEditBlueprintIdx = sSelectedBlueprint;
                    sVsSelected = -1;
                    sVsEditingGroup = 0;
                    sVsUndoStack.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Dup##bpdup")) {
                    BlueprintAsset dup = sBlueprintAssets[sSelectedBlueprint];
                    snprintf(dup.name, sizeof(dup.name), "%s Copy", sBlueprintAssets[sSelectedBlueprint].name);
                    sBlueprintAssets.push_back(std::move(dup));
                    sSelectedBlueprint = (int)sBlueprintAssets.size() - 1;
                    sProjectDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Del##bpdel")) {
                    if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx == sSelectedBlueprint) {
                        if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                            LoadMapSceneState(sMapScenes[sMapSelectedScene]);
                        sVsEditSource = VsEditSource::Scene;
                        sVsEditBlueprintIdx = -1;
                    }
                    sBlueprintAssets.erase(sBlueprintAssets.begin() + sSelectedBlueprint);
                    auto fixBpRef = [&](int& ref) {
                        if (ref == sSelectedBlueprint) ref = -1;
                        else if (ref > sSelectedBlueprint) ref--;
                    };
                    for (int si = 0; si < (int)sMapScenes.size(); si++) {
                        for (int j = 0; j < sMapScenes[si].spriteCount; j++)
                            fixBpRef(sMapScenes[si].sprites[j].blueprintIdx);
                        fixBpRef(sMapScenes[si].blueprintIdx);
                    }
                    for (int j = 0; j < sSpriteCount; j++)
                        fixBpRef(sSprites[j].blueprintIdx);
                    for (auto& obj : sTmObjects)
                        fixBpRef(obj.blueprintIdx);
                    for (auto& sc : sTmScenes) {
                        for (auto& obj : sc.objects)
                            fixBpRef(obj.blueprintIdx);
                        fixBpRef(sc.blueprintIdx);
                    }
                    if (sVsEditBlueprintIdx > sSelectedBlueprint) sVsEditBlueprintIdx--;
                    sSelectedBlueprint = std::min(sSelectedBlueprint, (int)sBlueprintAssets.size() - 1);
                    sProjectDirty = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Scaled(100));
                if (ImGui::InputText("##BpRename", sBlueprintAssets[sSelectedBlueprint].name, 32))
                    sProjectDirty = true;
            }
            ImGui::Separator();

            // Scrollable blueprint list
            ImGui::BeginChild("##BpList", ImVec2(0, 0), false);
            for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                BlueprintAsset& bp = sBlueprintAssets[bi];
                bool isSel = (sSelectedBlueprint == bi);
                bool isEditing = (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx == bi);
                char label[64];
                snprintf(label, sizeof(label), "%s%s###bp%d", bp.name, isEditing ? " (editing)" : "", bi);

                if (ImGui::Selectable(label, isSel, ImGuiSelectableFlags_AllowDoubleClick)) {
                    sSelectedBlueprint = bi;
                    // Double-click to edit
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (sVsEditSource == VsEditSource::Blueprint && sVsEditBlueprintIdx >= 0 && sVsEditBlueprintIdx < (int)sBlueprintAssets.size()) {
                            BlueprintAsset& old = sBlueprintAssets[sVsEditBlueprintIdx];
                            old.nodes = sVsNodes; old.links = sVsLinks; old.annotations = sVsAnnotations;
                            old.groupPins = sVsGroupPins; old.nextId = sVsNextId;
                            old.panX = sVsPanX; old.panY = sVsPanY; old.zoom = sVsZoom;
                        } else if (sVsEditSource == VsEditSource::Scene) {
                            if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                                SaveMapSceneState(sMapScenes[sMapSelectedScene]);
                        }
                        sVsNodes = bp.nodes; sVsLinks = bp.links; sVsAnnotations = bp.annotations;
                        sVsGroupPins = bp.groupPins; sVsNextId = bp.nextId;
                        sVsPanX = bp.panX; sVsPanY = bp.panY; sVsZoom = bp.zoom;
                        sVsEditSource = VsEditSource::Blueprint;
                        sVsEditBlueprintIdx = bi;
                        sVsSelected = -1;
                        sVsEditingGroup = 0;
                        sVsUndoStack.clear();
                    }
                }
                // Show node/param count as detail
                ImGui::SameLine(Scaled(180));
                ImGui::TextDisabled("%d nodes, %d params", (int)bp.nodes.size(), bp.paramCount);
            }
            if (sBlueprintAssets.empty()) {
                ImGui::TextDisabled("No blueprints. Click '+ New' to create one.");
            }
            ImGui::EndChild(); // ##BpList

            ImGui::End();       // ##BpBrowser
            ImGui::PopStyleColor(2);
        }
    }
    else if (sActiveTab == EditorTab::Mode7)
    {
        // Mode 7 tab: viewport + tileset/tilemap/palette panels
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
    else if (sActiveTab == EditorTab::Sprites)
    {
        // Sprites tab: full-width sprite asset editor
        DrawSpritesTab(
            ImVec2(vp->WorkPos.x, bodyY),
            ImVec2(totalW, bodyH), dt);
    }
    else
    {
        // Map / Tiles tabs: viewport + right panel
        float scenePanH = 0;

        DrawViewport(
            ImVec2(vp->WorkPos.x, bodyY),
            ImVec2(leftW, bodyH));

        // Scene panel (top-right, only on Map tab)
        if (sActiveTab == EditorTab::Map)
        {
            // Compute scene panel height: base for scene list + extra if blueprint attached
            int bpExtraH = 0;
            if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size() && sMapScenes[sMapSelectedScene].blueprintIdx >= 0)
                bpExtraH = 20 + sMapScenes[sMapSelectedScene].instanceParamCount * 22;
            scenePanH = 160 + 50 + bpExtraH;
            if (scenePanH > bodyH * 0.5f) scenePanH = (float)(int)(bodyH * 0.5f);
            ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + leftW, bodyY));
            ImGui::SetNextWindowSize(ImVec2(rightW, scenePanH));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
            ImGui::Begin("##MapScenePanel", nullptr, panelFlags);

            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Scenes");
            ImGui::SameLine();
            if (ImGui::SmallButton("+##addmapscene"))
            {
                if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                    SaveMapSceneState(sMapScenes[sMapSelectedScene]);
                MapScene ms;
                snprintf(ms.name, sizeof(ms.name), "Scene %d", (int)sMapScenes.size());
                ms.spriteCount = 0;
                sMapScenes.push_back(ms);
                sMapSelectedScene = (int)sMapScenes.size() - 1;
                LoadMapSceneState(sMapScenes[sMapSelectedScene]);
            }
            if ((int)sMapScenes.size() > 1) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x##delmapscene"))
                {
                    sMapScenes.erase(sMapScenes.begin() + sMapSelectedScene);
                    if (sMapSelectedScene >= (int)sMapScenes.size())
                        sMapSelectedScene = (int)sMapScenes.size() - 1;
                    LoadMapSceneState(sMapScenes[sMapSelectedScene]);
                }
            }

            // Scene list
            ImGui::BeginChild("##MapSceneList", ImVec2(0, 100));
            for (int i = 0; i < (int)sMapScenes.size(); i++)
            {
                bool sel = (i == sMapSelectedScene);
                if (sel)
                {
                    ImGui::PushItemWidth(-1);
                    char idBuf[32]; snprintf(idBuf, sizeof(idBuf), "##msname%d", i);
                    ImGui::InputText(idBuf, sMapScenes[i].name, sizeof(sMapScenes[i].name));
                    ImGui::PopItemWidth();
                }
                else
                {
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "%s##ms%d", sMapScenes[i].name, i);
                    if (ImGui::Selectable(lbl, false))
                    {
                        if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size())
                            SaveMapSceneState(sMapScenes[sMapSelectedScene]);
                        sMapSelectedScene = i;
                        LoadMapSceneState(sMapScenes[i]);
                    }
                }
            }
            ImGui::EndChild();

            // Scene-level blueprint
            if (sMapSelectedScene >= 0 && sMapSelectedScene < (int)sMapScenes.size()) {
                MapScene& ms = sMapScenes[sMapSelectedScene];
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Scene Blueprint");
                ImGui::PushItemWidth(-1);
                const char* bpPreview = (ms.blueprintIdx >= 0 && ms.blueprintIdx < (int)sBlueprintAssets.size())
                    ? sBlueprintAssets[ms.blueprintIdx].name : "(none)";
                if (ImGui::BeginCombo("Script##scenebp", bpPreview)) {
                    if (ImGui::Selectable("(none)##scenebpnone", ms.blueprintIdx < 0)) {
                        ms.blueprintIdx = -1;
                        ms.instanceParamCount = 0;
                        sProjectDirty = true;
                    }
                    for (int bi = 0; bi < (int)sBlueprintAssets.size(); bi++) {
                        bool sel = (ms.blueprintIdx == bi);
                        if (ImGui::Selectable(sBlueprintAssets[bi].name, sel)) {
                            ms.blueprintIdx = bi;
                            ms.instanceParamCount = 0;
                            sProjectDirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ms.blueprintIdx >= 0 && ms.blueprintIdx < (int)sBlueprintAssets.size()) {
                    const BlueprintAsset& bp = sBlueprintAssets[ms.blueprintIdx];
                    for (int pi = 0; pi < bp.paramCount; pi++) {
                        const BpParam& param = bp.params[pi];
                        int overrideIdx = -1;
                        for (int oi = 0; oi < ms.instanceParamCount; oi++)
                            if (ms.instanceParams[oi].paramIdx == pi) { overrideIdx = oi; break; }
                        int val = (overrideIdx >= 0) ? ms.instanceParams[overrideIdx].value : param.defaultInt;
                        ImGui::PushID(pi + 7000);
                        char label[48]; snprintf(label, sizeof(label), "%s##scbp%d", param.name, pi);
                        if (ImGui::DragInt(label, &val, 1.0f)) {
                            if (overrideIdx >= 0) {
                                ms.instanceParams[overrideIdx].value = val;
                            } else if (ms.instanceParamCount < 8) {
                                ms.instanceParams[ms.instanceParamCount].paramIdx = pi;
                                ms.instanceParams[ms.instanceParamCount].value = val;
                                ms.instanceParamCount++;
                            }
                            sProjectDirty = true;
                        }
                        if (overrideIdx >= 0) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Reset")) {
                                for (int oi = overrideIdx; oi < ms.instanceParamCount - 1; oi++)
                                    ms.instanceParams[oi] = ms.instanceParams[oi + 1];
                                ms.instanceParamCount--;
                                sProjectDirty = true;
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::PopItemWidth();
            }

            ImGui::End();
            ImGui::PopStyleColor();
        }

        if (sEditorMode == EditorMode::Edit)
            DrawObjectEditorPanel(
                ImVec2(vp->WorkPos.x + leftW, bodyY + scenePanH),
                ImVec2(rightW, bodyH - scenePanH));
        else
            DrawTilesetPanel(
                ImVec2(vp->WorkPos.x + leftW, bodyY + scenePanH),
                ImVec2(rightW, bodyH - scenePanH));
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
        ImGui::Begin(sBuildTarget == BuildTarget::NDS ? "NDS Package" : "GBA Package", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_AlwaysAutoResize);

        if (sPackaging)
        {
            ImGui::Text(sBuildTarget == BuildTarget::NDS ? "Building NDS ROM..." : "Building GBA ROM...");
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
                const char* openLabel = (sBuildTarget == BuildTarget::NDS) ? "  Open ROM  " : "  Open in mGBA  ";
                float mgbaW = std::max(140.0f * sUiScale, ImGui::CalcTextSize(openLabel).x + 20.0f);
                if (ImGui::Button(openLabel, ImVec2(mgbaW, btnH)))
                {
                    if (sBuildTarget == BuildTarget::GBA && sMgbaPath[0])
                    {
                        std::string cmd = "\"" + std::string(sMgbaPath) + "\" \"" + sPackageOutputPath + "\"";
                        STARTUPINFOA si = {}; si.cb = sizeof(si);
                        PROCESS_INFORMATION pi = {};
                        CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
                        if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
                    }
                    else
                    {
                        // Open with default file association (.nds or .gba)
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

            // Backface culling matching mesh settings
            if (ma.cullMode == CullMode::Back)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            }
            else if (ma.cullMode == CullMode::Front)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_FRONT);
            }
            // CullMode::None — no culling

            bool useTex = ma.textured && ma.glTexID != 0;
            if (useTex)
            {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, ma.glTexID);
                glColor3f(1, 1, 1);
            }
            else
            {
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
            }

            // Draw triangles
            if (!ma.indices.empty())
            {
                glBegin(GL_TRIANGLES);
                for (size_t ti = 0; ti < ma.indices.size(); ti++)
                {
                    const MeshVertex& v = ma.vertices[ma.indices[ti]];
                    if (useTex)
                        glTexCoord2f(v.u, 1.0f - v.v);
                    glNormal3f(v.nx, v.ny, v.nz);
                    glVertex3f(v.px, v.py, v.pz);
                }
                glEnd();
            }
            // Draw quads as fan-triangulated pairs
            if (!ma.quadIndices.empty())
            {
                glBegin(GL_TRIANGLES);
                for (size_t qi = 0; qi + 4 <= ma.quadIndices.size(); qi += 4)
                {
                    const MeshVertex& v0 = ma.vertices[ma.quadIndices[qi]];
                    const MeshVertex& v1 = ma.vertices[ma.quadIndices[qi+1]];
                    const MeshVertex& v2 = ma.vertices[ma.quadIndices[qi+2]];
                    const MeshVertex& v3 = ma.vertices[ma.quadIndices[qi+3]];
                    // tri 1: v0, v1, v2
                    if (useTex) glTexCoord2f(v0.u, 1.0f - v0.v);
                    glNormal3f(v0.nx, v0.ny, v0.nz); glVertex3f(v0.px, v0.py, v0.pz);
                    if (useTex) glTexCoord2f(v1.u, 1.0f - v1.v);
                    glNormal3f(v1.nx, v1.ny, v1.nz); glVertex3f(v1.px, v1.py, v1.pz);
                    if (useTex) glTexCoord2f(v2.u, 1.0f - v2.v);
                    glNormal3f(v2.nx, v2.ny, v2.nz); glVertex3f(v2.px, v2.py, v2.pz);
                    // tri 2: v0, v2, v3
                    if (useTex) glTexCoord2f(v0.u, 1.0f - v0.v);
                    glNormal3f(v0.nx, v0.ny, v0.nz); glVertex3f(v0.px, v0.py, v0.pz);
                    if (useTex) glTexCoord2f(v2.u, 1.0f - v2.v);
                    glNormal3f(v2.nx, v2.ny, v2.nz); glVertex3f(v2.px, v2.py, v2.pz);
                    if (useTex) glTexCoord2f(v3.u, 1.0f - v3.v);
                    glNormal3f(v3.nx, v3.ny, v3.nz); glVertex3f(v3.px, v3.py, v3.pz);
                }
                glEnd();
            }

            if (useTex)
            {
                glDisable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            else
            {
                glDisable(GL_LIGHTING);
                glDisable(GL_LIGHT0);
            }

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
            glDisable(GL_CULL_FACE);
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

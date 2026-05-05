#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Affinity
{

// Sprite data for GBA export
struct GBASpriteExport
{
    float x, y, z;     // world position (editor coords, ±512)
    float scale;
    float rotation;     // world-space facing angle in degrees
    int   palIdx;       // OBJ palette color index (1-5)
    int   assetIdx;     // sprite asset index (-1 = none/placeholder)
    int   animIdx;      // default animation index
    int   spriteType;   // SpriteType enum (0=Prop, 1=Player, ...)
    bool  animEnabled;  // false = static, no animation cycling
    int   meshIdx = -1; // mesh asset index (-1 = none)
};

// Player direction sprite for GBA export (RGBA8 image)
struct GBAPlayerDirExport
{
    const unsigned char* pixels;  // RGBA8 data (nullptr if empty)
    int width, height;
};

// Sprite asset frame for GBA export — 4bpp pixel data
static constexpr int kExportMaxFrameSize = 64;

struct GBASpriteFrameExport
{
    uint8_t pixels[kExportMaxFrameSize * kExportMaxFrameSize]; // palette indices 0-15
    int width, height;
};

// Sprite asset animation for GBA export
struct GBASpriteAnimExport
{
    int startFrame, endFrame;
    int fps;
    bool loop;
    float speed = 1.0f;
    int gameState = 0; // 0=None, 1=Idle, 2=Walk, 3=Run, 4=Sprint
};

// Sprite asset for GBA export — tile data + palette + animations
struct GBASpriteAssetExport
{
    std::string name;
    int baseSize;          // 8, 16, or 32
    int palBank;           // GBA OBJ palette bank (0-15)
    uint32_t palette[16];  // RGBA8 colors (index 0 = transparent)
    std::vector<GBASpriteFrameExport> frames;
    std::vector<GBASpriteAnimExport>  anims;
    int defaultAnim;

    // Directional sprite animation sets (each set = 8 direction RGBA8 images)
    struct DirAnimSetExport
    {
        std::string name;
        GBAPlayerDirExport dirImages[8] = {};
    };
    bool hasDirections = false;
    int paletteSrc = -1;       // -1 = own palette, >= 0 = share from asset index
    std::vector<DirAnimSetExport> dirAnimSets;
};

// Camera start data for GBA export
struct GBACameraExport
{
    float x, z;
    float height;
    float angle;        // radians
    float horizon;
    float walkSpeed   = 35.0f;
    float sprintSpeed = 53.0f;
    float walkEaseIn  = 19.0f;
    float walkEaseOut = 19.0f;
    float sprintEaseIn  = 6.0f;
    float sprintEaseOut = 12.0f;
    float jumpForce     = 2.0f;   // initial upward velocity (pixels)
    float gravity       = 0.09f;  // downward accel per frame (pixels)
    float maxFallSpeed  = 6.0f;   // terminal velocity (pixels)
    float jumpCamLand   = 37.0f;  // camera Y catch-up % when grounded
    float jumpCamAir    = 12.0f;  // camera Y catch-up % when airborne
    float autoOrbitSpeed = 205.0f; // brads per frame when strafing (0 = disabled)
    float jumpDampen = 0.75f;     // velocity multiplier per frame while A released + rising
    float drawDistance  = 0.0f;   // 0 = unlimited
    int   smallTriCull  = 0;      // min screen-space area to render (0=off)
    bool  skipFloor     = false;  // skip floor rendering entirely
    bool  coverageBuf   = false;  // front-to-back rendering with coverage buffer
};

// Mesh asset for GBA export
struct GBAMeshExport
{
    std::vector<float> positions; // px, py, pz per vertex (flat)
    std::vector<float> normals;   // nx, ny, nz per vertex (flat)
    std::vector<int>   objPosIdx; // original OBJ 'v' index per vertex (for welding)
    std::vector<uint32_t> indices;      // triangle indices (3 per face)
    std::vector<uint32_t> quadIndices;  // quad indices (4 per face)
    uint16_t colorRGB15;          // base color for shading
    int cullMode = 0;             // 0=Back, 1=Front, 2=None
    int exportMode = 0;           // 0=Quality (no weld), 1=Performance (welded)
    int lit = 1;                  // 1=lit (shaded), 0=unlit (flat color)
    int halfRes = 0;              // 1=rasterize every other scanline
    int wireframe = 0;            // 1=wireframe overlay
    int grayscale = 0;            // 1=grayscale shaded faces
    float drawDistance = 0.0f;    // per-mesh draw distance (0 = use global/unlimited)
    int collision = 1;            // 1 = generate collision faces, 0 = no collision
    int drawPriority = 0;         // 0 = draws on top (last), higher = draws first
    int visible = 1;              // 1 = render, 0 = invisible collision-only
    // Texture mapping
    std::vector<float> uvs;       // u, v per vertex (flat, interleaved)
    int textured = 0;             // 1 = textured, 0 = flat shaded
    int texW = 0, texH = 0;      // texture dimensions
    std::vector<uint8_t> texPixels;   // quantized indexed pixels (texW * texH)
    uint16_t texPalette[16] = {}; // RGB15 palette for this texture
};

// ---- Visual Script Export ----

// Node types matching editor VsNodeType enum
enum class GBAScriptNodeType : int {
    OnKeyPressed = 0, OnKeyReleased, OnKeyHeld,
    OnCollision, OnStart,
    Branch, CompareVar,
    MovePlayer, LookDirection, ChangeScene, SetVariable, AddVariable,
    PlaySound, Wait, Jump,
    Walk, Sprint, OrbitCamera, PlayAnim,
    SetGravity, SetMaxFall, DestroyObject,
    AutoOrbit, DampenJump,
    Integer, Key, Direction, Animation, Float,
    OnUpdate,
    Group,
    Object,
    CustomCode,
    IsMoving,
    IsOnGround,
    IsJumping,
    CheckFlag,
    SetFlag,
    ToggleFlag,
    FreezePlayer,
    UnfreezePlayer,
    SetCameraHeight,
    SetHorizon,
    Teleport,
    SetVisible,
    SetPosition,
    StopAnim,
    SetAnimSpeed,
    SetVelocityY,
    StopSound,
    AddMath,
    SubtractMath,
    MultiplyMath,
    NegateMath,
    RandomInt,
    GetFlag,
    AbsMath,
    MinMath,
    MaxMath,
    ModuloMath,
    ClampMath,
    SignMath,
    CompareInt,
    AndLogic,
    OrLogic,
    NotLogic,
    GetVariable,
    GetPlayerX,
    GetPlayerZ,
    OnTimer,
    SetScale,
    ScreenShake,
    FadeOut,
    FadeIn,
    MoveToward,
    LookAt,
    SetSpriteAnim,
    SpawnEffect,
    DoOnce,
    FlipFlop,
    Gate,
    ForLoop,
    Sequence,
    Select,
    Lerp,
    Distance,
    GetSpriteX,
    GetSpriteZ,
    IsKeyDown,
    SinWave,
    GetTime,
    SetHP,
    GetHP,
    DamageHP,
    AddScore,
    GetScore,
    Respawn,
    SaveData,
    LoadData,
    FlipSprite,
    SetDrawDist,
    EnableCollision,
    IsFlagSet,
    IsHPZero,
    IsNear,
    Countdown,
    ResetTimer,
    Increment,
    Decrement,
    SetFOV,
    ShakeStop,
    LockCamera,
    UnlockCamera,
    SetCamSpeed,
    ApplyForce,
    Bounce,
    SetFriction,
    CloneSprite,
    HideAll,
    ShowAll,
    Divide,
    Power,
    Remap,
    Average,
    GetHP2,
    PingPong,
    Print,
    SetColor,
    SwapSprite,
    GetAngle,
    GetPlayerY,
    GetSpriteY,
    SetSpriteY,
    WaitUntil,
    RepeatWhile,
    StopAll,
    SetLayer,
    GetLayer,
    SetAlpha,
    Flash,
    Delay,
    GetVelocityY,
    SetSpriteScale,
    RotateSprite,
    GetRotation,
    SetHP2,
    HealHP,
    GetMaxHP,
    SetMaxHP,
    IsAlive,
    SetBGColor,
    GetDeltaTime,
    MapValue,
    Wrap,
    SmoothStep,
    EaseIn,
    EaseOut,
    DistToPlayer,
    FacePlayer,
    MoveForward,
    Patrol,
    ChasePlayer,
    FleePlayer,
    SetAI,
    GetAI,
    OnDeath,
    OnHit,
    EmitParticle,
    SetTint,
    Shake,
    SetText,
    ShowHUD,
    HideHUD,
    GetRandom,
    ArrayGet,
    ArraySet,
    DrawNumber,
    DrawTextID,
    ClearText,
    SetTextColor,
    AddItem,
    RemoveItem,
    HasItem,
    GetItemCount,
    SetItemCount,
    UseItem,
    ShowDialogue,
    HideDialogue,
    NextLine,
    IsDialogueOpen,
    SetSpeaker,
    DialogueChoice,
    SetState,
    GetState,
    OnStateEnter,
    OnStateExit,
    IsInState,
    TransitionState,
    PrevState,
    StateTimer,
    OnTriggerEnter,
    OnTriggerExit,
    SetCollisionSize,
    IgnoreCollision,
    IsColliding,
    SpawnAt,
    DestroyAfter,
    SpawnProjectile,
    SetLifetime,
    GetLifetime,
    DrawBar,
    DrawSpriteIcon,
    ShowTimer,
    HideTimer,
    SetBarColor,
    SetBarMax,
    ReloadScene,
    GetScene,
    SetCheckpoint,
    LoadCheckpoint,
    GetInputAxis,
    OnAnyKey,
    GetLastKey,
    OnCollision2D,
    IsTrue,
    CursorUp,
    CursorDown,
    FollowLink,
    GetCursorStop,
    BlueprintRef,
    FollowPlayer,
    IsNear2D,
    IsFollowMoving,
    SetFollowFacing,
    SoundInstance,
    COUNT
};

struct GBAScriptNodeExport {
    int id;
    GBAScriptNodeType type;
    int paramInt[4];  // per-node params (key index, value, IEEE754 float bits, etc.)
    char customCode[512] = {};  // user-editable code override (empty = use default)
    char funcName[64] = {};     // custom function name (empty = use default)
    char ccPinCode[8][128] = {};  // per-pin code snippets for CustomCode nodes
    int ccPinCount = 0;           // number of data-in pins
    int ccExecIn = 1;             // number of exec-in pins
    int ccExecOut = 1;            // number of exec-out pins
    int ccDataOut = 0;            // number of data-out pins
};

struct GBAScriptLinkExport {
    int fromNodeId;
    int fromPinType;  // 0=execOut, 2=dataOut
    int fromPinIdx;
    int toNodeId;
    int toPinType;    // 1=execIn, 3=dataIn
    int toPinIdx;
};

struct GBAScriptExport {
    std::vector<GBAScriptNodeExport> nodes;
    std::vector<GBAScriptLinkExport> links;
};

// ---- Blueprint Export ----

struct GBABlueprintParamExport {
    int dataType;      // 0=int, 1=float, 2=key, 3=direction, 4=animation
    int defaultValue;
};

struct GBABlueprintExport {
    std::string name;
    GBAScriptExport script;  // nodes + links
    std::vector<GBABlueprintParamExport> params;
};

struct GBABlueprintInstanceExport {
    int blueprintIdx;
    int spriteIdx;      // -1 if tilemap object
    int tmObjIdx;       // -1 if 3D sprite
    int sceneMode;      // 0=Mode4/3D, 1=Mode0/tilemap
    int paramValues[8]; // resolved values (override or default)
    int paramCount;
};

// ---- Mode 0 Tilemap Export ----

struct GBATmObjectExport {
    int tileX, tileY;       // grid position
    int type;               // TmObjType enum
    int spriteAssetIdx;     // -1 = none
    int teleportScene;      // -1 = none
    bool camFollow;
    bool collision;
    float displayScale;
    int layer;
    bool animPlay;
    int animIdx;
    int facing;
    char name[32];
};

struct GBATmSceneExport {
    int mapW, mapH;                          // grid dimensions in tiles
    float zoom;                              // camera zoom (1.0 = 8px per tile)
    std::vector<uint16_t> tileIndices;       // mapW * mapH tile indices
    std::vector<GBATmObjectExport> objects;
    uint32_t palette[256];                   // tileset palette (RGBA8)
    std::vector<uint8_t> tilePixels;         // 8bpp tile pixel data (nTiles * 64 bytes)
    int tileCount;                           // number of unique tiles
};

// ---- HUD Element Export ----

struct GBAHudPieceExport {
    int spriteAssetIdx;
    int frame;
    int localX, localY;
    int size;   // 8, 16, 32, 64
};

struct GBAHudStopExport {
    int localX, localY;
    int linkedElement;  // -1 = none
};

struct GBAHudTextRowExport {
    char text[32];
    int localX, localY;
    uint16_t colorRGB15;
};

struct GBAHudElementExport {
    int screenX, screenY;
    bool visible;
    int runtimeMode;     // 0=Both, 1=Mode4, 2=Mode0
    std::vector<GBAHudPieceExport> pieces;
    std::vector<GBAHudPieceExport> sprites;
    std::vector<GBAHudStopExport> stops;
    std::vector<GBAHudTextRowExport> textRows;
    int cursorAssetIdx;
    int cursorFrame;
    int cursorOffX, cursorOffY;
    int layerPieces, layerSprites, layerText, layerCursor;
};

// Sound export: a single PCM sample (8-bit signed, 16384 Hz)
struct GBASoundSampleExport {
    std::string name;
    std::vector<int8_t> data;
    int sampleRate = 16384;
};

// Sound export: a note event in a sequence
struct GBASoundNoteExport {
    int tick;       // absolute tick position
    int channel;    // channel 0-15
    int note;       // MIDI note 0-127
    int velocity;   // 0-127
    int duration;   // ticks
    int sampleIdx;  // index into exported sample array
};

// Sound export: a complete sound instance (sequence + sample refs)
struct GBASoundInstanceExport {
    std::string name;
    int ticksPerBeat = 480;
    int tempo = 120;
    std::vector<GBASoundNoteExport> notes; // all notes merged
    std::vector<int> sampleIndices;        // which samples this instance uses
};

// Package the current map into a .gba ROM.
// runtimeDir: path to gba_runtime/ directory
// outputPath: where to write the final .gba
// Returns true on success. errorMsg receives details on failure.
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
                std::string& errorMsg);

} // namespace Affinity

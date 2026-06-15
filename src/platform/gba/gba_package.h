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
    float rotation;     // Y-axis rotation in degrees
    float rotationX = 0.0f;  // X-axis rotation in degrees
    float rotationZ = 0.0f;  // Z-axis rotation in degrees
    int   palIdx;       // OBJ palette color index (1-5)
    int   assetIdx;     // sprite asset index (-1 = none/placeholder)
    int   animIdx;      // default animation index
    int   spriteType;   // SpriteType enum (0=Prop, 1=Player, ...)
    bool  animEnabled;  // false = static, no animation cycling
    int   meshIdx = -1;   // mesh asset index (-1 = none)
    int   riggedMeshIdx = -1; // rigged (DSMA skinned) mesh asset index (-1 = none)
    int   rigAnimIdx = 0;     // default animation clip for the rigged mesh
    int   oamPrio = 0;    // OAM priority (0 = on top, 1 = behind)
    int   parentIdx = -1; // parent sprite index (-1 = standalone)
    float offsetX = 0, offsetY = 0, offsetZ = 0; // offset from parent
    bool  forceStatic = false; // force static rendering (ignore directions)
    bool  grounded = false;    // stay on ground (Y=0) instead of following parent Y
    bool  startHidden = false; // start invisible; a Cast Effect node shows + one-shots it
    uint32_t drawBehindExc = 0; // bitmask: bit N = mesh sprite[N] is exempt from draw-behind
    bool skipProximity = false; // true = always render regardless of draw distance
    bool billboard = false;     // mesh: always face camera (Y-axis billboard)
    float drawBehindThreshold = 0.0f; // Y offset for above/below mesh check
    uint32_t drawBehindClipPlane = 0; // bitmask: bit N = clip-by-plane for that mesh
    int spriteDrawPriority = 0; // OAM ordering tiebreaker (-8..+8)
    int blitSlot = -1;          // -1 = auto-assign, 0/1/2 = manual BG palette slot
    // Grind rail centerline (hand-authored in the 3D tab). railPath holds world
    // points (editor coords) the runtime slides the player along; empty = not a
    // rail / no path. Exported into mapdata.h keyed by sprite index.
    bool isGrindRail = false;
    bool railSpline = false;    // runtime follows a smooth Catmull-Rom curve through railPath
    // [0..2] = world xyz, [3] = isEnd, [4] = isBounce, [5] = isStart (1/0 flags).
    std::vector<std::array<float,6>> railPath;
    // Navigation (PSV navmesh, NPC/Enemy): emitted into psv_nav.h afn_npc_nav.
    int   navMode = 0;          // 0 = off, 1 = follow player, 2 = wander
    float navSpeed = 5.0f;      // editor units per frame (exporter converts /4 to world)
    float navStopDist = 32.0f;  // stop this close to the goal, editor units
    int   navRepath = 30;       // frames between path recomputes
    int   navMoveClip = -1;     // rig clip to play while moving (-1 = keep current)
    // Nav bounds box: axis-aligned volume selecting which scene geometry
    // participates in the PSV navmesh build (extents in editor units).
    bool  isNavPlane = false;
    float navPlaneW = 64.0f;
    float navPlaneD = 64.0f;
    float navPlaneH = 64.0f;
    bool  navNegate = false;    // negator box: force geometry inside NON-walkable
};

// Player direction sprite for GBA export (RGBA8 image)
struct GBAPlayerDirExport
{
    const unsigned char* pixels;  // RGBA8 data (nullptr if empty)
    int width, height;
};

// Sprite asset frame for GBA export — 4bpp pixel data. 128 supports the
// PSV-only 128x128 asset size (PSV billboards are RGBA textures with no OBJ
// hardware cap); GBA/NDS OAM sprites still top out at 64x64 — their
// emitters nearest-neighbour scale down to the largest OBJ size.
static constexpr int kExportMaxFrameSize = 128;

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
    float orbitPitch = 0.0f;  // initial orbit pitch (deg, 0 = auto); PSV-consumed
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
    float orbitCamEaseIn  = 25.0f; // orbit lerp while L/R held (ramping in)
    float orbitCamEaseOut = 50.0f; // orbit lerp after L/R released (settle)
    int   orbitMaxDelta   = 0;     // per-frame cap on orbit_angle change (brad). 0 = uncapped.
    bool  orbitSnapCam    = false; // legacy/unused
    float drawDistance  = 0.0f;   // 0 = unlimited (mesh)
    float spriteDrawDistance = 0.0f; // 0 = unlimited (sprite)
    int   smallTriCull  = 0;      // min screen-space area to render (0=off)
    bool  skipFloor     = false;  // skip floor rendering entirely
    bool  coverageBuf   = false;  // front-to-back rendering with coverage buffer
    float camPitch      = 0.0f;  // per-vertex depth-based pitch tilt
    bool  autoPitch     = false; // dynamically compute pitch from floor slope
    bool  horizonClamp  = false; // clamp vertices above camera to horizon
    bool  dynamicHorizon = false; // shift horizon line based on floor slope
    bool  faceCull       = false; // skip faces with any vertex above camera
    // Player camera presets (Mode 4): extra slots a SetCamera node blends to.
    // Runtime slot 0 = the scene default above; these are slots 1..N.
    struct CamSlot { float angle = 0.0f, horizon = 60.0f, distance = 0.0f, height = 14.0f, orbitPitch = 0.0f; };
    std::vector<CamSlot> camSlots;
};

// Mesh asset for GBA export
struct GBAMeshExport
{
    std::vector<float> positions; // px, py, pz per vertex (flat)
    std::vector<float> normals;   // nx, ny, nz per vertex (flat)
    std::vector<uint8_t> vertexColors; // r, g, b (0..255) per vertex (flat); empty = none
    int hasVertexColor = 0;       // 1 = OBJ 2.0 per-vertex colors present
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
    std::vector<uint8_t> texPixels;   // quantized indexed pixels (texW * texH), 0..255
    uint16_t texPalette[256] = {}; // RGB15 palette (16 or 256 entries used)
    int texture256 = 0;           // 1 = 256-colour (GL_RGB256, 8bpp), 0 = 16-colour (GL_RGB16, 4bpp)
    int textureHasAlpha = 0;      // 1 = palette[0] is transparent (NDS COLOR0_TRANSPARENT)
    int texFiltered = 0;          // 1 = NDS export pre-blurs the texture (software smoothing; DS has no HW filter)
    int perspCorrect = 0;         // 1=perspective-corrected texture mapping
    int texInIwram = 0;           // 1 = copy texture into IWRAM cache at boot
    int clampAbove = 0;           // 1=clamp vertices to never project above horizon
    int nearClip = 0;             // 1=view-space near-plane clipping (fixes slope walling)
    int faceCull = 0;             // 1=skip faces with vertices above camera (hard cutoff)
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
    IsFalling,
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
    PlayHudAnim,
    StopHudAnim,
    SetHudAnimSpeed,
    OnRise,
    ResetScene,
    SetPlayerHeight,
    SetHudValue,
    UpdateRespawnPos,
    SetVelocityX,
    SetVelocityZ,
    VelocityFalloff,
    BoostForward,
    HaltMomentum,
    StartGrind,
    StopGrind,
    IsGrinding,
    IsNotGrinding,
    GrindPower,
    GrindBoost,
    GrindBleed,
    GrindCatch,
    SetPlayerWidth,
    PlaySkelAnim,
    SkelAnim,
    SetSkelAnim,
    SetCamera,
    TankCamera,
    TurnPlayer,
    CastEffect,
    AttachedSprite,  // data: the owning BP instance's sprite index ("self" anchor)
    LockOnTarget,    // action: lock-on camera assist — orbit eases toward facing the target
    ReleaseLockOn,   // action: release the lock-on camera assist
    LockStrafe,      // action: target-relative movement while locked on (face target, backpedal/circle)
    IsLockedOn,      // gate: passes exec only while a Lock On target is active
    IsNotLockedOn,   // gate: passes exec only while NO Lock On target is active
    DashToTarget,    // action: lunge the player toward the lock target (bullet-punch)
    StrafeAnim,      // action: 8-way directional clip picker from lock-relative stick
    IsInView,        // gate: passes only if the target object is within camera FOV
    SnapStick8,      // action: gate the left stick to 8 directions (PSV); set on start
    Dodge,           // action: timed directional roll burst (PSV) — side/back dodge with a clip
    IsDodging,       // gate: passes exec while a Dodge is active
    IsNotDodging,    // gate: passes exec while NO Dodge is active (i-frame guard for damage)
    IsAirborne,      // gate: passes exec while the player is off the ground (inverse of Is On Ground)
    IsLanding,       // gate: passes exec for a short window right after touchdown (land anim)
    IsNotLanding,    // gate: passes exec when NOT in the post-touchdown land window
    COUNT
};

struct GBAScriptNodeExport {
    int id;
    GBAScriptNodeType type;
    int paramInt[4];  // per-node params (key index, value, IEEE754 float bits, etc.)
    char customCode[4096] = {};  // user-editable code override (empty = use default)
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
    std::vector<bool> m4SceneSkyEnabled;  // per Mode 4 scene sky enable
    std::vector<bool> m1SceneSkyEnabled;  // per Mode 1 scene sky enable
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
    uint32_t sceneMask; // bitmask of scenes (0xFFFFFFFF = all scenes)
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
    int pixelScale = 1;                      // pixel scale (1=normal, 2=2x zoom)
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
    bool blackTint = false;
    int opacity = 16;
};

struct GBAHudStopExport {
    int localX, localY;
    int linkedElement;  // -1 = none
};

struct GBAHudTextRowExport {
    char text[32];
    int localX, localY;
    uint16_t colorRGB15;
    int font; // 0=normal 8x8, 1=small pixel 4x5, 2=5x7 debug
    int sourceSlot; // -1=static, 0-3=afn_hud_value[N]
    int pad; // minimum digits for counter mode
    int spacing; // extra spacing adjustment added to font advance
    int scale;   // 1=normal, 2=double size
};

struct GBAHudKeyframeExport {
    int frame;
    int offX, offY;
    int rot;           // degrees 0-359 (converted to brads at export)
    int scaleX, scaleY; // 8.8 fixed point (256 = 1.0x)
};

struct GBAHudAnimLayerItemExport {
    int type;   // 0=piece, 1=sprite, 2=text, 3=cursor
    int index;
};

struct GBAHudAnimLayerExport {
    std::string name;
    int interp = 1;     // 0=constant, 1=linear, 2=bezier
    bool loop = false;
    int speed = 5;      // frames-per-tick (60/fps), e.g. 5 = 12fps
    int length = 60;    // total animation length in frames (from timeline range)
    std::vector<GBAHudAnimLayerItemExport> items;
    std::vector<GBAHudKeyframeExport> keyframes;
};

struct GBAHudElementExport {
    int screenX, screenY;
    bool visible;
    int runtimeMode;     // 0=Both, 1=Mode4, 2=Mode0
    uint32_t mode0SceneMask = 0xFFFFFFFF; // which Mode 0 scenes the element shows in
    uint32_t mode4SceneMask = 0xFFFFFFFF; // which Mode 4 scenes the element shows in
    std::vector<GBAHudPieceExport> pieces;
    std::vector<GBAHudPieceExport> sprites;
    std::vector<GBAHudStopExport> stops;
    std::vector<GBAHudTextRowExport> textRows;
    int cursorAssetIdx;
    int cursorFrame;
    int cursorOffX, cursorOffY;
    int layerPieces, layerSprites, layerText, layerCursor;
    std::vector<GBAHudKeyframeExport> keyframes;
    bool animLoop = false;
    std::vector<GBAHudAnimLayerExport> animLayers;
};

// Sound export: a single PCM sample (8-bit signed, 16384 Hz)
struct GBASoundSampleExport {
    std::string name;
    std::vector<int8_t> data;
    std::vector<int16_t> data16; // 16-bit source for high-quality export
    int sampleRate = 16384;
    bool loop = true;
    int loopStart = 0; // in samples (0 = loop from start)
    int loopEnd = 0;   // in samples (0 = loop to end)
    int decayPct = 0;  // volume decay over note duration (0-100)
    int decayMinMs = 500; // minimum note length for decay (ms)
    int releaseMs = 250; // release fade-out time in ms
    int volScale = 256;  // 8.8 fixed-point volume scale (256 = 1.0, for normalization compensation)
    int vibratoDepth = 0; // vibrato depth in cents (0 = off)
    int vibratoRate = 5;  // vibrato LFO rate in Hz
    // sampleRate above is the GBA-targeted rate with fineTune already baked
    // in via *2^(fineTuneCents/1200). NDS keeps the raw rate + cents
    // separate so it can apply finer-precision pitch math at runtime.
    int rawSampleRate = 16384; // sample rate without fineTune baked
    int fineTuneCents = 0;     // signed cents to apply on top of rawSampleRate
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
    int interpolation = 0;                 // 0 = nearest, 1 = linear
    int mixerGain = 0;                     // 0 = Normal (>>7), 1 = Loud (>>6)
    int voiceCount = 6;                    // max simultaneous voices (4-8)
    int softFade = 1;                      // legacy combined flag
    int softFadeA = 1;                     // fade when routed to FIFO A (polyphony-shifted)
    int softFadeB = 1;                     // fade when routed to FIFO B (flat)
    int attenuateFifoA = 0;                // FIFO A polyphony attenuation: 0 = off (louder), 1 = on
    int longRelease = 0;                   // force minimum 1672-sample release tail
    int hifiMode = 0;                      // 0 = normal (~25kHz), 1 = hi-fi (~32kHz)
    int compatMode = 0;                    // 0 = normal, 1 = compat (halved rate, less CPU)
    int bufferScale = 0;                   // 0 = normal, 1 = scale buffer to match frame time
    int mixPadding = 0;                    // 0 = exact buffer, 1 = mix 25% extra samples
    int lowRate = 0;                       // 0 = normal, 1 = ultra-low rate (~10kHz)
    int preMix = 0;                        // 0 = mix after render, 1 = mix right after VBlank swap
    int isrSwap = 0;                       // 0 = main-loop swap, 1 = VBlank-ISR swap (with preMix)
    int chunkedMixer = 0;                  // 0 = single-call mix, 1 = chunked HBlank mix
    bool loop = false;                     // loop between loopStartTick and loopEndTick
    int loopStartTick = 0;                 // MIDI tick to jump back to
    int loopEndTick = 0;                   // MIDI tick to trigger loop (0 = end of sequence)
    std::vector<GBASoundNoteExport> notes; // all notes merged
    std::vector<int> sampleIndices;        // which samples this instance uses
    bool isSfx = false;                    // true = one-shot SFX (uses afn_play_sfx)
    int sfxSampleIdx = -1;                 // exported sample index for SFX mode
    int fifoChannel = 0;                   // 0 = FIFO A, 1 = FIFO B
};

// Sky animation frame for export
struct GBASkyFrameExport {
    const unsigned char* pixels = nullptr; // RGBA pixels
    int w = 0, h = 0;
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
                std::string& errorMsg,
                const unsigned char* m7FloorPixels = nullptr,
                int m7FloorW = 0, int m7FloorH = 0,
                int m7FloorSize = 3,
                const std::vector<GBASkyFrameExport>& skyFrames = {},
                int skyAnimSpeed = 8,
                bool deltaTime = false,
                bool showFps = false,
                bool smoothSky = false);

} // namespace Affinity

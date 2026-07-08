#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Affinity
{

// Sprite data for GBA export
struct AfnSpriteExport
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
    int   boneIdx = -1;   // attached sub-sprite: ride this parent-rig bone (-1 = parent origin)
    int   driveElementIdx = -1; // attached sub-sprite: run this HUD element's keyframe animation
                                // (rotation/scale) on the sub-sprite's own graphic (-1 = none)
    int   effectKind = 0;       // attached sub-sprite: 0 = draw the sprite; >0 = draw a procedural
                                // effect there instead (1 = Aura Sphere)
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
struct AfnPlayerDirExport
{
    const unsigned char* pixels;  // RGBA8 data (nullptr if empty)
    int width, height;
};

// Sprite asset frame for GBA export — 4bpp pixel data. 128 supports the
// PSV-only 128x128 asset size (PSV billboards are RGBA textures with no OBJ
// hardware cap); GBA/NDS OAM sprites still top out at 64x64 — their
// emitters nearest-neighbour scale down to the largest OBJ size.
// Must match the editor's kMaxFrameSize so 256/512 (PSV) frames export at the
// right stride — otherwise large frames are clamped to 128 and read back garbled.
static constexpr int kExportMaxFrameSize = 960;

struct AfnSpriteFrameExport
{
    uint8_t pixels[kExportMaxFrameSize * kExportMaxFrameSize]; // palette indices 0-15
    int width, height;
    std::vector<uint8_t> alpha; // optional per-pixel alpha (w*h); empty = binary cutout (PSV soft alpha)
};

// Sprite asset animation for GBA export
struct AfnSpriteAnimExport
{
    int startFrame, endFrame;
    int fps;
    bool loop;
    float speed = 1.0f;
    int gameState = 0; // 0=None, 1=Idle, 2=Walk, 3=Run, 4=Sprint
};

// Sprite asset for GBA export — tile data + palette + animations
struct AfnSpriteAssetExport
{
    std::string name;
    int baseSize;          // 8, 16, or 32
    int palBank;           // GBA OBJ palette bank (0-15)
    uint32_t palette[16];  // RGBA8 colors (index 0 = transparent)
    // PSV-only higher-color path (psvColors = 32/64/128). When > 16 the PSV
    // exporter emits from psvPalette/psvFrames instead of the 16-color palette.
    int psvColors = 16;
    bool useAlpha = false;   // PSV: emit per-pixel alpha from frames[].alpha (soft edges)
    uint32_t psvPalette[128] = {};
    std::vector<AfnSpriteFrameExport> psvFrames;
    std::vector<AfnSpriteFrameExport> frames;
    std::vector<AfnSpriteAnimExport>  anims;
    int defaultAnim;

    // Directional sprite animation sets (each set = 8 direction RGBA8 images)
    struct DirAnimSetExport
    {
        std::string name;
        AfnPlayerDirExport dirImages[8] = {};
    };
    bool hasDirections = false;
    int paletteSrc = -1;       // -1 = own palette, >= 0 = share from asset index
    std::vector<DirAnimSetExport> dirAnimSets;
};

// Camera start data for GBA export
struct AfnCameraExport
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
    bool  wallAware      = false; // PSV: pull the orbit camera in off walls (no clip-through)
    // Player camera presets (Mode 4): extra slots a SetCamera node blends to.
    // Runtime slot 0 = the scene default above; these are slots 1..N.
    struct CamSlot { float angle = 0.0f, horizon = 60.0f, distance = 0.0f, height = 14.0f, orbitPitch = 0.0f, lookYaw = 0.0f, lockAware = 0.0f, hOffset = 0.0f, depthOffset = 0.0f, lookPitch = 0.0f, vOffset = 0.0f; };
    std::vector<CamSlot> camSlots;
    // Keyframed camera animations (cutscenes) from the player. eye in WORLD/rail space;
    // yaw/pitch = look direction (radians); interp 0=Const,1=Lin,2=Smooth. The Play
    // Camera Anim node selects one path by index (Anim pin).
    struct CamKf { int frame = 0; float ex = 0, ey = 0, ez = 0, yaw = 0, pitch = 0, fov = 45; int interp = 2; float speed = 1.0f; bool smoothIn = true, smoothOut = true; };
    struct CamAnimExp { char name[32] = "Anim"; int fps = 30; bool smoothPath = false; std::vector<CamKf> keyframes; };
    std::vector<CamAnimExp> camAnims;

    // Effects-tab layers (each a particle or lightning preset). A Play Effect node
    // triggers one by index. Spline points are normalised (x 0..1 along the path,
    // y 0..1 in the canvas — ~0.86 = floor — , th = per-point thickness).
    struct FxPt { float x = 0, y = 0, th = 1; };
    struct FxLayerExp {
        int   kind = 1;            // 0 = particle, 1 = lightning
        int   pCount = 10; float pSpeed=1.6f,pSpread=0.6f,pLife=45,pGrav=0.05f,pSize=10;
        float bWidth=0.55f,bBow=7,bJitter=1.3f,bDecay=0.97f,bPulse=0.0f; int bSegs=14,bBounces=12;  // WORLD units
        bool  bSurge=false; float bTaperS=0,bTaperE=0,bLifeIn=0,bLifeOut=0,bFalloffS=0,bFalloffE=0;  // legacy
        bool  bTravel=true;
        int   bTravelBounces=3;
        float bTravelLife=150;
        float bTravelPersist=0.55f;
        float bTravelFade=0.30f;
        float bArcLen=13;          // world units each bounce spans (reach = bArcLen * bounces)
        int   bFilaments=5;        // bundled crackling strands
        float bOrbSize=1.0f;       // head-orb radius multiplier
        float bColR=0.376f,bColG=0.690f,bColB=1.0f;
        // thunder (kind 2) params — defaults = the runtime hardcode
        float tCloudH=56,tAim=60,tCharge=90,tSpread=26,tCloudSize=12,tReticle=4; int tPuffs=18;
        float tCamPitch=40;        // cinematic camera up-tilt while charging (deg)
        float tCamSmooth=0.06f;    // ease-in rate of the tilt (lower = gentler)
        float tAimSpeed=2.0f;      // reticle distance slide speed (units/frame at full stick)
        float tAimOrbit=500;       // reticle L2/R2 orbit speed (brad/frame)
        float tCloudR=0.157f,tCloudG=0.188f,tCloudB=0.282f;
        std::vector<FxPt> spline;
    };
    // An instance = one node-callable effect = a set of layers composited together.
    struct FxInstanceExp { std::vector<FxLayerExp> layers; };
    std::vector<FxInstanceExp> fxInstances;
};

// Mesh asset for GBA export
struct AfnMeshExport
{
    std::vector<float> positions; // px, py, pz per vertex (flat)
    std::vector<float> normals;   // nx, ny, nz per vertex (flat)
    std::vector<uint8_t> vertexColors; // r, g, b (0..255) per vertex (flat); empty = none
    int hasVertexColor = 0;       // 1 = OBJ 2.0 per-vertex colors present
    // OBJ 2.0 light rig ("#light"/"#sun"/"#ambient" lines) — PSV only. The
    // exporter transforms each light by every placed instance of this mesh and
    // emits afn_lights[] for runtime vitaGL GL_LIGHTING. energy already includes
    // the editor's Light Intensity multiplier.
    struct LightExp { int type = 0; float x=0,y=0,z=0, r=1,g=1,b=1, energy=1000, radius=0; }; // type 0=point 1=sun
    std::vector<LightExp> lights;
    float lightAmbient[3] = { 0.05f, 0.05f, 0.05f };
    // Lightmap (OBJ 2.0 #lightmap + 4-component vt) — PSV only: RGBA texture
    // multiplied over the mesh through a second UV set in a second draw pass.
    std::vector<float> uvs2;        // u2, v2 per vertex (flat); empty = no lightmap
    std::vector<uint8_t> lmPixels;  // RGBA8 (lmW * lmH * 4)
    int lmW = 0, lmH = 0;
    // Editor/imported lights baked for a LIGHTMAPPED mesh: per-vertex light
    // term (zero ambient) drawn as an ADDITIVE third pass on top of the
    // lightmap — fb = albedo×lightmap + albedo×lights. Empty = no such pass.
    std::vector<uint8_t> addLightColors;   // r, g, b per vertex (flat)
    // AO map (OBJ 2.0 #aomap + 6-component vt) — PSV only: GRAYSCALE occlusion
    // multiplied through the THIRD UV set between the lightmap and the
    // additive lights, faded by aoStrength (fb *= lerp(1, ao, strength)).
    std::vector<float> uvs3;        // u3, v3 per vertex (flat); empty = no AO map
    std::vector<uint8_t> aoPixels;  // grayscale bytes (aoW * aoH)
    int aoW = 0, aoH = 0;
    float aoStrength = 1.0f;
    // MAP GROUPS (OBJ 2.0 v1.5) — PSV only: 2+ lightmap/AO pairs in one mesh,
    // applied per face. Empty = single-slot fields above.
    struct MapGroupExp {
        std::vector<uint8_t> lmPixels;  // RGBA8 (lmW*lmH*4); empty = none
        int lmW = 0, lmH = 0;
        std::vector<uint8_t> aoPixels;  // grayscale (aoW*aoH); empty = none
        int aoW = 0, aoH = 0;
    };
    std::vector<MapGroupExp> mapGroups;
    std::vector<uint8_t> triMapGroup, quadMapGroup;   // group per tri / quad
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
    std::vector<uint32_t> texRGBA;    // PSV "512 Colors": full RGBA8888 (bypasses the palette); empty = palette
    int texture256 = 0;           // 1 = 256-colour (GL_RGB256, 8bpp), 0 = 16-colour (GL_RGB16, 4bpp)
    int textureHasAlpha = 0;      // 1 = palette[0] is transparent (NDS COLOR0_TRANSPARENT)
    int softAlpha = 0;            // 1 = attached-model blended alpha: emit per-pixel alpha (texAlpha)
                                  // into the texture's A channel + draw the mesh GL_BLEND (PSV)
    std::vector<uint8_t> texAlpha;// per-pixel alpha plane (texW*texH) when softAlpha
    int texFiltered = 0;          // 1 = NDS export pre-blurs the texture (software smoothing; DS has no HW filter)
    int perspCorrect = 0;         // 1=perspective-corrected texture mapping
    int texInIwram = 0;           // 1 = copy texture into IWRAM cache at boot
    int clampAbove = 0;           // 1=clamp vertices to never project above horizon
    int nearClip = 0;             // 1=view-space near-plane clipping (fixes slope walling)
    int faceCull = 0;             // 1=skip faces with vertices above camera (hard cutoff)

    // Multi-material (OBJ usemtl) — PSV only. When `materials` is non-empty the
    // PSV exporter draws this mesh group-per-slot (one texture bound per slot),
    // selecting each triangle's slot via triMaterial/quadMaterial. Other targets
    // ignore these and use the single texture above (slot 0). Empty = single-tex.
    struct MatSlot {
        int textured = 0;
        int texW = 0, texH = 0;
        std::vector<uint8_t> texPixels;   // indexed pixels (texW*texH)
        uint16_t texPalette[256] = {};    // RGB15
        int texture256 = 0;
        int wrap = 0;                     // 0=Clip 1=Extend 2=Mirror
    };
    std::vector<MatSlot> materials;       // slots 0..N (includes slot 0); empty = single-texture
    std::vector<uint8_t> triMaterial;     // slot per triangle (parallel to indices/3)
    std::vector<uint8_t> quadMaterial;    // slot per quad (parallel to quadIndices/4)
};

// ---- Visual Script Export ----

// Node types matching editor VsNodeType enum
enum class AfnScriptNodeType : int {
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
    ChargeShot,      // action: hold-to-charge — grow the player's effect ball (focus blast) while held
    IsCharging,      // gate: passes exec while a Charge Shot is charging
    FireChargeShot,  // action: release — fire the charged ball as a homing projectile at the lock target
    IsFiring,        // gate: passes exec for a short window after a Charge Shot is fired (hold the launch anim)
    IsFalse,         // gate: passes exec while a condition data input is ZERO (if-not)
    SwitchInt,       // flow: route exec to case 0..3 by integer value, else Default
    Bool,            // data: constant true/false (1/0)
    Xor,             // data: logical exclusive-or of two inputs
    SetEnergy,       // action: set the player's energy resource (clamped 0..max)
    AddEnergy,       // action: accumulate energy (clamped to max)
    SpendEnergy,     // action: subtract energy (clamped to 0)
    SetMaxEnergy,    // action: set the energy capacity
    GetEnergy,       // data: outputs the current energy value
    HasEnergy,       // gate: passes exec while energy >= Amount
    SetHealth,       // action: set player health (clamped 0..max)
    DamageHealth,    // action: subtract health (clamped to 0)
    HealHealth,      // action: add health (clamped to max)
    SetMaxHealth,    // action: set health capacity
    GetHealth,       // data: outputs the current health value
    GetChargePct,    // data: outputs the Charge Shot charge level as 0-100% (live, read at release)
    SpendChargeEnergy, // action: subtract energy scaled by charge level (Min%..Max% over 0..100% charge)
    IsNotCharging,   // gate: passes exec while NO Charge Shot is charging (inverse of Is Charging)
    CycleHudValue,   // action: afn_hud_value[slot] = (val + delta) wrapped to [0,count)
    OrbitCameraOnObject, // action: orbit the camera around a target object (KO/death cinematic)
    HoldSkelClip,    // action: play a rig clip on an NPC once and freeze the last frame (die collapse)
    IsHealthZero,    // gate: passes exec while the player's health resource is <= 0
    FadeInHudElement, // action: show a HUD element + crossfade its alpha 0->full over N frames
    IsHudVisible,    // gate: passes exec while a given HUD element slot is visible
    StopMusic,       // action: stop ONLY the persistent music track, leaving SFX ringing
    LoopHudAnim,     // action: keep a HUD element's anim layers active + looping (re-arm per frame)
    BeamClash,       // action: enable the beam-clash mechanic + set its tunables (runtime clash_sense)
    IsClashReady,    // gate: passes exec while the runtime senses both full beams meeting
    SuppressBeams,   // action: kill both in-flight projectiles (player + enemy)
    ClashBegin,      // action: start a clash — centre the balance, reset, suppress beams
    ClashPush,       // action: player's Cross tap pushes the clash balance toward the enemy
    ClashAiStep,     // action: one struggle step — AI mash, clamp, mash-SFX pitch, button flash
    IsClashWon,      // gate: clash balance pushed fully to the enemy (>= 1.0)
    IsClashLost,     // gate: clash balance pushed fully into the player's zone (<= 0.0)
    // --- Enemy combat AI (enemy BP) ---
    IsAiState,       // gate: enemy AI state == param
    IsPlayerWithin,  // gate: distance to player <= Range
    IsPlayerBeyond,  // gate: distance to player > Range
    IsAiFlag,        // gate: an enemy-AI per-frame flag (param selects which) is set
    SetAiState,      // action: set the enemy AI state = param
    AiSense,         // action: per-frame sense (slot/death/dist/face/cooldowns/flags)
    AiRoam,          // action: ROAM clip (nav drives motion)
    AiChase,         // action: close toward the player
    AiStrafe,        // action: orbit the player at preferred distance
    AiDodgeBegin,    // action: start a side-roll dodge
    AiDodgeStep,     // action: integrate the dodge roll
    AiChargeBegin,   // action: start a shot wind-up (charge vs tap)
    AiChargeStep,    // action: grow the charge orb at the muzzle
    AiFireBeam,      // action: launch the projectile
    AiFireRecover,   // action: fire-recovery clip + timer
    EnemyAI,         // action: enable the enemy AI + set its tunables
    OrbitCamStep,    // action: advance the orbit-cam timer one frame (node-driven orbit)
    StopOrbitCam,    // action: end the orbit cam (afn_cam_orbit_active = 0)
    StepEnemyBeam,   // action: advance the enemy's in-flight projectile (flight + hit)
    StepFocusBlast,  // action: advance the player's in-flight Focus Blast (flight + hit)
    ShowHPBar,       // action: raise the floating HP bar for an object this frame
    IsBlastIncoming, // gate: a player Focus Blast is within the enemy's dodge range (+chance)
    ClashHitEnemy,   // action: clash win -> deal Clash Dmg % of the player's full attack to an object
    ClashHitPlayer,  // action: clash loss -> deal Clash Dmg % of the enemy's full attack to the player
    SetBlock,        // action: set the player's blocking flag (1 while guarding -> incoming dmg reduced)
    ShouldAiBlock,   // gate: a blast is incoming and the AI rolls to BLOCK (vs dodge)
    AiBlockBegin,    // action: enemy raises its guard (block clip) for a window
    AiBlockStep,     // action: hold the enemy block stance; flags done at the end
    CanFireBlast,    // gate: passes only when NO Focus Blast is in flight (so re-fire SFX/charge is suppressed)
    QuickAttack,     // action: dash-in melee lunge toward the lock target (or forward) + contact damage + skid
    IsDashing,       // gate: passes while a Quick Attack dash/skid is in progress (phase != 0)
    QuickAttackHit,  // gate: passes on the single frame a Quick Attack contact lands
    ChargeUp,        // action: hold-to-charge — reveal player's hidden attached effect + fill energy
    QuickAttackStarted, // gate: passes on the single frame a Quick Attack dash actually begins
    AiQuickAttack,   // action (enemy AI): per-frame melee reflex — dash-in Quick Attack + jump-evade
    EnemyAiTiming,   // action (enemy AI): set the remaining decision/timing knobs (de-aggro, strafe leg, etc.)
    AiClips,         // action (enemy AI): set the enemy anim clip indices (name-resolved -> drift-proof)
    PlayCameraAnim,  // action: take over the camera + play the player's keyframed cutscene path
    TogglePause,     // gate: flips the global scene-pause; exec A = On Paused, B = On Unpaused
    AiDodgeClips,    // action (enemy AI): set the enemy dodge + charge-dodge clip indices (name-resolved -> drift-proof)
    LockPlayerFunctions, // action: while it runs, lock out player abilities (menu nav still works)
    SpawnParticles,  // action: emit a burst of billboard particles (pure-code sim) at the player
    LightningBeam,   // action: cast a jittered ribbon (lightning/laser) from the player to the lock-on enemy / forward
    PlayEffect,      // action: trigger an authored effect LAYER (from the Effects tab) by index at the player
    FloorReticle,    // action: draw a glowing aim reticle on the floor ahead of the player
    ThunderCharge,   // action: charge the Thunder spell (clouds + reticle)
    ThunderStrike,   // action: release the Thunder strike at the aim
    AimStick,        // action: left stick moves the floor reticle + auto-orbits the camera
    AiOrbScale,      // action (enemy AI): set the enemy focus-orb charge Min/Max Scale%
    ThrowBall,       // action (On Key Released): throw the aimed pokeball
    AimBall,         // action (On Key Held): aim the pokeball throw
    PhysicalClash,   // action (config): arm the dash-vs-dash QTE struggle
    COUNT
};

struct AfnScriptNodeExport {
    int id;
    AfnScriptNodeType type;
    int paramInt[4];  // per-node params (key index, value, IEEE754 float bits, etc.)
    char customCode[4096] = {};  // user-editable code override (empty = use default)
    char funcName[64] = {};     // custom function name (empty = use default)
    char ccPinCode[8][128] = {};  // per-pin code snippets for CustomCode nodes
    int ccPinCount = 0;           // number of data-in pins
    int ccExecIn = 1;             // number of exec-in pins
    int ccExecOut = 1;            // number of exec-out pins
    int ccDataOut = 0;            // number of data-out pins
};

struct AfnScriptLinkExport {
    int fromNodeId;
    int fromPinType;  // 0=execOut, 2=dataOut
    int fromPinIdx;
    int toNodeId;
    int toPinType;    // 1=execIn, 3=dataIn
    int toPinIdx;
};

struct AfnScriptExport {
    std::vector<AfnScriptNodeExport> nodes;
    std::vector<AfnScriptLinkExport> links;
    std::vector<bool> m4SceneSkyEnabled;  // per Mode 4 scene sky enable
    std::vector<bool> m1SceneSkyEnabled;  // per Mode 1 scene sky enable
};

// ---- Blueprint Export ----

struct AfnBlueprintParamExport {
    int dataType;      // 0=int, 1=float, 2=key, 3=direction, 4=animation
    int defaultValue;
};

struct AfnBlueprintExport {
    std::string name;
    AfnScriptExport script;  // nodes + links
    std::vector<AfnBlueprintParamExport> params;
};

struct AfnBlueprintInstanceExport {
    int blueprintIdx;
    int spriteIdx;      // -1 if tilemap object
    int tmObjIdx;       // -1 if 3D sprite
    int sceneMode;      // 0=Mode4/3D, 1=Mode0/tilemap
    uint32_t sceneMask; // bitmask of scenes (0xFFFFFFFF = all scenes)
    int paramValues[8]; // resolved values (override or default)
    int paramCount;
};

// ---- Mode 0 Tilemap Export ----

struct AfnTmObjectExport {
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

struct AfnTmSceneExport {
    int mapW, mapH;                          // grid dimensions in tiles
    float zoom;                              // camera zoom (1.0 = 8px per tile)
    int pixelScale = 1;                      // pixel scale (1=normal, 2=2x zoom)
    std::vector<uint16_t> tileIndices;       // mapW * mapH tile indices
    std::vector<AfnTmObjectExport> objects;
    uint32_t palette[256];                   // tileset palette (RGBA8)
    std::vector<uint8_t> tilePixels;         // 8bpp tile pixel data (nTiles * 64 bytes)
    int tileCount;                           // number of unique tiles
};

// ---- HUD Element Export ----

struct AfnHudPieceExport {
    int spriteAssetIdx;
    int frame;
    int localX, localY;
    int size;   // 8, 16, 32, 64
    bool blackTint = false;
    int opacity = 16;
    int barSource = 0;   // 0=static, 1=Health, 2=Energy (PSV bar fill)
    int barAxis = 0;     // 0=horizontal, 1=vertical
    int barStart = 0, barEnd = 0;   // fill-edge travel (piece-local px along axis)
    int cycleSlot = -1;  // HUD value slot driving asset cycle (-1 = static)
    std::vector<int> cycleAssets;  // staged sprite asset per frame slot
    std::vector<int> cycleX, cycleY;  // per-frame-slot position offset
    // Crossfade: on the change to xfToScene, dissolve into element xfToElem's
    // piece xfToPiece in that scene (-1 = no crossfade).
    int xfToScene = -1, xfToElem = -1, xfToPiece = -1;
};

struct AfnHudStopExport {
    int localX, localY;
    int linkedElement;  // -1 = none
};

struct AfnHudTextRowExport {
    char text[32];
    int localX, localY;
    uint16_t colorRGB15;
    int font; // 0=normal 8x8, 1=small pixel 4x5, 2=5x7 debug
    int sourceSlot; // -1=static, 0-3=afn_hud_value[N]
    int pad; // minimum digits for counter mode
    int spacing; // extra spacing adjustment added to font advance
    int scale;   // 1=normal, 2=double size
};

struct AfnHudKeyframeExport {
    int frame;
    int offX, offY;
    int rot;           // degrees 0-359 (converted to brads at export)
    int scaleX, scaleY; // 8.8 fixed point (256 = 1.0x)
    int hidden = 0;     // 1 = piece hidden from this keyframe onward (blink)
    int opacity = 16;   // 0-16 opacity multiplier on the piece base opacity (16 = no change)
};

struct AfnHudAnimLayerItemExport {
    int type;   // 0=piece, 1=sprite, 2=text, 3=cursor
    int index;
    std::vector<AfnHudKeyframeExport> keyframes;  // this item's own animation track (PSV per-item tracks)
};

struct AfnHudAnimLayerExport {
    std::string name;
    int interp = 1;     // 0=constant, 1=linear, 2=bezier
    bool loop = false;
    int speed = 5;      // ticks-per-frame-advance for fps<=60 (60/fps), e.g. 5 = 12fps
    int step  = 1;      // frames advanced per tick for fps>60 (fps/60), e.g. 4 = 240fps; 1 otherwise
    int length = 60;    // total animation length in frames (from timeline range)
    std::vector<AfnHudAnimLayerItemExport> items;
    std::vector<AfnHudKeyframeExport> keyframes;  // LEGACY/representative track for NDS/GBA (= first item's track); PSV uses per-item items[].keyframes
};

struct AfnHudElementExport {
    int screenX, screenY;
    bool visible;
    int runtimeMode;     // 0=Both, 1=Mode4, 2=Mode0
    uint32_t mode0SceneMask = 0xFFFFFFFF; // which Mode 0 scenes the element shows in
    uint32_t mode4SceneMask = 0xFFFFFFFF; // which Mode 4 scenes the element shows in
    std::vector<AfnHudPieceExport> pieces;
    std::vector<AfnHudPieceExport> sprites;
    std::vector<AfnHudStopExport> stops;
    std::vector<AfnHudTextRowExport> textRows;
    int cursorAssetIdx;
    int cursorFrame;
    int cursorOffX, cursorOffY;
    int cursorSize = 16;   // cursor draw square (editor/GBA units); 0 = native frame size
    int cursorElementIdx = -1;   // use another element as the cursor (its pieces + keyframes); -1 = sprite
    int layerPieces, layerSprites, layerText, layerCursor;
    std::vector<AfnHudKeyframeExport> keyframes;
    bool animLoop = false;
    std::vector<AfnHudAnimLayerExport> animLayers;
};

// Sound export: a single PCM sample (8-bit signed, 16384 Hz)
struct AfnSoundSampleExport {
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
struct AfnSoundNoteExport {
    int tick;       // absolute tick position
    int channel;    // channel 0-15
    int note;       // MIDI note 0-127
    int velocity;   // 0-127
    int duration;   // ticks
    int sampleIdx;  // index into exported sample array
};

// Sound export: a complete sound instance (sequence + sample refs)
struct AfnSoundInstanceExport {
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
    std::vector<AfnSoundNoteExport> notes; // all notes merged
    std::vector<int> sampleIndices;        // which samples this instance uses
    bool isSfx = false;                    // true = one-shot SFX (uses afn_play_sfx)
    int sfxSampleIdx = -1;                 // exported sample index for SFX mode
    int fifoChannel = 0;                   // 0 = FIFO A, 1 = FIFO B
    bool persist = false;                  // keep playing across scene changes (don't stop/restart)
};

// Sky animation frame for export
struct AfnSkyFrameExport {
    const unsigned char* pixels = nullptr; // RGBA pixels
    int w = 0, h = 0;
};

// Package the current map into a .gba ROM.
// runtimeDir: path to gba_runtime/ directory
// outputPath: where to write the final .gba
// Returns true on success. errorMsg receives details on failure.
bool PackageGBA(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<AfnSpriteExport>& sprites,
                const std::vector<AfnSpriteAssetExport>& assets,
                const AfnCameraExport& camera,
                const std::vector<AfnMeshExport>& meshes,
                float orbitDist,
                const AfnScriptExport& script,
                const std::vector<AfnBlueprintExport>& blueprints,
                const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                const std::vector<AfnTmSceneExport>& tmScenes,
                const std::vector<AfnHudElementExport>& hudElements,
                const std::vector<AfnSoundSampleExport>& soundSamples,
                const std::vector<AfnSoundInstanceExport>& soundInstances,
                int startMode,
                std::string& errorMsg,
                const unsigned char* m7FloorPixels = nullptr,
                int m7FloorW = 0, int m7FloorH = 0,
                int m7FloorSize = 3,
                const std::vector<AfnSkyFrameExport>& skyFrames = {},
                int skyAnimSpeed = 8,
                bool deltaTime = false,
                bool showFps = false,
                bool smoothSky = false);

// A rigged (DSMA skinned) mesh, pre-converted to DS display-list / animation
// blobs (DsmaEmit). Part of the export IR: the shared PSV header generator
// accepts it (PSV ignores it and CPU-skins from AfnRigExport instead); the
// dormant NDS packager wrote these as static u32 arrays for DSMA_DrawModel.
struct AfnRiggedMeshExport
{
    std::string name;
    // One geometry group per material slot. Each group is its own DSM (only the
    // triangles tagged with that material) plus its own base-color texture; the
    // DS binds one texture per draw, so multi-material rigs draw group-by-group.
    // All groups share the same bones, so they share the clips' DSA below.
    struct MatGroup {
        std::vector<uint32_t> dsm;    // DSM (geometry display list) for this slot
        bool textured = false;
        int texW = 0, texH = 0;
        std::vector<uint8_t> texPixels;   // one byte per pixel, palette index 0..255
        uint32_t texPalette[256] = {};    // RGBA8 (256-colour, GL_RGB256)
        int wrapMode = 0;                 // UV addressing: 0=Clip(clamp) 1=Extend(tile) 2=Mirror
    };
    std::vector<MatGroup> groups;
    struct Clip {
        std::string name;
        int frames = 0;
        bool loop = true;             // false = play once, hold last frame
        std::vector<uint32_t> dsa;    // DSA (animation)
    };
    std::vector<Clip> clips;
    bool cameraLight = false;         // light follows the camera (headlamp)
    float lightX = 0.0f, lightY = 0.0f; // headlamp aim (pitch/yaw degrees)
    int  cullMode = 0;                // 0 = Back, 1 = Front, 2 = None
    bool useAlpha = false;            // palette index 0 = transparent (DS COLOR0_TRANSPARENT)
    int   collisionType = 0;          // 0 = None, 1 = Box (AABB proxy for player bump)
    float colCenter[3]  = {0,0,0};    // box center offset, rig-local units
    float colExtents[3] = {1,1,1};    // box half-extents, rig-local units

    // Convenience: a rig is renderable if it has at least one non-empty group.
    bool hasGeometry() const { return !groups.empty() && !groups[0].dsm.empty(); }
};

} // namespace Affinity

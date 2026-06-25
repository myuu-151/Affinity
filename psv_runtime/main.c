// Affinity PS Vita runtime — Mode 4 scene bring-up.
// Renders the exported level meshes with vitaGL. Ported from psp_runtime's
// scene.c, but simplified for the Vita's headroom: no per-frame frustum
// culling / bucketing and no texture swizzling (just upload GL textures and
// draw each mesh's full index buffer). Free-orbit camera so the level can be
// inspected on-device. Rig / sprites / sky / audio / collision come next.
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "psv_mapdata.h"   // defines afn_meshes / afn_sprites / camera start
#include "psv_rig.h"       // player rig data (skinned glTF), if AFN_HAS_PLAYER_RIG
#include "psv_player.h"    // AFN_PLAYER_COL_* (custom collision box, if authored)
#include "psv_sky.h"       // sky panorama texture (AFN_HAS_SKY)
#include "psv_sprites.h"   // billboard sprites (AFN_HAS_SPRITES)
#include "psv_rail.h"      // grind rail centerlines (AFN_HAS_RAIL_PATH)
#include "psv_hud.h"       // 2D HUD overlay elements/pieces/text (AFN_HAS_HUD)
#include "psv_nav.h"       // baked Recast navmesh blob + afn_npc_nav (AFN_HAS_NAVMESH)
#include "nav_bridge.h"    // Detour query wrapper (afn_nav_*)

#define SCR_W 960.0f
#define SCR_H 544.0f
#define DEG2RAD (3.14159265f / 180.0f)

static GLuint s_meshTex[256];   // one GL texture per mesh (0 = none)
#define AFN_MESH_MAX_MATS 8
static GLuint s_meshSlotTex[256][AFN_MESH_MAX_MATS];  // per-material-slot textures (multi-tex OBJ)

// ---------------------------------------------------------------------------
// Rigs: CPU rigid skinning (ported from psp_runtime/rig.c) + vitaGL draw.
// Multi-rig: the scene can carry several distinct models (player + each NPC /
// enemy type). afn_rigs[] holds one AfnRig descriptor per used model; each
// instance (the player, and afn_npc_inst[]) names a rig slot, position, facing,
// scale and clip, and is CPU-skinned into a shared buffer then drawn.
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_PLAYER_RIG
#ifndef AFN_RIG_YAW_OFFSET
#define AFN_RIG_YAW_OFFSET 0.0f
#endif
int afn_rig_clip = AFN_PLAYER_DEFAULT_CLIP;   // player clip selector (script-set; local for bring-up)
// Script-glue globals rigs_render reads (defined later in the script-glue block).
extern int afn_skel_anim_obj, afn_skel_anim_clip, afn_skel_anim_hold;   // Set/HoldSkelClip: NPC sprite idx + clip + hold-last-frame
extern unsigned char afn_sprite_visible[];          // SetVisible/DestroyObject per sprite
static AfnRigVertex s_skinned[AFN_RIG_MAX_VERTS];
static float  s_bonemat[AFN_RIG_MAX_BONES][12];   // 3x4 row-major per bone
static float  s_player_bone_world[AFN_RIG_MAX_BONES][3] = {{0}};  // player bones in WORLD space (bone-attach)
static GLuint s_rigTex[AFN_RIG_COUNT][AFN_RIG_MAX_MATS];
static float  s_pframe = 0.0f;                     // player anim frame
static int    s_pclip  = AFN_PLAYER_DEFAULT_CLIP;
static int    s_npcClip[AFN_NPC_COUNT + 1];        // per-NPC clip override (-1 = use default)
static unsigned char s_npcClipHold[AFN_NPC_COUNT + 1]; // HoldSkelClip: 1 = play clip once + freeze last frame
static int s_playerClipHold = 0, s_playerHoldClip = 0;  // HoldSkelClip on the player rig (die collapse)
static float  s_npcFrame[AFN_NPC_COUNT + 1];       // per-NPC anim frame (+1 avoids zero-size)
static float  s_npcY[AFN_NPC_COUNT + 1];           // gravity-driven dynamic Y (NDS s_npc_y parity)
static float  s_npcVY[AFN_NPC_COUNT + 1];          // vertical velocity
static unsigned char s_npcGround[AFN_NPC_COUNT + 1];
// Nav-driven dynamic X/Z/yaw (seeded from afn_npc_inst like s_npcY; the
// navmesh stepping in the NPC physics loop moves them, everything else —
// render, blocker, OnCollision — reads them instead of the const inst).
static float  s_npcX[AFN_NPC_COUNT + 1];
static float  s_npcZ[AFN_NPC_COUNT + 1];
static float  s_npcYaw[AFN_NPC_COUNT + 1];
static float  s_npcFloorN[AFN_NPC_COUNT + 1][3];   // smoothed floor normal (slope tilt, player parity)
#ifdef AFN_HAS_NAVMESH
#define NAV_MAX_WP 32
static float  s_npcPath[AFN_NPC_COUNT + 1][NAV_MAX_WP * 3];   // Detour waypoints
static int    s_npcPathLen[AFN_NPC_COUNT + 1];
static int    s_npcPathIdx[AFN_NPC_COUNT + 1];
static int    s_npcRepathT[AFN_NPC_COUNT + 1];                // frames until repath
static unsigned char s_npcNavMoving[AFN_NPC_COUNT + 1];       // moving this frame (move clip)
#endif
extern int afn_gravity, afn_terminal_vel;          // SetGravity/SetMaxFall (8.8 fixed, shared with player)

// ===================== HARDCODED: enemy NPC combat AI =====================
// Prototype enemy that mirrors the player controller (lock-on strafe + charge/tap
// projectiles + dodge) AI-driven. Gated to one NPC (editor sprite AFN_ENEMY_EIDX).
// Migrate to nodes later (the AI node-verbs are currently stubs). Tunable below.
#define AFN_ENEMY_EIDX        3        // exported editor sprite index of the NPC (afn_npc_inst col 7)
#define ENEMY_HP_MAX          100
// NOTE: this scene uses small world coords (~128-156, nav walk 0.35/frame), so all
// distances/speeds are in those units. Tune to taste.
#define ENEMY_DETECT_RANGE    60.0f    // start combat within this distance
#define ENEMY_LOSE_RANGE      95.0f    // outside this for LOSE_FRAMES -> back to roam
#define ENEMY_LOSE_FRAMES     150
#define ENEMY_PREF_DIST       22.0f    // preferred combat distance (strafe radius)
#define ENEMY_RANGE_K         0.06f    // how hard it corrects toward PREF_DIST
#define ENEMY_MOVE_SPEED      0.8f
#define ENEMY_STRAFE_LEG      90       // frames before re-rolling strafe direction
#define ENEMY_YAW_EASE        0.35f
#define ENEMY_ATK_CD          80       // frames between shots
#define ENEMY_CHARGE_PROB     0.40f    // chance a shot is a full charge vs a tap
#define ENEMY_TAP_WINDUP      12
#define ENEMY_TAP_DMG         4
#define ENEMY_TAP_SPEED       2.5f
#define ENEMY_TAP_SCALE       0.18f    // orb scale (relative to focus_gfx base, like the player)
#define ENEMY_CHARGE_WINDUP   55
#define ENEMY_CHG_DMG         20
#define ENEMY_CHG_SPEED       2.0f
#define ENEMY_CHG_SCALE       0.55f
#define ENEMY_CHG_HOMING      0.02f    // tiny so a sidestep still clears it (dodgeable)
#define ENEMY_FIRE_RECOVER    18
#define ENEMY_SHOT_LIFE       200
#define ENEMY_MUZZLE_FWD      3.0f      // chest-front muzzle (fallback only; bone is used)
#define ENEMY_MUZZLE_UP       12.0f
#define ENEMY_AIM_UP          12.0f     // aim at the player's chest (matches the player FB's +12)
#define ENEMY_HIT_R           6.0f
#define ENEMY_DODGE_TRIGGER_D 14.0f
#define ENEMY_DODGE_CHANCE    0.70f
#define ENEMY_DODGE_FRAMES    20
#define ENEMY_DODGE_RAMP      6
#define ENEMY_DODGE_SPEED     9.0f
#define ENEMY_DODGE_CD        45
#define ENEMY_BAR_HEIGHT      18.0f     // world units above the NPC origin
#define ENEMY_BAR_W           64.0f     // HUD pixels (960x544 space)
#define ENEMY_BAR_H           7.0f
enum { AI_ROAM = 0, AI_CHASE, AI_STRAFE, AI_CHARGE, AI_FIRE, AI_DODGE, AI_DEAD, AI_BLOCK };
static int   s_aiTimer = 0, s_aiLoseT = 0, s_aiInited = 0;
static int   s_aiStrafeDir = 1, s_aiStrafeLeg = 0, s_aiAtkCD = 0, s_aiDodgeCD = 0, s_aiChargeShot = 0;
static int   s_efbActive = 0, s_efbCharging = 0, s_efbDmg = 0, s_efbLife = 0;   // enemy projectile
static float s_efbX, s_efbY, s_efbZ, s_efbDirX = 0, s_efbDirZ = 1, s_efbScale = 0.05f, s_efbSpeed = 0, s_efbHoming = 0;
static int   s_eDodgeFrames = 0, s_eDodgeTotal = 0, s_eDodgeClip = 24;          // enemy dodge roll
static float s_eDodgeDX = 0, s_eDodgeDZ = 0;
// Enemy combat AI — FULLY NODE-ORCHESTRATED (enemy BP). The runtime keeps the
// heavy primitives (movement/strafe/dodge math, muzzle-bone aim, projectile,
// navmesh) as helper functions the nodes call (afn_ai_sense/roam/chase/strafe/
// dodge_begin/dodge_step/charge_begin/charge_step/fire_beam/fire_recover). These
// globals are the state + per-frame flags the node gates read, plus the tunables
// the Enemy AI node sets. Death KO cinematic = the enemy BP (Hold Skel Clip +
// Orbit Cam) — the old s_ko* statics are gone (s_npcClipHold drives the die hold).
int afn_ai_enabled = 0, afn_ai_state = AI_ROAM, afn_ai_slot = -1;
float afn_ai_dist = 0.0f;
int afn_ai_lose_ready = 0, afn_ai_dodge_ready = 0, afn_ai_can_fire = 0;
int afn_ai_charge_done = 0, afn_ai_dodge_done = 0, afn_ai_fire_done = 0, afn_ai_reached = 0;
// Tunables (Enemy AI node, defaults match the original #defines).
int afn_ai_detect_r = 60, afn_ai_lose_r = 95, afn_ai_pref_r = 22, afn_ai_atkcd = 80;
int afn_ai_chargeprob = 40, afn_ai_dodgeprob = 70, afn_ai_movespd_m = 800;
int afn_ai_dodge_trig = 24;   // dodge reaction range to an incoming blast (world px)
// Block: while blocking, an incoming hit is reduced to afn_block_pct % (20 = take
// 20%, block 80%) — for the player (afn_player_blocking, set by your Block node) and
// the enemy (afn_ai_blocking, set while in the AI_BLOCK state). afn_ai_block_prob is
// the AI's chance to block (vs dodge) an incoming blast.
int afn_player_blocking = 0, afn_ai_blocking = 0, afn_block_pct = 20;
int afn_ai_block_prob = 30, afn_ai_block_done = 0;
int afn_block_energy = 20;   // energy the player spends per blocked hit (Set Block node's Energy Cost pin)
// Enemy projectile launch speeds, in tenths of px/frame (Enemy AI node tunables;
// 20 = 2.0 full charge, 25 = 2.5 tap — like the player's Fire Charge Shot Speed).
int afn_ai_chg_speed_t = 20, afn_ai_tap_speed_t = 25;
static int s_eBlockFrames = 0;   // enemy block-stance countdown
// Player death cinematic (die clip + camera orbit on a loss) is now NODE-DRIVEN
// (ch_controller BP: Is Health Zero -> On Rise -> Hold Skel Clip + Orbit Cam On
// Obj). The old s_pkoActive/s_pkoTimer/s_pkoAngle0 statics were removed.
// Post-battle results menu is now NODE-DRIVEN (ch_controller BP). ~3s after a win
// (enemy HP 0 -> Is HP Zero) or a loss (player HP 0 -> Is Health Zero), a Delay
// fires Show HUD + Fade In Hud to crossfade the 'die' menu in (~1s). Up/Down pick
// Title (cursor stop 0) / Restart (stop 1); Cross (KEY_A) confirms, gated by Is
// Hud Visible. Sounds: beep on cursor move, select on confirm, victory on outcome.
// These indices are the exported HUD-element / sound-instance indices the BP wires.
#define AFN_TARGET_ELEM     0    // 'target' lock-on reticle element (hidden when the enemy dies)
#define AFN_RESULT_ELEM    10    // 'die' menu element (exported HUD index)
#define AFN_RESULT_CURSOR  11    // 'die_slct' cursor element
#define AFN_RESULT_DELAY  180    // ~3s @ 60fps before the menu appears
#define AFN_RESULT_FADE    60    // ~1s crossfade-in
#define AFN_SND_BEEP        8    // beep sound instance (cursor move)
#define AFN_SND_SELECT      2    // 'select' sound instance (confirm) — the menu-select blip
#define AFN_SND_VICTORY    10    // victory sound instance (win / battle over)
#define AFN_SND_SHOOT       5    // 'shoot' instance — decisive blast on clash resolve
#define AFN_SND_STRUGGLE   11    // 'struggle' instance — looping while the mash struggle runs
#define AFN_SND_CLASH      12    // 'clash' instance — one-shot when the beams meet
#define AFN_SND_WIN_CLASH  13    // 'win_clash' instance — played when the clash resolves (win or lose)
#define AFN_SND_MASH       14    // 'mashsound' instance — looping during the struggle, pitched by the balance
// The results-menu state machine + cursor-blink layer (s_resultState/Timer/
// CursorLayer) is gone — the BP graph drives it now. See results_tick() below,
// which is reduced to the per-frame facing upkeep nodes can't express.
// HARDCODED: beam clash (beam struggle). When the player AND the enemy BOTH
// release a FULL charge within AFN_CLASH_WINDOW frames of each other, both
// projectiles are suppressed and a ~5s side-view struggle begins: the 'clash'
// speed-line backdrop + 'mash' Cross prompt come up, the player taps Cross to
// push the meeting point toward the enemy while the AI auto-mashes back. Balance
// reaching the enemy side wins (enemy KO); reaching the player side — or being
// behind at the timeout — means the player takes the hit (lose menu).
#define AFN_CLASH_ELEM         12   // 'clash' speed-line backdrop element (exported HUD index)
#define AFN_MASH_ELEM          13   // 'mash' Cross-button prompt element
#define AFN_CLASH_PC_PRESSED   35   // global HUD piece index: button_pressed
#define AFN_CLASH_PC_UNPRESSED 36   // global HUD piece index: button_unpressed
#define AFN_CLASH_WINDOW       12   // both full-charge releases must land within this many frames
#define AFN_CLASH_FULL_FRAC  0.85f  // "fully charged" = >= this fraction of max charge
#define AFN_CLASH_PUSH       0.060f // balance gained per player Cross tap
// AI masher tuned to a HIGH-skill CPU that's trying to WIN: relentless, near-perfect
// cadence (~15 presses/sec) with almost no fumbles — it'll push you to your zone if
// you're not mashing flat-out. (Push/interval are Beam Clash node tunables: AI Push /
// AI Interval, set aggressive in the project.)
#define AFN_CLASH_AI_TAP     0.050f // balance removed per AI press (default; AI Push pin overrides)
#define AFN_CLASH_AI_MIN        6   // default; AI Interval pin overrides
#define AFN_CLASH_AI_JIT        1   // +0..0 -> dead-steady cadence
#define AFN_CLASH_AI_FUMBLE  0.01f  // ~1% chance of a brief slip (almost never)
#define AFN_CLASH_AI_FUMBLE_LEN 6   // short fumble pause
#define AFN_CLASH_MEET_R       18.0f// beams "meet" when their centers are within this (world units) — must be nearly touching
#define AFN_CLASH_AIR_FALLBACK   90 // ...or clash anyway after both beams have been airborne this long (~1.5s)
static int   s_clashAirT = 0, s_clashAiTap = 0;           // air-fallback timer + AI press countdown
static int   s_clashPressed = 0, s_clashPressTimer = 0;   // mash-press flash (button_pressed vs _unpressed)
static int   s_pbBeamFull = 0, s_ebBeamFull = 0;          // 1 while each side's IN-FLIGHT beam is a full charge
// Multi-blast pool: each FIRED Focus Blast becomes its own in-flight projectile so
// several can be on the field at once (a miss no longer locks out the next cast).
// Charging still uses the single orb (afn_fb_x/y/z/scale at the hands); on launch the
// charge state is copied into a free pool slot and the orb returns to idle, free to
// charge again immediately. afn_fb_active mirrors "any slot active" for the legacy
// sensors (AI dodge/block, clash, gates).
#define AFN_FB_POOL 6
typedef struct {
    int   active;
    float x, y, z, dirx, dirz, scale;
    int   life, dmg, tgt, full;
} AfnFbShot;
static AfnFbShot s_fbPool[AFN_FB_POOL] = {{0}};
// Beam clash — FULLY NODE-ORCHESTRATED now (ch_controller BP). The runtime keeps
// only the heavy primitives: the 2D struggle render (clash_render_2d), the beam
// geometry -> afn_clash_ready flag (clash_sense), and the RNG masher / pitch /
// button-flash step (afn_clash_ai_step). The node graph drives the flow: Is Clash
// Ready -> On Rise -> Clash Begin + Show HUD + Play Sound + Freeze; while the clash
// HUD is visible -> Suppress Beams + Clash AI Step + Cross->Clash Push; Is Clash
// Won/Lost -> hide + Set HP/Health. The Beam Clash node sets the tunables below.
// _m/_pct are fixed-point (push x1000, full charge x100) so int data pins carry the
// fractions. afn_clash_ready/balance are the live struggle state nodes read+drive.
int afn_clash_enabled = 0, afn_clash_ready = 0;
float afn_clash_balance = 0.5f;          // 0 = pushed into your zone (loss), 1 = enemy (win)
int afn_clash_full_pct = 85, afn_clash_push_m = 60, afn_clash_ai_push_m = 50;
int afn_clash_ai_min = 6, afn_clash_meet_r = 18;
// Clash resolve damage: the winner deals this % of their FULL attack to the loser
// (150 = 1.5x), instead of an instant KO — so a clash can't one-shot a full-HP
// fighter. Applied by the Clash Hit Enemy / Clash Hit Player nodes.
int afn_clash_dmg_pct = 150;
// Air fallback: clash anyway after BOTH beams have been airborne this many frames,
// even if they never meet (so it doesn't fizzle). 0 = OFF — only a real meet (within
// Meet Radius) triggers a clash. Tunable on the Beam Clash node.
int afn_clash_air_fb = 90;
// AI masher consistency (Beam Clash node tunables) — what sets the skill level:
// jitter = extra 0..N random frames on each press interval (0 = dead steady),
// fumble % = chance of a brief slip, fumble len = how long that slip lasts.
// Defaults = the high-skill profile.
int afn_clash_ai_jit = 1, afn_clash_fumble_pct = 1, afn_clash_fumble_len = 6;
// Punish: at random moments the masher breaks into a fast pressing BURST that
// hammers your balance — punish % = chance per press to start one, punish len =
// how many fast presses it lasts. 0 % = off. (Beam Clash node tunables.)
int afn_clash_punish_pct = 0, afn_clash_punish_len = 4;
static int s_clashPunishLeft = 0;   // fast-press presses remaining in the current burst
void afn_clash_suppress_beams(void);     // fwd (defined below) — node SuppressBeams / ClashBegin
void afn_clash_begin(void);              // fwd — node ClashBegin
void afn_clash_ai_step(void);            // fwd — node ClashAiStep
static float s_enemyBoneW[AFN_RIG_MAX_BONES][3] = {{0}};   // enemy NPC bones in WORLD (orb muzzle anchor)
static int   s_cacheEnemyBones = 0;                        // set true during the enemy's rig_draw pass
static int   s_drawingPlayer = 0;                          // set true ONLY during the player's own rig_draw
static int   s_efbDriveFrame = 0, s_efbDriveTick = 0;      // fbspin drive-element animation for the enemy orb
static int   s_fbInstCache = -2;                           // resolved focus_gfx sub-sprite instance (-2 = unresolved)
static unsigned s_aiRng = 0x1234567u;
static float ai_rand01(void) { s_aiRng ^= s_aiRng << 13; s_aiRng ^= s_aiRng >> 17; s_aiRng ^= s_aiRng << 5; return (s_aiRng & 0xFFFFFFu) / (float)0x1000000; }
static int   ai_chance(float p) { return ai_rand01() < p; }
// ==========================================================================

static void pose_to_mat(const float* p, float* m) {
    float px=p[0],py=p[1],pz=p[2], w=p[3],x=p[4],y=p[5],z=p[6];
    float n = w*w+x*x+y*y+z*z;
    if (n > 1e-8f) { n = 1.0f/sqrtf(n); w*=n; x*=n; y*=n; z*=n; }
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=1-2*(yy+zz); m[1]=2*(xy-wz);   m[2]=2*(xz+wy);   m[3]=px;
    m[4]=2*(xy+wz);   m[5]=1-2*(xx+zz); m[6]=2*(yz-wx);   m[7]=py;
    m[8]=2*(xz-wy);   m[9]=2*(yz+wx);   m[10]=1-2*(xx+yy);m[11]=pz;
}
static void build_bone_mats(const AfnRig* R, int clip, float frame) {
    const float* cd = R->clip[clip];
    int nf = R->clipframes[clip]; if (nf < 1) nf = 1;
    int f0 = (int)frame; if (f0 < 0) f0 = 0; if (f0 >= nf) f0 = nf - 1;
    int f1 = f0 + 1; if (f1 >= nf) f1 = R->cliploop[clip] ? 0 : nf - 1;
    float t = frame - (float)((int)frame);
    for (int b = 0; b < R->bones; b++) {
        const float* p0 = &cd[(f0 * R->bones + b) * 7];
        const float* p1 = &cd[(f1 * R->bones + b) * 7];
        float p[7];
        p[0]=p0[0]+(p1[0]-p0[0])*t; p[1]=p0[1]+(p1[1]-p0[1])*t; p[2]=p0[2]+(p1[2]-p0[2])*t;
        float d = p0[3]*p1[3]+p0[4]*p1[4]+p0[5]*p1[5]+p0[6]*p1[6];
        float s = (d < 0.0f) ? -1.0f : 1.0f;
        p[3]=p0[3]+(p1[3]*s-p0[3])*t; p[4]=p0[4]+(p1[4]*s-p0[4])*t;
        p[5]=p0[5]+(p1[5]*s-p0[5])*t; p[6]=p0[6]+(p1[6]*s-p0[6])*t;
        pose_to_mat(p, s_bonemat[b]);
    }
}
// Skin rig R into s_skinned (positions + normals + uv + white vertex color).
static void skin(const AfnRig* R) {
    for (int v = 0; v < R->verts; v++) {
        const float* m = s_bonemat[R->vbone[v]];
        float x=R->vpos[v*3+0], y=R->vpos[v*3+1], z=R->vpos[v*3+2];
        s_skinned[v].x = m[0]*x+m[1]*y+m[2]*z+m[3];
        s_skinned[v].y = m[4]*x+m[5]*y+m[6]*z+m[7];
        s_skinned[v].z = m[8]*x+m[9]*y+m[10]*z+m[11];
        float nx=R->vnorm[v*3+0], ny=R->vnorm[v*3+1], nz=R->vnorm[v*3+2];
        float wx=m[0]*nx+m[1]*ny+m[2]*nz, wy=m[4]*nx+m[5]*ny+m[6]*nz, wz=m[8]*nx+m[9]*ny+m[10]*nz;
        float nl=wx*wx+wy*wy+wz*wz; if (nl>1e-12f){nl=1.0f/sqrtf(nl);wx*=nl;wy*=nl;wz*=nl;}
        s_skinned[v].nx=wx; s_skinned[v].ny=wy; s_skinned[v].nz=wz;
        s_skinned[v].u = R->vuv[v*2+0]; s_skinned[v].v = R->vuv[v*2+1];
        s_skinned[v].color = 0xFFFFFFFF;
    }
}
static void rig_init(void) {
    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        s_npcClip[i] = -1; s_npcClipHold[i] = 0; // no SetSkelAnim/HoldSkelClip override yet
        s_npcY[i]  = afn_npc_inst[i][1];         // start at the authored Y; gravity settles it
        s_npcVY[i] = 0.0f; s_npcGround[i] = 0;
        s_npcX[i]  = afn_npc_inst[i][0];         // nav moves these; authored start
        s_npcZ[i]  = afn_npc_inst[i][2];
        s_npcYaw[i] = afn_npc_inst[i][3];
        s_npcFloorN[i][0] = 0.0f; s_npcFloorN[i][1] = 1.0f; s_npcFloorN[i][2] = 0.0f;
#ifdef AFN_HAS_NAVMESH
        s_npcPathLen[i] = 0; s_npcPathIdx[i] = 0; s_npcRepathT[i] = 0; s_npcNavMoving[i] = 0;
#endif
    }
    // HARDCODED: reset enemy combat AI on (re)init so a scene restart re-seeds HP/state.
    s_aiInited = 0; s_efbActive = 0; s_efbCharging = 0; s_eDodgeFrames = 0; afn_ai_state = AI_ROAM; afn_ai_slot = -1;
    for (int k = 0; k < AFN_FB_POOL; k++) s_fbPool[k].active = 0;   // clear any in-flight player blasts
    for (int r = 0; r < AFN_RIG_COUNT; r++) {
        const AfnRig* R = &afn_rigs[r];
        for (int g = 0; g < AFN_RIG_MAX_MATS; g++) {
            s_rigTex[r][g] = 0;
            if (g < R->mats && R->tex[g] && R->texw[g] > 0 && R->texh[g] > 0) {
                glGenTextures(1, &s_rigTex[r][g]);
                glBindTexture(GL_TEXTURE_2D, s_rigTex[r][g]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, R->texw[g], R->texh[g], 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, R->tex[g]);
            }
        }
    }
}
// Advance an animation frame for rig R's clip.
static float rig_advance(const AfnRig* R, int clip, float frame) {
    frame += 0.4f;
    int nf = R->clipframes[clip];
    if (nf > 1) {
        if (R->cliploop[clip]) { while (frame >= (float)nf) frame -= (float)nf; }
        else if (frame > (float)(nf-1)) frame = (float)(nf-1);
    } else frame = 0.0f;
    return frame;
}
// Draw one rig instance. s_skinned must already hold R's skinned pose. The
// headlamp is the same NDS-matched setup as before, per rig's baked direction.
static void rig_draw(const AfnRig* R, GLuint* texArr, const float* view,
                     float px, float py, float pz, float yawDeg, float instScale,
                     const float* upN) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    // Camera light is computed on the CPU (below, after the basis is built) and
    // baked into the vertex colors — the GPU draws UNLIT. vitaGL's fixed-
    // function GL_LIGHTING shader shades correctly through Vita3K but on real
    // SGX hardware the per-vertex lit values jitter/step while the camera
    // orbits (USSE precision in the generated lighting path). CPU fp32 N.L is
    // bit-identical on hardware and emulator, and we already touch every
    // vertex per frame in skin() anyway.
    glDisable(GL_LIGHTING);

    // Orient with a basis matrix: up = floor normal (slope tilt), forward = the
    // yaw heading projected onto the slope plane, right = up x fwd. Ported from
    // psp_runtime/rig.c. NPCs pass world-up so they stay vertical.
    float ux = upN ? upN[0] : 0.0f, uy = upN ? upN[1] : 1.0f, uz = upN ? upN[2] : 0.0f;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul > 1e-6f) { ux/=ul; uy/=ul; uz/=ul; }
    float yr = yawDeg * DEG2RAD + AFN_RIG_YAW_OFFSET + R->yawOff;   // + per-rig Model Yaw correction
    float ydx = sinf(yr), ydz = cosf(yr);
    float d = ydx*ux + ydz*uz;                              // yawDir . up (ydy = 0)
    float fx = ydx - ux*d, fy = -uy*d, fz = ydz - uz*d;     // project onto slope plane
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-6f) { fx/=fl; fy/=fl; fz/=fl; } else { fx=0; fy=0; fz=1; }
    float rgx = uy*fz - uz*fy, rgy = uz*fx - ux*fz, rgz = ux*fy - uy*fx;  // right = up x fwd
    float S = R->scale * instScale;
    float model[16] = {     // column-major: col0=right, col1=up, col2=forward
        rgx*S, rgy*S, rgz*S, 0,
        ux*S,  uy*S,  uz*S,  0,
        fx*S,  fy*S,  fz*S,  0,
        px,    py,    pz,    1
    };
    glLoadMatrixf(view);
    glMultMatrixf(model);

#ifdef AFN_HAS_SPR_BONE
    // Bone attach: cache each PLAYER bone's WORLD position (model * bone origin)
    // so attached sub-sprites can ride the live animated joint. s_bonemat is
    // overwritten per rig during this pass, so we snapshot only the player.
    if (s_drawingPlayer) {   // ONLY the player's own draw — NOT the same-rig enemy NPC
        for (int b = 0; b < R->bones; b++) {
            float bx = s_bonemat[b][3], by = s_bonemat[b][7], bz = s_bonemat[b][11];
            s_player_bone_world[b][0] = model[0]*bx + model[4]*by + model[8]*bz + model[12];
            s_player_bone_world[b][1] = model[1]*bx + model[5]*by + model[9]*bz + model[13];
            s_player_bone_world[b][2] = model[2]*bx + model[6]*by + model[10]*bz + model[14];
        }
    }
    // HARDCODED: also snapshot the enemy NPC's bones (for its projectile orb muzzle).
    if (s_cacheEnemyBones) {
        for (int b = 0; b < R->bones; b++) {
            float bx = s_bonemat[b][3], by = s_bonemat[b][7], bz = s_bonemat[b][11];
            s_enemyBoneW[b][0] = model[0]*bx + model[4]*by + model[8]*bz + model[12];
            s_enemyBoneW[b][1] = model[1]*bx + model[5]*by + model[9]*bz + model[13];
            s_enemyBoneW[b][2] = model[2]*bx + model[6]*by + model[10]*bz + model[14];
        }
    }
#endif

    if (R->camlight) {
        // CPU camera headlamp (NDS-matched: ambient 8/31 + diffuse 28/31 * N.L,
        // clamped). The baked light dir (R->ldx/y/z) is the EYE-space aim, so
        // "toward light" = -aim in eye space; carry it back through the view
        // rotation (transpose) to world, then through the instance basis
        // (transpose) to model space — where the skinned normals live — so one
        // transformed vector serves the whole rig and the per-vertex work is a
        // single dot product. Two-sided rigs light |N.L| (PSP's negated-light
        // back pass, folded into the abs).
        float lex = -R->ldx, ley = -R->ldy, lez = -R->ldz;
        float lwx = view[0]*lex + view[1]*ley + view[2]*lez;   // eye -> world
        float lwy = view[4]*lex + view[5]*ley + view[6]*lez;
        float lwz = view[8]*lex + view[9]*ley + view[10]*lez;
        float lmx = rgx*lwx + rgy*lwy + rgz*lwz;               // world -> model
        float lmy = ux*lwx  + uy*lwy  + uz*lwz;
        float lmz = fx*lwx  + fy*lwy  + fz*lwz;
        float ll = sqrtf(lmx*lmx + lmy*lmy + lmz*lmz);
        if (ll > 1e-6f) { lmx /= ll; lmy /= ll; lmz /= ll; }
        int twoSided = (R->cull == 2);
        for (int vi = 0; vi < R->verts; vi++) {
            float d = s_skinned[vi].nx*lmx + s_skinned[vi].ny*lmy + s_skinned[vi].nz*lmz;
            if (twoSided) { if (d < 0.0f) d = -d; }
            else if (d < 0.0f) d = 0.0f;
            float inten = (8.0f/31.0f) + (28.0f/31.0f)*d;
            if (inten > 1.0f) inten = 1.0f;
            unsigned int c = (unsigned int)(inten * 255.0f + 0.5f);
            s_skinned[vi].color = 0xFF000000u | (c << 16) | (c << 8) | c;
        }
    }

    glDisable(GL_BLEND);
    if (R->cull == 2) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(R->cull == 1 ? GL_CW : GL_CCW); }

    AfnRigVertex* v = s_skinned;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnRigVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnRigVertex), &v->color);
    glNormalPointer (   GL_FLOAT,         sizeof(AfnRigVertex), &v->nx);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnRigVertex), &v->x);
    for (int g = 0; g < R->mats; g++) {
        int ic = R->idxcount[g];
        if (ic <= 0) continue;
        if (texArr[g]) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texArr[g]); }
        else glDisable(GL_TEXTURE_2D);
        glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, R->idx[g]);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// Skin + draw the player and every NPC for this frame.
static void rigs_render(const float* view, float playerX, float playerY, float playerZ,
                        float playerYaw, const float* floorN) {
    const AfnRig* PR = &afn_rigs[AFN_PLAYER_RIG_SLOT];
    if (s_playerClipHold) {
        // HoldSkelClip on the player: force the held clip, ignore the normal clip selector.
        if (s_pclip != s_playerHoldClip) { s_pclip = s_playerHoldClip; s_pframe = 0.0f; }
    } else if (afn_rig_clip >= 0 && afn_rig_clip < PR->clips && afn_rig_clip != s_pclip) {
        s_pclip = afn_rig_clip; s_pframe = 0.0f;
    }
    // Hold (HoldSkelClip / KO): play the clip ONCE and hold the last frame (collapse).
    if (s_playerClipHold) {
        float last = (float)(PR->clipframes[s_pclip] - 1);
        s_pframe += 0.4f; if (s_pframe > last) s_pframe = last;
    } else {
        s_pframe = rig_advance(PR, s_pclip, s_pframe);
    }
    build_bone_mats(PR, s_pclip, s_pframe); skin(PR);
    s_drawingPlayer = 1;   // HARDCODED: snapshot ONLY the player's bones (enemy shares the rig)
    rig_draw(PR, s_rigTex[AFN_PLAYER_RIG_SLOT], view, playerX, playerY, playerZ, playerYaw, AFN_PLAYER_SCALE, floorN);
    s_drawingPlayer = 0;

    // SetSkelAnim: set the matching NPC's clip override (by editor sprite index).
    // Needs the 8-wide afn_npc_inst (editor index in col 7) from the new export.
#ifdef AFN_HAS_SPRITE_IDX
    if (afn_skel_anim_obj >= 0) {
#ifdef AFN_PLAYER_SPRITE_IDX
        if (afn_skel_anim_obj == AFN_PLAYER_SPRITE_IDX) {   // Hold/SetSkelAnim targeting the player rig
            s_playerHoldClip = afn_skel_anim_clip;
            s_playerClipHold = afn_skel_anim_hold ? 1 : 0;
            if (afn_skel_anim_hold) s_pframe = 0.0f;
        } else
#endif
        for (int i = 0; i < AFN_NPC_COUNT; i++)
            if ((int)afn_npc_inst[i][7] == afn_skel_anim_obj) {
                s_npcClip[i] = afn_skel_anim_clip;
                s_npcClipHold[i] = afn_skel_anim_hold ? 1 : 0;   // HoldSkelClip vs plain SetSkelAnim
                if (afn_skel_anim_hold) s_npcFrame[i] = 0.0f;    // restart the clip so the hold plays from the top
            }
        afn_skel_anim_obj = -1; afn_skel_anim_hold = 0;
    }
#endif
    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        const float* N = afn_npc_inst[i];
        int slot = (int)N[6]; if (slot < 0 || slot >= AFN_RIG_COUNT) continue;
        const AfnRig* R = &afn_rigs[slot];
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = (int)N[7];
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;   // hidden / destroyed
#endif
        int clip = s_npcClip[i] >= 0 ? s_npcClip[i] : (int)N[5];   // SetSkelAnim override
#ifdef AFN_HAS_NAVMESH
        // Nav move clip (walk cycle) while pathing; SetSkelAnim still wins.
        if (s_npcClip[i] < 0 && s_npcNavMoving[i] && (int)afn_npc_nav[i][4] >= 0)
            clip = (int)afn_npc_nav[i][4];
#endif
        if (clip < 0 || clip >= R->clips) clip = 0;
#ifdef AFN_HAS_SPRITE_IDX
        if (s_npcClipHold[i]) {   // HoldSkelClip (enemy BP KO cinematic) — hold the die collapse
            // Hold (HoldSkelClip / KO): the clip may be flagged Loop, but for a
            // collapse we play it once and freeze the final frame (ignore the wrap).
            float last = (float)(R->clipframes[clip] - 1);
            s_npcFrame[i] += 0.4f; if (s_npcFrame[i] > last) s_npcFrame[i] = last;
        } else
#endif
            s_npcFrame[i] = rig_advance(R, clip, s_npcFrame[i]);
        build_bone_mats(R, clip, s_npcFrame[i]); skin(R);
        // Draw at the nav-driven X/Z/yaw + gravity-settled Y (NPC physics loop),
        // tilted to the smoothed floor normal like the player (slope snap).
#ifdef AFN_HAS_SPRITE_IDX
        s_cacheEnemyBones = ((int)N[7] == AFN_ENEMY_EIDX);   // HARDCODED: snapshot enemy bones for its orb muzzle
#endif
        rig_draw(R, s_rigTex[slot], view, s_npcX[i], s_npcY[i], s_npcZ[i], s_npcYaw[i], N[4], s_npcFloorN[i]);
        s_cacheEnemyBones = 0;
    }
}
#endif // AFN_HAS_PLAYER_RIG

// Build a column-major 4x4 view matrix (gluLookAt equivalent — vitaGL has no GLU).
static void look_at(float m[16],
                    float ex, float ey, float ez,
                    float cx, float cy, float cz,
                    float ux, float uy, float uz)
{
    float fx = cx-ex, fy = cy-ey, fz = cz-ez;
    float fl = sqrtf(fx*fx+fy*fy+fz*fz); if (fl < 1e-6f) fl = 1.0f;
    fx/=fl; fy/=fl; fz/=fl;
    // s = f x up
    float sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    float sl = sqrtf(sx*sx+sy*sy+sz*sz); if (sl < 1e-6f) sl = 1.0f;
    sx/=sl; sy/=sl; sz/=sl;
    // u = s x f
    float ux2 = sy*fz - sz*fy, uy2 = sz*fx - sx*fz, uz2 = sx*fy - sy*fx;
    m[0]=sx;  m[4]=sy;  m[8]=sz;   m[12] = -(sx*ex + sy*ey + sz*ez);
    m[1]=ux2; m[5]=uy2; m[9]=uz2;  m[13] = -(ux2*ex + uy2*ey + uz2*ez);
    m[2]=-fx; m[6]=-fy; m[10]=-fz; m[14] =  (fx*ex + fy*ey + fz*ez);
    m[3]=0;   m[7]=0;   m[11]=0;   m[15] = 1.0f;
}

static void upload_textures(void)
{
    for (int mi = 0; mi < afn_mesh_count && mi < 256; mi++) {
        s_meshTex[mi] = 0;
        for (int g = 0; g < AFN_MESH_MAX_MATS; g++) s_meshSlotTex[mi][g] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        // Multi-material (OBJ usemtl): one GL texture per slot.
        if (m->mats > 0) {
            for (int g = 0; g < m->mats && g < AFN_MESH_MAX_MATS; g++) {
                if (!m->slotTex || !m->slotTex[g] || m->slotTexW[g] <= 0 || m->slotTexH[g] <= 0) continue;
                glGenTextures(1, &s_meshSlotTex[mi][g]);
                glBindTexture(GL_TEXTURE_2D, s_meshSlotTex[mi][g]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                // Level geometry commonly authors UVs outside [0,1] to tile a small
                // texture across a large surface — REPEAT so it tiles instead of
                // clamping (which stretches one edge texel and looks like a blur).
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m->slotTexW[g], m->slotTexH[g], 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, m->slotTex[g]);
            }
            continue;
        }
        if (m->textured && m->texPixels && m->texW > 0 && m->texH > 0) {
            glGenTextures(1, &s_meshTex[mi]);
            glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            // REPEAT so tiling level UVs (outside [0,1]) tile instead of clamp-stretch.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            // texPixels are 0xAABBGGRR == RGBA byte order in memory.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m->texW, m->texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, m->texPixels);
        }
    }
}

static void draw_mesh(int mi)
{
    const AfnMesh* m = &afn_meshes[mi];
    if (!m->visible || m->vertCount <= 0 || !m->verts || !m->indices) return;

    // Render level meshes TWO-SIDED. Single-sided meshes (a slope, a ramp) get
    // back-face culled when you orbit around to their back, so they vanish — the
    // disappearing-slope bug. The Vita GPU has the overdraw headroom and the depth
    // buffer sorts everything, so there's no reason to cull; draw both faces. The
    // exporter's cullMode (back/front/none) is intentionally ignored here.
    glDisable(GL_CULL_FACE);

    if (m->texHasAlpha) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);

    const AfnVertex* v = m->verts;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,        sizeof(AfnVertex), &v->x);

    if (m->mats > 0) {
        // Multi-material (OBJ usemtl): bind + draw each slot's triangle group.
        for (int g = 0; g < m->mats && g < AFN_MESH_MAX_MATS; g++) {
            int ic = m->slotIdxCount ? m->slotIdxCount[g] : 0;
            if (ic <= 0 || !m->slotIdx || !m->slotIdx[g]) continue;
            if (s_meshSlotTex[mi][g]) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, s_meshSlotTex[mi][g]); }
            else glDisable(GL_TEXTURE_2D);
            glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, m->slotIdx[g]);
        }
    } else {
        if (m->textured && s_meshTex[mi]) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_SHORT, m->indices);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// ---------------------------------------------------------------------------
// Sky panorama + sprite billboards (ported from psp_runtime/sky.c + billboard.c)
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_SKY
static GLuint s_skyTex = 0;
static void sky_init(void) {
    glGenTextures(1, &s_skyTex);
    glBindTexture(GL_TEXTURE_2D, s_skyTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, AFN_SKY_W, AFN_SKY_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, afn_sky_tex);
}
// Far textured quad in eye space (identity modelview) so it tracks the camera;
// U scrolls with yaw for the 360 wrap. No depth writes -> scene draws on top.
static void sky_render(float camAngle) {
    float u = camAngle / (2.0f * 3.14159265f);
    // Inside far=1500; X/Y keep the same angular coverage (ratios to D) as before.
    const float D = 1450.0f, X = 2088.0f, Y = 1218.0f;
    AfnVertex sky[4] = {
        { u,      0.0f, 0xFFFFFFFFu, -X,  Y, -D },
        { u+1.0f, 0.0f, 0xFFFFFFFFu,  X,  Y, -D },
        { u+1.0f, 1.0f, 0xFFFFFFFFu,  X, -Y, -D },
        { u,      1.0f, 0xFFFFFFFFu, -X, -Y, -D },
    };
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);  glDisable(GL_BLEND); glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);  glBindTexture(GL_TEXTURE_2D, s_skyTex);
    AfnVertex* v = sky;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
}
#endif // AFN_HAS_SKY

#ifdef AFN_HAS_SPRITES
static float  s_sprFrame[AFN_SPR_INST_COUNT];
#if defined(AFN_HAS_SPR_DRIVE_ELEM) && defined(AFN_HAS_HUD_ANIM)
static int    s_driveFrame[AFN_SPR_INST_COUNT] = {0};   // drive-through-element: per-instance anim frame
static int    s_driveTick[AFN_SPR_INST_COUNT]  = {0};   // and sub-frame tick
#endif
static GLuint s_sprTex[sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0])];
static void billboards_init(void) {
    int nf = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
    for (int f = 0; f < nf; f++) {
        glGenTextures(1, &s_sprTex[f]);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[f]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);   // crisp pixel-art sprites
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, afn_spr_frame_w[f], afn_spr_frame_h[f], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, afn_spr_frame_ptrs[f]);
    }
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) s_sprFrame[i] = (float)afn_spr_fstart[i];
}
// Focus Blast state (defined later in this file) — the charge/projectile machine
// fully drives the player's "effect" ball billboard's visibility/pos/scale.
extern int   afn_fb_inst, afn_fb_charging, afn_fb_active;
extern float afn_fb_x, afn_fb_y, afn_fb_z, afn_fb_scale;
// Camera-facing (Y-axis) textured quads in world space, drawn through the view.
// camEyeX/Z is the camera world position, used to pick an 8-facing direction for
// directional sprites (N,NE,E,SE,S,SW,W,NW = dir 0..7).
static void billboards_render(const float* view, float camAngle, float camEyeX, float camEyeZ) {
    float rx = cosf(camAngle), rz = -sinf(camAngle);   // camera right in XZ
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = afn_spr_editor_idx[i];
#ifdef AFN_HAS_PLAYER_RIG
        if (i == afn_fb_inst) {
            // Focus Blast owns this instance's visibility: the single orb is the
            // CHARGE ball only (drawn while charging). In-flight shots are the pool,
            // drawn separately below, so several can coexist.
            if (!afn_fb_charging) continue;
        } else
#endif
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
        int NF = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
        float sz = afn_spr_basesize[i] * afn_spr_scale[i] * 0.25f, hw = sz * 0.5f;
        float px = afn_spr_x[i], py = afn_spr_y[i], pz = afn_spr_z[i];
#if defined(AFN_HAS_SPR_PARENT) && defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
        // HARDCODED (pre-node): attached sprites re-anchor to their parent
        // NPC's LIVE position (nav/physics-moved s_npcX/Y/Z) + authored offset,
        // so a target marker attached to an NPC follows it around the scene.
        // The baked afn_spr_x/y/z only covers the authored spawn spot.
        int isAttached = (afn_spr_parent[i] >= 0);
        if (isAttached) {
            for (int n = 0; n < AFN_NPC_COUNT; n++)
                if ((int)afn_npc_inst[n][7] == afn_spr_parent[i]) {
                    px = s_npcX[n] + afn_spr_poff_x[i];
                    py = s_npcY[n] + afn_spr_poff_y[i];
                    pz = s_npcZ[n] + afn_spr_poff_z[i];
                    break;
                }
        }
#if defined(AFN_HAS_SPR_BONE) && defined(AFN_HAS_PLAYER_RIG)
        // Bone attach (player rig): ride the live animated joint cached during the
        // player's rig_draw. Offset X/Y/Z is added in world axes from the bone.
        if (afn_spr_bone[i] >= 0) {
            int bbn = afn_spr_bone[i];
            px = s_player_bone_world[bbn][0] + afn_spr_poff_x[i];
            py = s_player_bone_world[bbn][1] + afn_spr_poff_y[i];
            pz = s_player_bone_world[bbn][2] + afn_spr_poff_z[i];
        }
#endif
        // Focus Blast: the charge/projectile machine fully drives this instance's
        // world transform (the player isn't an NPC, so the re-anchor above never
        // tracks it) — place + scale the orb at the charge spot or flight pos.
        if (i == afn_fb_inst && afn_fb_charging) {
            sz = afn_spr_basesize[i] * afn_fb_scale * 0.25f; hw = sz * 0.5f;
            // Center the orb ON the spawn/bone point (the spherical quad below
            // centers at py + sz/2), so charging grows it symmetrically instead
            // of ballooning upward from the bottom edge.
            px = afn_fb_x; py = afn_fb_y - sz * 0.5f; pz = afn_fb_z;
            isAttached = 1;   // draw as a camera-facing (spherical) orb
        }
#endif
        int cf;
        if (afn_spr_directional[i]) {
            // 8-facing: pick the art for the direction the camera views from.
            // bearing 0 = camera at +Z (south) -> show S(4); +X(east) -> E(2).
            float bearing = atan2f(camEyeX - px, camEyeZ - pz);
            int n = (int)lroundf(bearing / (3.14159265f / 4.0f));
            int dir = (4 - n) & 7;
            cf = afn_spr_dir_base[i] + dir;
        } else {
            int lo = afn_spr_fstart[i], hi = afn_spr_fend[i]; if (hi < lo) hi = lo;
            if (hi > NF-1) hi = NF-1; if (lo > NF-1) lo = NF-1; if (lo < 0) lo = 0;  // never index past the table
            s_sprFrame[i] += afn_spr_fps[i] / 60.0f;
            if (s_sprFrame[i] >= (float)(hi+1)) s_sprFrame[i] = (float)lo;
            cf = (int)s_sprFrame[i]; if (cf < lo) cf = lo; if (cf > hi) cf = hi;
        }
        if (cf < 0) cf = 0; if (cf > NF-1) cf = NF-1;
        AfnVertex q[4];
#if defined(AFN_HAS_SPR_PARENT) && defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
        if (isAttached) {
            // TRUE camera-facing (spherical) for attached markers: span the
            // quad along the camera's right AND up axes (the view rotation's
            // rows are the camera basis in world space), centered on the
            // anchor. The Y-billboard below only yaws — it skews when the
            // camera pitches, which reads as warping on a marker ring.
            float Rwx = view[0], Rwy = view[4], Rwz = view[8];
            float Uwx = view[1], Uwy = view[5], Uwz = view[9];
            float cyq = py + sz * 0.5f;   // same vertical span as the Y-quad at level pitch
            float hh = sz * 0.5f;
#if defined(AFN_HAS_SPR_DRIVE_ELEM) && defined(AFN_HAS_HUD_ANIM)
            // Drive through element: run the linked HUD element's first anim layer
            // (rotation + scale keyframes) on this sub-sprite, keeping its own
            // graphic + exact position. Spin = roll the right/up basis; scale = grow hw/hh.
            if (afn_spr_drive_elem[i] >= 0) {
                int de = afn_spr_drive_elem[i];
                int dl = (de < (int)(sizeof(afn_hud_elem_first_layer)/sizeof(afn_hud_elem_first_layer[0])))
                       ? afn_hud_elem_first_layer[de] : -1;
                if (dl >= 0) {
                    const AfnHudLayer* L = &afn_hud_layer[dl];
                    int dspd = L->speed < 1 ? 1 : L->speed;
                    if (++s_driveTick[i] >= dspd) {
                        s_driveTick[i] = 0;
                        s_driveFrame[i]++;
                        if (L->length > 0 && s_driveFrame[i] >= L->length)
                            s_driveFrame[i] = L->loop ? 0 : (L->length - 1);
                    }
                    int ph = s_driveFrame[i], pI = -1, nI = -1;
                    for (int ki = 0; ki < L->kfCount; ki++) {
                        const AfnHudKf* k = &afn_hud_kf[L->kfStart + ki];
                        if (k->frame <= ph) pI = ki;
                        if (k->frame > ph && nI < 0) nI = ki;
                    }
                    if (pI < 0) pI = (nI < 0 ? 0 : nI);
                    if (nI < 0) nI = pI;
                    const AfnHudKf* A = &afn_hud_kf[L->kfStart + pI];
                    const AfnHudKf* B = &afn_hud_kf[L->kfStart + nI];
                    float frac = 0.0f;
                    if (A != B && L->interp != 0) {
                        float span = (float)(B->frame - A->frame);
                        frac = span > 0 ? (float)(ph - A->frame) / span : 0.0f;
                        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                        if (L->interp == 2) frac = frac * frac * (3.0f - 2.0f * frac);
                    }
                    float rotRad = (A->rot + (B->rot - A->rot) * frac) * 0.01745329f;
                    float dsc = ((A->sx + (B->sx - A->sx) * frac) + (A->sy + (B->sy - A->sy) * frac)) / 512.0f;
                    if (dsc > 0.0f) { hw *= dsc; hh *= dsc; }
                    if (rotRad != 0.0f) {
                        float ca = cosf(rotRad), sa = sinf(rotRad);
                        float nRx = Rwx*ca + Uwx*sa, nRy = Rwy*ca + Uwy*sa, nRz = Rwz*ca + Uwz*sa;
                        float nUx = Uwx*ca - Rwx*sa, nUy = Uwy*ca - Rwy*sa, nUz = Uwz*ca - Rwz*sa;
                        Rwx = nRx; Rwy = nRy; Rwz = nRz;
                        Uwx = nUx; Uwy = nUy; Uwz = nUz;
                    }
                }
            }
#endif
            AfnVertex t[4] = {
                { 0,0, 0xFFFFFFFFu, px - Rwx*hw + Uwx*hh, cyq - Rwy*hw + Uwy*hh, pz - Rwz*hw + Uwz*hh },
                { 1,0, 0xFFFFFFFFu, px + Rwx*hw + Uwx*hh, cyq + Rwy*hw + Uwy*hh, pz + Rwz*hw + Uwz*hh },
                { 1,1, 0xFFFFFFFFu, px + Rwx*hw - Uwx*hh, cyq + Rwy*hw - Uwy*hh, pz + Rwz*hw - Uwz*hh },
                { 0,1, 0xFFFFFFFFu, px - Rwx*hw - Uwx*hh, cyq - Rwy*hw - Uwy*hh, pz - Rwz*hw - Uwz*hh },
            };
            q[0]=t[0]; q[1]=t[1]; q[2]=t[2]; q[3]=t[3];
        } else
#endif
        {
            float lx = px - rx*hw, lz = pz - rz*hw, Rx = px + rx*hw, Rz = pz + rz*hw;
            float top = py + sz, bot = py;
            AfnVertex t[4] = {
                { 0,0, 0xFFFFFFFFu, lx, top, lz },
                { 1,0, 0xFFFFFFFFu, Rx, top, Rz },
                { 1,1, 0xFFFFFFFFu, Rx, bot, Rz },
                { 0,1, 0xFFFFFFFFu, lx, bot, lz },
            };
            q[0]=t[0]; q[1]=t[1]; q[2]=t[2]; q[3]=t[3];
        }
        glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &q->u);
        glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &q->color);
        glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &q->x);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[cf]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
#if defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
    // In-flight Focus Blast pool: draw each active projectile as a camera-facing
    // (spherical) orb reusing the player's focus_gfx sub-sprite texture, so multiple
    // blasts can be on the field at once independent of the single charge-orb instance.
    if (afn_fb_inst >= 0) {
        int oi = afn_fb_inst;
        int NF = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
        int of = afn_spr_fstart[oi]; if (of < 0) of = 0; if (of > NF-1) of = NF-1;
        float base = afn_spr_basesize[oi];
        float Rwx = view[0], Rwy = view[4], Rwz = view[8];
        float Uwx = view[1], Uwy = view[5], Uwz = view[9];
        for (int k = 0; k < AFN_FB_POOL; k++) {
            if (!s_fbPool[k].active) continue;
            float sz = base * s_fbPool[k].scale * 0.25f, hh = sz * 0.5f, hw = sz * 0.5f;
            float px = s_fbPool[k].x, cyq = s_fbPool[k].y, pz = s_fbPool[k].z;
            AfnVertex q[4] = {
                { 0,0, 0xFFFFFFFFu, px - Rwx*hw + Uwx*hh, cyq - Rwy*hw + Uwy*hh, pz - Rwz*hw + Uwz*hh },
                { 1,0, 0xFFFFFFFFu, px + Rwx*hw + Uwx*hh, cyq + Rwy*hw + Uwy*hh, pz + Rwz*hw + Uwz*hh },
                { 1,1, 0xFFFFFFFFu, px + Rwx*hw - Uwx*hh, cyq + Rwy*hw - Uwy*hh, pz + Rwz*hw - Uwz*hh },
                { 0,1, 0xFFFFFFFFu, px - Rwx*hw - Uwx*hh, cyq - Rwy*hw - Uwy*hh, pz - Rwz*hw - Uwz*hh },
            };
            glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &q->u);
            glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &q->color);
            glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &q->x);
            glBindTexture(GL_TEXTURE_2D, s_sprTex[of]);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        }
    }
#endif
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_CULL_FACE);
}
#endif // AFN_HAS_SPRITES

// ---------------------------------------------------------------------------
// Mesh collision (floor/wall) — float port of psp_runtime/collision.c. Faces are
// built once from the exported mesh geometry, transformed to world space, and
// bucketed into an XZ grid for cheap per-cell floor/wall queries.
// ---------------------------------------------------------------------------
#define COL_GN     16
#define COL_NCELL  (COL_GN * COL_GN)
typedef struct {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    float nx, ny, nz;
    int   flags;   // 1 floor, 2 ceiling, 4 wall
    int   sprite;  // editor sprite index of the source instance (-1 = none)
} ColFace;
int afn_floor_sprite = -1;   // editor sprite index of the floor the player stands on (grind)
static ColFace* s_faces = 0;
static int      s_faceCount = 0;
static int*     s_cellStart = 0;
static int*     s_cellCount = 0;
static int*     s_cellFaces = 0;
static float    s_minX, s_minZ, s_cellSize;
static unsigned* s_faceStamp = 0;
static unsigned  s_queryStamp = 0;

static int cell_x(float x) { int c=(int)((x-s_minX)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }
static int cell_z(float z) { int c=(int)((z-s_minZ)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }

static void collide_build(void) {
    int total = 0;
    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        total += afn_meshes[mi].indexCount / 3;
    }
    if (total <= 0) return;
    s_faces = (ColFace*)malloc(sizeof(ColFace) * total);
    if (!s_faces) return;
    float mnx=1e30f, mnz=1e30f, mxx=-1e30f, mxz=-1e30f;
    for (int si = 0; si < afn_sprite_count; si++) {
        const AfnSpriteInst* sp = &afn_sprites[si];
        int mi = sp->meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;
        float ry=sp->rotY*DEG2RAD, rx=sp->rotX*DEG2RAD, rz=sp->rotZ*DEG2RAD;
        float cY=cosf(ry),sY=sinf(ry),cX=cosf(rx),sX=sinf(rx),cZ=cosf(rz),sZ=sinf(rz);
        float scl=sp->scale;
        for (int t = 0; t + 3 <= m->indexCount; t += 3) {
            float wp[9];
            for (int k=0;k<3;k++){
                const AfnVertex* vv=&V[I[t+k]];
                float lx=vv->x*scl, ly=vv->y*scl, lz=vv->z*scl;
                float ax= lx*cY+lz*sY, az=-lx*sY+lz*cY, ay=ly;
                float ay2=ay*cX-az*sX, az2=ay*sX+az*cX;
                float ax2=ax*cZ-ay2*sZ, ay3=ax*sZ+ay2*cZ;
                wp[k*3+0]=sp->x+ax2; wp[k*3+1]=sp->y+ay3; wp[k*3+2]=sp->z+az2;
            }
            float e1x=wp[3]-wp[0],e1y=wp[4]-wp[1],e1z=wp[5]-wp[2];
            float e2x=wp[6]-wp[0],e2y=wp[7]-wp[1],e2z=wp[8]-wp[2];
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float len=sqrtf(nx*nx+ny*ny+nz*nz); if (len<1e-6f) continue;
            nx/=len; ny/=len; nz/=len;
            ColFace* F=&s_faces[s_faceCount++];
            F->ax=wp[0];F->ay=wp[1];F->az=wp[2]; F->bx=wp[3];F->by=wp[4];F->bz=wp[5]; F->cx=wp[6];F->cy=wp[7];F->cz=wp[8];
            F->flags=(ny>0.3f)?1:(ny<-0.7f)?2:4;
            F->nx=nx; F->ny=ny; F->nz=nz;
#ifdef AFN_HAS_SPRITE_IDX
            F->sprite = afn_mesh_inst_sprite[si];   // for afn_floor_sprite (grind rail detect)
#else
            F->sprite = -1;
#endif
            for (int k=0;k<3;k++){ float X=wp[k*3],Z=wp[k*3+2]; if(X<mnx)mnx=X; if(X>mxx)mxx=X; if(Z<mnz)mnz=Z; if(Z>mxz)mxz=Z; }
        }
    }
    if (s_faceCount<=0) return;
    s_minX=mnx; s_minZ=mnz;
    float span=(mxx-mnx)>(mxz-mnz)?(mxx-mnx):(mxz-mnz);
    s_cellSize=span/COL_GN; if (s_cellSize<1.0f) s_cellSize=1.0f;
    s_cellStart=(int*)calloc(COL_NCELL,sizeof(int));
    s_cellCount=(int*)calloc(COL_NCELL,sizeof(int));
    s_faceStamp=(unsigned*)calloc(s_faceCount,sizeof(unsigned));
    if (!s_cellStart||!s_cellCount||!s_faceStamp) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++) s_cellCount[gz*COL_GN+gx]++;
    }
    int te=0; for (int c=0;c<COL_NCELL;c++){ s_cellStart[c]=te; te+=s_cellCount[c]; s_cellCount[c]=0; }
    s_cellFaces=(int*)malloc(sizeof(int)*(te>0?te:1)); if (!s_cellFaces) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++){
            int c=gz*COL_GN+gx; s_cellFaces[s_cellStart[c]+s_cellCount[c]++]=i; }
    }
}

#ifdef AFN_HAS_PLAYER_COL
#define COL_RADIUS AFN_PLAYER_COL_RADIUS
#define COL_BOTTOM AFN_PLAYER_COL_BOTTOM
#define COL_TOP    AFN_PLAYER_COL_TOP
#else
#define COL_RADIUS 6.0f
#define COL_BOTTOM 0.0f
#define COL_TOP    24.0f
#endif
#define WALL_TOP_TOL 5.0f

static int collide_floor(float x, float z, float py, float* outY, float* outN) {
    if (!s_cellFaces) return 0;
    int c=cell_z(z)*COL_GN+cell_x(x);
    int start=s_cellStart[c], count=s_cellCount[c];
    float bestY=0; int found=0; const ColFace* bestF=0;
    for (int i=0;i<count;i++){
        const ColFace* F=&s_faces[s_cellFaces[start+i]];
        if (!(F->flags&1)) continue;
        float c0=(F->bx-F->ax)*(z-F->az)-(F->bz-F->az)*(x-F->ax);
        float c1=(F->cx-F->bx)*(z-F->bz)-(F->cz-F->bz)*(x-F->bx);
        float c2=(F->ax-F->cx)*(z-F->cz)-(F->az-F->cz)*(x-F->cx);
        if (!((c0>=0&&c1>=0&&c2>=0)||(c0<=0&&c1<=0&&c2<=0))) continue;
        float cs=c0+c1+c2;
        float fy=(cs==0)?(F->ay+F->by+F->cy)/3.0f:(c1*F->ay+c2*F->by+c0*F->cy)/cs;
        if (fy>py+COL_TOP) continue;
        if (!found||fy>bestY){ bestY=fy; found=1; bestF=F; }
    }
    *outY=bestY;
    if (outN){ if(bestF){outN[0]=bestF->nx;outN[1]=bestF->ny;outN[2]=bestF->nz;} else {outN[0]=0;outN[1]=1;outN[2]=0;} }
    afn_floor_sprite = bestF ? bestF->sprite : -1;   // which sprite's floor (grind rail detect)
    return found;
}

static void collide_walls(float* x, float* z, float py) {
    if (!s_cellFaces||!s_faceStamp) return;
    float ppx=*x, ppz=*z;
    int gx0=cell_x(ppx-COL_RADIUS), gx1=cell_x(ppx+COL_RADIUS);
    int gz0=cell_z(ppz-COL_RADIUS), gz1=cell_z(ppz+COL_RADIUS);
    unsigned stamp=++s_queryStamp;
    for (int gz=gz0;gz<=gz1;gz++) for (int gx=gx0;gx<=gx1;gx++){
        int c=gz*COL_GN+gx; int start=s_cellStart[c], count=s_cellCount[c];
        for (int i=0;i<count;i++){
            int fi=s_cellFaces[start+i];
            if (s_faceStamp[fi]==stamp) continue; s_faceStamp[fi]=stamp;
            const ColFace* F=&s_faces[fi];
            if (!(F->flags&4)) continue;
            float fMinY=fminf(F->ay,fminf(F->by,F->cy)), fMaxY=fmaxf(F->ay,fmaxf(F->by,F->cy));
            if (py+COL_TOP<fMinY || py+COL_BOTTOM>=fMaxY-WALL_TOP_TOL) continue;
            if (F->nx*F->nx+F->nz*F->nz<1e-8f) continue;
            float vx[3]={F->ax,F->bx,F->cx}, vz[3]={F->az,F->bz,F->cz};
            float bestPx=ppx,bestPz=ppz,bestD2=1e30f;
            for (int e=0;e<3;e++){
                float x0=vx[e],z0=vz[e], sx=vx[(e+1)%3]-x0, sz=vz[(e+1)%3]-z0;
                float L2=sx*sx+sz*sz;
                float t=(L2>1e-8f)?((ppx-x0)*sx+(ppz-z0)*sz)/L2:0.0f;
                if (t<0)t=0; else if (t>1)t=1;
                float Px=x0+sx*t, Pz=z0+sz*t;
                float dx=ppx-Px, dz=ppz-Pz, d2=dx*dx+dz*dz;
                if (d2<bestD2){ bestD2=d2; bestPx=Px; bestPz=Pz; }
            }
            if (bestD2>=COL_RADIUS*COL_RADIUS) continue;
            float d=sqrtf(bestD2), push=COL_RADIUS-d;
            if (d>1e-4f){ ppx+=(ppx-bestPx)/d*push; ppz+=(ppz-bestPz)/d*push; }
            else { float xl=sqrtf(F->nx*F->nx+F->nz*F->nz); ppx+=F->nx/xl*push; ppz+=F->nz/xl*push; }
        }
    }
    *x=ppx; *z=ppz;
}

// Nearest wall/ceiling hit along the segment p0 -> p1 (camera-vs-wall test).
// Returns the hit fraction t in (0,1], or -1 if clear. Floors (flag 1) are
// ignored so a low eye doesn't snap onto the ground. Möller–Trumbore over the
// faces in the cells the segment's XZ bbox covers.
static float collide_ray_walls(float x0,float y0,float z0, float x1,float y1,float z1) {
    if (!s_cellFaces || !s_faceStamp) return -1.0f;
    float dx=x1-x0, dy=y1-y0, dz=z1-z0;
    float bestT = 1.0f; int found = 0;
    unsigned stamp = ++s_queryStamp;
    int gx0=cell_x(fminf(x0,x1)), gx1=cell_x(fmaxf(x0,x1));
    int gz0=cell_z(fminf(z0,z1)), gz1=cell_z(fmaxf(z0,z1));
    for (int gz=gz0; gz<=gz1; gz++) for (int gx=gx0; gx<=gx1; gx++) {
        int c=gz*COL_GN+gx; int start=s_cellStart[c], count=s_cellCount[c];
        for (int i=0;i<count;i++){
            int fi=s_cellFaces[start+i];
            if (s_faceStamp[fi]==stamp) continue; s_faceStamp[fi]=stamp;
            const ColFace* F=&s_faces[fi];
            if (!(F->flags & (1|2|4))) continue;   // floors + walls + ceilings block the camera
            float e1x=F->bx-F->ax, e1y=F->by-F->ay, e1z=F->bz-F->az;
            float e2x=F->cx-F->ax, e2y=F->cy-F->ay, e2z=F->cz-F->az;
            float px=dy*e2z-dz*e2y, py=dz*e2x-dx*e2z, pz=dx*e2y-dy*e2x;
            float det=e1x*px+e1y*py+e1z*pz;
            if (det>-1e-6f && det<1e-6f) continue;
            float inv=1.0f/det;
            float tx=x0-F->ax, ty=y0-F->ay, tz=z0-F->az;
            float u=(tx*px+ty*py+tz*pz)*inv;
            if (u<0.0f||u>1.0f) continue;
            float qx=ty*e1z-tz*e1y, qy=tz*e1x-tx*e1z, qz=tx*e1y-ty*e1x;
            float v=(dx*qx+dy*qy+dz*qz)*inv;
            if (v<0.0f||u+v>1.0f) continue;
            float t=(e2x*qx+e2y*qy+e2z*qz)*inv;
            if (t>0.002f && t<bestT){ bestT=t; found=1; }
        }
    }
    return found ? bestT : -1.0f;
}

// ---------------------------------------------------------------------------
// Grind-rail path helpers — float port of fps3d.c afn_railpath_*. afn_rail_pts is
// world-px float, arc-length parameterized. (psv_rail.h, AFN_HAS_RAIL_PATH.)
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_RAIL_PATH
static float rail_seg_len(int start, int i) {
    float dx = afn_rail_pts[start+i][0]-afn_rail_pts[start+i-1][0];
    float dz = afn_rail_pts[start+i][2]-afn_rail_pts[start+i-1][2];
    return sqrtf(dx*dx+dz*dz);
}
static float rail_len(int rail) {
    int n = afn_rail_count[rail]; if (n < 2) return 0;
    int start = afn_rail_start[rail];
    float total = 0;
    for (int i = 1; i < n; i++) total += rail_seg_len(start, i);
    return total;
}
static void rail_sample(int rail, float s, float* ox, float* oy, float* oz, float* tdx, float* tdz) {
    int n = afn_rail_count[rail], start = afn_rail_start[rail];
    if (n < 2) { *ox=*oy=*oz=0; *tdx=1; *tdz=0; return; }
    if (s < 0) s = 0;
    float acc = 0;
    for (int i = 1; i < n; i++) {
        float ax=afn_rail_pts[start+i-1][0],ay=afn_rail_pts[start+i-1][1],az=afn_rail_pts[start+i-1][2];
        float bx=afn_rail_pts[start+i][0],by=afn_rail_pts[start+i][1],bz=afn_rail_pts[start+i][2];
        float seg = rail_seg_len(start, i); if (seg < 0.0001f) seg = 0.0001f;
        if (s <= acc+seg || i == n-1) {
            float t = (s - acc)/seg; if (t<0) t=0; if (t>1) t=1;
            *ox = ax+(bx-ax)*t; *oy = ay+(by-ay)*t; *oz = az+(bz-az)*t;
            float dx=bx-ax, dz=bz-az, l=sqrtf(dx*dx+dz*dz); if (l<0.0001f) l=0.0001f;
            *tdx = dx/l; *tdz = dz/l; return;
        }
        acc += seg;
    }
}
static float rail_nearest(int rail, float px, float pz, float* outD2) {
    int n = afn_rail_count[rail], start = afn_rail_start[rail];
    if (n < 2) { if(outD2)*outD2=0; return 0; }
    float bestD=1e30f, bestArc=0, acc=0;
    for (int i = 1; i < n; i++) {
        float ax=afn_rail_pts[start+i-1][0],az=afn_rail_pts[start+i-1][2];
        float bx=afn_rail_pts[start+i][0],bz=afn_rail_pts[start+i][2];
        float ex=bx-ax,ez=bz-az, el2=ex*ex+ez*ez; if(el2<0.0001f)el2=0.0001f;
        float t=((px-ax)*ex+(pz-az)*ez)/el2; if(t<0)t=0; if(t>1)t=1;
        float cx=ax+ex*t, cz=az+ez*t, dd=(px-cx)*(px-cx)+(pz-cz)*(pz-cz);
        float seg=rail_seg_len(start,i);
        if (dd<bestD){ bestD=dd; bestArc=acc+seg*t; }
        acc += seg;
    }
    if(outD2)*outD2=bestD;
    return bestArc;
}
#endif // AFN_HAS_RAIL_PATH

// ---------------------------------------------------------------------------
// Input + node-script variable layer. Behaviour is node-driven, per the engine
// convention: the emitted script (psv_script.h) sets these each frame and the
// movement/camera/rig below only READ them. input_update() supplies the raw
// analog/button defaults so the game is playable before/without a script
// (mirrors psp_runtime input.c + script_glue.c).
// ---------------------------------------------------------------------------
int afn_input_fwd = 0, afn_input_right = 0;   // camera-space move intent (256 = full)
int afn_stick_8way = 0;                       // 8-Way Stick node: snap move vector to 45 deg octants
int afn_move_speed = 0, afn_speed_prio = 0;   // node-set speed (0 = use walk default)
int afn_player_frozen = 0;
int afn_face_lock = 0;                        // MovePlayer "Consistent Facing": keep
                                              // rig yaw while moving (strafe/moonwalk)
int orbit_angle = 0;                          // camera yaw, brad (65536 = full circle)
int orbit_pitch = 0;                          // camera pitch, brad (node OrbitCamera Up/Down + right stick)

// Camera-delay ease rates (x/256 catch-up per frame), normally emitted into
// psv_mapdata.h by the exporter from the editor's camera settings. Fallbacks
// are the editor camera-panel defaults run through the same %*256/100
// conversion the exporters use, so a pre-easing export already feels like a
// default NDS export (re-export to pick up tuned values).
#ifndef AFN_WALK_EASE_IN
#define AFN_WALK_EASE_IN 48
#endif
#ifndef AFN_WALK_EASE_OUT
#define AFN_WALK_EASE_OUT 48
#endif
#ifndef AFN_SPRINT_EASE_IN
#define AFN_SPRINT_EASE_IN 15
#endif
#ifndef AFN_SPRINT_EASE_OUT
#define AFN_SPRINT_EASE_OUT 30
#endif
#ifndef AFN_ORBIT_EASE_IN
#define AFN_ORBIT_EASE_IN 64
#endif
#ifndef AFN_ORBIT_EASE_OUT
#define AFN_ORBIT_EASE_OUT 128
#endif
#ifndef AFN_JUMP_CAM_LAND
#define AFN_JUMP_CAM_LAND 94
#endif
#ifndef AFN_JUMP_CAM_AIR
#define AFN_JUMP_CAM_AIR 30
#endif

enum { KEY_A=1,KEY_B=2,KEY_X=4,KEY_Y=8,KEY_L=16,KEY_R=32,KEY_START=64,KEY_SELECT=128,
       KEY_UP=256,KEY_DOWN=512,KEY_LEFT=1024,KEY_RIGHT=2048,
       // Analog directions, reported separately from the d-pad so a node can bind
       // each stick independently (left stick != d-pad).
       KEY_LSTICK_UP=4096,    KEY_LSTICK_DOWN=8192,    KEY_LSTICK_LEFT=16384,   KEY_LSTICK_RIGHT=32768,
       KEY_RSTICK_UP=65536,   KEY_RSTICK_DOWN=131072,  KEY_RSTICK_LEFT=262144,  KEY_RSTICK_RIGHT=524288 };
unsigned afn_keys_held=0, afn_keys_pressed=0, afn_keys_released=0;
// Per-direction analog trip thresholds on the 0..127 axis range, ordered
// LU,LD,LL,LR,RU,RD,RL,RR. 48 = default; a Key node's Sensitivity slider
// retunes its direction via the emitted afn_emitted_script_init (the
// AFN_HAS_STICK_SENS define below compiles those assignments in).
int afn_stick_sens[8] = { 48, 48, 48, 48, 48, 48, 48, 48 };
#define AFN_HAS_STICK_SENS 1
// Per-direction analog deflection magnitude (same LU..RR order), 0..256.
// 0 below the trip threshold, then ramps 32..256 from the threshold to full
// deflection — a slight push moves slowly, a full push moves at node speed.
int afn_stick_mag[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
// Per-direction ramp scale, 0..256 (Key node Strength slider, emitted in
// script init like afn_stick_sens). 256 = full node speed at max deflection;
// 128 = the whole ramp halved, so a full push moves at half speed.
int afn_stick_strength[8] = { 256, 256, 256, 256, 256, 256, 256, 256 };
// Magnitude of the key gating the currently-running emitted chain. The
// emitted key dispatchers set it at chain entry (stick keys ->
// afn_stick_mag[dir], buttons -> 256); MovePlayer adds afn_key_mag to
// afn_input_fwd/right instead of a hardcoded 256.
int afn_key_mag = 256;
static int key_is_down(unsigned k){ return (afn_keys_held & k)!=0; }
static int key_hit(unsigned k){ return (afn_keys_pressed & k)!=0; }
static int key_released(unsigned k){ return (afn_keys_released & k)!=0; }

// Ramp one stick direction's deflection (v, 0..128 toward that direction)
// into a 0..256 magnitude: 0 until the trip threshold, then 32..256 linearly
// up to full deflection (thr maxes at 120 so the divisor never hits zero),
// finally scaled by the direction's Strength (256 = 100%).
static int stick_mag(int v, int thr, int strength) {
    if (v <= thr) return 0;
    int m = 32 + ((v - thr) * 224) / (127 - thr);
    if (m > 256) m = 256;
    return (m * strength) >> 8;
}

static void input_update(const SceCtrlData* pad) {
    unsigned b = pad->buttons, k = 0;
    if (b & SCE_CTRL_CROSS)    k|=KEY_A;
    if (b & SCE_CTRL_CIRCLE)   k|=KEY_B;
    if (b & SCE_CTRL_SQUARE)   k|=KEY_X;
    if (b & SCE_CTRL_TRIANGLE) k|=KEY_Y;
    if (b & SCE_CTRL_LTRIGGER) k|=KEY_L;
    if (b & SCE_CTRL_RTRIGGER) k|=KEY_R;
    if (b & SCE_CTRL_START)    k|=KEY_START;
    if (b & SCE_CTRL_SELECT)   k|=KEY_SELECT;
    if (b & SCE_CTRL_UP)       k|=KEY_UP;
    if (b & SCE_CTRL_DOWN)     k|=KEY_DOWN;
    if (b & SCE_CTRL_LEFT)     k|=KEY_LEFT;
    if (b & SCE_CTRL_RIGHT)    k|=KEY_RIGHT;
    // Sticks reported as their OWN keys (NOT folded into the d-pad), so a node can
    // bind the d-pad, left stick, and right stick independently. Each direction
    // trips at its afn_stick_sens threshold (default 48; Key node Sensitivity).
    int ax = (int)pad->lx - 128, ay = (int)pad->ly - 128;
    if (ay < -afn_stick_sens[0]) k|=KEY_LSTICK_UP;   if (ay > afn_stick_sens[1]) k|=KEY_LSTICK_DOWN;
    if (ax < -afn_stick_sens[2]) k|=KEY_LSTICK_LEFT; if (ax > afn_stick_sens[3]) k|=KEY_LSTICK_RIGHT;
    int rx = (int)pad->rx - 128, ry = (int)pad->ry - 128;
    if (ry < -afn_stick_sens[4]) k|=KEY_RSTICK_UP;   if (ry > afn_stick_sens[5]) k|=KEY_RSTICK_DOWN;
    if (rx < -afn_stick_sens[6]) k|=KEY_RSTICK_LEFT; if (rx > afn_stick_sens[7]) k|=KEY_RSTICK_RIGHT;
    // Analog ramp per direction: tripped keys also report HOW FAR the stick is
    // pushed (0..256) so a stick-bound MovePlayer scales from a creep to full
    // speed instead of snapping to max the moment the threshold trips.
    afn_stick_mag[0] = stick_mag(-ay, afn_stick_sens[0], afn_stick_strength[0]);
    afn_stick_mag[1] = stick_mag( ay, afn_stick_sens[1], afn_stick_strength[1]);
    afn_stick_mag[2] = stick_mag(-ax, afn_stick_sens[2], afn_stick_strength[2]);
    afn_stick_mag[3] = stick_mag( ax, afn_stick_sens[3], afn_stick_strength[3]);
    afn_stick_mag[4] = stick_mag(-ry, afn_stick_sens[4], afn_stick_strength[4]);
    afn_stick_mag[5] = stick_mag( ry, afn_stick_sens[5], afn_stick_strength[5]);
    afn_stick_mag[6] = stick_mag(-rx, afn_stick_sens[6], afn_stick_strength[6]);
    afn_stick_mag[7] = stick_mag( rx, afn_stick_sens[7], afn_stick_strength[7]);
    afn_keys_pressed  = k & ~afn_keys_held;
    afn_keys_released = ~k & afn_keys_held;
    afn_keys_held     = k;
}

// ---- Script-glue variables ------------------------------------------------
// The emitted node graph writes a LOT of variables that belong to subsystems
// the PSV runtime hasn't ported yet (HUD, grind rails, fades, sprite manip,
// Mode 0). They're defined here as inert globals — exactly like PSP's
// script_glue.c — so the generated code links; only the movement/camera/anim
// ones above are actually consumed. As each subsystem lands, its var stops
// being inert.
#ifndef NUM_SPRITES
#define NUM_SPRITES 64
#endif
int afn_play_anim=0, afn_anim_prio=0, afn_anim_speed=0, afn_auto_orbit_speed=0;
int afn_skel_anim_obj=-1, afn_skel_anim_clip=0, afn_skel_anim_hold=0, afn_sprite_anim_spr=-1, afn_sprite_anim_val=0;
int afn_collided_sprite=-1, afn_collided_tm_obj=-1, afn_bp_cur_spr_idx=-1, afn_bp_cur_tm_obj=-1;
int afn_current_mode=0, afn_current_scene=0, afn_scripts_stopped=0;
int afn_start_x=0, afn_start_y=0, afn_start_z=0, afn_text_color=0, afn_timer_visible=0;
int afn_wall_collided_sprite=-1, afn_fade_target=0, afn_fade_frames=0, afn_fade_counter=0, afn_fade_level=0;
int player_vy=0, player_ground_y=0, afn_player_vx_world=0, afn_player_vz_world=0;
int player_vy_now=0;   // ACTUAL current vertical velocity (8.8) exposed to nodes each frame;
                       // unlike player_vy (Jump-node impulse, zeroed after capture) this is
                       // reliable for Is Jumping (>0 rising) / Is Falling (<0 descending) on PSV.
int afn_land_timer=0;  // frames remaining in the post-touchdown "land" window (Is Landing gate)
int afn_velocity_falloff=0, afn_pending_boost_fwd=0;
int afn_grinding=0, afn_grinding_active=0, afn_grind_rail=-1, afn_grind_power=0;
int afn_grind_boost=0, afn_grind_bleed=0, afn_grind_catch_y=0, afn_grind_catch_x=0;
int afn_grind_vel=0, afn_grind_dx=0, afn_grind_dz=0;   // runtime grind state (IsGrinding reads vel)
// Gravity/terminal in 8.8 fixed (256 = 1 world unit/frame), so a SetGravity /
// SetMaxFall node (which writes value*256) drives them. Seeded to the PSV
// world defaults (0.8 / 30) rather than the weak editor-pixel defaults.
int afn_gravity=205, afn_terminal_vel=7680, afn_friction=0, afn_force_x=0, afn_force_z=0;
int afn_fall_force=0;   // Jump node Fall Force: extra downward accel (8.8) applied once past the apex
int afn_rise_float=0;   // Jump Rise Float: % of gravity removed WHILE RISING (anime float/hang), 0..90
int afn_fall_smooth=0;  // Jump Fall Smooth: frames to ease the Fall Force in past the apex (0 = snap)
int afn_cam_locked=0, afn_cam_speed=0, afn_tank_camera=0, afn_player_heading=0;
int afn_tank_move=1;   // 1 = movement axes follow the tank heading (classic tank);
                       // 0 = TurnPlayer(Camera Relative): heading only steers facing
int afn_player_height=0, afn_player_width=0, afn_bg_color=0, afn_anim_speed_dummy=0;
int afn_active_element=0, afn_elem_idx=0, afn_cursor_stop=0, afn_stop_count=0;
int afn_hud_value[128]={0}; // SetHudValue counter slots (text rows bind to these)
int afn_checkpoint_set=0, afn_checkpoint_x=0, afn_checkpoint_y=0, afn_checkpoint_z=0;
int afn_score=0, afn_shake_frames=0, afn_shake_intensity=0, afn_last_key=0;
int afn_frame_count=0, afn_dt_tick=0;
// Scene transition (ChangeScene/ReloadScene call this as a FUNCTION). PSV exports
// one scene, so a swap to a DIFFERENT scene index can only fade + reset to spawn
// (full multi-scene needs an all-scenes export); ReloadScene (same index) is a
// true respawn. Phase machine ticked in the main loop (it owns the player vars).
int afn_scene_phase = 0;       // 0 idle, 1 fading out (awaiting swap), 2 fading in
int afn_scene_pending = 0, afn_scene_pending_mode = 0;
// ChangeScene "Delay" pin: hold the current scene for afn_scene_delay frames,
// then start the transition to afn_scene_delay_scene/mode. The emitted script
// (psv_script.h) arms these when AFN_HAS_SCENE_DELAY is defined (see below).
#define AFN_HAS_SCENE_DELAY 1
int afn_scene_delay = 0, afn_scene_delay_scene = 0, afn_scene_delay_mode = 0, afn_scene_delay_frames = 15;
// --- Scene-change crossfade (per-piece "xfade" flag): snapshot the outgoing
// scene's flagged pieces and dissolve them out over the incoming scene, instead
// of fading to black. ---
typedef struct { int tex; float x, y, w, h, tox, toy, tow, toh; } AfnXfadePiece;
#define AFN_XFADE_MAX 12
AfnXfadePiece afn_xfade[AFN_XFADE_MAX];
int afn_xfade_count = 0, afn_xfade_counter = 0, afn_xfade_frames = 0;
static int afn_snapshot_xfade(void);   // defined below (needs HUD globals)

void afn_scene_start_transition(int scene, int mode, int frames) {
    afn_scene_pending = scene; afn_scene_pending_mode = mode;
    extern int afn_fade_target, afn_fade_frames, afn_fade_counter;
    int nf = frames > 0 ? frames : 15;
    afn_xfade_count = afn_snapshot_xfade();
    if (afn_xfade_count > 0) {
        // Crossfade: swap instantly (no black), dissolve the flagged pieces out.
        afn_xfade_frames = nf; afn_xfade_counter = nf;
        afn_fade_target = 0; afn_fade_frames = 1; afn_fade_counter = 1;
    } else {
        afn_fade_target = -16; afn_fade_frames = nf; afn_fade_counter = nf;
    }
    afn_scene_phase = 1;
}
// Player physics vars the emitted code reads/writes (NDS defines these in
// fps3d.c). Kept inert for now — the movement loop uses its own playerX/Y/Z;
// wiring teleport/IsMoving/Jump nodes to these is a follow-up.
int player_x=0, player_y=0, player_z=0, player_vy_unused=0;
int player_on_ground=1, player_moving=0;
unsigned int afn_flags=0, afn_rng=1;
// afn_hud_visible is indexed by HUD ELEMENT (not sprite); size to the larger of
// the two so ShowHUD/CursorUp (element indices) and any sprite-keyed use both fit.
#if defined(AFN_HAS_HUD) && (AFN_HUD_ELEM_COUNT > NUM_SPRITES)
  #define AFN_HUD_VIS_N AFN_HUD_ELEM_COUNT
#else
  #define AFN_HUD_VIS_N NUM_SPRITES
#endif
unsigned char afn_hud_visible[AFN_HUD_VIS_N]={0};
// Per-element opacity multiplier (0-256, 256 = opaque). hud_render scales each
// element's piece + cursor alpha by this, on top of the per-piece/keyframe alpha.
// Seeded to 256 at init; the results-menu controller ramps it for the fade-in.
int afn_hud_elem_fade[AFN_HUD_VIS_N];
// FadeInHudElement node: per-element alpha crossfade. _dur[e] = frames remaining
// (0 = not fading), _len[e] = total ramp length. The node arms an element;
// hud_render advances EVERY active fade each frame. Per-element (not one global)
// so several elements can crossfade in together (e.g. die menu + its cursor).
int afn_hud_fade_dur[AFN_HUD_VIS_N] = {0};
int afn_hud_fade_len[AFN_HUD_VIS_N] = {0};
// HARDCODED (pre-node): world-anchored HUD elements. A blueprint's ShowHUD
// records its owner sprite here (scene ShowHUD writes -1); hud_render then
// projects that NPC's attached-sprite world position to screen and draws the
// element's origin THERE instead of at the authored screenX/Y — so a target
// element pops over the NPC the blueprint is attached to.
#define AFN_HAS_HUD_ANCHOR 1
int afn_hud_anchor_sprite[AFN_HUD_VIS_N];   // -1 = screen-space (init at boot)
// Anchored-element size clamps (percent, from the Attached Sprite node's
// Max/Min Size sliders). The element scales with camera distance like a world
// object (orbit distance = 100%), clamped to [min,max]; 100/100 pins the
// scale to 1 = constant screen size (the original behavior).
unsigned char afn_hud_anchor_min[AFN_HUD_VIS_N], afn_hud_anchor_max[AFN_HUD_VIS_N];
// Proximity-scale distance range (world px): Min size at/under Near, Max at/over Far.
short afn_hud_anchor_near[AFN_HUD_VIS_N], afn_hud_anchor_far[AFN_HUD_VIS_N];
// Lock-on camera assist target (Lock Camera / Unlock Camera nodes): the
// editor sprite index the orbit eases toward facing, -1 = no lock. Consumed
// by the assist block in the main loop (before the orbit-delta detection).
#define AFN_HAS_CAM_LOCK 1
int afn_cam_lock_target = -1;
// OrbitCameraOnObject node: KO/death cinematic camera. The node latches angle0 +
// the target object (editor sprite idx, -1 = player) and sets active; the camera
// block orbits the object's box center until a scene swap clears it.
int   afn_cam_orbit_active = 0, afn_cam_orbit_obj = -1, afn_cam_orbit_timer = 0;
float afn_cam_orbit_angle0 = 0.0f;
// Tunables (OrbitCameraOnObject data pins): zoom % of cam distance, orbit rate in
// milli-rad/frame, pitch in centi-rad. Defaults = the original KO-cinematic feel.
int   afn_cam_orbit_zoom_pct = 45, afn_cam_orbit_rate_mr = 12, afn_cam_orbit_pitch_cr = 32;
// More tunables: zoom ease in per-mille/frame (60 = 0.06), and an added look-at
// height offset (world px) on top of the target's chest/box-center. The orbit
// timer is advanced by the Orbit Cam Step node now (not the runtime), so the
// whole orbit is node-driven: Orbit Cam On Obj (begin) + Orbit Cam Step (advance)
// + Stop Orbit Cam (end).
int   afn_cam_orbit_ease_pm = 60, afn_cam_orbit_lookh = 0;
// Floating HP bar — node-gated (Show HP Bar node). The runtime renders the bar
// only when a node raised it this frame; afn_hpbar_obj = which sprite, afn_hpbar_max
// = its full HP (for the fill fraction). Cleared each frame so it must be re-raised.
int   afn_hpbar_active = 0, afn_hpbar_obj = -1, afn_hpbar_max = 100;
// Over-the-shoulder framing params (Lock On node Zoom/Side sliders). Set at
// lock time; defaults = the tuned values. zoom = percent pull-back, side =
// world-px lateral shift.
int afn_lock_zoom = 18, afn_lock_side = 8;
int afn_lock_zoom_in = 0;   // 0 = zoom OUT (pull back), 1 = zoom IN (pull closer)
int afn_lock_height = 8;    // raises the locked pitch aim (world px): camera rides
                            // higher and looks down a bit instead of dead level
int afn_lock_no_lookdown = 0;  // 1 = clamp the locked pitch so it never looks DOWN
                               // (on approach); still eases UP when the target rises
// Lock Strafe node: while a lock target is active, movement is TARGET-
// relative (Up closes in, Down backpedals, L/R circle-strafe) and the rig
// always faces the target. Inert when no target is locked.
int afn_lock_strafe = 0;
// Dash To Target node (bullet-punch lunge): frames>0 = lunging toward
// afn_dash_target's live position at afn_dash_speed*0.08 px/frame, facing it.
int afn_dash_frames = 0, afn_dash_speed = 0, afn_dash_target = -1;
// Strafe Anim node: a Lock-On 8-way directional clip picker. The node only
// REGISTERS the 8 clips + a per-frame active flag during script_tick; the
// movement block picks the octant AFTER input is finalized (the node runs on
// OnUpdate, before MovePlayer has set afn_input_fwd/right, so it can't read
// the stick itself). Octant order: Fwd,Fwd-R,Right,Back-R,Back,Back-L,Left,Fwd-L.
int afn_strafe_anim = 0, afn_strafe_clip[8] = {0};
// Dodge node (one-button pure left/right side roll): frames>0 = rolling along
// the move-basis RIGHT axis (never fwd/back), at afn_dodge_speed*0.08 px/frame,
// holding afn_dodge_clip_l/_r on the rig. The node raises afn_dodge_trigger so
// the movement block picks the side once from the live stick's horizontal
// component (input is finalized there). Is Dodging / Is Not Dodging gate on
// frames>0.
int afn_dodge_frames = 0, afn_dodge_speed = 0, afn_dodge_trigger = 0, afn_dodge_clip_l = 0, afn_dodge_clip_r = 0;
int afn_dodge_clip_f = -1, afn_dodge_clip_b = -1;   // Dodge forward/back clips (-1 = unwired -> fall back to lateral L/R roll)
int afn_dodge_idle = -1;   // clip to snap back to when the roll ends (-1 = leave it; Strafe Anim/base layer reclaims)
int afn_dodge_ramp = 0;    // frames to ease the roll speed in from 0 (quadratic; 0 = instant/stiff)
int afn_dodge_falloff = 0; // frames to ease the roll speed back to 0 at the end (quadratic; 0 = hard stop)
int afn_dodge_cd = 0;      // spam-gate lockout countdown; the node only fires when <= 0, set to Cooldown on a dodge
// Focus Blast (Charge Shot / Is Charging / Fire Charge Shot nodes): hold-to-
// charge homing projectile. The CHARGE node asserts afn_fb_charge_req each held
// frame and the runtime grows the player's hidden effect sub-sprite (the ball);
// FIRE snapshots it into a projectile that homes the lock target (or flies
// forward) and damages it on contact. _req/_fire flags are cleared each frame
// before blueprint dispatch so they reflect only what the nodes set THIS frame.
int   afn_fb_charge_req = 0;          // ChargeShot: set =1 every held frame
int   afn_fb_fire_req   = 0;          // FireChargeShot: set =1 on the release frame
int   afn_fb_charging   = 0;          // Is Charging gate: 1 while charging (not yet fired)
float afn_fb_level      = 0.0f;       // accumulated charge frames (0..afn_fb_max)
int   afn_fb_max        = 180;        // frames to full charge (3s @60)
float afn_fb_min_scale  = 0.05f, afn_fb_max_scale = 0.70f;  // ball size range (instance scale)
int   afn_fb_parent     = -1;         // editor sprite idx the ball is attached to (player = self)
int   afn_fb_inst       = -1;         // resolved billboard instance idx of the ball (cached)
int   afn_fb_dmg_max    = 30;         // damage at full charge (scales down with level)
float afn_fb_speed      = 6.0f;       // projectile world px/frame (overwritten at fire by Fire Charge Shot's Speed pin)
int   afn_fb_hit_r      = 4;          // hit slop (world px) added to speed — smaller = must be closer to connect
float afn_fb_homing     = 0.12f;      // flight-direction ease toward the target/frame; bends in hard while the target is ahead, but Circle Home off means a dodge still clears it
int   afn_fb_circle     = 0;          // 0 = stop homing once the target is BEHIND (fly straight off, never circle back); 1 = keep homing (orbits the target)
int   afn_fb_tgt        = -1;         // captured homing target (editor idx, -1 = forward)
// Live projectile state.
int   afn_fb_active     = 0;          // 1 = a shot is in flight
float afn_fb_x, afn_fb_y, afn_fb_z;   // CHARGE orb world position (flight is the pool)
float afn_fb_scale      = 0.05f;      // current render scale of the ball
float afn_fb_dirx = 0, afn_fb_dirz = 1;   // forward direction (no-lock fallback)
int   afn_fb_cur_dmg    = 0;          // damage this shot will deal (locked at fire)
int   afn_fb_life       = 0;          // (legacy, unused) forward-shot lifetime countdown
int   afn_fb_life_homing = 240;       // Fire Charge Shot tunable: homing-shot lifetime (frames)
int   afn_fb_life_fwd    = 90;        // Fire Charge Shot tunable: forward-shot lifetime (frames)
int   afn_fb_fire_timer = 0;          // Is Firing gate: frames remaining in the post-launch
                                      // window (set on fire, counts down) so the launch anim
                                      // can hold instead of flashing for one frame
unsigned char afn_sprite_visible[NUM_SPRITES]={0};
unsigned char afn_sprite_flip[NUM_SPRITES]={0}, afn_collision_enabled[NUM_SPRITES]={0};
int afn_hp[NUM_SPRITES]={0}, afn_state_timer[NUM_SPRITES]={0};
int afn_energy=0, afn_energy_max=100;   // player energy resource (node-driven: Add/Spend/Set/SetMax Energy)
int afn_health=100, afn_health_max=100; // player health resource (node-driven: Damage/Heal/Set/SetMax Health)
int afn_stop_links[16]={0};

// HARDCODED: resolve the player's lone attached sub-sprite = the focus_gfx orb
// (cached). The enemy reuses its texture / bone / fbspin element to look identical.
static int resolve_focus_inst(void) {
    if (s_fbInstCache != -2) return s_fbInstCache;
    int found = -1;
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
    if (afn_fb_inst >= 0) found = afn_fb_inst;
    else for (int i = 0; i < AFN_SPR_INST_COUNT; i++)
        if (afn_spr_parent[i] >= 0) { found = i; break; }
#endif
    s_fbInstCache = found;
    return found;
}

// HARDCODED: enemy projectile muzzle = the focus_gfx bone on the NPC (true bone
// anchor) with the authored attach offset, rotated by the NPC's facing. Falls
// back to a chest-front point if the bone/sprite can't be resolved.
static void enemy_muzzle(int i, float* ox, float* oy, float* oz) {
#if defined(AFN_HAS_SPR_BONE) && defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
    int fb = resolve_focus_inst();
    int fbBone = (fb >= 0) ? afn_spr_bone[fb] : -1;
    if (fbBone >= 0 && fbBone < AFN_RIG_MAX_BONES) {
        float ax = afn_spr_poff_x[fb], ay = afn_spr_poff_y[fb], az = afn_spr_poff_z[fb];
        int slot = (int)afn_npc_inst[i][6];
        float yr = s_npcYaw[i]*DEG2RAD + AFN_RIG_YAW_OFFSET + afn_rigs[slot].yawOff;
        float yc = cosf(yr), ys = sinf(yr);   // fwd=(sin,cos), right=(cos,-sin) — matches rig_draw
        *ox = s_enemyBoneW[fbBone][0] + ax*yc + az*ys;
        *oy = s_enemyBoneW[fbBone][1] + ay;
        *oz = s_enemyBoneW[fbBone][2] - ax*ys + az*yc;
        return;
    }
#endif
    float ey = s_npcYaw[i]*DEG2RAD, efx = sinf(ey), efz = cosf(ey);
    *ox = s_npcX[i] + efx*ENEMY_MUZZLE_FWD; *oy = s_npcY[i] + ENEMY_MUZZLE_UP; *oz = s_npcZ[i] + efz*ENEMY_MUZZLE_FWD;
}

// HARDCODED: draw the enemy's projectile orb with the player's focus_gfx graphic +
// fbspin spin — a spherical camera-facing textured quad, mirroring billboards_render.
static void enemy_orb_render(const float* view) {
    if (!s_efbActive && !s_efbCharging) return;
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
    int fb = resolve_focus_inst();
    if (fb < 0) return;
    int NF = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
    int cf = afn_spr_fstart[fb]; if (cf < 0) cf = 0; if (cf > NF-1) cf = NF-1;
    float sz = afn_spr_basesize[fb] * s_efbScale * 0.25f;
    float hw = sz * 0.5f, hh = sz * 0.5f;
    float Rwx = view[0], Rwy = view[4], Rwz = view[8];   // camera right (world)
    float Uwx = view[1], Uwy = view[5], Uwz = view[9];   // camera up (world)
#if defined(AFN_HAS_SPR_DRIVE_ELEM) && defined(AFN_HAS_HUD_ANIM)
    // fbspin: run the linked element's first anim layer (rotation + scale) like the player orb.
    if (afn_spr_drive_elem[fb] >= 0) {
        int de = afn_spr_drive_elem[fb];
        int dl = (de < (int)(sizeof(afn_hud_elem_first_layer)/sizeof(afn_hud_elem_first_layer[0]))) ? afn_hud_elem_first_layer[de] : -1;
        if (dl >= 0) {
            const AfnHudLayer* L = &afn_hud_layer[dl];
            int dspd = L->speed < 1 ? 1 : L->speed;
            if (++s_efbDriveTick >= dspd) { s_efbDriveTick = 0; s_efbDriveFrame += (L->step > 0 ? L->step : 1); if (L->length > 0 && s_efbDriveFrame >= L->length) s_efbDriveFrame = L->loop ? (s_efbDriveFrame % L->length) : (L->length - 1); }
            int ph = s_efbDriveFrame, pI = -1, nI = -1;
            for (int ki = 0; ki < L->kfCount; ki++) { const AfnHudKf* k = &afn_hud_kf[L->kfStart + ki]; if (k->frame <= ph) pI = ki; if (k->frame > ph && nI < 0) nI = ki; }
            if (pI < 0) pI = (nI < 0 ? 0 : nI); if (nI < 0) nI = pI;
            const AfnHudKf* A = &afn_hud_kf[L->kfStart + pI]; const AfnHudKf* B = &afn_hud_kf[L->kfStart + nI];
            float frac = 0.0f;
            if (A != B && L->interp != 0) { float span=(float)(B->frame-A->frame); frac=span>0?(float)(ph-A->frame)/span:0.0f; if(frac<0)frac=0; if(frac>1)frac=1; if(L->interp==2) frac=frac*frac*(3.0f-2.0f*frac); }
            float rotRad = (A->rot + (B->rot - A->rot)*frac) * 0.01745329f;
            float dsc = ((A->sx + (B->sx - A->sx)*frac) + (A->sy + (B->sy - A->sy)*frac)) / 512.0f;
            if (dsc > 0.0f) { hw *= dsc; hh *= dsc; }
            if (rotRad != 0.0f) {
                float ca = cosf(rotRad), sa = sinf(rotRad);
                float nRx = Rwx*ca + Uwx*sa, nRy = Rwy*ca + Uwy*sa, nRz = Rwz*ca + Uwz*sa;
                float nUx = Uwx*ca - Rwx*sa, nUy = Uwy*ca - Rwy*sa, nUz = Uwz*ca - Rwz*sa;
                Rwx = nRx; Rwy = nRy; Rwz = nRz; Uwx = nUx; Uwy = nUy; Uwz = nUz;
            }
        }
    }
#endif
    float cx = s_efbX, cy = s_efbY, cz = s_efbZ;
    AfnVertex q[4] = {
        { 0,0, 0xFFFFFFFFu, cx - Rwx*hw + Uwx*hh, cy - Rwy*hw + Uwy*hh, cz - Rwz*hw + Uwz*hh },
        { 1,0, 0xFFFFFFFFu, cx + Rwx*hw + Uwx*hh, cy + Rwy*hw + Uwy*hh, cz + Rwz*hw + Uwz*hh },
        { 1,1, 0xFFFFFFFFu, cx + Rwx*hw - Uwx*hh, cy + Rwy*hw - Uwy*hh, cz + Rwz*hw - Uwz*hh },
        { 0,1, 0xFFFFFFFFu, cx - Rwx*hw - Uwx*hh, cy - Rwy*hw - Uwy*hh, cz - Rwz*hw - Uwz*hh },
    };
    AfnVertex* v = q;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D); glDisable(GL_CULL_FACE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer   (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer  (3, GL_FLOAT,        sizeof(AfnVertex), &v->x);
    glBindTexture(GL_TEXTURE_2D, s_sprTex[cf]);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_BLEND);
#endif
}

// Node primitive (Step Enemy Beam): advance the enemy's fired projectile. Driven
// once per frame by the node graph, independent of the enemy's life/visibility, so
// a shot always completes (hits the player or expires) even if the enemy dies
// mid-flight. Reads the player position from player_x/y/z (1-frame lag is fine).
void afn_enemy_beam_step(void) {
    if (!s_efbActive) return;
    float px = (float)player_x, py = (float)player_y, pz = (float)player_z;
    float tx = px, ty = py + (COL_BOTTOM + COL_TOP) * 0.5f, tz = pz;   // player collision-box center
    if (s_efbHoming > 0.0f) {
        float ax = tx - s_efbX, az = tz - s_efbZ, al = sqrtf(ax*ax + az*az); if (al < 1e-3f) al = 1.0f;
        s_efbDirX += (ax/al - s_efbDirX) * s_efbHoming;
        s_efbDirZ += (az/al - s_efbDirZ) * s_efbHoming;
        float nl = sqrtf(s_efbDirX*s_efbDirX + s_efbDirZ*s_efbDirZ); if (nl > 1e-3f) { s_efbDirX/=nl; s_efbDirZ/=nl; }
    }
    s_efbX += s_efbDirX * s_efbSpeed; s_efbZ += s_efbDirZ * s_efbSpeed; s_efbY += (ty - s_efbY) * 0.15f;
    float hx = s_efbX - px, hz = s_efbZ - pz;
    if (hx*hx + hz*hz <= ENEMY_HIT_R*ENEMY_HIT_R) {
        int dmg = s_efbDmg;
        if (afn_player_blocking) {                       // blocking a hit: reduce dmg + spend energy
            dmg = (s_efbDmg * afn_block_pct) / 100;
            afn_energy -= afn_block_energy; if (afn_energy < 0) afn_energy = 0;
        }
        afn_health -= dmg; if (afn_health < 0) afn_health = 0; s_efbActive = 0;
    }
    else if (--s_efbLife <= 0) s_efbActive = 0;
}

// Node primitive (Step Focus Blast): advance the player's in-flight Focus Blast —
// home toward the captured target (or fly forward), deal damage + despawn on
// contact, expire on lifetime. Charging/release stays in the main loop (already
// node-gated by Charge Shot / Fire Charge Shot). Driven once per frame by a node.
void afn_focus_blast_step(void) {
    int anyActive = 0, anyFull = 0;
    float sp = afn_fb_speed;
    for (int k = 0; k < AFN_FB_POOL; k++) {
        AfnFbShot* s = &s_fbPool[k];
        if (!s->active) continue;
        float tx = 0, ty = 0, tz = 0; int homing = 0;
        if (s->tgt >= 0) {
            for (int n = 0; n < AFN_NPC_COUNT; n++)
                if ((int)afn_npc_inst[n][7] == s->tgt) {
                    tx = s_npcX[n] + afn_npc_col[n][3];
                    ty = s_npcY[n] + afn_npc_col[n][4];
                    tz = s_npcZ[n] + afn_npc_col[n][5];
                    homing = 1; break;
                }
        }
        if (!homing) { tx = s->x + s->dirx*100.0f; tz = s->z + s->dirz*100.0f; ty = s->y; }
        float dx = tx - s->x, dy = ty - s->y, dz = tz - s->z;
        float dl = sqrtf(dx*dx + dy*dy + dz*dz);
        if (homing && dl <= sp + (float)afn_fb_hit_r) {
            if (s->tgt >= 0 && s->tgt < NUM_SPRITES) {
                int dmg = afn_ai_blocking ? (s->dmg * afn_block_pct) / 100 : s->dmg;   // enemy Block reduces it
                afn_hp[s->tgt] -= dmg;
                if (afn_hp[s->tgt] < 0) afn_hp[s->tgt] = 0;
            }
            s->active = 0;          // contact: deal damage + despawn
            continue;
        } else if (dl > 0.001f) {
            if (homing) {
                // Ease the flight direction toward the target by afn_fb_homing, then move
                // along it (gentle = a sidestep clears it). But unless Circle Home is on,
                // STOP homing once the target is behind the flight direction (dot < 0) — so
                // an overshot blast flies straight off instead of turning around to orbit.
                float ux = dx/dl, uz = dz/dl;
                float dot = s->dirx*ux + s->dirz*uz;
                if (afn_fb_circle || dot > 0.0f) {
                    s->dirx += (ux - s->dirx) * afn_fb_homing;
                    s->dirz += (uz - s->dirz) * afn_fb_homing;
                    float nl = sqrtf(s->dirx*s->dirx + s->dirz*s->dirz);
                    if (nl > 1e-3f) { s->dirx /= nl; s->dirz /= nl; }
                    s->y += dy/dl*sp;      // track the target's height only while homing
                }
                s->x += s->dirx*sp; s->z += s->dirz*sp;   // always move along the direction
            } else {
                s->x += dx/dl*sp; s->y += dy/dl*sp; s->z += dz/dl*sp;   // forward (unchanged)
            }
        }
        if (--s->life <= 0) { s->active = 0; continue; }   // lifetime / forward-shot range
        anyActive = 1;
        if (s->full) anyFull = 1;
    }
    afn_fb_active = anyActive;     // legacy mirror: any blast in flight
    if (!anyFull) s_pbBeamFull = 0;
}

// HARDCODED: enemy combat AI state machine. Drives NPC slot i only if it's the enemy.
// ---- Enemy AI runtime primitives (node-called; the enemy BP graph orchestrates
// the state machine). All keep the heavy math here; the node gates read the
// afn_ai_* flags these set, and the node SetAiState transitions afn_ai_state. ----

// Per-frame sense: cache the enemy slot, handle death, compute distance, face the
// player, tick cooldowns, and set the gate flags (lose/dodge/can-fire). Frozen
// during a beam clash (the clash owns the enemy then).
void afn_ai_sense(void) {
    if (afn_ai_slot < 0)
        for (int n = 0; n < AFN_NPC_COUNT; n++)
            if ((int)afn_npc_inst[n][7] == AFN_ENEMY_EIDX) { afn_ai_slot = n; break; }
    int i = afn_ai_slot; if (i < 0) return;
    int eidx = AFN_ENEMY_EIDX;
    if (!s_aiInited) { afn_hp[eidx] = ENEMY_HP_MAX; afn_sprite_visible[eidx] = 1; afn_ai_state = AI_ROAM; s_aiInited = 1; }
#if defined(AFN_HAS_HUD) && defined(AFN_HAS_SPRITE_IDX)
    if (afn_hud_visible[AFN_CLASH_ELEM]) return;   // beam clash owns the enemy — freeze the AI
#endif
    if (s_aiAtkCD > 0) s_aiAtkCD--;
    if (s_aiDodgeCD > 0) s_aiDodgeCD--;
    if (afn_ai_state != AI_CHARGE) s_efbCharging = 0;   // never leave a charge orb hanging

    if (afn_ai_state != AI_BLOCK) afn_ai_blocking = 0;   // only the BLOCK state keeps the guard up

    // Death: at 0 HP go DEAD (the enemy BP's Hold Skel Clip + Orbit Cam runs the
    // KO cinematic; the die-clip hold is s_npcClipHold). Cancel a charging orb but
    // let an already-fired ball finish its flight.
    if (afn_ai_state != AI_DEAD && afn_hp[eidx] <= 0) { afn_ai_state = AI_DEAD; s_efbCharging = 0; s_efbActive = 0; }
    if (afn_ai_state == AI_DEAD) {
        afn_hud_visible[AFN_TARGET_ELEM] = 0;          // drop reticle + lock on the corpse
        afn_cam_lock_target = -1; afn_lock_strafe = 0;
        afn_ai_dist = 0.0f; afn_ai_dodge_ready = afn_ai_can_fire = afn_ai_lose_ready = 0;
        return;
    }

    float px = (float)player_x, pz = (float)player_z;
    float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
    afn_ai_dist = sqrtf(dx*dx + dz*dz);

    // De-aggro: outside lose range for LOSE_FRAMES -> ready to return to roam.
    if (afn_ai_state != AI_ROAM && afn_ai_dist > (float)afn_ai_lose_r) {
        afn_ai_lose_ready = (++s_aiLoseT >= ENEMY_LOSE_FRAMES) ? 1 : 0;
    } else { s_aiLoseT = 0; afn_ai_lose_ready = 0; }

    // Face the player every combat frame (the lock-on, no reticle).
    if (afn_ai_state != AI_ROAM) {
        float toYaw = atan2f(dx, dz) * (180.0f / 3.14159265f);
        float diff = toYaw - s_npcYaw[i]; while (diff > 180.0f) diff -= 360.0f; while (diff < -180.0f) diff += 360.0f;
        s_npcYaw[i] += diff * ENEMY_YAW_EASE;
    }

    // (Dodge-ready is now a node: the Is Blast Incoming gate calls
    // afn_ai_blast_incoming() — see below. AI Sense no longer rolls it.)
    afn_ai_can_fire = (s_aiAtkCD == 0 && !s_efbActive && !s_efbCharging) ? 1 : 0;
}

// ROAM: nav drives motion; just pick walk/idle clip.
void afn_ai_roam(void) {
    int i = afn_ai_slot; if (i < 0) return;
#ifdef AFN_HAS_NAVMESH
    s_npcClip[i] = s_npcNavMoving[i] ? 30 : 26;
#endif
}

// CHASE: close toward the player; set afn_ai_reached at the strafe radius.
void afn_ai_chase(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_npcClip[i] = 30;
    float px = (float)player_x, pz = (float)player_z;
    float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
    float pl = afn_ai_dist > 1e-3f ? afn_ai_dist : 1.0f, sp = afn_ai_movespd_m * 0.001f;
    s_npcX[i] += dx/pl * sp; s_npcZ[i] += dz/pl * sp;
    collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]);
    afn_ai_reached = (afn_ai_dist <= (float)afn_ai_pref_r + 30.0f) ? 1 : 0;
}

// STRAFE: orbit the player at the preferred distance (8-direction clip).
void afn_ai_strafe(void) {
    int i = afn_ai_slot; if (i < 0) return;
    static const int eStrafe[8] = { 30,33,31,35,18,32,34,36 };
    if (--s_aiStrafeLeg <= 0) { s_aiStrafeDir = ai_chance(0.5f) ? 1 : -1; s_aiStrafeLeg = ENEMY_STRAFE_LEG; }
    float ey = s_npcYaw[i] * DEG2RAD;
    float efx = sinf(ey), efz = cosf(ey), erx = cosf(ey), erz = -sinf(ey);
    float radial = (afn_ai_dist - (float)afn_ai_pref_r) * ENEMY_RANGE_K; if (radial > 1.0f) radial = 1.0f; if (radial < -1.0f) radial = -1.0f;
    float wdx = erx * (float)s_aiStrafeDir + efx * radial;
    float wdz = erz * (float)s_aiStrafeDir + efz * radial;
    float inF = wdx*efx + wdz*efz, inR = wdx*erx + wdz*erz;
    int so = ((int)lroundf(atan2f(inR, inF) / 0.7853982f) + 8) & 7;
    s_npcClip[i] = eStrafe[so];
    float ml = sqrtf(wdx*wdx + wdz*wdz);
    if (ml > 1e-3f) { float sp = afn_ai_movespd_m * 0.001f; s_npcX[i] += wdx/ml*sp; s_npcZ[i] += wdz/ml*sp; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
}

// Node primitive (Is Blast Incoming gate): 1 when ANY of the player's in-flight
// Focus Blasts (homing OR forward) is within Dodge Range of the enemy and the dodge
// chance rolls true — and the enemy isn't already dodging / on cooldown. The chance
// is rolled here each frame (the gate is evaluated per frame), so pair it with On
// Rise to fire the dodge once.
// Nearest active pooled blast to (ex,ez); returns squared distance (or -1 if none).
static float afn_fb_nearest_sq(float ex, float ez) {
    float best = -1.0f;
    for (int k = 0; k < AFN_FB_POOL; k++) {
        if (!s_fbPool[k].active) continue;
        float dx = ex - s_fbPool[k].x, dz = ez - s_fbPool[k].z;
        float d2 = dx*dx + dz*dz;
        if (best < 0.0f || d2 < best) best = d2;
    }
    return best;
}
int afn_ai_blast_incoming(void) {
    int i = afn_ai_slot; if (i < 0) return 0;
    if (s_eDodgeFrames != 0 || s_eBlockFrames != 0 || s_aiDodgeCD != 0 || !afn_fb_active) return 0;
    float d2 = afn_fb_nearest_sq(s_npcX[i], s_npcZ[i]);
    float dtr = (float)afn_ai_dodge_trig;
    if (d2 < 0.0f || d2 > dtr*dtr) return 0;
    return ai_chance(afn_ai_dodgeprob * 0.01f) ? 1 : 0;
}

// Node primitive (Should Ai Block gate): like Is Blast Incoming but rolls the BLOCK
// chance — so the AI sometimes raises its guard instead of rolling away. Only when a
// blast is in dodge range and it isn't already dodging/blocking.
int afn_ai_blast_block(void) {
    int i = afn_ai_slot; if (i < 0) return 0;
    if (s_eDodgeFrames != 0 || s_eBlockFrames != 0 || afn_ai_state == AI_BLOCK || !afn_fb_active) return 0;
    float d2 = afn_fb_nearest_sq(s_npcX[i], s_npcZ[i]);
    float dtr = (float)afn_ai_dodge_trig;
    if (d2 < 0.0f || d2 > dtr*dtr) return 0;
    return ai_chance(afn_ai_block_prob * 0.01f) ? 1 : 0;
}

// BLOCK begin: raise the guard (Block clip 19, shared rig) for a short window.
void afn_ai_block_begin(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_eBlockFrames = 24; afn_ai_blocking = 1; afn_ai_block_done = 0;
    s_npcClip[i] = 19;
}

// BLOCK step: hold the guard + clip; set afn_ai_block_done when the window ends.
void afn_ai_block_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    afn_ai_blocking = 1; s_npcClip[i] = 19;
    afn_ai_block_done = (--s_eBlockFrames <= 0) ? 1 : 0;
}

// DODGE begin: set up the side-roll (called once on the dodge-ready edge). Mirrors
// the PLAYER's dodge: same committed side burst, same dodge clips (Dodge node's
// afn_dodge_clip_l/r, fallback to the rig's DodgeL/R 24/25) — but L/R swapped, since
// the enemy faces the player so its screen-left/right is mirrored.
void afn_ai_dodge_begin(void) {
    int i = afn_ai_slot; if (i < 0) return;
    float px = (float)player_x, pz = (float)player_z;
    float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
    float pl = afn_ai_dist > 1e-3f ? afn_ai_dist : 1.0f, fx = dx/pl, fz = dz/pl, rx = fz, rz = -fx;
    int side = ai_chance(0.5f) ? 1 : -1;
    s_eDodgeDX = rx * side; s_eDodgeDZ = rz * side;
    int clipL = afn_dodge_clip_l > 0 ? afn_dodge_clip_l : 24;   // DodgeL
    int clipR = afn_dodge_clip_r > 0 ? afn_dodge_clip_r : 25;   // DodgeR
    s_eDodgeClip = side > 0 ? clipL : clipR;                    // swapped vs the player
    s_eDodgeFrames = s_eDodgeTotal = ENEMY_DODGE_FRAMES; s_aiDodgeCD = ENEMY_DODGE_CD; afn_ai_dodge_done = 0;
}

// DODGE step: integrate the roll; set afn_ai_dodge_done when finished. Mirrors the
// player's roll — speed from the Dodge node (afn_dodge_speed, default 70 — the old
// enemy 9 barely moved), quadratic ease-IN over afn_dodge_ramp and ease-OUT over
// afn_dodge_falloff, with the same sub-stepped wall collision so the fast roll can't
// tunnel through a wall.
void afn_ai_dodge_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (s_eDodgeFrames > 0) {
        int total = s_eDodgeTotal, frames = s_eDodgeFrames;
        int ramp = afn_dodge_ramp  > 0 ? afn_dodge_ramp  : 6;
        int fall = afn_dodge_falloff > 0 ? afn_dodge_falloff : 6;
        if (ramp > total) ramp = total; if (fall > total) fall = total;
        float env = 1.0f;
        if (ramp > 0) { float t = (float)(total - frames + 1) / (float)ramp; if (t > 1.0f) t = 1.0f; if (t*t < env) env = t*t; }
        if (fall > 0) { float u = (float)frames / (float)fall; if (u > 1.0f) u = 1.0f; if (u*u < env) env = u*u; }
        int spd = afn_dodge_speed > 0 ? afn_dodge_speed : 70;
        float sp = spd * 0.08f * env;
        float ix = s_eDodgeDX * sp, iz = s_eDodgeDZ * sp;
        int sub = (int)(sp / 3.0f) + 1;
        for (int st = 0; st < sub; st++) { s_npcX[i] += ix/sub; s_npcZ[i] += iz/sub; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
        s_npcClip[i] = s_eDodgeClip;
        afn_ai_dodge_done = (--s_eDodgeFrames == 0) ? 1 : 0;
    } else afn_ai_dodge_done = 1;
}

// CHARGE begin: roll charge-vs-tap, start the wind-up (called once on entry).
void afn_ai_charge_begin(void) {
    s_aiChargeShot = ai_chance(afn_ai_chargeprob * 0.01f);
    s_aiTimer = s_aiChargeShot ? ENEMY_CHARGE_WINDUP : ENEMY_TAP_WINDUP;
    s_efbScale = ENEMY_TAP_SCALE; s_efbCharging = 1; afn_ai_charge_done = 0;
}

// CHARGE step: hold the charge pose, grow the orb at the muzzle; set
// afn_ai_charge_done when the wind-up elapses (the FireBeam node then launches).
void afn_ai_charge_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_npcClip[i] = 1; s_efbCharging = 1;
    float mx, my, mz; enemy_muzzle(i, &mx, &my, &mz);
    s_efbX = mx; s_efbY = my; s_efbZ = mz;
    float tgt = s_aiChargeShot ? ENEMY_CHG_SCALE : ENEMY_TAP_SCALE;
    s_efbScale += (tgt - s_efbScale) * 0.2f;
    afn_ai_charge_done = (--s_aiTimer <= 0) ? 1 : 0;
}

// FIRE: launch the projectile from the muzzle toward the player; start recovery.
void afn_ai_fire_beam(void) {
    int i = afn_ai_slot; if (i < 0) return;
    float px = (float)player_x, pz = (float)player_z;
    float mx, my, mz; enemy_muzzle(i, &mx, &my, &mz);
    float ddx = px - mx, ddz = pz - mz, dl = sqrtf(ddx*ddx + ddz*ddz); if (dl < 1e-3f) dl = 1.0f;
    s_efbDirX = ddx/dl; s_efbDirZ = ddz/dl;
    s_efbX = mx + s_efbDirX*8.0f; s_efbZ = mz + s_efbDirZ*8.0f; s_efbY = my;
    if (s_aiChargeShot) { s_efbDmg = ENEMY_CHG_DMG; s_efbSpeed = afn_ai_chg_speed_t / 10.0f; s_efbScale = ENEMY_CHG_SCALE; s_efbHoming = ENEMY_CHG_HOMING; s_ebBeamFull = 1; }
    else                { s_efbDmg = ENEMY_TAP_DMG; s_efbSpeed = afn_ai_tap_speed_t / 10.0f; s_efbScale = ENEMY_TAP_SCALE; s_efbHoming = 0.0f; s_ebBeamFull = 0; }
    s_efbActive = 1; s_efbCharging = 0; s_efbLife = ENEMY_SHOT_LIFE;
    s_aiAtkCD = afn_ai_atkcd; s_aiTimer = ENEMY_FIRE_RECOVER;
}

// FIRE recover: hold the launch clip; set afn_ai_fire_done when recovery elapses.
void afn_ai_fire_recover(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_npcClip[i] = 16;
    afn_ai_fire_done = (--s_aiTimer <= 0) ? 1 : 0;
}
// HUD anim layer state — node-driven (PlayHudAnim resets+activates,
// StopHudAnim deactivates, SetHudAnimSpeed overrides). hud_render advances
// active layers and evaluates the keyframe transform at the layer's frame.
#if defined(AFN_HAS_HUD_ANIM) && (AFN_HUD_LAYER_COUNT > 8)
  #define AFN_HUD_LAY_N AFN_HUD_LAYER_COUNT
#else
  #define AFN_HUD_LAY_N 8
#endif
int afn_hud_layer_frame[AFN_HUD_LAY_N]={0}, afn_hud_layer_tick[AFN_HUD_LAY_N]={0};
unsigned char afn_hud_layer_active[AFN_HUD_LAY_N]={0}, afn_hud_layer_speed_override[AFN_HUD_LAY_N]={0};
int tm_fol_active=0, tm_fol_obj=-1, tm_fol_dist=0, tm_fol_facing=0, tm_fol_moving=0, tm_fol_speed=0;
int tm_player_tx=0, tm_player_ty=0;
// Audio entry points — defined in audio.c (sceAudio software mixer + sequencer).
void afn_play_sound(int id, int link);
void afn_play_sfx(int smpIdx, int gain, int fifo);
void afn_stop_sound(void);
void afn_stop_all(void);
void afn_stop_sfx_sample(int smpIdx);   // stop every voice playing this sample (for stopping a looped SFX)
void afn_stop_music(void);              // stop only the persistent battle music, leaving one-shot SFX ringing
void afn_set_sfx_pitch(int smpIdx, int pitchPct);   // repitch a playing (looped) SFX: 100 = natural, 200 = +oct
int  afn_sfx_active(int smpIdx);                    // 1 if any voice is currently playing this sample
void afn_stop_sfx_sample(int smpIdx);
void afn_audio_init(void);
void afn_audio_tick(void);

// Camera eye position (XZ), updated each frame by the render block — the
// true origin for the FOV test (the camera sits BEHIND the player, so an
// edge target subtends a smaller angle from the eye than from the player).
static float g_camEyeX = 0.0f, g_camEyeZ = 0.0f;
static int   g_camEyeValid = 0;

// Is In View gate (PSV): true if sprite `spr` is within the camera's
// horizontal FOV right now — i.e. on-screen. Measures the angle from the
// CAMERA EYE (last frame's) against the camera forward (orbit_angle). Defined
// before the script include so the emitted `if (afn_in_view(target))` works.
static int afn_in_view(int spr) {
#ifdef AFN_HAS_PLAYER_RIG
    if (spr < 0) return 0;
    for (int n = 0; n < AFN_NPC_COUNT; n++)
        if ((int)afn_npc_inst[n][7] == spr) {
            float ox = g_camEyeValid ? g_camEyeX : (float)player_x;
            float oz = g_camEyeValid ? g_camEyeZ : (float)player_z;
            float dx = s_npcX[n] - ox, dz = s_npcZ[n] - oz;
            if (dx*dx + dz*dz < 1.0f) return 1;       // on top of it = visible
            float cur = orbit_angle * (6.2831853f / 65536.0f);
            float dd = atan2f(dx, dz) - cur;
            while (dd >  3.14159265f) dd -= 6.2831853f;
            while (dd < -3.14159265f) dd += 6.2831853f;
            return (dd > -1.0f && dd < 1.0f);         // ~57 deg half horizontal FOV (edge margin)
        }
#endif
    (void)spr; return 0;
}

// The emitted node graph. Included AFTER the variables/keys above so its static
// functions can reference them (single translation unit). Defines AFN_HAS_SCRIPT
// when the scene actually has nodes; otherwise this is an inert stub and the raw
// stick input above drives movement.
#include "psv_script.h"

#ifdef AFN_HAS_SCRIPT
static void script_start(void) { afn_emitted_script_init(); afn_emitted_script_start(); afn_bp_dispatch_start(); }
static void script_tick(void) {
    // Node-driven inputs are recomputed from scratch each frame by the graph.
    afn_input_fwd = 0; afn_input_right = 0; afn_speed_prio = 0; afn_move_speed = 0;
    afn_key_mag = 256;   // chains re-set it on entry; full-on outside key chains
    afn_face_lock = 0;   // MovePlayer(Consistent Facing) re-sets it while held
    afn_strafe_anim = 0; // Strafe Anim re-registers it each frame it runs
    afn_fb_charge_req = 0; afn_fb_fire_req = 0;  // Focus Blast: nodes re-assert each frame
    // Dispatch order: RELEASED before HELD, so ongoing held state wins ties
    // within a tick. Rolling the stick from Up to Right releases Up while
    // Right is still held — with released-last, a Released->idle chain
    // stomped the Held->walk clip for one frame and reset the animation.
    // Pressed stays last so one-shots (attack on press) override held walks.
    afn_emitted_script_update();
    afn_emitted_script_key_released();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_released();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
}
static int script_present(void) { return 1; }
#else
static void script_start(void) {}
static void script_tick(void)  {}
static int  script_present(void){ return 0; }
#endif

// ---------------------------------------------------------------------------
// HUD overlay (Phase 8). 2D screen-space pieces/text/cursor authored at the
// PSV-native 960x544 (the Elements tab uses this canvas when the build target is
// PSV), drawn 1:1 in an ortho pass. Pieces/cursor are RGBA frame textures from
// psv_hud.h; text uses an embedded 8x8 font (uppercase + digits + symbols).
//
// The code compiles inert (no AFN_HAS_HUD) and activates when a scene with HUD
// data is re-exported. Author coords are 960x544; glOrthof maps that 1:1.
#ifdef AFN_HAS_HUD
static GLuint s_hudTex[AFN_HUD_FRAME_COUNT];
static GLuint s_hudFontTex;

// Compact 8x8 font: space, 0-9, A-Z, then : / % - . x + !  (lowercase folds up).
// 1 byte per row, bit7 = leftmost pixel. Index 0 is a blank cell.
#define HUD_FONT_GLYPHS 45
static const unsigned char s_hudFont[HUD_FONT_GLYPHS * 8] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // ' '
    0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00, // 0
    0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00, // 1
    0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00, // 2
    0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00, // 3
    0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00, // 4
    0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00, // 5
    0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00, // 6
    0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00, // 7
    0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00, // 8
    0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00, // 9
    0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00, // A
    0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00, // B
    0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00, // C
    0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00, // D
    0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00, // E
    0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00, // F
    0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00, // G
    0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00, // H
    0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00, // I
    0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00, // J
    0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00, // K
    0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00, // L
    0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00, // M
    0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00, // N
    0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00, // O
    0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00, // P
    0x3C,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00, // Q
    0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00, // R
    0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00, // S
    0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00, // T
    0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00, // U
    0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00, // V
    0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00, // W
    0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00, // X
    0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00, // Y
    0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00, // Z
    0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00, // :
    0x06,0x0C,0x0C,0x18,0x30,0x30,0x60,0x00, // /
    0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00, // %
    0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00, // -
    0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00, // .
    0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00, // x
    0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00, // +
    0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00, // !
};
// Map an ASCII char to a glyph slot (0 = blank for anything unknown).
static int hud_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    switch (c) { case ':': return 37; case '/': return 38; case '%': return 39;
                 case '-': return 40; case '.': return 41; case 'x': return 11+('X'-'A');
                 case '+': return 43; case '!': return 44; }
    return 0;
}

static void hud_init(void) {
    for (int i = 0; i < AFN_HUD_FRAME_COUNT; i++) {
        glGenTextures(1, &s_hudTex[i]);
        glBindTexture(GL_TEXTURE_2D, s_hudTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, afn_hud_frame_w[i], afn_hud_frame_h[i], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, afn_hud_frames[i]);
    }
    // Build a font atlas: HUD_FONT_GLYPHS columns of 8x8, white where the bit is
    // set (alpha 0 elsewhere) so glColor modulation tints the text.
    static unsigned int atlas[HUD_FONT_GLYPHS * 8 * 8];
    int aw = HUD_FONT_GLYPHS * 8;
    for (int g = 0; g < HUD_FONT_GLYPHS; g++)
        for (int row = 0; row < 8; row++) {
            unsigned char bits = s_hudFont[g * 8 + row];
            for (int col = 0; col < 8; col++) {
                int on = (bits >> (7 - col)) & 1;
                atlas[row * aw + g * 8 + col] = on ? 0xFFFFFFFFu : 0u;
            }
        }
    glGenTextures(1, &s_hudFontTex);
    glBindTexture(GL_TEXTURE_2D, s_hudFontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
}

// Draw a textured quad rotated rotDeg (clockwise) about its center, scaled to
// w x h. rot 0 / scale 1 matches hud_quad exactly. Used by keyframe anim.
static void hud_quad_xf(GLuint tex, float cxq, float cyq, float w, float h,
                        float rotDeg, unsigned int col) {
    float rr = rotDeg * (3.14159265f / 180.0f);
    float c = cosf(rr), s = sinf(rr);
    float hx = w * 0.5f, hy = h * 0.5f;
    float px[4] = { -hx,  hx,  hx, -hx };
    float py[4] = { -hy, -hy,  hy,  hy };
    AfnVertex q[4];
    const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        q[i].u = us[i]; q[i].v = vs[i]; q[i].color = col;
        q[i].x = cxq + px[i] * c - py[i] * s;
        q[i].y = cyq + px[i] * s + py[i] * c;
        q[i].z = 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,         sizeof(AfnVertex), &q[0].u);
    glColorPointer   (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &q[0].color);
    glVertexPointer  (3, GL_FLOAT,         sizeof(AfnVertex), &q[0].x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void hud_quad(GLuint tex, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, unsigned int col) {
    AfnVertex q[4] = {
        { u0, v0, col, x0, y0, 0 }, { u1, v0, col, x1, y0, 0 },
        { u1, v1, col, x1, y1, 0 }, { u0, v1, col, x0, y1, 0 },
    };
    AfnVertex* v = q;
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,         sizeof(AfnVertex), &v->u);
    glColorPointer   (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer  (3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// HARDCODED: untextured solid-color quad in HUD coords (for the enemy HP bar).
static void hud_solid_quad(float x0, float y0, float x1, float y1, unsigned int col) {
    AfnVertex q[4] = {
        { 0,0, col, x0, y0, 0 }, { 0,0, col, x1, y0, 0 },
        { 0,0, col, x1, y1, 0 }, { 0,0, col, x0, y1, 0 },
    };
    AfnVertex* v = q;
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer(3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_TEXTURE_2D);
}

// Draw a string at (x,y) in HUD coords; returns the pen x after the string.
static float hud_text(const char* s, float x, float y, int scale, unsigned int col) {
    const float aw = (float)(HUD_FONT_GLYPHS * 8);
    float gw = 8.0f * scale, adv = 6.0f * scale;   // glyphs are 8 wide, 6 px advance
    for (const char* p = s; *p; p++) {
        int g = hud_glyph(*p);
        if (g != 0) {   // skip drawing blanks, still advance
            float u0 = (g * 8) / aw, u1 = (g * 8 + 8) / aw;
            hud_quad(s_hudFontTex, x, y, x + gw, y + gw, u0, 0.0f, u1, 1.0f, col);
        }
        x += adv;
    }
    return x;
}

// Scene view matrix snapshot (set in the main loop after look_at) so HUD
// anchoring can project world points into the 240x160 HUD space with the
// scene's exact frustum (near 1.0, vfov ~75, Vita aspect).
static float s_hudSceneView[16];
static int   s_hudSceneViewValid = 0;
static float s_hudCamDist = 60.0f;   // camera orbit distance (scale reference)

// Project a world position into HUD coords. Returns 0 when behind the camera.
// outDepth (optional) receives the eye-space distance along the view axis.
static int hud_project(float wx, float wy, float wz, float* outX, float* outY, float* outDepth) {
    if (!s_hudSceneViewValid) return 0;
    const float* v = s_hudSceneView;
    float exq = v[0]*wx + v[4]*wy + v[8]*wz  + v[12];
    float eyq = v[1]*wx + v[5]*wy + v[9]*wz  + v[13];
    float ezq = v[2]*wx + v[6]*wy + v[10]*wz + v[14];
    if (ezq > -0.1f) return 0;                       // behind / at the camera
    const float nearp = 1.0f, top = nearp * 0.767f;  // matches the scene glFrustum
    const float right = top * (SCR_W / SCR_H);
    float ndcX = (exq * (nearp / right)) / -ezq;
    float ndcY = (eyq * (nearp / top))  / -ezq;
    *outX = (ndcX * 0.5f + 0.5f) * 960.0f;
    *outY = (1.0f - (ndcY * 0.5f + 0.5f)) * 544.0f;
    if (outDepth) *outDepth = -ezq;
    return 1;
}

// Capture the current scene's crossfade-flagged visible pieces (texture + screen
// rect) so the transition can dissolve them out over the incoming scene.
static int afn_snapshot_xfade(void) {
#if defined(AFN_HAS_HUD) && defined(AFN_HUD_PIECE_XFADE)
    int n = 0;
    for (int e = 0; e < AFN_HUD_ELEM_COUNT && n < AFN_XFADE_MAX; e++) {
        const AfnHudElem* el = &afn_hud_elems[e];
        if (!afn_hud_visible[e]) continue;
        if (afn_current_mode == 1) { if (el->mode == 1) continue; } else { if (el->mode == 2) continue; }
#ifdef AFN_HUD_MODE0_MASK
        unsigned int m = (afn_current_mode == 1) ? el->sceneMask2D : el->sceneMask;
#else
        unsigned int m = el->sceneMask;
#endif
        if (!(m & (1u << afn_current_scene))) continue;
        for (int k = 0; k < el->pieceCount && n < AFN_XFADE_MAX; k++) {
            const AfnHudPiece* pc = &afn_hud_piece[el->pieceStart + k];
            if (pc->xfToScene < 0 || pc->xfToScene != afn_scene_pending) continue;   // only this transition
            int ptex = pc->tex;
#ifdef AFN_HUD_PIECE_CYCLE
            if (pc->cycleSlot >= 0) { int cv = afn_hud_value[pc->cycleSlot]; if (cv < 0) cv = 0; if (cv >= pc->cycleCount) cv = pc->cycleCount - 1; ptex = pc->tex + cv; }
#endif
            afn_xfade[n].tex = ptex;
            afn_xfade[n].x = el->screenX + pc->x;
            afn_xfade[n].y = el->screenY + pc->y;
            afn_xfade[n].w = pc->w; afn_xfade[n].h = pc->h;
            // Target rect = the To piece in the incoming scene (for the morph). Defaults
            // to the From rect (pure dissolve in place) if unset/invalid.
            afn_xfade[n].tox = afn_xfade[n].x; afn_xfade[n].toy = afn_xfade[n].y;
            afn_xfade[n].tow = afn_xfade[n].w; afn_xfade[n].toh = afn_xfade[n].h;
            if (pc->xfToElem >= 0 && pc->xfToElem < AFN_HUD_ELEM_COUNT) {
                const AfnHudElem* te = &afn_hud_elems[pc->xfToElem];
                if (pc->xfToPiece >= 0 && pc->xfToPiece < te->pieceCount) {
                    const AfnHudPiece* tp = &afn_hud_piece[te->pieceStart + pc->xfToPiece];
                    afn_xfade[n].tox = te->screenX + tp->x; afn_xfade[n].toy = te->screenY + tp->y;
                    afn_xfade[n].tow = tp->w; afn_xfade[n].toh = tp->h;
                }
            }
            n++;
        }
    }
    return n;
#else
    return 0;
#endif
}

// HARDCODED: beam-clash 2D render. The clash is a flat overlay (the backdrop art
// is opaque), so the whole thing draws in HUD/ortho space: the speed-line
// backdrop forced fullscreen (its dot layer scrolling + wrap-tiled to hide the
// seam), then two energy beams meeting at a point that slides with the mash
// balance, plus a clash ball. Called from hud_render in place of the 'clash'
// element so the mash prompt still draws on top.
static void clash_render_2d(void) {
#if defined(AFN_HAS_HUD)
    const AfnHudElem* el = &afn_hud_elems[AFN_CLASH_ELEM];
    for (int k = 0; k < el->pieceCount; k++) {
        int gpi = el->pieceStart + k;
        const AfnHudPiece* pc = &afn_hud_piece[gpi];
        float ox = 0.0f;
#ifdef AFN_HAS_HUD_ANIM
        int li = afn_hud_piece_layer[gpi];
        if (li >= 0) {
            const AfnHudLayer* L = &afn_hud_layer[li];
            int ph = afn_hud_layer_frame[li], pI = -1, nI = -1;
            for (int ki = 0; ki < L->kfCount; ki++) {
                const AfnHudKf* kk = &afn_hud_kf[L->kfStart + ki];
                if (kk->frame <= ph) pI = ki;
                if (kk->frame > ph && nI < 0) nI = ki;
            }
            if (pI < 0) pI = (nI < 0 ? 0 : nI); if (nI < 0) nI = pI;
            const AfnHudKf* A = &afn_hud_kf[L->kfStart + pI]; const AfnHudKf* B = &afn_hud_kf[L->kfStart + nI];
            float frac = 0.0f;
            if (A != B && L->interp != 0) { float span = (float)(B->frame - A->frame); frac = span > 0 ? (float)(ph - A->frame) / span : 0.0f; if (frac < 0) frac = 0; if (frac > 1) frac = 1; }
            ox = A->ox + (B->ox - A->ox) * frac;
        }
#endif
        const float W = 960.0f, H = 544.0f;   // force fullscreen (ignore the authored 8px element origin)
        if (ox != 0.0f)
            for (int t = -1; t <= 1; t++)      // 3-wide tile so the scroll wraps seamlessly
                hud_quad(s_hudTex[pc->tex], ox + t*W, 0, ox + t*W + W, H, 0,0,1,1, 0xFFFFFFFFu);
        else
            hud_quad(s_hudTex[pc->tex], 0, 0, W, H, 0,0,1,1, 0xFFFFFFFFu);
    }
    // Beams + ball (additive energy). Player left, enemy right; meeting point slides with the balance.
    {
        float cy = 250.0f, lX = 70.0f, rX = 890.0f;
        float mX = lX + (rX - lX) * afn_clash_balance;
        float th = 64.0f;
        GLuint tex = 0; int useTex = 0;
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
        int fb = resolve_focus_inst();
        if (fb >= 0) { int NF = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0])); int cf = afn_spr_fstart[fb]; if (cf < 0) cf = 0; if (cf > NF-1) cf = NF-1; tex = s_sprTex[cf]; useTex = 1; }
#endif
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive
        if (useTex) {
            hud_quad(tex, lX, cy - th*0.5f, mX, cy + th*0.5f, 0,0,1,1, 0xFFFFFFFFu);   // player beam
            hud_quad(tex, mX, cy - th*0.5f, rX, cy + th*0.5f, 0,0,1,1, 0xFFFFFFFFu);   // enemy beam
            float r = 60.0f;
            hud_quad(tex, mX - r, cy - r, mX + r, cy + r, 0,0,1,1, 0xFFFFFFFFu);       // clash ball
        } else {
            hud_solid_quad(lX, cy - th*0.5f, rX, cy + th*0.5f, 0x88FFFFFFu);
            hud_solid_quad(mX - 60, cy - 60, mX + 60, cy + 60, 0xCCFFFFFFu);
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);       // restore alpha for the mash UI
    }
#endif
}

static void hud_render(void) {
    // FadeInHudElement node: advance EVERY active per-element alpha crossfade-in.
    for (int _fe = 0; _fe < AFN_HUD_VIS_N; _fe++) {
        if (afn_hud_fade_dur[_fe] > 0) {
            afn_hud_fade_dur[_fe]--;
            int _len  = afn_hud_fade_len[_fe] > 0 ? afn_hud_fade_len[_fe] : 1;
            int _done = _len - afn_hud_fade_dur[_fe];
            int _a = 256 * _done / _len; if (_a > 256) _a = 256;
            afn_hud_elem_fade[_fe] = (afn_hud_fade_dur[_fe] == 0) ? 256 : _a;
        }
    }
    // Ortho 960x544 with a top-left origin (y grows downward) to match the
    // editor's PSV authoring space (native Vita resolution). 1:1 with the screen
    // — HUD pieces/text/cursor are authored directly in 960x544 pixels.
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

#ifdef AFN_HAS_HUD_ANIM
    // Keyframe anim layers — node-driven (NDS hud.c parity): PlayHudAnim
    // resets frame/tick and activates; only ACTIVE layers advance (tick to
    // speed = 60/fps, then frame++, wrapping at length when looping); an
    // inactive layer holds its pose, so before any PlayHudAnim the pieces
    // sit at keyframe 0. Interp: 0=constant snap, 1=linear, 2=bezier.
    float layOx[AFN_HUD_LAYER_COUNT], layOy[AFN_HUD_LAYER_COUNT];
    float layRot[AFN_HUD_LAYER_COUNT], laySx[AFN_HUD_LAYER_COUNT], laySy[AFN_HUD_LAYER_COUNT];
    float layAlpha[AFN_HUD_LAYER_COUNT];                 // per-layer opacity multiplier (0..1; glow pulse)
    for (int li = 0; li < AFN_HUD_LAYER_COUNT; li++) layAlpha[li] = 1.0f;
    unsigned char layHide[AFN_HUD_LAYER_COUNT] = {0};   // per-layer hide at the current frame (blink)
    for (int li = 0; li < AFN_HUD_LAYER_COUNT; li++) {
        const AfnHudLayer* L = &afn_hud_layer[li];
        if (afn_hud_layer_active[li]) {
            int spd = afn_hud_layer_speed_override[li]
                    ? afn_hud_layer_speed_override[li] : L->speed;
            if (spd < 1) spd = 1;
            if (++afn_hud_layer_tick[li] >= spd) {
                afn_hud_layer_tick[li] = 0;
                afn_hud_layer_frame[li] += (L->step > 0 ? L->step : 1);   // step>1 = fps>60 (advance N frames/tick)
                if (L->loop && L->length > 0)
                    while (afn_hud_layer_frame[li] >= L->length) afn_hud_layer_frame[li] -= L->length;
            }
        }
        int ph = afn_hud_layer_frame[li];
        int prevI = -1, nextI = -1;
        for (int ki = 0; ki < L->kfCount; ki++) {
            const AfnHudKf* k = &afn_hud_kf[L->kfStart + ki];
            if (k->frame <= ph) prevI = ki;
            if (k->frame > ph && nextI < 0) nextI = ki;
        }
        if (prevI < 0) prevI = nextI;
        if (nextI < 0) nextI = prevI;
        const AfnHudKf* A = &afn_hud_kf[L->kfStart + (prevI < 0 ? 0 : prevI)];
        const AfnHudKf* B = &afn_hud_kf[L->kfStart + (nextI < 0 ? 0 : nextI)];
        float frac = 0.0f;
        if (A != B && L->interp != 0) {
            float span = (float)(B->frame - A->frame);
            frac = span > 0 ? (float)(ph - A->frame) / span : 0.0f;
            if (frac < 0) frac = 0; if (frac > 1) frac = 1;
            if (L->interp == 2) frac = frac * frac * (3.0f - 2.0f * frac);
        }
        layOx[li]  = A->ox + (B->ox - A->ox) * frac;
        layOy[li]  = A->oy + (B->oy - A->oy) * frac;
        layRot[li] = A->rot + (B->rot - A->rot) * frac;
        laySx[li]  = (A->sx + (B->sx - A->sx) * frac) / 256.0f;
        laySy[li]  = (A->sy + (B->sy - A->sy) * frac) / 256.0f;
#ifdef AFN_HUD_KF_OPACITY
        layAlpha[li] = (A->op + (B->op - A->op) * frac) / 16.0f;   // 0-16 keyframe opacity -> multiplier
        if (layAlpha[li] < 0.0f) layAlpha[li] = 0.0f; if (layAlpha[li] > 1.0f) layAlpha[li] = 1.0f;
#endif
#ifdef AFN_HUD_KF_HIDE
        layHide[li] = (unsigned char)(A->hide != 0);   // step (use the active keyframe)
#endif
    }
#endif

    for (int e = 0; e < AFN_HUD_ELEM_COUNT; e++) {
        const AfnHudElem* el = &afn_hud_elems[e];
#ifdef AFN_HAS_SPRITE_IDX
        // HARDCODED: the 'clash' backdrop draws via the custom 2D clash renderer
        // (fullscreen + beams) in place of the normal element draw.
        // The 'clash' backdrop draws via the custom 2D renderer in place of the
        // normal element draw — but ONLY when the node graph has raised it (this is
        // above the loop's visibility guard, so check visibility explicitly here).
        if (e == AFN_CLASH_ELEM) { if (afn_hud_visible[AFN_CLASH_ELEM]) clash_render_2d(); continue; }
#endif
        float bx, by;
#ifdef AFN_HUD_CURSOR_ELEM
        if (el->trackCursor >= 0) {
            // Cursor-element (a pointer): render this whole element (its pieces +
            // keyframe blink) at the OWNING menu's active cursor stop. Gated by
            // that menu being the active, visible element with stops — so it only
            // appears as the cursor and moves with it. Its own visibility/scene/
            // mode gating is bypassed (driven by the menu instead).
            int m = el->trackCursor;
            const AfnHudElem* mel = &afn_hud_elems[m];
            if (afn_active_element != m || !afn_hud_visible[m] || mel->stopCount <= 0) continue;
            int sidx = afn_cursor_stop; if (sidx < 0) sidx = 0; if (sidx >= mel->stopCount) sidx = mel->stopCount - 1;
            const AfnHudStop* cst = &afn_hud_stops[mel->stopStart + sidx];
            bx = mel->screenX + cst->x + mel->curX;
            by = mel->screenY + cst->y + mel->curY;
            // NOTE: a cursor-element's keyframe layers (e.g. a blink) are NOT
            // auto-played here — that was a runtime override that broke the
            // "purely node-driven" rule. Drive the cursor's blink explicitly with
            // a Play Hud Anim node (on the menu's OnStart). The layer must be
            // flagged Loop in the editor so it doesn't stick on a frame-0/op=0
            // keyframe and blank the cursor out.
        } else
#endif
        {
            if (!afn_hud_visible[e]) continue;
            // Mode gating: el->mode 0 = Both, 1 = 3D-only, 2 = 2D-only. In 2D (menu)
            // mode draw Both + 2D-only; in 3D draw Both + 3D-only.
            if (afn_current_mode == 1) { if (el->mode == 1) continue; }   // 2D: skip 3D-only
            else                       { if (el->mode == 2) continue; }   // 3D: skip 2D-only
#ifdef AFN_HUD_MODE0_MASK
            unsigned int elemSceneMask = (afn_current_mode == 1) ? el->sceneMask2D : el->sceneMask;
#else
            unsigned int elemSceneMask = el->sceneMask;
#endif
            if (!(elemSceneMask & (1u << afn_current_scene))) continue;
            bx = el->screenX; by = el->screenY;
        }
        float elScale = 1.0f;                              // anchored distance scale
#ifdef AFN_HAS_HUD_ANCHOR
        // World-anchored element (ShowHUD fired from a blueprint): origin =
        // the owner NPC's attached-sprite world position projected to screen.
        // Author piece offsets relative to (0,0) — e.g. -64,-64 centers a
        // 128px ring on the anchor. Skipped while behind the camera.
        if (afn_hud_anchor_sprite[e] >= 0) {
            float wx = 0, wy = 0, wz = 0; int found = 0;
#ifdef AFN_HAS_PLAYER_RIG
            int anchor = afn_hud_anchor_sprite[e];
            for (int n = 0; n < AFN_NPC_COUNT && !found; n++)
                if ((int)afn_npc_inst[n][7] == anchor) {
                    wx = s_npcX[n]; wy = s_npcY[n]; wz = s_npcZ[n];
                    found = 1;
#ifdef AFN_HAS_WORLD_ANCHORS
                    // Attached-sprite offset — works with OR without an asset
                    // (an asset-less attached sprite is a pure anchor point).
                    for (int ai = 0; ai < AFN_ANCHOR_COUNT; ai++)
                        if ((int)afn_anchors[ai][0] == anchor) {
                            wx += afn_anchors[ai][1];
                            wy += afn_anchors[ai][2];
                            wz += afn_anchors[ai][3];
                            break;
                        }
#endif
                }
#endif
            if (found) {
                float sxp, syp, depth = 1.0f;
                if (!hud_project(wx, wy, wz, &sxp, &syp, &depth)) continue;   // behind camera
                // Proximity scale (Attached Sprite node Min/Max Size + Near/Far):
                // driven by the PLAYER->target distance, not camera depth —
                // Min size at/under Near (close in), growing to Max at/over Far.
                // So the marker shrinks as you approach. Min==Max = flat size.
                float smin = afn_hud_anchor_min[e] / 100.0f;
                float smax = afn_hud_anchor_max[e] / 100.0f;
                if (smin != smax) {
                    float pdx = (float)player_x - wx, pdz = (float)player_z - wz;
                    float pd = sqrtf(pdx*pdx + pdz*pdz);
                    float nearD = afn_hud_anchor_near[e] > 0 ? (float)afn_hud_anchor_near[e] : 12.0f;
                    float farD  = afn_hud_anchor_far[e]  > nearD ? (float)afn_hud_anchor_far[e] : nearD + 88.0f;
                    float t = (pd - nearD) / (farD - nearD);
                    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
                    elScale = smin + (smax - smin) * t;
                }
                // Center the element's piece-bounds on the anchor so the
                // authored layout (any piece offsets) lands centered on the
                // NPC — no special offsets needed in the editor.
                float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
                for (int k = 0; k < el->pieceCount; k++) {
                    const AfnHudPiece* pp = &afn_hud_piece[el->pieceStart + k];
                    if (pp->x < mnx) mnx = pp->x;
                    if (pp->y < mny) mny = pp->y;
                    if (pp->x + pp->w > mxx) mxx = pp->x + pp->w;
                    if (pp->y + pp->h > mxy) mxy = pp->y + pp->h;
                }
                if (el->pieceCount > 0) {
                    bx = sxp - (mnx + mxx) * 0.5f * elScale;
                    by = syp - (mny + mxy) * 0.5f * elScale;
                } else { bx = sxp; by = syp; }
            }
        }
#endif
        // Pieces (graphics).
        for (int k = 0; k < el->pieceCount; k++) {
            int gpi = el->pieceStart + k;
#ifdef AFN_HAS_SPRITE_IDX
            // HARDCODED: beam-clash mash prompt — show button_pressed only on a
            // mash-press frame, button_unpressed otherwise (overrides the authored blink).
            if (afn_hud_visible[AFN_MASH_ELEM]) {   // clash mash prompt is up (node-raised)
                if (gpi == AFN_CLASH_PC_PRESSED   && !s_clashPressed) continue;
                if (gpi == AFN_CLASH_PC_UNPRESSED &&  s_clashPressed) continue;
            }
#endif
            const AfnHudPiece* pc = &afn_hud_piece[gpi];
            int ptex = pc->tex;   // displayed texture (may be cycled by a HUD value)
#ifdef AFN_HUD_PIECE_CYCLE
            // Cycle ← Value: pick the asset by hud_value[slot] over cycleCount consecutive
            // baked textures (e.g. a character-select portrait driven by the selection).
            if (pc->cycleSlot >= 0) {
                int cv = afn_hud_value[pc->cycleSlot];
                if (cv < 0) cv = 0; if (cv >= pc->cycleCount) cv = pc->cycleCount - 1;
                ptex = pc->tex + cv;
            }
#endif
            // Per-frame position offset: a cycled frame slot can carry its own X/Y
            // so each staged graphic (e.g. a portrait) aligns independently.
            int cofx = 0, cofy = 0;
#ifdef AFN_HUD_PIECE_CYCLE_OFF
            cofx = afn_hud_cycle_off_x[ptex]; cofy = afn_hud_cycle_off_y[ptex];
#endif
            // Per-piece tint via the MODULATE color: Black toggle zeroes RGB (a
            // black silhouette — used for drop shadows), Opacity (0-16) scales
            // alpha. White+16 = the texture as-authored. (byte order: R,G,B,A)
            unsigned pieceCol = 0xFFFFFFFFu;
#ifdef AFN_HUD_PIECE_TINT
            unsigned a8 = (unsigned)(pc->opacity * 255 / 16); if (a8 > 255) a8 = 255;
            pieceCol = (a8 << 24) | (pc->black ? 0u : 0xFFFFFFu);
#endif
            // Per-element opacity multiplier (results-menu crossfade-in).
            if (afn_hud_elem_fade[e] < 256) {
                unsigned ba = (pieceCol >> 24) & 0xFFu;
                pieceCol = ((ba * (unsigned)afn_hud_elem_fade[e] / 256u) << 24) | (pieceCol & 0x00FFFFFFu);
            }
#ifdef AFN_HAS_HUD_ANIM
            int li = afn_hud_piece_layer[gpi];
            if (li >= 0) {
                if (layHide[li]) continue;   // blink: keyframe Hide
                // Keyframe opacity pulse: scale the modulate alpha by the layer's
                // animated opacity multiplier (glow fade), on top of the piece's base.
                {
                    unsigned baseA = (pieceCol >> 24) & 0xFFu;
                    unsigned newA = (unsigned)(baseA * layAlpha[li]);
                    if (newA > 255) newA = 255;
                    pieceCol = (newA << 24) | (pieceCol & 0x00FFFFFFu);
                }
                // Animated: keyframe offset + scale + rotation about center,
                // all in element space scaled by the anchored distance scale.
                float w = pc->w * laySx[li] * elScale, h = pc->h * laySy[li] * elScale;
                hud_quad_xf(s_hudTex[ptex],
                            bx + (pc->x + cofx + layOx[li] + pc->w * 0.5f) * elScale,
                            by + (pc->y + cofy + layOy[li] + pc->h * 0.5f) * elScale,
                            w, h, layRot[li], pieceCol);
                continue;
            }
#endif
#ifdef AFN_HUD_PIECE_BAR
            // Bar fill: clip the piece to value/max so it drains. The fill edge
            // travels barStart (full) -> barEnd (empty) along the axis; only the
            // span between the edge and barEnd is drawn (UV-clipped, no stretch).
            if (pc->barSrc) {
                int cur = (pc->barSrc == 1) ? afn_health : afn_energy;
                int mx  = (pc->barSrc == 1) ? afn_health_max : afn_energy_max;
                float fr = (mx > 0) ? (float)cur / (float)mx : 0.0f;
                if (fr < 0.0f) fr = 0.0f; if (fr > 1.0f) fr = 1.0f;
                float edge = pc->barEnd + (pc->barStart - pc->barEnd) * fr;
                float lo = edge < pc->barEnd ? edge : pc->barEnd;
                float hi = edge > pc->barEnd ? edge : pc->barEnd;
                float px0 = bx + (pc->x + cofx) * elScale, py0 = by + (pc->y + cofy) * elScale;
                float pw = pc->w * elScale, ph = pc->h * elScale;
                if (pc->barAxis == 0 && pc->w > 0 && hi > lo) {
                    hud_quad(s_hudTex[ptex], px0 + lo * elScale, py0, px0 + hi * elScale, py0 + ph,
                             lo / pc->w, 0, hi / pc->w, 1, pieceCol);
                } else if (pc->barAxis != 0 && pc->h > 0 && hi > lo) {
                    hud_quad(s_hudTex[ptex], px0, py0 + lo * elScale, px0 + pw, py0 + hi * elScale,
                             0, lo / pc->h, 1, hi / pc->h, pieceCol);
                }
                continue;
            }
#endif
            // Wrap at the screen edges to match the editor canvas: a piece whose
            // stored position runs past one side (the editor lets you drag it off
            // and shows it wrapped) appears on the other. Draw up to 3x3 wrapped
            // copies; off-screen ones are culled, so on-screen pieces draw once.
            {
                float px0 = bx + (pc->x + cofx) * elScale, py0 = by + (pc->y + cofy) * elScale;
                float pw = pc->w * elScale, ph = pc->h * elScale;
                for (int wy = 0; wy < 3; wy++)
                for (int wx = 0; wx < 3; wx++) {
                    float ox = (wx == 1) ? -SCR_W : (wx == 2) ? SCR_W : 0.0f;
                    float oy = (wy == 1) ? -SCR_H : (wy == 2) ? SCR_H : 0.0f;
                    float x0 = px0 + ox, y0 = py0 + oy;
                    if (x0 + pw <= 0.0f || x0 >= SCR_W || y0 + ph <= 0.0f || y0 >= SCR_H) continue;
                    hud_quad(s_hudTex[ptex], x0, y0, x0 + pw, y0 + ph, 0, 0, 1, 1, pieceCol);
                }
            }
        }
        // Text rows (static label or counter bound to afn_hud_value[slot]).
        for (int k = 0; k < el->textCount; k++) {
            const AfnHudText* tr = &afn_hud_text[el->textStart + k];
            char buf[40];
            const char* str = tr->text;
            if (tr->slot < 4 && tr->pad >= 0 && tr->text[0] == '\0') {
                // pure counter row (no static text): render the slot value
                int val = afn_hud_value[tr->slot];
                int n = 0; char tmp[16];
                if (val < 0) { buf[n++] = '-'; val = -val; }
                int d = 0; do { tmp[d++] = (char)('0' + val % 10); val /= 10; } while (val && d < 15);
                while (d < tr->pad && n + (tr->pad - d) < 39) buf[n++] = '0';   // zero-pad
                while (d > 0) buf[n++] = tmp[--d];
                buf[n] = '\0'; str = buf;
            }
            hud_text(str, bx + tr->x, by + tr->y, tr->scale < 1 ? 1 : tr->scale, tr->color);
        }
        // Cursor at the active stop (menu selection).
        if (el->curTex >= 0 && afn_active_element == e && el->stopCount > 0) {
            int sidx = afn_cursor_stop; if (sidx < 0) sidx = 0; if (sidx >= el->stopCount) sidx = el->stopCount - 1;
            const AfnHudStop* st = &afn_hud_stops[el->stopStart + sidx];
            float cw = afn_hud_frame_w[el->curTex], ch = afn_hud_frame_h[el->curTex];
#ifdef AFN_HUD_CURSOR_SIZE
            if (el->curSize > 0) { cw = ch = (float)el->curSize; }  // authored cursor draw square
#endif
            unsigned curCol = 0xFFFFFFFFu;
            if (afn_hud_elem_fade[e] < 256) curCol = ((255u * (unsigned)afn_hud_elem_fade[e] / 256u) << 24) | 0x00FFFFFFu;
            hud_quad(s_hudTex[el->curTex], bx + st->x + el->curX, by + st->y + el->curY,
                     bx + st->x + el->curX + cw, by + st->y + el->curY + ch, 0, 0, 1, 1, curCol);
        }
    }
#ifdef AFN_HUD_PIECE_XFADE
    // Scene-change crossfade: dissolve the snapshotted outgoing pieces out over the
    // (already-drawn) new scene — alpha falls full -> 0 across afn_xfade_frames.
    if (afn_xfade_count > 0 && afn_xfade_counter > 0 && afn_xfade_frames > 0) {
        float prog = 1.0f - (float)afn_xfade_counter / (float)afn_xfade_frames;   // 0 -> 1
        unsigned a8 = (unsigned)(255u * (unsigned)afn_xfade_counter / (unsigned)afn_xfade_frames);
        if (a8 > 255) a8 = 255;
        unsigned col = (a8 << 24) | 0x00FFFFFFu;
        for (int i = 0; i < afn_xfade_count; i++) {
            const AfnXfadePiece* xp = &afn_xfade[i];
            // Morph the From rect toward the To piece's rect as it dissolves (a
            // pure in-place dissolve when they match, e.g. full-screen bg -> bg).
            float x = xp->x + (xp->tox - xp->x) * prog, y = xp->y + (xp->toy - xp->y) * prog;
            float w = xp->w + (xp->tow - xp->w) * prog, h = xp->h + (xp->toh - xp->h) * prog;
            hud_quad(s_hudTex[xp->tex], x, y, x + w, y + h, 0, 0, 1, 1, col);
        }
    }
#endif
#if defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_NAVMESH)
    // Floating HP bar — node-gated (Show HP Bar). Renders only when a node raised
    // it this frame, for afn_hpbar_obj. 3D gameplay only (the 2D menu/title scenes
    // keep the sprite "visible", so gate those out too).
    if (afn_current_mode != 1 && afn_hpbar_active && afn_hpbar_obj >= 0 &&
        afn_sprite_visible[afn_hpbar_obj] && afn_hp[afn_hpbar_obj] > 0) {
        for (int n = 0; n < AFN_NPC_COUNT; n++) {
            if ((int)afn_npc_inst[n][7] != afn_hpbar_obj) continue;
            float sx, sy, depth;
            if (hud_project(s_npcX[n], s_npcY[n] + ENEMY_BAR_HEIGHT, s_npcZ[n], &sx, &sy, &depth)) {
                float w = ENEMY_BAR_W, h = ENEMY_BAR_H, x0 = sx - w*0.5f, y0 = sy - h*0.5f;
                float mx = afn_hpbar_max > 0 ? (float)afn_hpbar_max : 1.0f;
                float fr = (float)afn_hp[afn_hpbar_obj] / mx; if (fr < 0) fr = 0; if (fr > 1) fr = 1;
                hud_solid_quad(x0 - 2, y0 - 2, x0 + w + 2, y0 + h + 2, 0xE0000000u);  // border/bg
                hud_solid_quad(x0, y0, x0 + w, y0 + h, 0xFF202020u);                  // empty track
                hud_solid_quad(x0, y0, x0 + w * fr, y0 + h, 0xFF2828E6u);             // red fill
            }
            break;
        }
    }
    afn_hpbar_active = 0;   // per-frame: the Show HP Bar node must re-raise it
#endif
    glEnable(GL_DEPTH_TEST);
}
#endif // AFN_HAS_HUD

// Post-battle results menu AND the player-down upkeep are NODE-DRIVEN now
// (ch_controller BP). The whole win/lose -> delay -> menu -> confirm flow lives in
// nodes; the per-frame "while down: freeze + drop lock-on + hide reticle" upkeep is
// a node chain too: Is Health Zero -> Release Lock On -> Tank Camera(0) ->
// Hide HUD(reticle) -> Freeze Player. results_tick() is gone.

#if defined(AFN_HAS_HUD) && defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
// HARDCODED: activate/deactivate the clash backdrop's scroll layer (the dot
// layer that rushes across). Resolves layers via the element's pieces so there's
// no hardcoded layer index. The mash button's pressed/unpressed pieces are NOT
// driven by their layers here — clash_tick auto-cycles them instead, so they sit
// at keyframe 0 (no authored blink) and the cycle alone toggles visibility.
static void clash_set_layers(int on) {
#ifdef AFN_HAS_HUD_ANIM
    const AfnHudElem* el = &afn_hud_elems[AFN_CLASH_ELEM];
    for (int k = 0; k < el->pieceCount; k++) {
        int lyr = afn_hud_piece_layer[el->pieceStart + k];
        if (lyr < 0) continue;
        afn_hud_layer_active[lyr] = on ? 1 : 0;
        if (on) { afn_hud_layer_frame[lyr] = 0; afn_hud_layer_tick[lyr] = 0; }
    }
#endif
}

// Beam-clash SENSE (runtime primitive). Called once per frame after the Focus
// Blast + enemy AI ticks so both beam-in-flight flags are current. Computes
// afn_clash_ready = 1 when both full-charge beams are airborne and either meet
// (proximity) or have both been up a beat (fallback). The node graph reads it via
// the Is Clash Ready gate and drives everything else (begin/struggle/resolve).
static void clash_sense(void) {
    if (!afn_clash_enabled) { afn_clash_ready = 0; s_clashAirT = 0; return; }
    // The "full beam in flight" flags only hold while each beam is actually active.
    if (!afn_fb_active) s_pbBeamFull = 0;
    if (!s_efbActive)   s_ebBeamFull = 0;
    // Pick the player's full-charge pooled beam closest to the enemy beam (the clash
    // candidate). s_pbBeamFull stays set only while such a beam exists.
    float pbx = 0, pby = 0, pbz = 0, bestD = -1.0f; int havePb = 0;
    for (int k = 0; k < AFN_FB_POOL; k++) {
        if (!s_fbPool[k].active || !s_fbPool[k].full) continue;
        float ddx = s_fbPool[k].x - s_efbX, ddz = s_fbPool[k].z - s_efbZ;
        float d2 = ddx*ddx + ddz*ddz;
        if (bestD < 0.0f || d2 < bestD) { bestD = d2; pbx = s_fbPool[k].x; pby = s_fbPool[k].y; pbz = s_fbPool[k].z; havePb = 1; }
    }
    if (!havePb) s_pbBeamFull = 0;
    int bothUp = (havePb && s_pbBeamFull && s_efbActive && s_ebBeamFull && afn_ai_state != AI_DEAD && afn_scene_phase == 0);
    if (!bothUp) { s_clashAirT = 0; afn_clash_ready = 0; return; }
    s_clashAirT++;
    float dx = pbx - s_efbX, dy = pby - s_efbY, dz = pbz - s_efbZ;
    float meetR = (float)afn_clash_meet_r;
    int meet = (dx*dx + dy*dy + dz*dz) <= (meetR * meetR);
    afn_clash_ready = (meet || (afn_clash_air_fb > 0 && s_clashAirT >= afn_clash_air_fb)) ? 1 : 0;
}

// Node primitive: suppress BOTH projectiles (Suppress Beams / Clash Begin).
void afn_clash_suppress_beams(void) {
    for (int k = 0; k < AFN_FB_POOL; k++) s_fbPool[k].active = 0;   // despawn all in-flight blasts
    afn_fb_active = 0; afn_fb_charging = 0; afn_fb_level = 0.0f; afn_fb_fire_timer = 0;
    s_efbActive = 0; s_efbCharging = 0;
}

// Node primitive: start a clash (Clash Begin). Centre the balance, reset the AI
// press countdown + button flash, and suppress the beams that triggered it.
void afn_clash_begin(void) {
    afn_clash_balance = 0.5f;
    s_clashAiTap = afn_clash_ai_min;             // AI's first mash press
    s_clashPunishLeft = 0;
    s_clashPressed = 0; s_clashPressTimer = 0;
    s_pbBeamFull = s_ebBeamFull = 0; s_clashAirT = 0; afn_clash_ready = 0;
    afn_clash_suppress_beams();
}

// Node primitive: one struggle step (Clash AI Step). Mid-skilled-human AI press on
// a varying interval (with an occasional fumble pause), clamp the balance, keep the
// mash SFX looping + pitched by the balance, and cycle the mash button flash. The
// player's own pushes come from the Clash Push node on Cross.
void afn_clash_ai_step(void) {
    if (++s_clashPressTimer >= 8) { s_clashPressTimer = 0; s_clashPressed = !s_clashPressed; }
    if (--s_clashAiTap <= 0) {
        afn_clash_balance -= afn_clash_ai_push_m * 0.001f;
        if (s_clashPunishLeft > 0) {                 // mid punish burst: hammer fast
            s_clashPunishLeft--;
            s_clashAiTap = 3;
        } else {
            s_clashAiTap = afn_clash_ai_min + (int)(ai_rand01() * afn_clash_ai_jit);
            if (ai_chance(afn_clash_fumble_pct * 0.01f)) s_clashAiTap += afn_clash_fumble_len;
            else if (ai_chance(afn_clash_punish_pct * 0.01f)) s_clashPunishLeft = afn_clash_punish_len;  // random punish burst
        }
    }
    if (afn_clash_balance < 0.0f) afn_clash_balance = 0.0f;
    if (afn_clash_balance > 1.0f) afn_clash_balance = 1.0f;
    if (!afn_sfx_active(AFN_SND_MASH)) afn_play_sfx(AFN_SND_MASH, 0, 0);
    afn_set_sfx_pitch(AFN_SND_MASH, 50 + (int)(afn_clash_balance * 100.0f));
}
#endif

int main(void)
{
    // Max out the Vita clocks. 444 MHz is the hardware/API ceiling for the
    // Cortex-A9 cores (the default app clock is 333 MHz); scePower clamps
    // anything higher, so a true 500 MHz needs a kernel overclock plugin and is
    // not done here. Bus/GPU are pushed to their maxima too for headroom.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Force the GLSL/fixed-function pipeline to FULL precision. vitaGL can fold
    // shader floats down to half (fp16); at world coords ~150 that quantizes
    // positions to ~0.1 units, which z-fights and shifts as the view rotates
    // (large floor/slope flicker while the small-coord rig stays clean).
    vglUseLowPrecision(GL_FALSE);
    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    upload_textures();
    afn_audio_init();   // software mixer thread (no-op if the scene has no sound)
#ifdef AFN_HAS_SKY
    sky_init();
#endif
#ifdef AFN_HAS_SPRITES
    billboards_init();
#endif
#ifdef AFN_HAS_PLAYER_RIG
    rig_init();
    afn_hp[AFN_ENEMY_EIDX] = ENEMY_HP_MAX;   // seed at boot so a BP Is HP Zero(self) doesn't fire in menus (enemy BP runs all scenes)
#endif
#ifdef AFN_HAS_HUD
    hud_init();
    // Seed per-element visibility from the authored "visible" flag (ShowHUD /
    // CursorUp toggle it at runtime).
    for (int i = 0; i < AFN_HUD_ELEM_COUNT; i++) afn_hud_visible[i] = afn_hud_elems[i].startVis;
    for (int i = 0; i < AFN_HUD_VIS_N; i++) afn_hud_elem_fade[i] = 256;   // opaque until a fade ramps it
    for (int i = 0; i < AFN_HUD_VIS_N; i++) {
        afn_hud_anchor_sprite[i] = -1;                                   // screen-space default
        afn_hud_anchor_min[i] = 100; afn_hud_anchor_max[i] = 100;        // constant size default
        afn_hud_anchor_near[i] = 0; afn_hud_anchor_far[i] = 0;           // runtime defaults if unset
    }
#endif

    // Player state. The follow camera and NPCs read playerX/Y/Z; the movement
    // step below drives it (camera-relative left stick + gravity + floor/wall
    // collision against the level mesh).
#ifdef AFN_HAS_PLAYER_RIG
    float playerX = AFN_PLAYER_START_X, playerY = AFN_PLAYER_START_Y, playerZ = AFN_PLAYER_START_Z;
#else
    float playerX = afn_cam_start_x, playerY = afn_cam_start_h, playerZ = afn_cam_start_z;
#endif
    float playerYaw = 0.0f;   // rig facing (degrees)
    float playerVY  = 0.0f;   // vertical velocity (gravity / jump)
    int   grounded  = 1;
    float s_floorN[3] = {0.0f, 1.0f, 0.0f};   // smoothed floor normal (slope tilt)
#ifdef AFN_HAS_RAIL_PATH
    int   gr_on = 0; float gr_arc = 0.0f; int gr_dir = 1; float gr_speed = 0.0f, gr_prevY = 0.0f;
#endif
    collide_build();
#ifdef AFN_HAS_NAVMESH
    // Detour navmesh (baked by the editor's Recast build at export). NPCs with
    // a Navigation mode path across it in the per-NPC physics loop.
    afn_nav_init(afn_navmesh_bin, afn_navmesh_bin_size);
#endif
    {   // Drop onto the floor at spawn so we don't start mid-air.
        float fy, fn[3];
        if (collide_floor(playerX, playerZ, playerY + 200.0f, &fy, fn))
            playerY = fy - COL_BOTTOM;
    }

    // Orbit-follow camera, anchored to the active camera slot
    // (afn_cam_slots[afn_active_camera] = { yaw, dist, height, horizon }). The
    // player orbits freely with the right stick / L-R; triggers zoom. The slot's
    // height sets the default downward tilt and the look-at target height. PSV
    // has no scripts yet so afn_active_camera stays 0 (scene default), but the
    // runtime re-reads it each frame so a future SetCamera node just works.
    if (afn_active_camera < 0 || afn_active_camera >= AFN_CAM_SLOT_COUNT) afn_active_camera = 0;
    const float* slot0 = afn_cam_slots[afn_active_camera];
    orbit_angle       = (int)(slot0[0] * (65536.0f / 6.2831853f));  // GLOBAL brad (node + manual)
    afn_player_heading = orbit_angle;   // tank heading starts facing the camera-forward
    float camDist     = slot0[1] > 1.0f ? slot0[1] : 60.0f;  // world px
    float camHeight   = slot0[2];                            // world px
    // Pitch is also brad in the global orbit_pitch so the OrbitCamera Up/Down
    // node and the right stick drive it together. The Camera Properties "Pitch"
    // field (afn_cam_start_pitch, degrees) sets a fixed starting angle; 0 = auto:
    // derive the tilt from the slot's height/distance (the legacy behavior).
    if (afn_cam_start_pitch != 0.0f)
        orbit_pitch   = (int)(afn_cam_start_pitch * (65536.0f / 360.0f));   // explicit degrees -> brad
    else
        orbit_pitch   = (int)(atan2f(camHeight > 0.0f ? camHeight : 8.0f, camDist) * (65536.0f / 6.2831853f));

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#ifdef AFN_HAS_SPRITE_IDX
    // Sprites start visible + collidable UNLESS flagged "Hidden (effect)" — those
    // (e.g. the Focus Blast orb) stay hidden until a node shows them.
    // DestroyObject/SetVisible/Cast Effect nodes flip these at runtime.
    for (int i = 0; i < NUM_SPRITES; i++) { afn_sprite_visible[i] = !afn_sprite_start_hidden[i]; afn_collision_enabled[i] = 1; }
#endif
    // Boot into the mode/scene the build was started from (Export PSV bakes
    // AFN_START_MODE: 0 = 3D, 1 = 2D menu). Seed before script_start so OnStart
    // and blueprint hooks see the right mode/scene.
#ifdef AFN_START_MODE
    afn_current_mode  = AFN_START_MODE;
    afn_current_scene = AFN_START_SCENE;
#endif
    script_start();   // OnStart + blueprint start hooks

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        // No hardcoded quit: Start is a normal node-readable key (KEY_START).
        // Exit the app via the PS/home menu. (Previously Start broke the loop,
        // so binding Start to a node closed the app — looked like a crash.)

        // Expose engine state to the node graph BEFORE it ticks: player_on_ground
        // (Jump/IsJumping gates), player_moving (IsMoving), and player_x/y/z
        // (teleport/distance/checkpoint nodes read+write the world position).
        player_on_ground = grounded;
        player_vy_now = (int)(playerVY * 256.0f);   // last frame's real vy -> Is Jumping/Is Falling
        int pteleX = player_x = (int)playerX;
        int pteleY = player_y = (int)playerY;
        int pteleZ = player_z = (int)playerZ;

        // Node-driven: input_update() sets the raw defaults, then the emitted
        // graph (script_tick) overrides afn_input_fwd/right, afn_move_speed,
        // orbit_angle and afn_rig_clip. The movement/camera below only READ them.
        input_update(&pad);
        script_tick();
        afn_audio_tick();   // 60 Hz sequencer clock (envelopes / note scheduling)

        // ChangeScene Delay pin: hold the current scene, then kick off the
        // transition when the countdown elapses (the fade/swap below takes over).
        if (afn_scene_delay > 0 && --afn_scene_delay == 0)
            afn_scene_start_transition(afn_scene_delay_scene, afn_scene_delay_mode, afn_scene_delay_frames);

        // Screen-fade tick: ease afn_fade_level toward afn_fade_target over
        // afn_fade_counter frames (set by ChangeScene / fade nodes). Rendered as
        // a fullscreen overlay below (vitaGL has no REG_MASTER_BRIGHT).
        if (afn_fade_counter > 0) {
            afn_fade_counter--;
            int span = afn_fade_frames > 0 ? afn_fade_frames : 1;
            afn_fade_level = afn_fade_target * (span - afn_fade_counter) / span;
            if (afn_fade_counter == 0) afn_fade_level = afn_fade_target;
        }
        if (afn_xfade_counter > 0) afn_xfade_counter--;   // scene-change crossfade dissolve
        // Scene transition: at fade-out completion, swap scene index + respawn,
        // then fade back in. ReloadScene = true respawn; ChangeScene to another
        // index resets in the SAME geometry (full multi-scene needs an all-scenes
        // export — only one scene's data is present).
        if (afn_scene_phase == 1 && afn_fade_counter == 0) {
            afn_current_scene = afn_scene_pending; afn_current_mode = afn_scene_pending_mode;
#ifdef AFN_HAS_PLAYER_RIG
            playerX = AFN_PLAYER_START_X; playerY = AFN_PLAYER_START_Y; playerZ = AFN_PLAYER_START_Z;
#else
            playerX = afn_cam_start_x; playerY = afn_cam_start_h; playerZ = afn_cam_start_z;
#endif
            playerVY = 0.0f; grounded = 1; afn_player_heading = orbit_angle;
#ifdef AFN_HAS_SPRITE_IDX
            for (int i = 0; i < NUM_SPRITES; i++) { afn_sprite_visible[i] = !afn_sprite_start_hidden[i]; afn_collision_enabled[i] = 1; }
#endif
            // Silence the previous scene's music/SFX so the entered scene starts
            // clean (its OnStart Play Sound below starts fresh).
            afn_stop_sound();
            // Clear any transient menu freeze before re-running OnStart. A 2D
            // cursor menu's ShowHUD sets afn_player_frozen=1 (+ afn_play_anim=-1),
            // and those are globals that otherwise LEAK across the scene swap —
            // loading from the char-select menu into the 3D gameplay scene would
            // leave the player stuck until something happened to UnfreezePlayer.
            // A menu scene re-freezes itself in afn_bp_dispatch_start() below.
            afn_player_frozen = 0; afn_play_anim = 0;
#ifdef AFN_HAS_PLAYER_RIG
            // HARDCODED: re-seed combat + results state on every scene (re)entry so
            // Restart/Title from the results menu starts a clean battle (rig_init's
            // reset only runs at boot, not on a scene swap).
            s_aiInited = 0; afn_ai_state = AI_ROAM; afn_ai_slot = -1; s_efbActive = 0; s_efbCharging = 0; s_eDodgeFrames = 0;
            afn_hp[AFN_ENEMY_EIDX] = ENEMY_HP_MAX;   // seed HP before the first script tick (else BP Is HP Zero fires at start)
            afn_ai_dodge_done = afn_ai_charge_done = afn_ai_fire_done = afn_ai_reached = 0;
            s_playerClipHold = 0;   // clear the player die-clip hold (HoldSkelClip)
            afn_cam_orbit_active = 0; afn_cam_orbit_timer = 0; afn_cam_orbit_obj = -1;   // OrbitCameraOnObject node
            afn_health = afn_health_max;
            // Results menu is node-driven; clear its element fade + hide its HUD so a
            // prior battle's 'die' menu never leaks across a scene swap. The BP's
            // Delay self-rearms once afn_hp / afn_health refill above.
            afn_hud_elem_fade[AFN_RESULT_ELEM] = 256; afn_hud_elem_fade[AFN_RESULT_CURSOR] = 256;
            afn_hud_visible[AFN_RESULT_ELEM] = 0; afn_hud_visible[AFN_RESULT_CURSOR] = 0;
            for (int _fe = 0; _fe < AFN_HUD_VIS_N; _fe++) afn_hud_fade_dur[_fe] = 0;   // cancel in-flight Fade In Hud ramps
            // Beam-clash: clear state + hide its HUD so a clash never leaks across a scene swap.
            afn_clash_ready = 0; afn_clash_balance = 0.5f; s_clashPressed = 0;
            s_pbBeamFull = 0; s_ebBeamFull = 0; s_clashAirT = 0;
#if defined(AFN_HAS_HUD) && defined(AFN_HAS_SPRITE_IDX)
            afn_hud_visible[AFN_CLASH_ELEM] = 0; afn_hud_visible[AFN_MASH_ELEM] = 0;
            clash_set_layers(0);
            afn_stop_sfx_sample(AFN_SND_STRUGGLE);   // never leave the struggle/mash loops ringing across a swap
            afn_stop_sfx_sample(AFN_SND_MASH);
#endif
#endif
            // Re-run the entered scene's blueprint OnStart hooks (ShowHUD, music,
            // cursor init, ...). afn_bp_dispatch_start is scene-gated, so only the
            // NEW scene's blueprints fire — boot only ran it for the start scene,
            // so without this a ChangeScene swap left the next 2D scene dark/silent.
            afn_bp_dispatch_start();
            if (afn_xfade_count > 0) {
                // Crossfade swap: no black fade-in — the dissolve overlay carries the visual.
                afn_fade_target = 0; afn_fade_frames = 1; afn_fade_counter = 0; afn_fade_level = 0;
            } else {
                afn_fade_target = 0; afn_fade_frames = 15; afn_fade_counter = 15; afn_fade_level = -16;
            }
            afn_scene_phase = 2;
        } else if (afn_scene_phase == 2 && afn_fade_counter == 0) {
            afn_scene_phase = 0;
        }

        // A teleport/checkpoint node wrote player_x/y/z — apply it to the float
        // position (normal frames leave them unchanged, so no precision loss).
        if (player_x != pteleX) playerX = (float)player_x;
        if (player_y != pteleY) playerY = (float)player_y;
        if (player_z != pteleZ) playerZ = (float)player_z;

        // Re-read the active slot each frame (a SetCamera node retargets it).
        // Clamp first (matches NDS fps3d.c): a Set Camera fed an out-of-range slot
        // (e.g. an Integer >= AFN_CAM_SLOT_COUNT) would otherwise index past
        // afn_cam_slots and crash. Out-of-range falls back to slot 0 (scene default).
        // NDS fps3d.c parity (update_camera_slot): don't hard-cut on SetCamera.
        // Distance/height ease toward the slot every frame (1/8 step ~= the NDS
        // ">> 3" ramp, ~0.3s glide). Yaw/pitch are a ONE-SHOT ease: when the
        // active slot changes we swing orbit_angle/orbit_pitch to the slot's
        // view, then release so OrbitCamera nodes orbit freely from the new
        // angle — moving orbit_angle itself keeps movement camera-relative
        // during the swing (no spin), same as the NDS.
        if (afn_active_camera < 0 || afn_active_camera >= AFN_CAM_SLOT_COUNT) afn_active_camera = 0;
        const float* S = afn_cam_slots[afn_active_camera];
        float camDistTgt   = S[1] > 1.0f ? S[1] : camDist;  // keep manual zoom unless slot overrides
        float camHeightTgt = S[2];
        camDist   += (camDistTgt   - camDist)   * 0.125f;
        camHeight += (camHeightTgt - camHeight) * 0.125f;
        {
            static int s_prevCamSlot = 0, s_camYawEasing = 0;
            if (afn_active_camera != s_prevCamSlot) { s_prevCamSlot = afn_active_camera; s_camYawEasing = 1; }
            // While locked on, the lock assist (below) owns yaw + pitch — it eases
            // the orbit to frame the target every frame. If the slot-switch ALSO
            // eased yaw toward the slot's absolute S[0], a Set Camera would yank the
            // camera toward that authored angle (sometimes the character's front)
            // before the lock pulls it back — a visible snap. So when locked, drop
            // the yaw/pitch ease and let Set Camera only blend distance/height (zoom).
            if (afn_cam_lock_target >= 0) s_camYawEasing = 0;
            if (s_camYawEasing) {
                int yawTgt = (int)(S[0] * (65536.0f / 6.2831853f));
                int d = (int)(int16_t)(uint16_t)(yawTgt - orbit_angle);   // brad, wrap-safe
                orbit_angle = (int)(uint16_t)(orbit_angle + (d >> 3));
                // Every slot carries an explicit orbit Pitch (deg) in column 4
                // (slot 0 = afn_cam_start_pitch). Honor it uniformly so authoring
                // matches Camera Properties > Pitch; 0 = auto, derive the tilt from
                // the slot's height/distance (the legacy behavior).
                float slotPitchDeg = S[4];
                int pitchTgt;
                if (slotPitchDeg != 0.0f)
                    pitchTgt = (int)(slotPitchDeg * (65536.0f / 360.0f));
                else
                    pitchTgt = (int)(atan2f(camHeightTgt > 0.0f ? camHeightTgt : 8.0f, camDistTgt) * (65536.0f / 6.2831853f));
                int pd = pitchTgt - orbit_pitch;
                orbit_pitch += pd >> 3;
                if (d > -300 && d < 300 && pd > -300 && pd < 300) {
                    orbit_angle = (int)(uint16_t)yawTgt; orbit_pitch = pitchTgt; s_camYawEasing = 0;
                }
            }
        }

        // Orbit-rate clamp + "orbiting?" detection (NDS fps3d.c:1108-1122).
        // AFN_ORBIT_MAX_DELTA caps this frame's orbit_angle change so the
        // camera-position chase below can keep up. PSV input is fully node-
        // driven, so instead of NDS's held-L/R check we detect that something
        // Lock-on camera assist — node-driven: a Lock Camera node sets
        // afn_cam_lock_target (Unlock Camera resets it to -1). While the
        // target is in the camera's horizontal field, the orbit yaw EASES
        // toward facing it — riding the normal camera chase delay. When the
        // target drifts off screen the assist RELEASES (camera back to normal)
        // and re-acquires only when a manual orbit brings it back into view.
#ifdef AFN_HAS_PLAYER_RIG
        {
            int lockSpr = afn_cam_lock_target;
            if (lockSpr >= 0) {
                float tx = 0, ty = 0, tz = 0; int tFound = 0;
                for (int n = 0; n < AFN_NPC_COUNT && !tFound; n++)
                    if ((int)afn_npc_inst[n][7] == lockSpr) { tx = s_npcX[n]; ty = s_npcY[n]; tz = s_npcZ[n]; tFound = 1; }
                float ldx = tx - playerX, ldz = tz - playerZ;
                if (tFound && (ldx*ldx + ldz*ldz) > 4.0f) {
                    // ALWAYS ease the orbit toward facing the target while
                    // locked — even when it's off-screen / behind, so locking
                    // on swings the camera around to frame it. Camera forward
                    // is +(sin,cos)(camAngle), so facing the target means
                    // forward ∝ (target - player).
                    float desired = atan2f(ldx, ldz);
                    float cur = orbit_angle * (6.2831853f / 65536.0f);
                    float diff = desired - cur;
                    while (diff >  3.14159265f) diff -= 6.2831853f;
                    while (diff < -3.14159265f) diff += 6.2831853f;
                    orbit_angle = (int)(uint16_t)(orbit_angle
                                 + (int)(diff * (65536.0f / 6.2831853f) * 0.10f));   // ease toward
                    // Up/down follows the same convention: ease orbit_pitch toward
                    // the player->target VERTICAL angle (positive pitch = look down,
                    // matching the boot seed atan2(camHeight,camDist)). Manual up/down
                    // can nudge off it but the pull caps the deviation — you can't
                    // flip over the top/bottom of the locked target.
                    float horiz = sqrtf(ldx*ldx + ldz*ldz);
                    // Two independent pitch contributions:
                    //  heightP = CONSTANT elevated look-down bias from Height. Referenced
                    //    to the orbit distance (not the live horiz) so it frames the same
                    //    at rest and doesn't steepen as you close in. Always applied.
                    //  trackP  = ease up/down toward the target's actual height (negative
                    //    = look up on a jump, positive = look down at a lower target).
                    //  No Look-Down clamps ONLY trackP's DOWN side, so closing in / a low
                    //    target adds no tilt — but Height still sets the framing (and is
                    //    still adjustable) and a jump still pulls the view up.
                    float heightP = atan2f((float)afn_lock_height, camDist > 1.0f ? camDist : 60.0f);
                    float trackP  = atan2f(playerY - ty, horiz);
                    if (afn_lock_no_lookdown && trackP > 0.0f) trackP = 0.0f;
                    float desiredP = heightP + trackP;
                    float curP = orbit_pitch * (6.2831853f / 65536.0f);
                    float diffP = desiredP - curP;
                    orbit_pitch = orbit_pitch
                                 + (int)(diffP * (65536.0f / 6.2831853f) * 0.10f);   // ease toward
                }
            }
        }
#endif

        // (an OrbitCamera node / the slot swing) actually moved orbit_angle
        // this frame — that selects AFN_ORBIT_EASE_IN for the chase.
        int orbitingNow;
        {
            static int s_prevOrbit = 0, s_prevOrbitP = 0, s_prevOrbitInit = 0;
            if (!s_prevOrbitInit) { s_prevOrbit = orbit_angle; s_prevOrbitP = orbit_pitch; s_prevOrbitInit = 1; }
            int odelta = (int)(int16_t)(uint16_t)(orbit_angle - s_prevOrbit);
            int pdelta = orbit_pitch - s_prevOrbitP;
            orbitingNow = (odelta != 0 || pdelta != 0);
#ifdef AFN_ORBIT_MAX_DELTA
            if (odelta >  AFN_ORBIT_MAX_DELTA) odelta =  AFN_ORBIT_MAX_DELTA;
            if (odelta < -AFN_ORBIT_MAX_DELTA) odelta = -AFN_ORBIT_MAX_DELTA;
            orbit_angle = (int)(uint16_t)(s_prevOrbit + odelta);
            if (pdelta >  AFN_ORBIT_MAX_DELTA) pdelta =  AFN_ORBIT_MAX_DELTA;
            if (pdelta < -AFN_ORBIT_MAX_DELTA) pdelta = -AFN_ORBIT_MAX_DELTA;
            orbit_pitch = s_prevOrbitP + pdelta;
#endif
            s_prevOrbit = orbit_angle; s_prevOrbitP = orbit_pitch;
        }

        // Camera orbit is purely node-driven: the OrbitCamera node writes
        // orbit_angle/orbit_pitch (reading KEY_L/KEY_R etc.). No hardcoded right
        // stick or trigger control here.
        // Clamp pitch to ~±80 deg in brad (65536 = 360 deg -> ±14563).
        if (orbit_pitch >  14563) orbit_pitch =  14563;
        if (orbit_pitch < -14563) orbit_pitch = -14563;

        // Camera yaw + pitch (radians) from the node/manual orbit_angle/pitch (brad).
        float camAngle = orbit_angle * (6.2831853f / 65536.0f);
        float pitch    = orbit_pitch * (6.2831853f / 65536.0f);

        // --- Player movement: reads the node-set move intent (afn_input_fwd/right,
        //     256 = full) in camera space, scaled by afn_move_speed (or the walk
        //     default when no script). Ported from PSP scene_update. ---
        // 8-Way Stick node: snap the analog move vector to the nearest 45 deg
        // octant (magnitude preserved) so movement AND the Strafe Anim clip pick
        // read a crisp 8-way direction instead of fighting analog drift.
        if (afn_stick_8way && (afn_input_fwd || afn_input_right)) {
            float m = sqrtf((float)afn_input_fwd*afn_input_fwd + (float)afn_input_right*afn_input_right);
            float a = atan2f((float)afn_input_right, (float)afn_input_fwd);
            a = lroundf(a / 0.7853982f) * 0.7853982f;   // snap to 45 deg
            afn_input_fwd   = (int)lroundf(m * cosf(a));
            afn_input_right = (int)lroundf(m * sinf(a));
        }
        float fAmt = afn_input_fwd  / 256.0f;
        float rAmt = afn_input_right / 256.0f;
        // Lock Strafe (Z-targeting): with an active Lock On target, movement
        // axes become TARGET-relative (Up closes in, Down backpedals, L/R
        // circle-strafe) and the rig always faces the target — see below.
        int lockStrafing = 0;
        float lockAngle = 0.0f;
#ifdef AFN_HAS_PLAYER_RIG
        if (afn_lock_strafe && afn_cam_lock_target >= 0) {
            for (int n = 0; n < AFN_NPC_COUNT; n++)
                if ((int)afn_npc_inst[n][7] == afn_cam_lock_target) {
                    float tdx = s_npcX[n] - playerX, tdz = s_npcZ[n] - playerZ;
                    if (tdx*tdx + tdz*tdz > 1.0f) {
                        lockAngle = atan2f(tdx, tdz);   // +sin/+cos convention
                        lockStrafing = 1;
                    }
                    break;
                }
        }
#endif
        // Tank controls: when afn_tank_camera is set (a Turn Player / tank node),
        // movement + facing follow afn_player_heading (D-pad turned), so the
        // camera orbits independently. Otherwise movement is camera-relative.
        // Lock Strafe overrides both: axes follow the player->target line.
        float moveAngle = lockStrafing ? lockAngle
                        : (afn_tank_camera && afn_tank_move)
                          ? (afn_player_heading * (6.2831853f/65536.0f)) : camAngle;
        float fwdX = sinf(moveAngle), fwdZ = cosf(moveAngle);
        float rgtX = cosf(moveAngle), rgtZ = -sinf(moveAngle);
        float mvX = fAmt*fwdX + rAmt*rgtX;
        float mvZ = fAmt*fwdZ + rAmt*rgtZ;
        // Move only when the node graph asks: a movement node sets afn_input_fwd/
        // right AND afn_move_speed. No walk-speed fallback — purely node-driven.
        int facedByMove = 0;
        static float sTankRelFace = 0.0f;   // tank drive: facing offset from the heading (deg)
        if ((mvX*mvX + mvZ*mvZ > 0.0001f) && afn_move_speed > 0 && !afn_player_frozen) {
            float speed = afn_move_speed * 0.08f;
            float dx = mvX*speed, dz = mvZ*speed;
            float dlen = sqrtf(dx*dx + dz*dz);
            int steps = (int)(dlen / 3.0f) + 1;   // MAX_MOVE_STEP: don't tunnel walls
            float ix = dx/steps, iz = dz/steps;
            for (int st = 0; st < steps; st++) { playerX += ix; playerZ += iz; collide_walls(&playerX, &playerZ, playerY); }
            // Face the movement unless a MovePlayer node asked for Consistent
            // Facing (afn_face_lock). Two flavors:
            // - camera-relative movement: yaw = atan2 of the world move vector.
            // - tank drive (heading-relative movement): the model faces
            //   heading + the INPUT's relative angle — up drives forward
            //   facing forward, down turns the model around and drives the
            //   other way on the same axis (steering then reads mirrored,
            //   "on that side"). The heading itself NEVER flips, so no
            //   back-and-forth polarity churn.
            if (lockStrafing) {
                // Z-targeting: facing is handled below (always face target).
            } else if (!afn_face_lock) {
                if (afn_tank_camera && afn_tank_move) {
                    sTankRelFace = atan2f(rAmt, fAmt) * (180.0f/3.14159265f);
                } else {
                    playerYaw = atan2f(mvX, mvZ) * (180.0f/3.14159265f);
                    facedByMove = 1;
                }
            }
        }
        // Lock Strafe: the rig faces the target at all times — moving,
        // backpedaling, circling, or standing still.
        if (lockStrafing) {
            playerYaw = lockAngle * (180.0f/3.14159265f);
            facedByMove = 1;   // target facing wins over the tank heading
        }
        // Strafe Anim: now that afn_input_fwd/right are final (script ran), pick
        // the 8-way directional clip the node registered. Only while moving, so
        // a still lock-on leaves the clip to the idle chain.
        if (afn_strafe_anim && (afn_input_fwd || afn_input_right)) {
            float sa = atan2f((float)afn_input_right, (float)afn_input_fwd);
            int so = ((int)lroundf(sa / 0.7853982f) + 8) & 7;
            afn_rig_clip = afn_strafe_clip[so];
        }
        // Tank heading owns the facing whenever camera-relative Direction
        // Facing isn't steering it. In tank drive the persisted relative
        // facing (sTankRelFace) rides on top — so after walking "down" the
        // model stays turned around at idle (no release snap) and TurnPlayer
        // visibly rotates the whole frame. When a camera-relative walk ENDS,
        // bake its final facing into the heading (no snap there either).
        static int sWasFacedByMove = 0;
        if (afn_tank_camera) {
            if (sWasFacedByMove && !facedByMove)
                afn_player_heading = (int)(playerYaw * (65536.0f / 360.0f));
            if (afn_tank_move) {
                // Tank drive: the facing ALWAYS rides the heading frame —
                // Consistent Facing only freezes the relative part
                // (sTankRelFace stops updating), so strafing keeps its
                // facing while TurnPlayer still rotates the model with the
                // axis (no "stuck" yaw).
                playerYaw = afn_player_heading * (360.0f/65536.0f) + sTankRelFace;
            } else if (!facedByMove && !afn_face_lock) {
                playerYaw = afn_player_heading * (360.0f/65536.0f);
            }
        }
        sWasFacedByMove = facedByMove;

        // Dash To Target (bullet-punch): a committed lunge toward the captured
        // target's LIVE position, facing it, with wall collision; ends at melee
        // range or when the frame budget runs out. No target -> lunge along the
        // current facing. Overrides input movement while active.
#ifdef AFN_HAS_PLAYER_RIG
        if (afn_dash_frames > 0 && !afn_player_frozen) {
            float ddx, ddz; int homing = 0;
            if (afn_dash_target >= 0) {
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_dash_target) {
                        ddx = s_npcX[n] - playerX; ddz = s_npcZ[n] - playerZ; homing = 1; break;
                    }
            }
            if (!homing) { ddx = sinf(playerYaw*DEG2RAD); ddz = cosf(playerYaw*DEG2RAD); }
            float dl = sqrtf(ddx*ddx + ddz*ddz);
            float melee = COL_RADIUS * 2.0f + 6.0f;
            float sp = afn_dash_speed * 0.08f;
            if (homing && dl <= melee) {
                afn_dash_frames = 0;                       // reached the enemy
            } else if (dl > 0.001f) {
                float step = sp;
                if (homing && dl - sp < melee) step = dl - melee;   // don't overshoot into it
                if (step < 0.0f) step = 0.0f;
                float ix = ddx/dl*step, iz = ddz/dl*step;
                int sub = (int)(step/3.0f) + 1;            // wall-tunnel guard like normal move
                for (int st = 0; st < sub; st++) { playerX += ix/sub; playerZ += iz/sub; collide_walls(&playerX, &playerZ, playerY); }
                playerYaw = atan2f(ddx, ddz) * (180.0f/3.14159265f);   // face the lunge
                if (--afn_dash_frames <= 0) afn_dash_frames = 0;
            } else {
                afn_dash_frames = 0;
            }
        }
#endif

#ifdef AFN_HAS_PLAYER_RIG
        // Focus Blast: hold-to-charge homing projectile. Charge Shot asserts
        // afn_fb_charge_req each held frame; Fire Charge Shot raises fire_req on
        // release. The ball is the player's hidden "effect" sub-sprite, whose
        // billboard transform we drive here (charge spot, then flight).
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
        if (afn_fb_inst < 0 && (afn_fb_charge_req || afn_fb_fire_req)) {
            // Match the ball to the player's attached sub-sprite. Prefer the
            // blueprint's self sprite, then the exported player sprite index,
            // then ANY attached sub-sprite — the player carries exactly one
            // (focus_gfx), so the orb resolves even if self came back unset.
            int want = afn_fb_parent;
#ifdef AFN_PLAYER_SPRITE_IDX
            if (want < 0) want = AFN_PLAYER_SPRITE_IDX;
#endif
            // Two-pass: prefer the exact parent sprite, but the player carries
            // exactly one attached effect sub-sprite (focus_gfx). If `want` came
            // back as a wrong/stale index (sub-sprite index spaces can diverge),
            // fall back to ANY attached sub-sprite instead of leaving the orb
            // unresolved — an unresolved inst means the shot fires + deals damage
            // + plays its SFX but the ball is never drawn ("beam won't summon").
            int fbAny = -1;
            for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
                if (afn_spr_parent[i] < 0) continue;
                if (fbAny < 0) fbAny = i;
                if (want < 0 || afn_spr_parent[i] == want) { afn_fb_inst = i; break; }
            }
            if (afn_fb_inst < 0) afn_fb_inst = fbAny;   // exact match failed -> any attached sub-sprite
        }
#endif
        {
            float fbFx = sinf(playerYaw*DEG2RAD), fbFz = cosf(playerYaw*DEG2RAD);
            float fbRx = cosf(playerYaw*DEG2RAD), fbRz = -sinf(playerYaw*DEG2RAD);  // player's right axis
            // Carry the orb at its AUTHORED attached-sprite offset (the hand spot),
            // rotated by facing and tracked to the live player position — the
            // generic re-anchor never follows the player sprite. Falls back to a
            // chest-front spot if the ball instance hasn't resolved.
            float fbOx = 0.0f, fbOy = 14.0f, fbOz = 6.0f;
            float fbBaseX = playerX, fbBaseY = playerY, fbBaseZ = playerZ;
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_PARENT)
            if (afn_fb_inst >= 0) { fbOx = afn_spr_poff_x[afn_fb_inst]; fbOy = afn_spr_poff_y[afn_fb_inst]; fbOz = afn_spr_poff_z[afn_fb_inst]; }
#endif
            int fbBone = -1;
#ifdef AFN_HAS_SPR_BONE
            // Ride the live animated bone if one is assigned (bone world pos is absolute).
            if (afn_fb_inst >= 0 && afn_spr_bone[afn_fb_inst] >= 0) {
                fbBone = afn_spr_bone[afn_fb_inst];
                fbBaseX = s_player_bone_world[fbBone][0];
                fbBaseY = s_player_bone_world[fbBone][1];
                fbBaseZ = s_player_bone_world[fbBone][2];
            }
#endif
            float fbAttachX, fbAttachY, fbAttachZ;
            if (fbBone >= 0) {
                // Offset in the rig's LOCAL frame (X=right, Z=forward), rotated by
                // the rig's FULL orientation (facing + Model Yaw) — the exact yaw
                // rig_draw uses — so a +Z nudge is "in front of the character" both
                // in the editor preview and at runtime, regardless of facing.
                float yr = playerYaw*DEG2RAD + AFN_RIG_YAW_OFFSET + afn_rigs[AFN_PLAYER_RIG_SLOT].yawOff;
                float yc = cosf(yr), ys = sinf(yr);   // fwd=(sin,cos), right=(cos,-sin)
                fbAttachX = fbBaseX + fbOx*yc + fbOz*ys;
                fbAttachY = fbBaseY + fbOy;
                fbAttachZ = fbBaseZ - fbOx*ys + fbOz*yc;
            } else {
                // No bone: offset is relative to the player's facing.
                fbAttachX = fbBaseX + fbOx*fbRx + fbOz*fbFx;
                fbAttachY = fbBaseY + fbOy;
                fbAttachZ = fbBaseZ + fbOx*fbRz + fbOz*fbFz;
            }
            if (afn_fb_fire_timer > 0) afn_fb_fire_timer--;   // Is Firing window counts down each frame
            // Charge / release / idle. Charging is now INDEPENDENT of in-flight shots:
            // each release spawns its own pooled projectile (Step Focus Blast advances
            // them all), so a miss never locks out the next cast and several blasts can
            // share the field.
            {
                if (afn_fb_fire_req && afn_fb_level > 0.0f) {
                    // Release with a held charge: spawn a pooled projectile from the
                    // charge state, then free the orb to charge again immediately.
                    float frac = (afn_fb_max > 0) ? afn_fb_level / (float)afn_fb_max : 1.0f;
                    int slot = -1;
                    for (int k = 0; k < AFN_FB_POOL; k++) if (!s_fbPool[k].active) { slot = k; break; }
                    if (slot < 0) {   // pool full: recycle the slot with the least life left
                        slot = 0; for (int k = 1; k < AFN_FB_POOL; k++) if (s_fbPool[k].life < s_fbPool[slot].life) slot = k;
                    }
                    AfnFbShot* s = &s_fbPool[slot];
                    s->active = 1;
                    s->x = afn_fb_x; s->y = afn_fb_y; s->z = afn_fb_z;   // launch from the charge orb spot
                    s->dirx = fbFx;  s->dirz = fbFz;
                    s->scale = afn_fb_scale;
                    s->dmg = (int)(afn_fb_dmg_max * frac); if (s->dmg < 1) s->dmg = 1;
                    s->life = (afn_fb_tgt >= 0) ? afn_fb_life_homing : afn_fb_life_fwd;   // Fire Charge Shot tunables
                    s->tgt = afn_fb_tgt;
                    s->full = (frac >= afn_clash_full_pct * 0.01f) ? 1 : 0;
                    if (s->full) s_pbBeamFull = 1;   // a full-charge beam is now in flight (clash sense)
                    afn_fb_active = 1;
                    afn_fb_fire_timer = 30;                             // Is Firing window: hold the launch anim ~0.5s
                    afn_fb_charging = 0; afn_fb_level = 0.0f;
                } else if (afn_fb_charge_req) {
                    // Charging: grow + carry the orb at chest height in front of the player.
                    afn_fb_charging = 1;
                    afn_fb_level += 1.0f; if (afn_fb_level > afn_fb_max) afn_fb_level = (float)afn_fb_max;
                    float frac = (afn_fb_max > 0) ? afn_fb_level / (float)afn_fb_max : 1.0f;
                    afn_fb_scale = afn_fb_min_scale + (afn_fb_max_scale - afn_fb_min_scale) * frac;
                    afn_fb_x = fbAttachX; afn_fb_y = fbAttachY; afn_fb_z = fbAttachZ;
                } else {
                    // Released without a charge, or idle.
                    afn_fb_charging = 0; afn_fb_level = 0.0f;
                }
            }
        }
#endif

        // Dodge (one-button side roll): a committed PURE LEFT/RIGHT burst, never
        // forward/back. Captured once when the node triggers (input is final
        // here). Only the stick's horizontal component decides the side, so
        // diagonals count (up-left/down-left = left); a neutral or pure-vertical
        // stick defaults to a right dodge. The roll vector is +/- the move-basis
        // RIGHT axis (camera/lock/tank relative — while locked that's
        // perpendicular to player->target, a circle-strafe). Wall-collides; ends
        // when the frame budget runs out.
#ifdef AFN_HAS_PLAYER_RIG
        {
            static float s_dodgeDX = 0.0f, s_dodgeDZ = 0.0f;
            static int s_dodgeClip = 0;
            static int s_dodgeFlip = 1;                  // neutral-press alternator (starts right)
            static int s_dodgeTotal = 0;                 // captured duration (for the ramp)
            static float s_dodgeYaw = 0.0f;              // facing captured at trigger: hold it so directional clips match the roll
            if (afn_dodge_trigger) {
                int af = afn_input_fwd, ar = afn_input_right;
                int afa = af < 0 ? -af : af, ara = ar < 0 ? -ar : ar;
                if (lockStrafing) {
                    // --- Lock-on: target-relative directional roll. The rig faces
                    // the target, so Up/Down/L-R map straight to fwd/back/left/right
                    // clips (vector = +/- the target-relative move basis). ---
                    if (afa > ara && af > 0 && afn_dodge_clip_f >= 0) {
                        s_dodgeDX =  fwdX; s_dodgeDZ =  fwdZ; s_dodgeClip = afn_dodge_clip_f;   // forward
                    } else if (afa > ara && af < 0 && afn_dodge_clip_b >= 0) {
                        s_dodgeDX = -fwdX; s_dodgeDZ = -fwdZ; s_dodgeClip = afn_dodge_clip_b;   // back
                    } else {
                        int left;
                        if (ar < 0)      left = 1;  // stick held left-ish: roll left
                        else if (ar > 0) left = 0;  // right-ish: roll right
                        else { s_dodgeFlip ^= 1; left = s_dodgeFlip; } // neutral: ping-pong L/R each press
                        if (left) { s_dodgeDX = -rgtX; s_dodgeDZ = -rgtZ; s_dodgeClip = afn_dodge_clip_l; }
                        else      { s_dodgeDX =  rgtX; s_dodgeDZ =  rgtZ; s_dodgeClip = afn_dodge_clip_r; }
                    }
                    s_dodgeYaw = moveAngle * (180.0f/3.14159265f);   // (lock holds live target facing below)
                } else {
                    // --- Non-lock: a camera-relative controller turns the model to
                    // FACE its movement, so it's always running forward. A dodge
                    // while moving is therefore a FORWARD dash along the heading
                    // (running ANY way, incl. toward the camera, rolls forward); a
                    // NEUTRAL stick is a backstep (roll backward off the facing). ---
                    if (af || ar) {
                        float l = sqrtf((float)mvX*mvX + (float)mvZ*mvZ);
                        s_dodgeDX = l > 0.0001f ? mvX/l : fwdX;
                        s_dodgeDZ = l > 0.0001f ? mvZ/l : fwdZ;
                        s_dodgeClip = afn_dodge_clip_f >= 0 ? afn_dodge_clip_f : afn_dodge_clip_r;
                    } else {
                        float fy = playerYaw * (3.14159265f/180.0f);
                        s_dodgeDX = -sinf(fy); s_dodgeDZ = -cosf(fy);   // backstep
                        s_dodgeClip = afn_dodge_clip_b >= 0 ? afn_dodge_clip_b : afn_dodge_clip_l;
                    }
                    s_dodgeYaw = playerYaw;   // hold the current heading (no snap; the run flows into the roll)
                }
                s_dodgeTotal = afn_dodge_frames;
                afn_dodge_trigger = 0;
            }
            if (afn_dodge_frames > 0 && !afn_player_frozen) {
                float sp = afn_dodge_speed * 0.08f;
                // Speed envelope (quadratic): ease IN over the first afn_dodge_ramp
                // frames and ease OUT over the last afn_dodge_falloff frames, so the
                // roll accelerates out of a windup and decelerates into the end
                // instead of snapping on/off — softens the stiff feel and the ramp
                // gives a window to time. On overlap the smaller factor wins.
                float env = 1.0f;
                int ramp = afn_dodge_ramp, fall = afn_dodge_falloff;
                if (ramp > s_dodgeTotal) ramp = s_dodgeTotal;
                if (fall > s_dodgeTotal) fall = s_dodgeTotal;
                if (ramp > 0) {
                    float t = (float)(s_dodgeTotal - afn_dodge_frames + 1) / (float)ramp;
                    if (t > 1.0f) t = 1.0f;
                    if (t*t < env) env = t*t;
                }
                if (fall > 0) {
                    float u = (float)afn_dodge_frames / (float)fall;   // remaining frames
                    if (u > 1.0f) u = 1.0f;
                    if (u*u < env) env = u*u;
                }
                sp *= env;
                float ix = s_dodgeDX * sp, iz = s_dodgeDZ * sp;
                int sub = (int)(sp / 3.0f) + 1;          // wall-tunnel guard like normal move
                for (int st = 0; st < sub; st++) { playerX += ix/sub; playerZ += iz/sub; collide_walls(&playerX, &playerZ, playerY); }
                afn_rig_clip = s_dodgeClip;              // hold the dodge clip while rolling
                // Hold facing = the captured roll basis so the directional clip
                // matches the motion (movement above may have re-faced toward the
                // stick). Lock-on keeps its live target facing, set earlier.
                if (!lockStrafing) playerYaw = s_dodgeYaw;
                if (--afn_dodge_frames <= 0) {
                    afn_dodge_frames = 0;
                    // Roll over: hand the rig back to the idle clip so it doesn't
                    // freeze on the dodge pose. Only when standing still — if the
                    // stick is held, Strafe Anim (above) already set the walk clip.
                    if (afn_dodge_idle >= 0 && afn_input_fwd == 0 && afn_input_right == 0)
                        afn_rig_clip = afn_dodge_idle;
                }
            }
            // Spam gate: the node only re-fires when afn_dodge_cd <= 0 (set to the
            // node's Cooldown on a dodge); bleed it down one per frame.
            if (afn_dodge_cd > 0) afn_dodge_cd--;
        }
#endif

        // Node-driven world-axis push velocity (boost pads / knockback). 8.8
        // fixed. BoostForward wrote a pending magnitude -> decompose along the
        // camera forward; SetVelocityX/Z write the globals directly. Linear
        // VelocityFalloff(N) decay over N frames. (Mirrors fps3d.c:1190-1207.)
        if (afn_pending_boost_fwd) {
            afn_player_vx_world = (int)(sinf(camAngle) * afn_pending_boost_fwd);
            afn_player_vz_world = (int)(cosf(camAngle) * afn_pending_boost_fwd);
            afn_pending_boost_fwd = 0;
        }
        if (afn_player_vx_world || afn_player_vz_world) {
            playerX += afn_player_vx_world / 256.0f;
            playerZ += afn_player_vz_world / 256.0f;
            collide_walls(&playerX, &playerZ, playerY);
            if (afn_velocity_falloff > 0) {
                afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
                afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
                if (--afn_velocity_falloff == 0) { afn_player_vx_world = 0; afn_player_vz_world = 0; }
            }
        }

        player_moving = (afn_input_fwd != 0 || afn_input_right != 0);   // IsMoving gate
        // Jump is node-driven: a Jump node sets player_vy (8.8). Capture it once
        // here so both the normal jump and the grind exit (below) react to the
        // same node event — no hardcoded jump button.
        float jumpVel = 0.0f;
        if (player_vy != 0) { jumpVel = player_vy / 256.0f; player_vy = 0; }
        if (jumpVel != 0.0f) { playerVY = jumpVel; grounded = 0; }   // Jump node
        collide_walls(&playerX, &playerZ, playerY);
        {   // Anime-jump envelope: floaty rise (reduced gravity while rising,
            // for hangtime at the apex) + heavier, optionally-eased fall past it.
            static int s_fallFrames = 0;
            float g = afn_gravity / 256.0f;                 // gravity (SetGravity node, 8.8)
            if (playerVY > 0.0f && afn_rise_float > 0)      // Rise Float: float up / hang
                g *= 1.0f - afn_rise_float * 0.01f;
            playerVY -= g;
            if (playerVY < 0.0f) {                          // past the apex: extra downward push
                float extra = afn_fall_force / 256.0f;
                if (afn_fall_smooth > 0) {                  // ease the Fall Force in (quadratic)
                    if (++s_fallFrames > afn_fall_smooth) s_fallFrames = afn_fall_smooth;
                    float t = (float)s_fallFrames / (float)afn_fall_smooth;
                    extra *= t * t;
                }
                playerVY -= extra;
            } else {
                s_fallFrames = 0;
            }
        }
        float term = afn_terminal_vel / 256.0f;
        if (playerVY < -term) playerVY = -term;            // terminal velocity (SetMaxFall node)
        playerY += playerVY;
        {
            int wasGrounded = grounded;
            float fy, fn[3];
            if (collide_floor(playerX, playerZ, playerY, &fy, fn) && playerY <= fy - COL_BOTTOM) {
                playerY = fy - COL_BOTTOM; playerVY = 0.0f; grounded = 1;
                if (!wasGrounded) afn_land_timer = 12;   // just touched down -> open the land-anim window
                // Ease the rig's up-axis toward the floor normal (slope tilt).
                s_floorN[0] += (fn[0]-s_floorN[0])*0.2f;
                s_floorN[1] += (fn[1]-s_floorN[1])*0.2f;
                s_floorN[2] += (fn[2]-s_floorN[2])*0.2f;
            } else {
                grounded = 0;
                s_floorN[0] += (0.0f-s_floorN[0])*0.1f;   // airborne: ease back upright
                s_floorN[1] += (1.0f-s_floorN[1])*0.1f;
                s_floorN[2] += (0.0f-s_floorN[2])*0.1f;
            }
        }
        if (afn_land_timer > 0) afn_land_timer--;   // bleed the land-anim window (Is Landing gate)

#ifdef AFN_HAS_PLAYER_RIG
        // Per-NPC gravity + floor landing (NDS render_npc_rigs parity): enemies
        // fall, settle on the ground, and can be knocked airborne (set s_npcVY>0
        // to launch). Same gravity/terminal as the player; each NPC rests with
        // its authored collision-box bottom on the floor surface. collide_floor
        // tags afn_floor_sprite as a side effect, so save/restore it — the
        // player's grind floor-sprite must not be clobbered by NPC queries.
        {
            int savedFloorSpr = afn_floor_sprite;
            float ng = afn_gravity / 256.0f, nterm = afn_terminal_vel / 256.0f;
            // Enemy projectile flight is node-driven now (Step Enemy Beam in a BP).
            for (int i = 0; i < AFN_NPC_COUNT; i++) {
                int eidx = (int)afn_npc_inst[i][7];
                if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;
                // Enemy combat AI is NODE-DRIVEN now (enemy BP: On Update -> Ai Sense +
                // state dispatch). It runs in script_tick(), not here.
                // True box: half-extents (hx,hy,hz) + center offset (cx,cy,cz),
                // all in world px relative to the NPC origin. Y rests the box
                // bottom (cy-hy) on the floor.
                float hx = afn_npc_col[i][0], hy = afn_npc_col[i][1], hz = afn_npc_col[i][2];
                float cx = afn_npc_col[i][3], cy = afn_npc_col[i][4], cz = afn_npc_col[i][5];
                float nbottom = cy - hy;

#ifdef AFN_HAS_NAVMESH
                // Navigation (editor NPC Navigation section): walk the Detour
                // path toward the goal. Mode 1 = follow player (repath on a
                // timer, stop inside stopDist), mode 2 = wander (new random
                // navmesh goal whenever the current path is exhausted). Only
                // X/Z move here — gravity below keeps the Y honest.
                s_npcNavMoving[i] = 0;
                {
                    int   mode  = (int)afn_npc_nav[i][0];
                    if (eidx == AFN_ENEMY_EIDX && afn_ai_state != AI_ROAM) mode = 0;   // AI drives motion in combat (node-set state)
                    float speed = afn_npc_nav[i][1];
                    float stopD = afn_npc_nav[i][2];
                    int   repat = (int)afn_npc_nav[i][3];
                    // NOTE: do NOT gate on afn_player_frozen here. That flag is a
                    // player-INPUT freeze (FreezePlayer node — used for block
                    // stances, hit-stun, etc.); enemies are world entities and
                    // must keep navigating while the player is locked in place.
                    if (mode != 0 && afn_nav_is_ready()) {
                        float pdx = playerX - s_npcX[i], pdz = playerZ - s_npcZ[i];
                        float pd2 = pdx*pdx + pdz*pdz;
                        if (mode == 1 && pd2 <= stopD*stopD) {
                            s_npcPathLen[i] = 0;           // close enough — idle
                            s_npcRepathT[i] = 0;           // re-engage instantly when player leaves
                        } else if (--s_npcRepathT[i] <= 0 ||
                                   (s_npcPathIdx[i] >= s_npcPathLen[i] && mode == 1)) {
                            float gx = playerX, gy = playerY, gz = playerZ;
                            int ok = 1;
                            if (mode == 2 && (s_npcPathIdx[i] >= s_npcPathLen[i])) {
                                float rp[3];
                                ok = afn_nav_find_random_point(rp);
                                if (ok) { gx = rp[0]; gy = rp[1]; gz = rp[2]; }
                            } else if (mode == 2) {
                                ok = 0;                    // wander: keep current leg until done
                            }
                            if (ok) {
                                s_npcPathLen[i] = afn_nav_find_path(
                                    s_npcX[i], s_npcY[i], s_npcZ[i], gx, gy, gz,
                                    s_npcPath[i], NAV_MAX_WP);
                                s_npcPathIdx[i] = (s_npcPathLen[i] > 1) ? 1 : 0;  // wp 0 = start
                            }
                            s_npcRepathT[i] = (repat > 0) ? repat : 30;
                        }
                        // Advance along the path at speed.
                        while (s_npcPathIdx[i] < s_npcPathLen[i]) {
                            float* wp = &s_npcPath[i][s_npcPathIdx[i] * 3];
                            float dx = wp[0] - s_npcX[i], dz = wp[2] - s_npcZ[i];
                            float d = sqrtf(dx*dx + dz*dz);
                            if (d <= speed) {              // reached this waypoint
                                s_npcX[i] = wp[0]; s_npcZ[i] = wp[2];
                                s_npcPathIdx[i]++;
                                continue;
                            }
                            s_npcX[i] += dx / d * speed;
                            s_npcZ[i] += dz / d * speed;
                            // Ease the facing toward the travel direction
                            // (shortest arc; same deg convention as playerYaw).
                            float want = atan2f(dx, dz) * (180.0f / 3.14159265f);
                            float diff = want - s_npcYaw[i];
                            while (diff > 180.0f) diff -= 360.0f;
                            while (diff < -180.0f) diff += 360.0f;
                            s_npcYaw[i] += diff * 0.25f;
                            s_npcNavMoving[i] = 1;
                            break;
                        }
                    }
                }
#endif
                s_npcVY[i] -= ng;
                if (s_npcVY[i] < -nterm) s_npcVY[i] = -nterm;
                s_npcY[i] += s_npcVY[i];
                float nfy, nfn[3];
                if (collide_floor(s_npcX[i], s_npcZ[i], s_npcY[i], &nfy, nfn)
                    && s_npcY[i] <= nfy - nbottom) {
                    s_npcY[i] = nfy - nbottom; s_npcVY[i] = 0.0f; s_npcGround[i] = 1;
                    // Ease the rig's up-axis toward the floor normal (slope
                    // tilt) — same rates as the player (main player block).
                    s_npcFloorN[i][0] += (nfn[0] - s_npcFloorN[i][0]) * 0.2f;
                    s_npcFloorN[i][1] += (nfn[1] - s_npcFloorN[i][1]) * 0.2f;
                    s_npcFloorN[i][2] += (nfn[2] - s_npcFloorN[i][2]) * 0.2f;
                } else {
                    s_npcGround[i] = 0;
                    s_npcFloorN[i][0] += (0.0f - s_npcFloorN[i][0]) * 0.1f;   // airborne: ease upright
                    s_npcFloorN[i][1] += (1.0f - s_npcFloorN[i][1]) * 0.1f;
                    s_npcFloorN[i][2] += (0.0f - s_npcFloorN[i][2]) * 0.1f;
                }

                // SOLID blocker: push the player (a cylinder of COL_RADIUS) out
                // of the NPC's AABB. True box — independent X/Z half-extents and
                // the box's center offset are honored (circle-vs-AABB resolve).
                if (eidx < 0 || afn_collision_enabled[eidx]) {
                    float bcx = s_npcX[i] + cx, bcz = s_npcZ[i] + cz;
                    float xmin = bcx - hx, xmax = bcx + hx, zmin = bcz - hz, zmax = bcz + hz;
                    float npcBot = s_npcY[i] + cy - hy, npcTop = s_npcY[i] + cy + hy;
                    float plBot  = playerY + COL_BOTTOM, plTop = playerY + COL_TOP;
                    if (plTop > npcBot && plBot < npcTop) {            // vertical overlap
                        float R = COL_RADIUS;
                        if (playerX > xmin - R && playerX < xmax + R &&
                            playerZ > zmin - R && playerZ < zmax + R) {
                            if (playerX > xmin && playerX < xmax &&
                                playerZ > zmin && playerZ < zmax) {
                                // Center inside the core box: eject out the nearest face.
                                float pxl = playerX - xmin, pxr = xmax - playerX;
                                float pzl = playerZ - zmin, pzr = zmax - playerZ;
                                float mx = pxl < pxr ? pxl : pxr, mz = pzl < pzr ? pzl : pzr;
                                if (mx < mz) playerX = (pxl < pxr) ? xmin - R : xmax + R;
                                else         playerZ = (pzl < pzr) ? zmin - R : zmax + R;
                            } else {
                                // Edge/corner: push to the circle boundary off the
                                // closest point on the box.
                                float qx = playerX < xmin ? xmin : (playerX > xmax ? xmax : playerX);
                                float qz = playerZ < zmin ? zmin : (playerZ > zmax ? zmax : playerZ);
                                float dx = playerX - qx, dz = playerZ - qz, d2 = dx*dx + dz*dz;
                                if (d2 < R*R && d2 > 1e-8f) {
                                    float d = sqrtf(d2), k = R / d;
                                    playerX = qx + dx*k; playerZ = qz + dz*k;
                                }
                            }
                        }
                    }
                }
            }
            afn_floor_sprite = savedFloorSpr;
        }
#endif

#if defined(AFN_HAS_HUD) && defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
        clash_sense();  // beam-clash SENSE: set afn_clash_ready (after the AI tick so both release latches are current); the node graph drives begin/struggle/resolve
#endif

#ifdef AFN_HAS_RAIL_PATH
        // Grind rails: StartGrind sets afn_grinding + afn_grind_rail (a sprite
        // index that matches afn_floor_sprite). Engage when on/near that rail's
        // floor, then slide the centerline — faster downhill (grade), launch off
        // the end / on jump with momentum. Float-spirit port of fps3d.c's grind.
        {
            int rail = afn_grind_rail;
            if (!gr_on && afn_grinding && rail >= 0 && rail < NUM_SPRITES
                && afn_rail_count[rail] >= 2 && playerVY <= 0.5f) {
                float d2; float arc = rail_nearest(rail, playerX, playerZ, &d2);
                float gx,gy,gz,tdx,tdz; rail_sample(rail, arc, &gx,&gy,&gz,&tdx,&tdz);
                float catchR = afn_grind_catch_x > 0 ? (float)afn_grind_catch_x : COL_RADIUS*3.0f;
                float vWin   = COL_TOP + (afn_grind_catch_y > 0 ? (float)afn_grind_catch_y : COL_TOP);
                int onRailFloor = (grounded && afn_floor_sprite == rail);
                if ((onRailFloor || d2 <= catchR*catchR) && fabsf(playerY - gy) <= vWin) {
                    gr_on = 1; gr_arc = arc; gr_prevY = gy;
                    float h = sinf(playerYaw*DEG2RAD)*tdx + cosf(playerYaw*DEG2RAD)*tdz;
                    gr_dir = (h >= 0.0f) ? 1 : -1;
                    gr_speed = 12.0f;          // initial slide speed (px/frame)
                    afn_grind_vel = 1;         // flag: grinding (node SFX gates read this)
                }
            }
            if (gr_on) {
                int rs = afn_rail_start[rail], rn = afn_rail_count[rail];
                float total = rail_len(rail);
                if ((jumpVel != 0.0f) || !afn_grinding) {   // jump off (node) or grind disabled
                    float gx,gy,gz,tdx,tdz; rail_sample(rail, gr_arc, &gx,&gy,&gz,&tdx,&tdz);
                    afn_player_vx_world = (int)(tdx * gr_dir * gr_speed * 256.0f);   // launch w/ momentum
                    afn_player_vz_world = (int)(tdz * gr_dir * gr_speed * 256.0f);
                    afn_velocity_falloff = 30;
                    if (jumpVel != 0.0f) playerVY = jumpVel;
                    gr_on = 0; afn_grind_vel = 0; afn_grinding = 0;
                } else {
                    float gx,gy,gz,tdx,tdz; rail_sample(rail, gr_arc, &gx,&gy,&gz,&tdx,&tdz);
                    playerX = gx; playerY = gy - COL_BOTTOM; playerZ = gz; playerVY = 0.0f; grounded = 1;
                    playerYaw = atan2f(tdx*gr_dir, tdz*gr_dir) * (180.0f/3.14159265f);
                    float grade = gr_prevY - gy; gr_prevY = gy;          // +down / -up
                    float gpow = (afn_grind_power ? afn_grind_power : 24) / 256.0f;
                    gr_speed += (grade > 0.0f ? grade * gpow : grade * 0.05f);
                    if (gr_speed < 3.0f)  gr_speed = 3.0f;
                    if (gr_speed > 45.0f) gr_speed = 45.0f;
                    gr_arc += gr_dir * gr_speed;
                    if (gr_arc <= 0.0f || gr_arc >= total) {
                        int term = (gr_arc <= 0.0f) ? 0 : (rn-1);
                        if (afn_rail_pt_bounce[rs+term]) {              // bumper -> reverse
                            gr_dir = -gr_dir; gr_arc = (gr_arc <= 0.0f) ? 0.0f : total;
                        } else {                                       // launch off end
                            float lx,ly,lz,ltx,ltz; rail_sample(rail, gr_arc<=0.0f?0.0f:total, &lx,&ly,&lz,&ltx,&ltz);
                            afn_player_vx_world = (int)(ltx * gr_dir * gr_speed * 256.0f);
                            afn_player_vz_world = (int)(ltz * gr_dir * gr_speed * 256.0f);
                            afn_velocity_falloff = 30;
                            gr_on = 0; afn_grind_vel = 0; afn_grinding = 0;
                        }
                    }
                }
            }
        }
#endif // AFN_HAS_RAIL_PATH

        // Sprite collision -> OnCollision chains. Player circle vs each NPC's
        // position (proximity). Sets afn_collided_sprite to the NPC's editor
        // sprite index, then fires the scene + blueprint collision dispatchers
        // (a blueprint attached to that sprite reacts). 10-frame spawn grace.
        afn_frame_count++;
#ifdef AFN_HAS_SPRITE_IDX
        if (afn_frame_count > 10) {
            for (int i = 0; i < AFN_NPC_COUNT; i++) {
                int sp = (int)afn_npc_inst[i][7];
                if (sp < 0 || sp >= NUM_SPRITES) continue;
                if (!afn_sprite_visible[sp] || !afn_collision_enabled[sp]) continue;
                // OnCollision trigger: player cylinder (COL_RADIUS) vs the NPC's
                // true AABB (independent X/Z extents + center offset + settled Y),
                // matching the solid-blocker test above.
#ifdef AFN_HAS_PLAYER_RIG
                float hx = afn_npc_col[i][0], hy = afn_npc_col[i][1], hz = afn_npc_col[i][2];
                float cx = afn_npc_col[i][3], cy = afn_npc_col[i][4], cz = afn_npc_col[i][5];
                float bcx = s_npcX[i] + cx, bcz = s_npcZ[i] + cz;
                float xmin = bcx - hx, xmax = bcx + hx, zmin = bcz - hz, zmax = bcz + hz;
                float qx = playerX < xmin ? xmin : (playerX > xmax ? xmax : playerX);
                float qz = playerZ < zmin ? zmin : (playerZ > zmax ? zmax : playerZ);
                float ddx = playerX - qx, ddz = playerZ - qz;
                float npcBot = s_npcY[i] + cy - hy, npcTop = s_npcY[i] + cy + hy;
                float plBot = playerY + COL_BOTTOM, plTop = playerY + COL_TOP;
                int hit = (ddx*ddx + ddz*ddz < COL_RADIUS*COL_RADIUS) && (plTop > npcBot && plBot < npcTop);
#else
                float dx = playerX - afn_npc_inst[i][0], dz = playerZ - afn_npc_inst[i][2];   // no rig: static NPCs
                float dy = playerY - afn_npc_inst[i][1];
                int hit = (dx*dx + dz*dz < (COL_RADIUS*2)*(COL_RADIUS*2)) && (dy > -COL_TOP && dy < COL_TOP);
#endif
                if (hit) {
                    afn_collided_sprite = sp;
                    afn_emitted_script_collision();
                    afn_bp_dispatch_collision();
                }
            }
        }
#endif

        // Follow the player, framed at ~half the camera height (torso, not feet).
        // NDS fps3d.c parity (1746-1863): the camera position CHASES its orbit
        // point with the walk/sprint/orbit ease rates instead of sitting rigidly
        // at it, the XZ radius is re-projected onto the orbit circle so the lag
        // reads as an angular glide (not a zoom), and the Y follow is quick on
        // the way down (landing) but lazy in the air so the camera trails a beat
        // behind a jump's apex.
        static float s_camEyeX, s_camEyeZ;     // eased camera XZ (NDS cam_x/cam_z)
        static float s_camFollowY;             // smoothed player-Y follow (NDS s_camYSmooth)
        static float s_camPosPitch;            // eased POSITION pitch (rad) — view pitch snaps
        static int   s_camEyeInit = 0;

        {   // Smooth Y follow — AFN_JUMP_CAM_LAND grounded / AFN_JUMP_CAM_AIR airborne.
            if (!s_camEyeInit) s_camFollowY = playerY;
            float dy = playerY - s_camFollowY;
            int yrate = grounded ? AFN_JUMP_CAM_LAND : AFN_JUMP_CAM_AIR;
            s_camFollowY += dy * ((float)yrate / 256.0f);
            if (dy > -0.05f && dy < 0.05f) s_camFollowY = playerY;
        }
        float targetX = playerX, targetY = s_camFollowY + camHeight * 0.5f, targetZ = playerZ;

        // Lock-on framing (over-the-shoulder): while a target is locked, ease
        // in a slight zoom-out + lateral camera shift so the player sits to one
        // side and the target frames toward center. Shifting the LOOK POINT
        // sideways pans eye+target together (the eye eases to target - viewDir*R),
        // so it's a pure lateral offset, not a rotation. Eased so it blends on
        // lock/unlock. Node-driven via the Lock On node's sliders (afn_lock_side /
        // afn_lock_zoom / afn_lock_zoom_in below).
        float effDist = camDist;
#ifdef AFN_HAS_CAM_LOCK
        {
            static float s_lockFrame = 0.0f;     // 0..1 lock blend
            static float s_lockSideEased = 0.0f; // eased signed lateral offset (smooths flips)
            static float s_lockSideSign = 1.0f;  // which side the player sits on
            const float LOCK_SIDE = (float)afn_lock_side;   // world px lateral shift (Lock On slider)
            // Signed zoom fraction: +out (pull back) / -in (pull closer).
            const float LOCK_ZOOM = (afn_lock_zoom_in ? -afn_lock_zoom : afn_lock_zoom) / 100.0f;
            float want = (afn_cam_lock_target >= 0) ? 1.0f : 0.0f;
            s_lockFrame += (want - s_lockFrame) * 0.10f;
#ifdef AFN_HAS_PLAYER_RIG
            // Pick the side from where the target sits relative to camera
            // forward: keep the player on the OPPOSITE side so it never blocks
            // the target. Hysteresis (0.15 rad) stops jitter near center; the
            // eased offset below smooths the actual switch. (Flip the two signs
            // if the player ends up on the wrong side.)
            if (afn_cam_lock_target >= 0) {
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_cam_lock_target) {
                        float dd = atan2f(s_npcX[n] - playerX, s_npcZ[n] - playerZ) - camAngle;
                        while (dd >  3.14159265f) dd -= 6.2831853f;
                        while (dd < -3.14159265f) dd += 6.2831853f;
                        if (dd >  0.15f) s_lockSideSign =  1.0f;
                        else if (dd < -0.15f) s_lockSideSign = -1.0f;
                        break;
                    }
            }
#endif
            float sideTgt = LOCK_SIDE * s_lockSideSign * s_lockFrame;
            s_lockSideEased += (sideTgt - s_lockSideEased) * 0.08f;   // smooth shift + side switch
            if (s_lockFrame > 0.001f) {
                float rX = cosf(camAngle), rZ = -sinf(camAngle);   // camera right in XZ
                targetX += rX * s_lockSideEased;
                targetZ += rZ * s_lockSideEased;
                effDist  = camDist * (1.0f + LOCK_ZOOM * s_lockFrame);
                if (effDist < camDist * 0.25f) effDist = camDist * 0.25f;   // don't collapse on big zoom-in
            }
        }
#endif

        // Orbit Cam (KO / death cinematic and any node-triggered orbit). NODE-DRIVEN:
        // Orbit Cam On Obj begins it (target box center, zoom-in, slow orbit), Orbit
        // Cam Step advances it, Stop Orbit Cam / a scene swap ends it. The eye/effDist/
        // pitch easers below glide into and out of it so the transition is smooth.
        if (afn_cam_orbit_active) {
            float ox = playerX, oz = playerZ, oy = playerY + camHeight * 0.5f;
#ifdef AFN_HAS_SPRITE_IDX
            if (afn_cam_orbit_obj >= 0)
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_cam_orbit_obj) {
                        ox = s_npcX[n]; oz = s_npcZ[n]; oy = s_npcY[n] + afn_npc_col[n][4]; break;
                    }
#endif
            targetX = ox; targetZ = oz; targetY = oy + (float)afn_cam_orbit_lookh;   // Orbit Cam Look Height
            camAngle = afn_cam_orbit_angle0 + (afn_cam_orbit_rate_mr * 0.001f) * (float)afn_cam_orbit_timer;
            effDist += (camDist * (afn_cam_orbit_zoom_pct * 0.01f) - effDist) * (afn_cam_orbit_ease_pm * 0.001f);
            pitch = afn_cam_orbit_pitch_cr * 0.01f;
            // (timer advance is node-driven now — the Orbit Cam Step node)
        }

        // Ease rate from what the player is doing: Sprint node sets
        // afn_speed_prio this tick (script ran above, so it's current);
        // orbit input picks the max so the camera never lags behind.
        int camMoving = (afn_input_fwd != 0 || afn_input_right != 0);
        int camEase = (afn_speed_prio != 0)
            ? (camMoving ? AFN_SPRINT_EASE_IN : AFN_SPRINT_EASE_OUT)
            : (camMoving ? AFN_WALK_EASE_IN   : AFN_WALK_EASE_OUT);
        if (orbitingNow) { if (AFN_ORBIT_EASE_IN  > camEase) camEase = AFN_ORBIT_EASE_IN; }
        else             { if (AFN_ORBIT_EASE_OUT > camEase) camEase = AFN_ORBIT_EASE_OUT; }

        float cp = cosf(pitch);                // VIEW pitch — snaps with orbit_pitch
        {   // Pitch chase: orbit up/down gets the same delay as yaw. The eye's
            // height/radius ease toward the target pitch while the VIEW pitch
            // snaps, mirroring the yaw behavior (position lags, direction
            // doesn't), so pitching slides the player off-center vertically
            // and re-centers as the chase catches up.
            if (!s_camEyeInit) s_camPosPitch = pitch;
            float dp = pitch - s_camPosPitch;
            s_camPosPitch += dp * ((float)camEase / 256.0f);
            if (dp > -0.002f && dp < 0.002f) s_camPosPitch = pitch;
        }
        float horizR = cosf(s_camPosPitch) * effDist;   // orbit-circle radius in XZ (effDist = lock zoom)
        {
            float tgtEx = targetX - sinf(camAngle)*horizR;
            float tgtEz = targetZ - cosf(camAngle)*horizR;
            if (!s_camEyeInit) { s_camEyeX = tgtEx; s_camEyeZ = tgtEz; s_camEyeInit = 1; }
            float ddx = tgtEx - s_camEyeX, ddz = tgtEz - s_camEyeZ;
            if (ddx > -0.0625f && ddx < 0.0625f && ddz > -0.0625f && ddz < 0.0625f) {
                s_camEyeX = tgtEx; s_camEyeZ = tgtEz;
            } else {
                int ease = camEase;
                s_camEyeX += ddx * ((float)ease / 256.0f);
                s_camEyeZ += ddz * ((float)ease / 256.0f);
            }
            // Re-project onto the orbit circle: the lerp cuts across the chord
            // while the target sweeps the circle, which would pull the camera
            // inward (reads as zoom-in, then zoom-out on settle). Snapping the
            // radius back keeps the angular glide but holds distance constant.
            float rdx = s_camEyeX - targetX, rdz = s_camEyeZ - targetZ;
            float rlen = sqrtf(rdx*rdx + rdz*rdz);
            if (rlen > 1.0f) {
                float k = horizR / rlen;
                s_camEyeX = targetX + rdx*k;
                s_camEyeZ = targetZ + rdz*k;
            }
        }
        float ex = s_camEyeX;
        float ez = s_camEyeZ;
        float ey = targetY + sinf(s_camPosPitch)*effDist;   // eased pitch: eye height lags too
#ifdef AFN_HAS_CAM_WALL
        // Wall-aware: if a wall, ceiling OR floor is between the player and the
        // orbit eye, pull the eye in to just in front of it so the camera doesn't
        // clip through (you keep seeing the player) — incl. the ground when the
        // camera pitches low. Per-frame only — the eased orbit state is untouched,
        // so the camera springs back out when the obstruction clears.
        if (afn_cam_wall_aware && afn_current_mode != 1) {
            // Ray from the PLAYER (not the look point) to the eye, so it keeps the
            // player visible even in lock-on — which shifts the look point sideways
            // (over-the-shoulder), leaving the player off the look-point->eye ray.
            // In free-orbit the look point == player, so this is unchanged there.
            float ox = playerX, oy = targetY, oz = playerZ;
            float t = collide_ray_walls(ox, oy, oz, ex, ey, ez);
            if (t > 0.0f && t < 1.0f) {
                float dxw=ex-ox, dyw=ey-oy, dzw=ez-oz;
                float segLen = sqrtf(dxw*dxw+dyw*dyw+dzw*dzw);
                float ft = t - (segLen > 1.0f ? 8.0f / segLen : 0.0f);   // 8px in front of the wall
                if (ft < 0.12f) ft = 0.12f;                              // never collapse onto the player
                ex = ox + dxw*ft; ey = oy + dyw*ft; ez = oz + dzw*ft;
            }
        }
#endif
        g_camEyeX = ex; g_camEyeZ = ez; g_camEyeValid = 1;   // origin for Is In View FOV test

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 2D menu scene (afn_current_mode == 1): skip the entire 3D world
        // (sky/meshes/rigs/billboards) and just draw the HUD over the clear
        // color. Scripts + BP instances already ticked this frame, so cursor/
        // menu logic and scene changes still run.
        if (afn_current_mode != 1) {

        // Projection.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        {
            // near small (1.0) so the slope — which starts close to the camera —
            // isn't near-clipped; far pulled in to 1500 for better 16-bit depth
            // precision at the floor/slope intersection without touching near.
            const float nearp = 1.0f, farp = 1500.0f, aspect = SCR_W / SCR_H;
            const float top = nearp * 0.767f;     // tan(37.5 deg) ~ vfov 75
            const float right = top * aspect;
            glFrustum(-right, right, -top, top, nearp, farp);
        }

        // View. NDS parity: the view DIRECTION snaps with orbit_angle/pitch
        // every frame (fps3d.c does cam_angle = orbit_angle) — only the camera
        // POSITION lags. So aim along yaw/pitch from the eased eye instead of
        // look-at-ing the player: under camera delay the player slides
        // off-center on screen and re-centers as the chase catches up. (A
        // look-at would re-aim at the player every frame and the whole view
        // would just glide — no visible delay.) When fully caught up this is
        // the exact same view as the old look-at(player).
        glMatrixMode(GL_MODELVIEW);
        float view[16];
        float fwdVX = sinf(camAngle) * cp;
        float fwdVY = -sinf(pitch);
        float fwdVZ = cosf(camAngle) * cp;
        look_at(view, ex, ey, ez, ex + fwdVX, ey + fwdVY, ez + fwdVZ, 0.0f, 1.0f, 0.0f);
#ifdef AFN_HAS_HUD
        memcpy(s_hudSceneView, view, sizeof(s_hudSceneView));   // for world-anchored HUD elements
        s_hudSceneViewValid = 1;
        s_hudCamDist = camDist;                                 // distance-scale reference (100%)
#endif

#ifdef AFN_HAS_SKY
        sky_render(camAngle);   // far panorama behind everything (no depth write)
#endif

        // Intersecting/coplanar level meshes (floor vs slope) shimmer on PSV
        // but not NDS/PSP: those consoles' coarse fixed-point depth quantizes
        // two faces in the same plane to EQUAL values, so LEQUAL resolves to a
        // stable winner — the Vita's float32 depth (vitaGL allocates DF32M)
        // never quantizes equal, so the per-pixel winner flips as the camera
        // glides. Fix: give each mesh INSTANCE its own constant depth bias
        // (units only — a slope FACTOR rescales with camera angle and crawls,
        // see reverted aa1b2e4; identical offsets on every mesh separate
        // nothing). Near-equal-depth fights between two meshes then always
        // resolve the same way. The per-instance step is far below visible
        // displacement but far above interpolation noise.
        glEnable(GL_POLYGON_OFFSET_FILL);
        for (int si = 0; si < afn_sprite_count; si++) {
            int mi = afn_sprites[si].meshIdx;
            if (mi < 0 || mi >= afn_mesh_count) continue;
#ifdef AFN_HAS_SPRITE_IDX
            int eidx = afn_mesh_inst_sprite[si];
            if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
            const AfnSpriteInst* sp = &afn_sprites[si];
            // 128 units ~= one PSP D16 depth quantum (2^-16 ~= 128 float-depth
            // ULPs near z=1) — the PSP proves that step is invisible yet big
            // enough to separate these faces.
            glPolygonOffset(0.0f, (float)((si + 1) * 128));   // later instances sit a hair deeper
            glLoadMatrixf(view);
            glTranslatef(sp->x, sp->y, sp->z);
            if (sp->rotZ != 0.0f) glRotatef(sp->rotZ, 0,0,1);
            if (sp->rotX != 0.0f) glRotatef(sp->rotX, 1,0,0);
            if (sp->rotY != 0.0f) glRotatef(sp->rotY, 0,1,0);
            glScalef(sp->scale, sp->scale, sp->scale);
            draw_mesh(mi);
        }
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);

#ifdef AFN_HAS_PLAYER_RIG
        // Player rig + every NPC: each skinned from its own rig at its own
        // transform/clip (player follows the camera; NPCs at their world spots).
        rigs_render(view, playerX, playerY, playerZ, playerYaw, s_floorN);
#endif

#ifdef AFN_HAS_SPRITES
        billboards_render(view, camAngle, ex, ez);   // camera-facing animated/directional sprites
#endif
#ifdef AFN_HAS_PLAYER_RIG
        enemy_orb_render(view);   // HARDCODED: enemy projectile orb (charge spot / in flight)
#endif

        }   // end 3D world (skipped in 2D menu mode)

#ifdef AFN_HAS_HUD
        hud_render();   // 2D overlay (pieces/text/cursor) — always, on top of 3D or as the menu
#endif

        // Fade overlay: a fullscreen quad over the scene. level<0 darkens (fade
        // out to black), level>0 brightens (fade in from white). |level|/16 alpha.
        if (afn_fade_level != 0) {
            int lv = afn_fade_level; if (lv > 16) lv = 16; if (lv < -16) lv = -16;
            unsigned int A = (unsigned int)((lv < 0 ? -lv : lv) * 255 / 16);
            unsigned int col = (A << 24) | (lv > 0 ? 0x00FFFFFFu : 0u);   // 0xAABBGGRR
            AfnVertex fq[4] = { {0,0,col,0,0,0}, {0,0,col,1,0,0}, {0,0,col,1,1,0}, {0,0,col,0,1,0} };
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,0,1,-1,1);
            glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
            glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            AfnVertex* v = fq;
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
            glVertexPointer(3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glEnable(GL_DEPTH_TEST);
        }

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}

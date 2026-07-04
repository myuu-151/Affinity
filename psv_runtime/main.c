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
// Enemy melee reflex (Quick Attack + jump-evade) — NODE-DRIVEN via the Ai Quick Attack
// node, which sets these afn_qa_* tunables each frame from its pins (defaults below match
// the original #defines). Frame budgets stay compile-time.
//  - Quick Attack: occasional dash-in melee mirroring the player's (lunge/skid clips).
//  - Jump-evade: hop a player Quick Attack that's dashing straight at the enemy.
#define ENEMY_QA_MAX          26        // dash frame budget (whiff timeout)
#define ENEMY_QA_SKID         12        // skid recovery frames
// NB: prefix is afn_eqa_ (Enemy QA) — the player's Quick Attack already owns the
// afn_qa_* names (afn_qa_range/speed/dmg/cd below at the player block). Don't merge.
int afn_eqa_range       = 70;   // dash-in only when the player is within this range (px)
int afn_eqa_chance_m    = 12;   // per-frame trigger chance, per-1000 (12 = 0.012)
int afn_eqa_speed       = 34;   // dash speed feed (matches the player's QA)
int afn_eqa_stop        = 14;   // contact range — deal damage to the player (px)
int afn_eqa_dmg         = 8;    // melee damage
int afn_eqa_cd          = 90;   // cooldown after a dash
int afn_eqa_jump_vel_m  = 150;  // jump-evade launch velocity, per-100 (150 = 1.5 = player's jump arc)
int afn_eqa_jump_chance = 65;   // RESERVED/UNUSED: future chance to evade (the reflex always jumps)
int afn_eqa_jump_cd     = 40;   // jump-evade cooldown
#define ENEMY_BAR_HEIGHT      18.0f     // world units above the NPC origin
#define ENEMY_BAR_W           64.0f     // HUD pixels (960x544 space)
#define ENEMY_BAR_H           7.0f
enum { AI_ROAM = 0, AI_CHASE, AI_STRAFE, AI_CHARGE, AI_FIRE, AI_DODGE, AI_DEAD, AI_BLOCK };
static int   s_aiTimer = 0, s_aiLoseT = 0, s_aiInited = 0;
static int   s_aiStrafeDir = 1, s_aiStrafeLeg = 0, s_aiAtkCD = 0, s_aiDodgeCD = 0, s_aiChargeShot = 0;
static int   s_eChargeVoice = -1;   // the enemy charge SFX's own voice (instance 4 is shared with the player's chargefocus)
static int   s_efbActive = 0, s_efbCharging = 0, s_efbDmg = 0, s_efbLife = 0;   // enemy projectile
static float s_efbX, s_efbY, s_efbZ, s_efbDirX = 0, s_efbDirZ = 1, s_efbScale = 0.05f, s_efbSpeed = 0, s_efbHoming = 0;
static int   s_eDodgeFrames = 0, s_eDodgeTotal = 0, s_eDodgeClip = 28;          // enemy dodge roll (28 = DodgeL)
static float s_eDodgeDX = 0, s_eDodgeDZ = 0;
// Charge-dodge: the enemy sidesteps an incoming blast WHILE charging — keeps the charge
// alive (no state switch) and plays the player's charge-dodge clips, instead of the old
// behaviour where a dodge dropped the charge entirely. Clips HARDCODED (drift-protected
// like the other enemy clips; re-point via Ai Clips if the glTF re-sorts).
static int   s_eChgDodgeFrames = 0, s_eChgDodgeTotal = 0, s_aiChgDodgeCD = 0, s_eChgDodgeClip = 9;
static float s_eChgDodgeDX = 0, s_eChgDodgeDZ = 0;
int afn_ai_chgdodge_clip_l = 9, afn_ai_chgdodge_clip_r = 10;   // atk_spc_chg_dodge_L / _R
// Enemy's OWN standard-dodge clips (DodgeL/R). MUST NOT reuse the player's afn_dodge_clip_l/r:
// player combat actions (block/launch/charge) rewrite those shared globals to other clip sets
// (29/28, even the charge-dodge 10/9), which swapped the enemy's dodge clips mid-fight.
int afn_ai_dodge_clip_l = 28, afn_ai_dodge_clip_r = 29, afn_ai_dodge_clip_f = 27, afn_ai_dodge_clip_b = 26;   // DodgeL / DodgeR / DodgeFW / DodgeBWD
// HARDCODED melee-reflex state (enemy Quick Attack + jump-evade).
static int   s_eqaPhase = 0, s_eqaFrames = 0, s_eqaCD = 0, s_eqaDealt = 0;       // enemy QA dash/skid
static float s_eqaDirX = 0.0f, s_eqaDirZ = 1.0f, s_eqaYaw = 0.0f;
static int   s_eJumpCD = 0, s_ePrevPlayerQA = 0;                                 // jump-evade cooldown + edge
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
float afn_ai_orb_min = 0.05f, afn_ai_orb_max = ENEMY_CHG_SCALE;   // Ai Orb Scale node: enemy focus-orb charge seed / full size
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
// Enemy AI decision/timing knobs — NODE-DRIVEN (AI Timing node sets these each frame
// before AI Sense). Defaults = the original ENEMY_* #defines, so no node / unwired
// pins = unchanged behaviour. The state machine itself lives in the enemy BP graph.
int afn_ait_lose_frames  = 150;  // outside Lose Range this many frames -> back to roam
int afn_ait_strafe_leg   = 90;   // frames before re-rolling strafe direction
int afn_ait_yaw_ease_m   = 35;   // x100: turn-to-face lerp (0.35)
int afn_ait_tap_windup   = 12;   // quick-shot (tap) charge frames
int afn_ait_fire_recover = 18;   // post-launch recovery frames
int afn_ait_dodge_frames = 20;   // dodge roll duration
int afn_ait_dodge_cd     = 45;   // frames between dodges
// Enemy dodge roll feel — default -1 = INHERIT the player's Dodge-node values
// (afn_dodge_speed/ramp/falloff), so the enemy rolls identically to the player; set
// >=0 on the AI Timing node to give the enemy its own feel.
int afn_ait_dodge_speed   = -1;  // -1 = inherit afn_dodge_speed
int afn_ait_dodge_ramp    = -1;  // -1 = inherit afn_dodge_ramp
int afn_ait_dodge_falloff = -1;  // -1 = inherit afn_dodge_falloff
// Enemy AI animation clip indices — NODE-DRIVEN (AI Clips node, name-resolved at export
// so they survive a glTF re-sort). Defaults = the current 44-anim indices, so no node =
// unchanged. Used by afn_ai_roam/chase/strafe/charge_step/fire_recover/block + reflex.
int afn_aic_move = 36, afn_aic_idle = 30;
int afn_aic_strafe_l = 37, afn_aic_strafe_ld = 38, afn_aic_strafe_ldfw = 39;
int afn_aic_strafe_r = 40, afn_aic_strafe_rd = 41, afn_aic_strafe_rdfw = 42;
int afn_aic_backpeddle = 21, afn_aic_block = 22;
int afn_aic_charge = 4, afn_aic_launch = 19;
int afn_aic_lunge = 34, afn_aic_skid = 35, afn_aic_jump = 31, afn_aic_jumpfall = 32;
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
#define AFN_PAUSE_ELEM     16    // 'pause' overlay element (exported HUD index — last in the list)
#define AFN_SND_SELECT      2    // 'select' sound instance (confirm/pause SFX)
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
// Enemy combat SFX — the SAME instances the player uses, played proximity-gained so
// you can hear the enemy's attacks from across the arena (afn_play_sfx_inst_gain).
#define AFN_SND_QSWEEP     17    // 'quicksweep' — Quick Attack dash whoosh
#define AFN_SND_SMALLBLAST  6    // 'smallblast' — tap (uncharged) beam launch
#define AFN_SND_MIDBLAST    7    // 'midblast' — charged beam launch
#define AFN_SND_BCHARGE     4    // 'charge' — beam charge wind-up
// Enemy combat SFX instances — NODE-DRIVEN: set each frame by the AI nodes' SFX pins
// (AI Charge Begin -> Charge SFX, AI Fire Beam -> Charged/Tap SFX, Ai Quick Attack ->
// Whoosh SFX). Defaults below = the original hardcoded instances, so unwired pins keep
// the exact same sounds. The proximity-gain (afn_enemy_sfx_gain) + charge-loop voice
// tracking (s_eChargeVoice) stay in the runtime as audio infra — only WHICH instance
// plays is node-chosen, so the audio behaviour is unchanged.
int afn_ai_sfx_charge = AFN_SND_BCHARGE;    // charge wind-up (looping, voice-tracked)
int afn_ai_sfx_shoot  = AFN_SND_SHOOT;      // full-charge blast launch
int afn_ai_sfx_tap    = AFN_SND_SMALLBLAST; // tap (uncharged) blast launch
int afn_ai_sfx_whoosh = AFN_SND_QSWEEP;     // Quick Attack dash whoosh
int  afn_play_sfx_inst_gain(int inst, int gain);   // audio.c — play SFX instance, returns its voice idx
void afn_stop_sfx_inst(int inst);                  // audio.c — stop a looping SFX instance (all voices)
void afn_stop_looping_sfx(void);                   // audio.c — stop ALL looping SFX voices (keeps one-shots/music); used by Lock Player Functions
int  afn_sfx_active_inst(int inst);                // audio.c — is the instance's sample playing
void afn_set_sfx_pitch_inst(int inst, int pitch);  // audio.c — pitch an SFX instance's sample
int  afn_inst_voice_active(int v, int inst);       // audio.c — is voice v still this instance's sample
void afn_stop_inst_voice(int v, int inst);         // audio.c — stop ONLY voice v (this instance's sample)
extern int afn_sfx_protect_voice;                  // audio.c — voice a sample-wide stop must NOT cut (the enemy charge loop)
// Distance-attenuated gain (0..127) for enemy combat SFX: full up close, floored so
// it stays faintly audible across the arena. dist = enemy->player distance (afn_ai_dist).
static int afn_enemy_sfx_gain(float dist) {
    const float FAR = 700.0f;
    float t = 1.0f - dist / FAR;
    if (t < 0.18f) t = 0.18f;
    if (t > 1.0f)  t = 1.0f;
    return (int)(127.0f * t + 0.5f);
}
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
// All clash FEEL is NODE-DRIVEN (Beam Clash node -> afn_clash_* globals below): full-
// charge threshold, player push, meet radius, air-fallback, clash damage %, and the AI
// masher (push / interval / jitter / fumble / punish). The old AFN_CLASH_* tunable
// #defines (window/full_frac/push/ai_tap/ai_min/ai_jit/ai_fumble*/meet_r/air_fallback)
// were removed here — all dead, superseded by those globals. The masher is tuned to a
// high-skill CPU trying to WIN (relentless ~15 presses/sec, near-zero fumbles); set the
// node's AI Push / AI Interval aggressive in the project.
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

// ---------------------------------------------------------------------------
// Particle system — pure code: each particle is a billboard whose motion is
// vector-integrated every frame (velocity + gravity), with size/colour lerped
// over its life. Fixed 60 Hz step, so velocities are world-units PER FRAME (no
// dt). The Spawn Particles node fills the per-frame spawn request below; the
// main loop emits at the player, integrates the pool, and afn_particles_render
// billboards each live one camera-facing (same right/up basis as enemy_orb_render).
// Spline pathing (afn_part_spline) layers on top in phase 2.
// ---------------------------------------------------------------------------
#define AFN_PART_POOL 256
typedef struct {
    unsigned char active, blend;        // blend: 0 = alpha, 1 = additive
    short  frame;                       // sprite frame for the quad (-1 = solid)
    float  x, y, z, vx, vy, vz;
    float  life, maxLife, grav;
    float  size0, size1;                // start/end size (world units), lerp by age
    unsigned char r0,g0,b0,a0, r1,g1,b1,a1;  // start/end colour
} AfnParticle;
static AfnParticle s_parts[AFN_PART_POOL] = {{0}};
static unsigned s_partRng = 0x9E3779B9u;
static inline float part_rand(void) {   // 0..1, xorshift (runtime-only RNG)
    s_partRng ^= s_partRng << 13; s_partRng ^= s_partRng >> 17; s_partRng ^= s_partRng << 5;
    return (float)(s_partRng & 0xFFFFFFu) / (float)0x1000000u;
}
static inline float part_sym(void) { return part_rand() * 2.0f - 1.0f; }  // -1..1

// Spawn request — Spawn Particles node sets these in script_tick; the main loop
// emits afn_part_spawn particles at the player (+offset) then clears the count.
int   afn_part_spawn   = 0;          // particles to emit THIS frame (0 = none)
int   afn_part_frame   = -1;         // billboard sprite frame (-1 = solid quad)
int   afn_part_blend   = 1;          // 0 = alpha, 1 = additive
float afn_part_speed   = 1.5f;       // initial speed (world units/frame)
float afn_part_spread  = 0.6f;       // lateral spread (0 = straight up .. 1 = wide cone)
float afn_part_life    = 40.0f;      // lifetime (frames)
float afn_part_grav    = 0.04f;      // downward accel (units/frame^2)
float afn_part_size0   = 0.5f;       // start size  (world units)
float afn_part_size1   = 0.0f;       // end size
unsigned afn_part_col0 = 0xFFFFFFFFu;// start colour 0xAABBGGRR
unsigned afn_part_col1 = 0x00FFFFFFu;// end colour (alpha 0 = fade out)
float afn_part_ox = 0.0f, afn_part_oy = 14.0f, afn_part_oz = 0.0f;  // spawn offset from player

// ---------------------------------------------------------------------------
// Beam / lightning ribbon — a CONNECTED strip (vs the independent particle dots).
// A path of points from source to target, each pushed sideways (perpendicular to
// the path IN SCREEN SPACE) by a fixed bow (the arc) + random jitter that decays to
// 0 at both ends (so it stays anchored). The jitter re-rolls every frame = crackle.
// Rendered as a camera-facing triangle strip, additive (glows on its own, no texture).
// All pure vector math. The impact flash at the end is the particle pool above.
// ---------------------------------------------------------------------------
#define AFN_BEAM_POOL 4
#define AFN_BEAM_MAX_SEGS 48
#define AFN_BEAM_FILAMENTS_MAX 12   // upper bound on bundled crackling strands
typedef struct {
    int   active, life, maxLife;
    float sx, sy, sz, tx, ty, tz;     // source / target (world)
    float width, bow, jitter, decay, pulse;
    int   segs, bounces;
    unsigned col;                     // core colour 0xAABBGGRR
    const float (*spts)[3];           // authored spline (normalised x,y,th), or NULL = parametric bounce
    int   nspts;
    int   travel;                     // 1 = crawl a bright packet from source->target over its life
    int   travelBounces;              // how many times the spline tiles across the floor (decaying) before it fizzles
    float travelPersist;              // fraction of the path the lit ribbon trails behind the head (0..1)
    float travelFade;                 // over what fraction of the END of its life it fades out (0 = no fade)
    int   filaments;                  // # bundled crackling strands (1..AFN_BEAM_FILAMENTS_MAX)
    float orbSize;                    // head-orb radius multiplier (0 = no orb)
} AfnBeam;
static AfnBeam s_beams[AFN_BEAM_POOL] = {{0}};

// Spawn request — the Lightning Beam node sets these in script_tick; the main loop
// resolves source = player chest, target = lock-on enemy (else forward*range), spawns.
int   afn_beam_spawn  = 0;            // 1 = cast a bolt THIS frame
int   afn_beam_life   = 12;          // frames it lasts (flickering)
int   afn_beam_range  = 80;          // forward distance if no lock target (world units)
int   afn_beam_segs   = 14;          // jaggedness (coil resolution)
float afn_beam_width  = 0.4f;        // ribbon half-width (world units)
float afn_beam_bow    = 6.0f;        // fixed arc bow (perpendicular bulge)
float afn_beam_jitter = 3.0f;        // random zigzag amount (lightning crackle)
int   afn_beam_bounces = 3;          // arches the bolt makes across the floor (touches down between each)
float afn_beam_decay  = 0.78f;       // each bounce reaches this fraction of the previous height (1 = even)
float afn_beam_pulse  = 0.018f;      // animated "ball" travels along the arcs at this speed (frac/frame; 0 = none)
unsigned afn_beam_col = 0xFFFFFFFFu; // core colour
int   afn_beam_filaments = 5;        // # bundled crackling strands (electric bundle look)
float afn_beam_orb = 1.0f;           // head-orb radius multiplier (0 = no orb)
// Authored spline override (Play Effect node / effect layer) — when set, the bolt
// follows these normalised control points (x 0..1 along path, y 0..1 height) instead
// of the parametric parabolic bounce. Cleared by afn_beam_resolve after one cast.
const float (*afn_beam_spline)[3] = 0;
int afn_beam_spline_n = 0;
int afn_beam_travel = 0;             // 1 = a bright packet crawls source->target over the bolt's life
int afn_beam_travel_bounces = 3;     // spline tiles this many times across the floor (decaying) before fizzling
float afn_beam_travel_persist = 0.30f; // fraction of the path the lit ribbon trails behind the crawling head
float afn_beam_travel_fade = 0.35f;  // fade only over this last fraction of the life (0 = stays full bright, no fade)
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
    s_eqaPhase = 0; s_eqaCD = 0; s_eJumpCD = 0; s_ePrevPlayerQA = 0;   // clear melee reflexes
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

// HARDCODED Quick Attack afterimage (player only): faint cyan ghost silhouettes of
// the rig at recent trail positions, to sell the dash speed. Reuses the CURRENT
// skinned pose (s_skinned) drawn at past world positions; overwrites s_skinned
// colors with a flat tint — skin() resets them to white next frame, so no
// save/restore needed. Untextured, alpha-blended, depth-write off so the trail
// layers behind the live player. Could become a Quick Attack node tunable later.
#define PA_TRAIL_N 16
static float s_paTrail[PA_TRAIL_N][4];   // player x,y,z,yawDeg history (one sample / rendered frame)
static int   s_paHead = -1, s_paFilled = 0;
static float s_eaTrail[PA_TRAIL_N][4];   // enemy x,y,z,yawDeg history (Quick Attack afterimage)
static int   s_eaHead = -1, s_eaFilled = 0;
extern int afn_qa_active;                // defined later in this TU; forward-declared for the afterimage gate
extern int afn_paused;                   // defined later; forward-declared so rigs_render can hold the animation while paused
extern int afn_player_frozen;            // defined later; forward-declared so rigs_render can drop the QA trail while frozen
// Quick Attack afterimage tunables — NODE-DRIVEN (Quick Attack / Ai Quick Attack nodes).
// Trail Alpha scales the per-ghost alpha ramp (default 96 = the original peak); Trail
// Length = how many of the 6 trail samples to draw (nearest N). Defaults = original look.
int afn_qa_trail_alpha = 96, afn_qa_trail_len = 6;    // player (cyan)
int afn_eqa_trail_alpha = 96, afn_eqa_trail_len = 6;  // enemy (white)
int afn_wild_charge = 0;   // 1 while a Wild Charge dash is running: yellow trail + blue sparks (Quick Attack reused)

// tint24 = 0xBBGGRR speed-ghost colour (player cyan, enemy red); alpha 0..255.
static void draw_rig_afterimage(const AfnRig* R, const float* view,
                                   float px, float py, float pz, float yawDeg,
                                   float instScale, const float* upN, int alpha, unsigned int tint24) {
    float ux = upN ? upN[0] : 0.0f, uy = upN ? upN[1] : 1.0f, uz = upN ? upN[2] : 0.0f;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul > 1e-6f) { ux/=ul; uy/=ul; uz/=ul; }
    float yr = yawDeg * DEG2RAD + AFN_RIG_YAW_OFFSET + R->yawOff;
    float ydx = sinf(yr), ydz = cosf(yr);
    float dd = ydx*ux + ydz*uz;
    float fx = ydx - ux*dd, fy = -uy*dd, fz = ydz - uz*dd;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-6f) { fx/=fl; fy/=fl; fz/=fl; } else { fx=0; fy=0; fz=1; }
    float rgx = uy*fz - uz*fy, rgy = uz*fx - ux*fz, rgz = ux*fy - uy*fx;
    float S = R->scale * instScale;
    float model[16] = {
        rgx*S, rgy*S, rgz*S, 0,
        ux*S,  uy*S,  uz*S,  0,
        fx*S,  fy*S,  fz*S,  0,
        px,    py,    pz,    1
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view); glMultMatrixf(model);

    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    unsigned int col = ((unsigned)alpha << 24) | (tint24 & 0xFFFFFFu);  // 0xAABBGGRR
    for (int vi = 0; vi < R->verts; vi++) s_skinned[vi].color = col;

    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    if (R->cull == 2) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(R->cull == 1 ? GL_CW : GL_CCW); }

    AfnRigVertex* v = s_skinned;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(AfnRigVertex), &v->color);
    glVertexPointer(3, GL_FLOAT,         sizeof(AfnRigVertex), &v->x);
    for (int g = 0; g < R->mats; g++) {
        int ic = R->idxcount[g];
        if (ic > 0) glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, R->idx[g]);
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
}

// Skin + draw the player and every NPC for this frame.
static void rigs_render(const float* view, float playerX, float playerY, float playerZ,
                        float playerYaw, const float* floorN) {
    const AfnRig* PR = &afn_rigs[AFN_PLAYER_RIG_SLOT];
    // Record this frame's player pose into the afterimage trail ring.
    s_paHead = (s_paHead + 1) % PA_TRAIL_N;
    s_paTrail[s_paHead][0] = playerX; s_paTrail[s_paHead][1] = playerY;
    s_paTrail[s_paHead][2] = playerZ; s_paTrail[s_paHead][3] = playerYaw;
    if (s_paFilled < PA_TRAIL_N) s_paFilled++;
    if (s_playerClipHold) {
        // HoldSkelClip on the player: force the held clip, ignore the normal clip selector.
        if (s_pclip != s_playerHoldClip) { s_pclip = s_playerHoldClip; s_pframe = 0.0f; }
    } else if (afn_rig_clip >= 0 && afn_rig_clip < PR->clips && afn_rig_clip != s_pclip) {
        s_pclip = afn_rig_clip; s_pframe = 0.0f;
    }
    // Hold (HoldSkelClip / KO): play the clip ONCE and hold the last frame (collapse).
    if (afn_paused) {
        /* paused: hold the current pose — no frame advance */
    } else if (s_playerClipHold) {
        float last = (float)(PR->clipframes[s_pclip] - 1);
        s_pframe += 0.4f; if (s_pframe > last) s_pframe = last;
    } else {
        s_pframe = rig_advance(PR, s_pclip, s_pframe);
    }
    build_bone_mats(PR, s_pclip, s_pframe); skin(PR);
    s_drawingPlayer = 1;   // HARDCODED: snapshot ONLY the player's bones (enemy shares the rig)
    rig_draw(PR, s_rigTex[AFN_PLAYER_RIG_SLOT], view, playerX, playerY, playerZ, playerYaw, AFN_PLAYER_SCALE, floorN);
    s_drawingPlayer = 0;

    // Quick Attack afterimage: cyan ghost trail behind the dashing player. Drawn
    // far/old -> near/recent so the brighter recent ghosts overlay the faint ones.
    // Never trail while frozen (a cutscene): the stacked ghosts washed the model white
    // when you won mid-Quick-Attack and the victory cut took over.
    if (afn_qa_active && !afn_player_frozen) {
        static const int paBack[6] = { 12, 10, 8, 6, 4, 2 };  // frames back
        int n = afn_qa_trail_len; if (n < 0) n = 0; if (n > 6) n = 6;   // Trail Length: draw nearest N
        for (int k = 6 - n; k < 6; k++) {
            if (paBack[k] >= s_paFilled) continue;
            int gi = (s_paHead - paBack[k] + PA_TRAIL_N) % PA_TRAIL_N;
            int a = (18 + (k + 1) * 13) * afn_qa_trail_alpha / 96;   // far fainter -> near brighter, scaled by Trail Alpha
            unsigned trailCol = afn_wild_charge ? 0x0000E0FFu : 0x00FFDC96u;   // Wild Charge = yellow, else cyan (0xBBGGRR)
            draw_rig_afterimage(PR, view, s_paTrail[gi][0], s_paTrail[gi][1],
                                s_paTrail[gi][2], s_paTrail[gi][3],
                                AFN_PLAYER_SCALE, floorN, a, trailCol);
        }
    }

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
            if (!afn_paused) { s_npcFrame[i] += 0.4f; if (s_npcFrame[i] > last) s_npcFrame[i] = last; }
        } else
#endif
            if (!afn_paused) s_npcFrame[i] = rig_advance(R, clip, s_npcFrame[i]);
        build_bone_mats(R, clip, s_npcFrame[i]); skin(R);
        // Draw at the nav-driven X/Z/yaw + gravity-settled Y (NPC physics loop),
        // tilted to the smoothed floor normal like the player (slope snap).
#ifdef AFN_HAS_SPRITE_IDX
        s_cacheEnemyBones = ((int)N[7] == AFN_ENEMY_EIDX);   // HARDCODED: snapshot enemy bones for its orb muzzle
#endif
        rig_draw(R, s_rigTex[slot], view, s_npcX[i], s_npcY[i], s_npcZ[i], s_npcYaw[i], N[4], s_npcFloorN[i]);
        s_cacheEnemyBones = 0;
#ifdef AFN_HAS_SPRITE_IDX
        // Enemy Quick Attack afterimage: red ghost trail behind the dashing enemy.
        // Record its pose each frame; draw ghosts while its melee dash/skid runs
        // (s_eqaPhase != 0). Reuses the enemy's just-skinned pose (s_skinned).
        if ((int)N[7] == AFN_ENEMY_EIDX) {
            s_eaHead = (s_eaHead + 1) % PA_TRAIL_N;
            s_eaTrail[s_eaHead][0] = s_npcX[i]; s_eaTrail[s_eaHead][1] = s_npcY[i];
            s_eaTrail[s_eaHead][2] = s_npcZ[i]; s_eaTrail[s_eaHead][3] = s_npcYaw[i];
            if (s_eaFilled < PA_TRAIL_N) s_eaFilled++;
            if (s_eqaPhase != 0) {
                static const int paBack[6] = { 12, 10, 8, 6, 4, 2 };
                int n = afn_eqa_trail_len; if (n < 0) n = 0; if (n > 6) n = 6;   // Trail Length
                for (int k = 6 - n; k < 6; k++) {
                    if (paBack[k] >= s_eaFilled) continue;
                    int gi = (s_eaHead - paBack[k] + PA_TRAIL_N) % PA_TRAIL_N;
                    int a = (18 + (k + 1) * 13) * afn_eqa_trail_alpha / 96;
                    draw_rig_afterimage(R, view, s_eaTrail[gi][0], s_eaTrail[gi][1],
                                        s_eaTrail[gi][2], s_eaTrail[gi][3],
                                        N[4], s_npcFloorN[i], a, 0x00FFFFFFu);   // white
                }
            }
        }
#endif
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

#ifdef AFN_HAS_LIGHTS
// OBJ 2.0 scene lights (the Blender light rig imported through the editor).
// Load the static world-space lights into GL once per frame with the BARE view
// matrix on the modelview — GL stores light positions in eye space at glLightfv
// time, so the per-instance transforms applied afterwards don't move them.
// Diffuse/attenuation are pre-folded by the exporter (see psv_export_common).
// The lights are enabled here but only take effect while draw_mesh() turns on
// GL_LIGHTING for a lit mesh; rigs/billboards/sky all draw with lighting off.
static void lights_setup(const float* view)
{
    static const float zero4[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    glMatrixMode(GL_MODELVIEW);
    // Identity modelview + CPU-side view transform. Spec GL multiplies light
    // positions by the CURRENT modelview at glLightfv time, but vitaGL builds
    // have passed them through raw to a shader that lights in eye space —
    // world-space positions through that path made the lighting swim/garbage
    // while the camera rotated. With identity loaded and the transform done
    // here, both behaviors receive the same correct eye-space light.
    glLoadIdentity();
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, afn_light_ambient);
    for (int i = 0; i < afn_light_count && i < 8; i++) {
        const float* p = afn_lights[i].pos;
        float ep[4];
        if (p[3] != 0.0f) {   // point: full view transform (view is column-major)
            ep[0] = view[0]*p[0] + view[4]*p[1] + view[8]*p[2]  + view[12];
            ep[1] = view[1]*p[0] + view[5]*p[1] + view[9]*p[2]  + view[13];
            ep[2] = view[2]*p[0] + view[6]*p[1] + view[10]*p[2] + view[14];
            ep[3] = 1.0f;
        } else {              // directional ("toward the light"): rotation only
            ep[0] = view[0]*p[0] + view[4]*p[1] + view[8]*p[2];
            ep[1] = view[1]*p[0] + view[5]*p[1] + view[9]*p[2];
            ep[2] = view[2]*p[0] + view[6]*p[1] + view[10]*p[2];
            ep[3] = 0.0f;
        }
        glLightfv(GL_LIGHT0 + i, GL_POSITION, ep);
        glLightfv(GL_LIGHT0 + i, GL_DIFFUSE,  afn_lights[i].col);
        glLightfv(GL_LIGHT0 + i, GL_AMBIENT,  zero4);
        glLightfv(GL_LIGHT0 + i, GL_SPECULAR, zero4);
        // vitaGL exposes only the *v forms — feed the scalars as 1-elem arrays.
        glLightfv(GL_LIGHT0 + i, GL_CONSTANT_ATTENUATION,  &afn_lights[i].kc);
        glLightfv(GL_LIGHT0 + i, GL_LINEAR_ATTENUATION,    &afn_lights[i].kl);
        glLightfv(GL_LIGHT0 + i, GL_QUADRATIC_ATTENUATION, &afn_lights[i].kq);
        glEnable(GL_LIGHT0 + i);
    }
}
#endif

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

    if (m->texHasAlpha || m->blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);

#ifdef AFN_HAS_LIGHTS
    // Scene lights: real vitaGL GL_LIGHTING. lights_setup() already loaded the
    // eye-space light positions this frame; here we just switch lighting on and
    // feed this mesh's normals. COLOR_MATERIAL keeps the per-vertex color array
    // as the albedo, so textures still modulate like the unlit path. NOTE: FFP
    // lighting is known-clean on Vita3K but per-vertex lit values have shown
    // stepping on real SGX before (see rig_draw) — if that shows on hardware,
    // the fallback is baking these lights into vertex colors at export.
    int sceneLit = m->lit && m->normals != 0;
    if (sceneLit) {
        glEnable(GL_LIGHTING);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glEnable(GL_NORMALIZE);              // instance glScalef would shrink normals
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, 0, m->normals);
    }
#endif

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
#ifdef AFN_HAS_LIGHTS
    if (sceneLit) {
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisable(GL_LIGHTING);
        glDisable(GL_COLOR_MATERIAL);
        glDisable(GL_NORMALIZE);
    }
#endif
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
#ifdef AFN_HAS_SPR_EFFECT
            // Effect set on the focus orb (aura): the sprite is drawn as an FX
            // (afn_focusblast_aura_render) instead of this quad — skip it.
            if (afn_spr_effect[i]) continue;
#endif
            // Otherwise the sprite IS the charge ball (drawn only while charging).
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
#ifdef AFN_HAS_SPR_EFFECT
            if (afn_fb_inst >= 0 && afn_spr_effect[afn_fb_inst]) break;   // drawn as aura spheres instead
#endif
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

#ifdef AFN_HAS_CAM_ANIM
// Sample the keyframed camera path (cutscene) at a fractional frame -> eye + look
// forward vector. Catmull-Rom eye; angle-unwrapped yaw/pitch. Time easing is per-keyframe
// Smooth In/Out (decel arriving / accel leaving), applied to all eye modes. This is the
// line-for-line mirror of the editor's SampleCameraAnim() so the in-game cutscene matches
// the Meshes-tab preview. Per-keyframe `interp` is the eye-path shape LEAVING it.
static void afn_cam_anim_sample(int anim, float frame, float* ex, float* ey, float* ez,
                                float* fx, float* fy, float* fz) {
    if (anim < 0 || anim >= AFN_CAM_ANIM_COUNT) anim = 0;
    const AfnCamKf* K = &afn_cam_anim_kf[afn_cam_anim_start[anim]];   // this animation's keyframes
    int n = afn_cam_anim_count[anim];
    if (n < 1) { *ex=*ey=*ez=0.0f; *fx=0.0f; *fy=0.0f; *fz=1.0f; return; }
    if (n < 2) {
        const AfnCamKf* A = &K[0];
        float cp = cosf(A->pitch);
        *ex = A->ex; *ey = A->ey; *ez = A->ez;
        *fx = sinf(A->yaw)*cp; *fy = sinf(A->pitch); *fz = cosf(A->yaw)*cp;
        return;
    }
    if (afn_cam_anim_smooth[anim]) {
        /* Whole-path smooth travel (mirror of editor SampleCameraAnim smooth branch): one
           continuous motion from first to last keyframe. Global smoothstep eases the whole
           timeline; a UNIFORM-knot Catmull-Rom through every eye keeps velocity continuous
           at interior points (no per-keyframe ramp/stop). Per-keyframe interp/ease/speed
           are ignored here. */
        float first = (float)K[0].frame, lastf = (float)K[n-1].frame;
        float p = (lastf > first) ? (frame - first) / (lastf - first) : 0.0f;
        if (p < 0.0f) p = 0.0f; if (p > 1.0f) p = 1.0f;
        float P = p*p*(3.0f-2.0f*p);
        float gu = P * (float)(n-1);
        int si = (int)gu; if (si > n-2) si = n-2; if (si < 0) si = 0;
        float t = gu - (float)si, t2 = t*t, t3 = t2*t;
        const AfnCamKf* A = &K[si]; const AfnCamKf* B = &K[si+1];
        const AfnCamKf* P0 = &K[si>0 ? si-1 : si]; const AfnCamKf* P3 = &K[(si+2)<n ? si+2 : si+1];
        #define AFN_CRV(a,b,c,d) (0.5f*((2.0f*(b)) + (-(a)+(c))*t + (2.0f*(a)-5.0f*(b)+4.0f*(c)-(d))*t2 + (-(a)+3.0f*(b)-3.0f*(c)+(d))*t3))
        *ex = AFN_CRV(P0->ex,A->ex,B->ex,P3->ex); *ey = AFN_CRV(P0->ey,A->ey,B->ey,P3->ey); *ez = AFN_CRV(P0->ez,A->ez,B->ez,P3->ez);
        #undef AFN_CRV
        float dyaw = B->yaw - A->yaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        float yaw = A->yaw + dyaw*t, pit = A->pitch + (B->pitch - A->pitch)*t;
        float cp = cosf(pit);
        *fx = sinf(yaw)*cp; *fy = sinf(pit); *fz = cosf(yaw)*cp;
        return;
    }
    int i = 0;
    if (frame <= (float)K[0].frame) i = 0;
    else if (frame >= (float)K[n-1].frame) i = n-2;
    else { while (i < n-2 && frame >= (float)K[i+1].frame) i++; }
    const AfnCamKf* A = &K[i];
    const AfnCamKf* B = &K[i+1];
    float span = (float)(B->frame - A->frame);
    float t = span > 0.0f ? (frame - (float)A->frame)/span : 0.0f;
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    int mode = A->interp;
    if (mode == 0) t = 0.0f;
    /* Per-keyframe time ease (mirror of editor SampleCameraAnim): smooth-out of A and
       smooth-in to B as independent toggles; cubic Hermite with endpoint slopes s0/s1. */
    float s0 = A->smoothOut ? 0.0f : 1.0f;
    float s1 = B->smoothIn  ? 0.0f : 1.0f;
    float te = (s0+s1-2.0f)*t*t*t + (3.0f-2.0f*s0-s1)*t*t + s0*t;
    if (mode == 2) {
        const AfnCamKf* P0 = &K[i>0 ? i-1 : i];
        const AfnCamKf* P3 = &K[(i+2)<n ? i+2 : i+1];
        float u = te, u2 = u*u, u3 = u2*u;
        #define AFN_CRV(a,b,c,d) (0.5f*((2.0f*(b)) + (-(a)+(c))*u + (2.0f*(a)-5.0f*(b)+4.0f*(c)-(d))*u2 + (-(a)+3.0f*(b)-3.0f*(c)+(d))*u3))
        *ex = AFN_CRV(P0->ex,A->ex,B->ex,P3->ex); *ey = AFN_CRV(P0->ey,A->ey,B->ey,P3->ey); *ez = AFN_CRV(P0->ez,A->ez,B->ez,P3->ez);
        #undef AFN_CRV
    } else {
        *ex = A->ex + (B->ex-A->ex)*te; *ey = A->ey + (B->ey-A->ey)*te; *ez = A->ez + (B->ez-A->ez)*te;
    }
    float dyaw = B->yaw - A->yaw;
    while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
    while (dyaw < -3.14159265f) dyaw += 6.28318531f;
    float yaw = A->yaw + dyaw*te;
    float pit = A->pitch + (B->pitch - A->pitch)*te;
    float cp = cosf(pit);
    *fx = sinf(yaw)*cp; *fy = sinf(pit); *fz = cosf(yaw)*cp;
}
// Per-keyframe speed: the playback-rate multiplier of the SEGMENT starting at the
// keyframe that `frame` currently sits in (returns 1.0 outside a valid segment).
static float afn_cam_seg_speed(int anim, float frame) {
    if (anim < 0 || anim >= AFN_CAM_ANIM_COUNT) anim = 0;
    const AfnCamKf* K = &afn_cam_anim_kf[afn_cam_anim_start[anim]];
    int n = afn_cam_anim_count[anim];
    if (n < 1) return 1.0f;
    int i = 0;
    if (frame <= (float)K[0].frame) i = 0;
    else if (frame >= (float)K[n-1].frame) i = (n >= 2) ? n-2 : 0;
    else { while (i < n-2 && frame >= (float)K[i+1].frame) i++; }
    float s = K[i].speed;
    return (s > 0.0001f) ? s : 1.0f;
}
// Cutscene playback state (Play Camera Anim node sets active + params; the camera
// block ticks the path while active and feeds the eye/look straight into look_at).
// afn_cam_cut_fframe is the fractional playhead (frames); per-segment speed warps
// how fast it advances. afn_cam_cut_frame mirrors it as an int for legacy resets.
int afn_cam_cut_active = 0, afn_cam_cut_anim = 0, afn_cam_cut_timer = 0, afn_cam_cut_frame = 0;
int afn_cam_cut_loop = 0, afn_cam_cut_hold = 0, afn_cam_cut_done = 0;
int afn_cam_cut_player_clip = -1;   // Play Camera Anim "Player Clip" pin: force this player rig clip while the cut is active (-1 = no override)
int afn_cam_cut_snap = 0;           // Play Camera Anim "Snap Player" pin: teleport the player to the scene-start pose at cut start (so a mid-fight cutscene frames the player)
int afn_cam_cut_face_lock = 0;      // set by Snap: FORCE the player facing to the scene-default angle for the whole cut (bypass the heading, which re-syncs to the rotated gameplay orbit each frame)
// Optional snap-pose overrides (Play Camera Anim "Snap X/Z" + "Face Angle" pins). When
// the _has flag is 0 the snap uses the scene spawn / default facing (back-compat); when
// 1 it snaps to the wired world-X/Z (Y stays at spawn ground) / faces the wired degrees.
int afn_cam_cut_snap_has_pos = 0, afn_cam_cut_snap_x = 0, afn_cam_cut_snap_z = 0;
int afn_cam_cut_snap_has_face = 0, afn_cam_cut_snap_face_deg = 0;
int afn_cam_cut_freeze_input = 0;   // Play Camera Anim "Freeze Input" pin: mask ALL buttons while the path is ANIMATING (active + not done) so no ability/lock-on/charge/dodge/move fires; releases when the path completes (so a Hold-Last cut's results menu still gets input)
float afn_cam_cut_fframe = 0.0f;
#endif // AFN_HAS_CAM_ANIM

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
int afn_paused = 0;                           // Start-toggle pause: freezes the WHOLE scene (skips
                                              // script_tick = player + AI + projectiles, zeros input,
                                              // holds the rig animation advance) + shows the pause overlay
int afn_enemy_frozen = 0;                     // freeze the enemy AI (movement + decisions);
                                              // Play Camera Anim's "Freeze Enemy" pin sets it
                                              // for a cutscene, cleared when the cut ends.
int afn_face_lock = 0;                        // MovePlayer "Consistent Facing": keep
                                              // rig yaw while moving (strafe/moonwalk)
int orbit_angle = 0;                          // camera yaw, brad (65536 = full circle)
int afn_cam_reinit = 0;                        // set on scene (re)start: force the follow-cam eye
                                               // statics to re-seed so the camera SNAPS to the correct
                                               // position relative to the player (no stale swing/rotate)
int orbit_pitch = 0;                          // camera pitch, brad (node OrbitCamera Up/Down + right stick)
static float s_camLookYaw = 0.0f;             // eased camera AIM pan (rad) — the active slot's lookYaw (column 5)
static float s_camHOffset = 0.0f;             // eased camera lateral framing TRANSLATE (world px) — slot column 7
static float s_camDepth   = 0.0f;             // eased camera forward/back dolly (world px) — slot column 8
static float s_camLookPitch = 0.0f;           // eased camera AIM tilt (rad, + = up) — the active slot's lookPitch (column 9)
static float s_camVOffset = 0.0f;             // eased camera vertical framing TRANSLATE (world px, + = up) — slot column 10

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
       KEY_RSTICK_UP=65536,   KEY_RSTICK_DOWN=131072,  KEY_RSTICK_LEFT=262144,  KEY_RSTICK_RIGHT=524288,
       KEY_L3=1048576,        KEY_R3=2097152,      // stick clicks (rear-touch / external pad)
       KEY_L2=4194304,        KEY_R2=8388608 };    // rear touch panel (extended ctrl buffer)
unsigned afn_keys_held=0, afn_keys_pressed=0, afn_keys_released=0;
unsigned afn_pause_key = 64;   // KEY_START default; the Toggle Pause node captures the key that paused (so the resume key auto-matches whatever On Key Pressed drives it — Select, Start, etc.)
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
    // Extended (Ext2) buffer: the physical shoulders are L1/R1 (SCE_CTRL_LTRIGGER is the L2
    // bit, which here is the REAR TOUCH — not the shoulder). Map the shoulders to L1/R1.
    if (b & SCE_CTRL_L1)       k|=KEY_L;    // physical L1 shoulder
    if (b & SCE_CTRL_R1)       k|=KEY_R;    // physical R1 shoulder
    if (b & SCE_CTRL_L2)       k|=KEY_L2;   // rear touch panel (top-left)
    if (b & SCE_CTRL_R2)       k|=KEY_R2;   // rear touch panel (top-right)
    if (b & SCE_CTRL_L3)       k|=KEY_L3;   // rear touch panel (bottom) / external pad
    if (b & SCE_CTRL_R3)       k|=KEY_R3;
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
    // Edge detection uses a PRIVATE previous-held snapshot (the RAW pad state), NOT
    // afn_keys_held — because afn_keys_held is the script-facing value that the pause /
    // cutscene-freeze / lock-functions masks below overwrite each frame. If pressed/
    // released were derived from a masked afn_keys_held, the frame AFTER a mask every
    // still-held key would read as a fresh press (cursor spazz + repeated SFX triggers).
    static unsigned s_prevHeldRaw = 0;
    afn_keys_pressed  = k & ~s_prevHeldRaw;
    afn_keys_released = ~k & s_prevHeldRaw;
    afn_keys_held     = k;
    s_prevHeldRaw     = k;
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
// Quick Attack node (dash-in melee): a committed lunge toward the lock target
// (or straight forward with no lock), contact damage, then a skid recovery.
// Movement mirrors the dodge envelope (ramp ease-in + wall sub-stepping). The
// node raises afn_qa_trigger + sets the tunables/clips; the movement block runs
// the 3-phase machine. Is Dashing gates on phase != 0; Quick Attack Hit on the
// contact frame.
int afn_qa_trigger = 0, afn_qa_phase = 0;   // phase: 0 idle, 1 dash, 2 skid
int afn_qa_frames = 0, afn_qa_active = 0, afn_qa_hit = 0, afn_qa_tgt = -1;
int afn_qa_started = 0;   // 1 only on the frame the player's QA dash actually begins (Quick Attack Started gate)
int afn_qa_speed = 90, afn_qa_range = 14, afn_qa_dmg = 12;          // speed (*0.08 px/f), contact range (px), damage
int afn_qa_max = 28, afn_qa_skid = 12, afn_qa_punch = 20, afn_qa_cd = 0;  // dash budget, skid frames, cam punch %, cooldown
int afn_qa_clip_lunge = -1, afn_qa_clip_skid = -1, afn_qa_clip_idle = -1; // rig clips for each phase
static float s_qaDirX = 0.0f, s_qaDirZ = 1.0f, s_qaYaw = 0.0f, s_qaCamPunch = 0.0f;
static int   s_qaDealt = 0, s_qaTotal = 0;
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
// On Hit event flags (per frame): afn_obj_hit[i]=1 when object i lost HP this
// frame; afn_any_hit=1 when the player OR any object took damage. Computed by a
// central HP-drop detector before script_tick (catches every damage source:
// melee, beams, clash nodes, focus blast) and read by the On Hit dispatcher.
int afn_obj_hit[NUM_SPRITES]={0}, afn_any_hit=0;
int afn_energy=0, afn_energy_max=100;   // player energy resource (node-driven: Add/Spend/Set/SetMax Energy)
int afn_charging_up=0;                   // ChargeUp node sets =1 each frame it runs; reset pre-tick, drives the charge aura
int afn_lock_functions=0;                // Lock Player Functions node sets =1 each frame it runs; held keys are masked (so On-Key-Held abilities don't run) + ability triggers cleared post-tick. For menus/game-over: nav (On Key Pressed) still works
int afn_charge_clip=23;                  // rig clip held while charging (44-anim r8 default = "charge"; ChargeUp's Charge Clip pin overrides, name-resolved)
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
#ifdef AFN_HAS_SPR_EFFECT
    // Effect assigned to the focus orb: the enemy orb is drawn as that FX in the aura pass
    // (afn_focusblast_aura_render), same as the player's — skip this sprite quad.
    if (afn_spr_effect[fb] > 0) return;
#endif
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

// Particle pool: integrate every live particle one fixed step — gravity into the
// velocity, velocity into the position, age down. Pure vector math, no dt.
static void afn_particles_update(void) {
    if (afn_paused) return;
    for (int i = 0; i < AFN_PART_POOL; i++) {
        AfnParticle* p = &s_parts[i];
        if (!p->active) continue;
        p->vy -= p->grav;
        p->x += p->vx; p->y += p->vy; p->z += p->vz;
        if ((p->life -= 1.0f) <= 0.0f) p->active = 0;
    }
}

// Emit afn_part_spawn particles at (ox,oy,oz) using the current request params, then
// clear the request. Velocity = a mostly-upward cone scaled by speed/spread.
static void afn_particles_emit(float ox, float oy, float oz) {
    int n = afn_part_spawn; afn_part_spawn = 0;
    for (int e = 0; e < n; e++) {
        int slot = -1;
        for (int i = 0; i < AFN_PART_POOL; i++) if (!s_parts[i].active) { slot = i; break; }
        if (slot < 0) break;   // pool full — drop the rest
        AfnParticle* p = &s_parts[slot];
        p->active = 1; p->blend = (unsigned char)afn_part_blend; p->frame = (short)afn_part_frame;
        p->x = ox; p->y = oy; p->z = oz;
        float sp = afn_part_speed;
        p->vx = part_sym() * afn_part_spread * sp;
        p->vz = part_sym() * afn_part_spread * sp;
        p->vy = (0.4f + part_rand() * 0.6f) * sp;          // mostly up
        p->maxLife = p->life = afn_part_life * (0.7f + part_rand() * 0.6f);
        p->grav = afn_part_grav; p->size0 = afn_part_size0; p->size1 = afn_part_size1;
        p->r0 = afn_part_col0 & 0xFF; p->g0 = (afn_part_col0 >> 8) & 0xFF; p->b0 = (afn_part_col0 >> 16) & 0xFF; p->a0 = (afn_part_col0 >> 24) & 0xFF;
        p->r1 = afn_part_col1 & 0xFF; p->g1 = (afn_part_col1 >> 8) & 0xFF; p->b1 = (afn_part_col1 >> 16) & 0xFF; p->a1 = (afn_part_col1 >> 24) & 0xFF;
    }
}

// Billboard every live particle camera-facing (right/up = the view matrix's basis
// columns, same as enemy_orb_render), size + colour lerped over its life.
static void afn_particles_render(const float* view) {
    float Rwx = view[0], Rwy = view[4], Rwz = view[8];   // camera right (world)
    float Uwx = view[1], Uwy = view[5], Uwz = view[9];   // camera up (world)
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);   // texture * vertex colour (alpha fade)
    glDepthMask(GL_FALSE);   // soft/additive: test depth but don't write it
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    int curBlend = -1, texOn = -1; GLuint curTex = 0;
    for (int i = 0; i < AFN_PART_POOL; i++) {
        AfnParticle* p = &s_parts[i];
        if (!p->active) continue;
        float age = 1.0f - p->life / (p->maxLife > 0.0f ? p->maxLife : 1.0f);  // 0..1
        float sz = p->size0 + (p->size1 - p->size0) * age, hw = sz * 0.5f, hh = sz * 0.5f;
        unsigned int r = p->r0 + (int)((p->r1 - p->r0) * age), g = p->g0 + (int)((p->g1 - p->g0) * age);
        unsigned int b = p->b0 + (int)((p->b1 - p->b0) * age), a = p->a0 + (int)((p->a1 - p->a0) * age);
        unsigned int col = (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | ((a & 0xFF) << 24);
        if (p->blend != curBlend) { glBlendFunc(GL_SRC_ALPHA, p->blend ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); curBlend = p->blend; }
        if (p->frame >= 0) { if (texOn != 1) { glEnable(GL_TEXTURE_2D); texOn = 1; } GLuint tx = s_sprTex[p->frame]; if (tx != curTex) { glBindTexture(GL_TEXTURE_2D, tx); curTex = tx; } }
        else if (texOn != 0) { glDisable(GL_TEXTURE_2D); texOn = 0; }
        float cx = p->x, cy = p->y, cz = p->z;
        AfnVertex q[4] = {
            { 0,0, col, cx - Rwx*hw + Uwx*hh, cy - Rwy*hw + Uwy*hh, cz - Rwz*hw + Uwz*hh },
            { 1,0, col, cx + Rwx*hw + Uwx*hh, cy + Rwy*hw + Uwy*hh, cz + Rwz*hw + Uwz*hh },
            { 1,1, col, cx + Rwx*hw - Uwx*hh, cy + Rwy*hw - Uwy*hh, cz + Rwz*hw - Uwz*hh },
            { 0,1, col, cx - Rwx*hw - Uwx*hh, cy - Rwy*hw - Uwy*hh, cz - Rwz*hw - Uwz*hh },
        };
        AfnVertex* v = q;
        glTexCoordPointer(2, GL_FLOAT, sizeof(AfnVertex), &v->u);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
        glVertexPointer(3, GL_FLOAT, sizeof(AfnVertex), &v->x);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Meteor Mash — HARDCODED prototype (fired on Select). A cast-in-place burst of
// additive, texture-free billboards that glow on their own:
//   * CIRCLE STREAK : big spinning rings that expand over the life
//   * OVALS         : blue swirl loops orbiting the centre, each tilted + spinning
//   * STARS         : yellow spinning stars flung outward, each with a GLOW halo
//                     and a comet GLOW TRAIL streaming back toward the centre
//   * IMPACT FLASH  : a bright white core disc that pops + fades at the start
// Camera-facing (uses the view matrix's right/up like afn_particles_render).
// Migrate to a data-driven Effects layer + node once the look is locked.
// ---------------------------------------------------------------------------
#define MM_LIFE 56
#define MM_COL(r,g,b,a) ((unsigned)((r)&0xFF) | (((unsigned)(g)&0xFF)<<8) | (((unsigned)(b)&0xFF)<<16) | (((unsigned)(a)&0xFF)<<24))
static int   s_mm_timer = 0;
static float s_mm_ox, s_mm_oy, s_mm_oz;         // launch point (muzzle) in front of the player
static float s_mm_fx, s_mm_fy, s_mm_fz;         // forward (shot travels along this)
static float s_mm_ax, s_mm_ay, s_mm_az;         // perp basis A (right)  — spans the tube cross-section
static float s_mm_bx, s_mm_by, s_mm_bz;         // perp basis B (up)     — with A, the plane perp to forward
static AfnVertex s_mmVB[132];
#define MMV(I,X,Y,Z,C) do{ s_mmVB[I].u=0; s_mmVB[I].v=0; s_mmVB[I].color=(C); s_mmVB[I].x=(X); s_mmVB[I].y=(Y); s_mmVB[I].z=(Z);}while(0)
static void mm_flush(int n, GLenum mode) {
    AfnVertex* v = s_mmVB;
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,        sizeof(AfnVertex), &v->x);
    glDrawArrays(mode, 0, n);
}
// Oval RING (triangle strip between inner/outer edge) in the world plane spanned
// by unit dirs A (major) and B (minor). rx/ry = radii, thick = ring wall.
static void mm_ring(float cx,float cy,float cz, float Ax,float Ay,float Az, float Bx,float By,float Bz,
                    float rx,float ry,float thick, unsigned col) {
    int K=30, n=0;
    for (int j=0;j<=K;j++){ float a=(float)j/(float)K*6.2831853f, ca=cosf(a), sa=sinf(a);
        float ox=Ax*(rx*ca)+Bx*(ry*sa), oy=Ay*(rx*ca)+By*(ry*sa), oz=Az*(rx*ca)+Bz*(ry*sa);
        float ix=Ax*((rx-thick)*ca)+Bx*((ry-thick)*sa), iy=Ay*((rx-thick)*ca)+By*((ry-thick)*sa), iz=Az*((rx-thick)*ca)+Bz*((ry-thick)*sa);
        MMV(n,cx+ox,cy+oy,cz+oz,col); n++; MMV(n,cx+ix,cy+iy,cz+iz,col); n++; }
    mm_flush(n, GL_TRIANGLE_STRIP);
}
// Filled 5-point STAR (triangle fan), camera-facing in the R/U plane.
static void mm_star(float cx,float cy,float cz, float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                    float rad, float rot, unsigned col) {
    int P=5, n=0; MMV(n,cx,cy,cz,col); n++;
    for (int j=0;j<=P*2;j++){ float a=rot+(float)j/(float)(P*2)*6.2831853f; float r=(j&1)?rad*0.40f:rad;
        float ca=cosf(a), sa=sinf(a);
        MMV(n, cx+(Rx*ca+Ux*sa)*r, cy+(Ry*ca+Uy*sa)*r, cz+(Rz*ca+Uz*sa)*r, col); n++; }
    mm_flush(n, GL_TRIANGLE_FAN);
}
// Four-point SHINE sparkle (the Sword/Shield "Icy Wind" glint): 4 sharp thin spikes in a plus,
// bright hot core fading to transparent tips. `spikeCol` should carry the tip alpha (fades out);
// the centre is drawn with `coreCol` (bright). Camera-facing; `rad` = spike length, `rot` spins it.
static void mm_shine4(float cx,float cy,float cz, float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                      float rad, float rot, unsigned spikeCol, unsigned coreCol) {
    int P=4, n=0; MMV(n,cx,cy,cz,coreCol); n++;
    for (int j=0;j<=P*2;j++){ float a=rot+(float)j/(float)(P*2)*6.2831853f; float r=(j&1)?rad*0.12f:rad;
        float ca=cosf(a), sa=sinf(a);
        unsigned c=(j&1)?coreCol:spikeCol;                 // valleys hug the bright core; tips fade out
        MMV(n, cx+(Rx*ca+Ux*sa)*r, cy+(Ry*ca+Uy*sa)*r, cz+(Rz*ca+Uz*sa)*r, c); n++; }
    mm_flush(n, GL_TRIANGLE_FAN);
}
// Filled DISC (triangle fan), camera-facing — glow halo / impact flash core.
// Filled OVAL (triangle fan) in the plane spanned by unit dirs A (major) / B (minor).
static void mm_fill_oval(float cx,float cy,float cz, float Ax,float Ay,float Az, float Bx,float By,float Bz,
                         float rx,float ry, unsigned col) {
    int K=18, n=0; MMV(n,cx,cy,cz,col); n++;
    for (int j=0;j<=K;j++){ float a=(float)j/(float)K*6.2831853f, ca=cosf(a), sa=sinf(a);
        MMV(n, cx+Ax*(rx*ca)+Bx*(ry*sa), cy+Ay*(rx*ca)+By*(ry*sa), cz+Az*(rx*ca)+Bz*(ry*sa), col); n++; }
    mm_flush(n, GL_TRIANGLE_FAN);
}
// Soft GLOW (triangle fan) — bright colCtr at the centre fading to colRim (alpha 0) at the
// rim, so it's a soft radial falloff instead of a hard-edged flat disc. A/B = plane dirs.
static void mm_glow(float cx,float cy,float cz, float Ax,float Ay,float Az, float Bx,float By,float Bz,
                    float rx,float ry, unsigned ctr, unsigned rim) {
    int K=20, n=0; MMV(n,cx,cy,cz,ctr); n++;
    for (int j=0;j<=K;j++){ float a=(float)j/(float)K*6.2831853f, ca=cosf(a), sa=sinf(a);
        MMV(n, cx+Ax*(rx*ca)+Bx*(ry*sa), cy+Ay*(rx*ca)+By*(ry*sa), cz+Az*(rx*ca)+Bz*(ry*sa), rim); n++; }
    mm_flush(n, GL_TRIANGLE_FAN);
}
// Comet GLOW TRAIL: a soft streak from head backward along -dir. The LENGTH follows the true
// WORLD travel direction (so it stays anchored as the camera orbits — no side-swing); only the
// WIDTH billboards to face the camera. Bright centre spine fading to transparent edges + tail.
static void mm_trail(float hx,float hy,float hz, float dux,float duy,float duz,
                     float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                     float len, float wHead, unsigned colHead, unsigned colTail) {
    float dl = sqrtf(dux*dux+duy*duy+duz*duz); if (dl<0.001f) dl=0.001f;
    float ax=dux/dl, ay=duy/dl, az=duz/dl;                        // WORLD travel dir (not screen-projected)
    float Fx=Ry*Uz-Rz*Uy, Fy=Rz*Ux-Rx*Uz, Fz=Rx*Uy-Ry*Ux;        // camera forward = right x up
    float pex=ay*Fz-az*Fy, pey=az*Fx-ax*Fz, pez=ax*Fy-ay*Fx;      // width = dir x camFwd (perp to dir, faces cam)
    float pl=sqrtf(pex*pex+pey*pey+pez*pez); if(pl<0.001f)pl=0.001f; pex/=pl; pey/=pl; pez/=pl;
    float tx=hx-ax*len, ty=hy-ay*len, tz=hz-az*len;              // tail streams back in WORLD space
    float hw=wHead*0.5f, tw=wHead*0.12f;
    unsigned edgeH = colHead & 0x00FFFFFFu, edgeT = colTail & 0x00FFFFFFu;   // same rgb, alpha 0
    // left half (edge -> centre spine), then right half — soft falloff across the width.
    MMV(0, hx+pex*hw, hy+pey*hw, hz+pez*hw, edgeH); MMV(1, hx,hy,hz, colHead);
    MMV(2, tx+pex*tw, ty+pey*tw, tz+pez*tw, edgeT); MMV(3, tx,ty,tz, colTail);
    mm_flush(4, GL_TRIANGLE_STRIP);
    MMV(0, hx-pex*hw, hy-pey*hw, hz-pez*hw, edgeH); MMV(1, hx,hy,hz, colHead);
    MMV(2, tx-pex*tw, ty-pey*tw, tz-pez*tw, edgeT); MMV(3, tx,ty,tz, colTail);
    mm_flush(4, GL_TRIANGLE_STRIP);
}
static void afn_meteor_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_mm_fx = sinf(yr); s_mm_fy = 0.0f; s_mm_fz = cosf(yr);        // forward (level)
    s_mm_ax = cosf(yr); s_mm_ay = 0.0f; s_mm_az = -sinf(yr);       // right
    s_mm_bx = 0.0f;     s_mm_by = 1.0f; s_mm_bz = 0.0f;            // up
    s_mm_ox = px + s_mm_fx*4.0f; s_mm_oy = py + 8.0f; s_mm_oz = pz + s_mm_fz*4.0f;   // muzzle just in front, chest height
    s_mm_timer = MM_LIFE;
}
static void afn_meteor_step(void) { if (s_mm_timer > 0 && !afn_paused) s_mm_timer--; }
static void afn_meteor_render(const float* view) {
    if (s_mm_timer <= 0) return;
    float prog = 1.0f - (float)s_mm_timer/(float)MM_LIFE;   // 0..1 over life
    float t    = (float)(MM_LIFE - s_mm_timer);             // frames elapsed (animation clock)
    float fadeIn  = prog < 0.10f ? prog/0.10f : 1.0f;
    float fadeOut = s_mm_timer < 12 ? (float)s_mm_timer/12.0f : 1.0f;
    float fade = fadeIn * fadeOut;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];   // camera right / up (world)
    float Fx=s_mm_fx,Fy=s_mm_fy,Fz=s_mm_fz;                                       // shot forward
    float Ax=s_mm_ax,Ay=s_mm_ay,Az=s_mm_az, Bx=s_mm_bx,By=s_mm_by,Bz=s_mm_bz;     // tube cross-section basis
    float ox=s_mm_ox, oy=s_mm_oy, oz=s_mm_oz;                                     // launch point
    float travel = prog * 34.0f;                                                 // distance flown
    float Px=ox+Fx*travel, Py=oy+Fy*travel, Pz=oz+Fz*travel;                      // star (projectile) position
    // camera-facing streak basis: forward projected into the screen plane (elongates the ovals
    // along the direction of travel so they read as motion bullets, always facing the camera).
    float fr=Fx*Rx+Fy*Ry+Fz*Rz, fu=Fx*Ux+Fy*Uy+Fz*Uz, fm=sqrtf(fr*fr+fu*fu);
    if (fm<0.05f){ fr=1.0f; fu=0.0f; fm=1.0f; } fr/=fm; fu/=fm;
    float MJx=Rx*fr+Ux*fu, MJy=Ry*fr+Uy*fu, MJz=Rz*fr+Uz*fu;                      // screen "along travel"
    float MNx=Rx*(-fu)+Ux*fr, MNy=Ry*(-fu)+Uy*fr, MNz=Rz*(-fu)+Uz*fr;            // screen "across"
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive glow
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // SUMMON RINGS: two rings at the launch point (facing down the shot), spinning; they
    // shrink + fade as the star leaves — the star is "summoned" out of them.
    for (int s=0;s<2;s++){ float sp=t*0.22f+(float)s*1.6f, cs=cosf(sp), ss=sinf(sp);
        float RAx=Ax*cs+Bx*ss, RAy=Ay*cs+By*ss, RAz=Az*cs+Bz*ss;
        float RBx=-Ax*ss+Bx*cs, RBy=-Ay*ss+By*cs, RBz=-Az*ss+Bz*cs;
        float rad=(5.5f+(float)s*2.2f)*(1.0f-prog*0.55f); if(rad<0.4f)rad=0.4f;
        int a=(int)(160*fade*(1.0f-prog*0.7f)); if(a<0)a=0;
        mm_ring(ox,oy,oz, RAx,RAy,RAz, RBx,RBy,RBz, rad, rad, rad*0.16f, MM_COL(120,210,255,a)); }

    // OVALS: small blue speed-streaks that shoot FORWARD faster than the star, scattered THROUGH
    // the whole path corridor (varied radius within the tube). Each loops on its own fast phase —
    // fades in, streaks up toward the head, then VANISHES — so it reads like a comet's particle rush.
    int NOV=28; float span = travel < 1.0f ? 1.0f : travel; float pathR = 2.6f;
    for (int i=0;i<NOV;i++){ unsigned h=(unsigned)(i+1)*2654435761u ^ 0x9E3779B9u;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f;
        float ph=fmodf(t*0.055f + fc, 1.0f);                                   // fast forward cycle, staggered per streak
        float lf=fmodf(fa + ph, 1.0f);                                         // longitudinal fraction 0..1 (moves fwd fast, wraps)
        float dist=lf*span;                                                    // world distance from launch along forward
        float ang=(float)i*2.399963f + fb*6.2831853f, ca=cosf(ang), sa=sinf(ang);
        float rdx=Ax*ca+Bx*sa, rdy=Ay*ca+By*sa, rdz=Az*ca+Bz*sa;
        float R=pathR*(0.20f + fb*0.80f);                                      // within the path radius, spread across it
        float pcx=ox+Fx*dist+rdx*R, pcy=oy+Fy*dist+rdy*R, pcz=oz+Fz*dist+rdz*R;
        float av=sinf(ph*3.14159265f);                                         // appear -> peak -> VANISH over the cycle
        int a=(int)(200*fade*av); if(a<0)a=0;
        mm_fill_oval(pcx,pcy,pcz, MJx,MJy,MJz, MNx,MNy,MNz, 0.5f,0.16f, MM_COL(70,150,255,a)); }   // tiny blue speed-streaks

    // STAR: single projectile at the head — comet GLOW TRAIL back along -forward, GLOW halo, star.
    { float srad=2.6f*(1.0f-prog*0.20f), rot=t*0.18f; int a=(int)(255*fade);
        mm_trail(Px,Py,Pz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, 12.0f, srad*1.7f, MM_COL(110,185,255,(int)(a*0.75f)), MM_COL(80,150,255,0));   // BLUE trail
        mm_glow(Px,Py,Pz, Rx,Ry,Rz, Ux,Uy,Uz, srad*3.2f,srad*3.2f, MM_COL(90,170,255,(int)(a*0.55f)), MM_COL(80,150,255,0));   // BLUE glow halo
        mm_star(Px,Py,Pz, Rx,Ry,Rz, Ux,Uy,Uz, srad, rot, MM_COL(255,225,70,a));                 // yellow star
        mm_star(Px,Py,Pz, Rx,Ry,Rz, Ux,Uy,Uz, srad*0.5f, rot, MM_COL(255,255,190,a)); }         // bright yellow core

    // IMPACT FLASH: bright core disc at the launch point, fast expand + fade at the start.
    if (prog < 0.35f){ float f=1.0f-prog/0.35f; float rad=4.0f+(1.0f-f)*9.0f;
        mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad,rad,           MM_COL(200,235,255,(int)(170*f)), MM_COL(170,215,255,0));
        mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad*0.5f,rad*0.5f, MM_COL(255,255,255,(int)(230*f)), MM_COL(255,255,255,0)); }

    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Electroweb — HARDCODED status-move prototype. Spawns a spider-web lattice of
// crackling electric lines on the ground ahead of the player. Anyone who walks
// into its radius is SNARED (immobilised) for ~2 seconds. Additive, jittery,
// with a shimmer that ripples through the strands. Migrate to a node/effect later.
// ---------------------------------------------------------------------------
static int   s_ew_active = 0, s_ew_life = 0, s_ew_inPrev = 0;
static float s_ew_x = 0.0f, s_ew_y = 0.0f, s_ew_z = 0.0f, s_ew_r = 22.0f;
int afn_player_stuck = 0;   // frames the player is immobilised by an Electroweb (0 = free)
#define EW_MAXLIFE 360      // web persists ~6s

static void afn_electroweb_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_ew_x = px + sinf(yr)*22.0f; s_ew_z = pz + cosf(yr)*22.0f; s_ew_y = py + 0.5f;   // on the ground ahead
    s_ew_r = 22.0f; s_ew_life = EW_MAXLIFE; s_ew_active = 1; s_ew_inPrev = 0;
}
static void afn_electroweb_step(void) { if (s_ew_active && !afn_paused) { if (--s_ew_life <= 0) s_ew_active = 0; } }

// One crackling web strand a->b: a billboarded additive ribbon jittered sideways (in the
// ground plane) + a little in Y, tapering the jitter to 0 at the endpoints so the web joints stay put.
static void afn_ew_edge(float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                        float ax,float ay,float az, float bx,float by,float bz,
                        float w, unsigned col, unsigned rng) {
    float dx=bx-ax, dz=bz-az; float dl=sqrtf(dx*dx+dz*dz); if(dl<0.001f)dl=0.001f;
    float hpx=-dz/dl, hpz=dx/dl;                                  // horizontal perp (crackle jitter)
    float sdr=dx*Rx+dz*Rz, sdu=dx*Ux+dz*Uz; float sm=sqrtf(sdr*sdr+sdu*sdu); if(sm<0.001f)sm=0.001f; sdr/=sm; sdu/=sm;
    float wpx=Rx*(-sdu)+Ux*sdr, wpy=Ry*(-sdu)+Uy*sdr, wpz=Rz*(-sdu)+Uz*sdr;   // screen-perp (ribbon width)
    int M=5, n=0;
    for (int i=0;i<=M;i++){ float t=(float)i/(float)M;
        float px=ax+(bx-ax)*t, py=ay+(by-ay)*t, pz=az+(bz-az)*t;
        float edge=t<0.5f?t:(1.0f-t), amt=edge*4.0f<1.0f?edge*4.0f:1.0f;
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; px += hpx*((float)(rng&0xFFFF)/32768.0f-1.0f)*1.1f*amt;
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; pz += hpz*((float)(rng&0xFFFF)/32768.0f-1.0f)*1.1f*amt;
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; py += ((float)(rng&0xFFFF)/32768.0f-1.0f)*0.7f*amt;
        MMV(n, px-wpx*w, py-wpy*w, pz-wpz*w, col); n++;
        MMV(n, px+wpx*w, py+wpy*w, pz+wpz*w, col); n++;
    }
    mm_flush(n, GL_TRIANGLE_STRIP);
}
static void afn_electroweb_render(const float* view) {
    if (!s_ew_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float cx=s_ew_x, cy=s_ew_y, cz=s_ew_z;
    float age = 1.0f - (float)s_ew_life/(float)EW_MAXLIFE;
    float fade = (s_ew_life < 30) ? (float)s_ew_life/30.0f : 1.0f;   // fade out at the end
    if (age < 0.08f) fade *= age/0.08f;                             // ...and in at the start
    static int frame = 0; frame++;                                  // shimmer clock (frame counter, resume-safe)
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int SP=8, RG=3; float w=0.35f; unsigned eidx=0;
    for (int si=0; si<SP; si++){
        float a0=(float)si*(6.2831853f/(float)SP), a1=(float)(si+1)*(6.2831853f/(float)SP);
        float c0=cosf(a0), s0=sinf(a0), c1=cosf(a1), s1=sinf(a1);
        float prevx=cx, prevz=cz;                                   // start each spoke at the centre
        for (int ri=1; ri<=RG; ri++){
            float rr=s_ew_r*(float)ri/(float)RG;
            float px=cx+c0*rr, pz=cz+s0*rr;
            float pul=0.55f+0.45f*sinf((float)frame*0.28f+(float)(eidx++)*0.7f);   // shimmer ripples along the web
            int a=(int)(210*fade*pul); if(a<0)a=0;
            afn_ew_edge(Rx,Ry,Rz,Ux,Uy,Uz, prevx,cy,prevz, px,cy,pz, w, MM_COL(255,235,110,a), (eidx*2654435761u)^(unsigned)frame);   // spoke segment
            prevx=px; prevz=pz;
            float qx=cx+c1*rr, qz=cz+s1*rr;
            float pul2=0.55f+0.45f*sinf((float)frame*0.28f+(float)(eidx++)*0.7f);
            int a2=(int)(210*fade*pul2); if(a2<0)a2=0;
            afn_ew_edge(Rx,Ry,Rz,Ux,Uy,Uz, px,cy,pz, qx,cy,qz, w, MM_COL(255,235,110,a2), (eidx*2654435761u)^(unsigned)frame);   // ring segment
        }
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Fire Spin — HARDCODED prototype. A swirling vortex of flames around the player:
// camera-facing flame billboards (orange glow + yellow core) laid out along a
// helix that winds up a column, rotating + rising + looping so it reads as a
// spinning fire tornado. Follows the player each frame. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int s_fs_active = 0, s_fs_life = 0;
#define FS_MAXLIFE 240
static void afn_firespin_fire(void)  { s_fs_active = 1; s_fs_life = FS_MAXLIFE; }
static void afn_firespin_step(void)  { if (s_fs_active && !afn_paused) { if (--s_fs_life <= 0) s_fs_active = 0; } }
// One camera-facing FLAME: a triangle standing on the floor — base is camera-right wide, apex
// points straight UP in world. Per-vertex gradient (hot colBase at the floor -> colTip fading at
// the apex) so it reads as a licking flame rather than a soft blob.
static void mm_flame(float cx,float cz,float fy, float Rx,float Ry,float Rz, float w,float h, unsigned colBase, unsigned colTip) {
    int n=0;
    MMV(n, cx-Rx*w, fy-Ry*w, cz-Rz*w, colBase); n++;
    MMV(n, cx+Rx*w, fy+Ry*w, cz+Rz*w, colBase); n++;
    MMV(n, cx,      fy+h,     cz,      colTip ); n++;
    mm_flush(n, GL_TRIANGLES);
}
static void afn_firespin_render(const float* view, float px, float py, float pz) {
    if (!s_fs_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8];
    float fade = (s_fs_life < 20) ? (float)s_fs_life/20.0f : 1.0f;
    if (FS_MAXLIFE - s_fs_life < 12) fade *= (float)(FS_MAXLIFE - s_fs_life)/12.0f;   // ease in
    static int frame = 0; frame++;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive fire
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    // A dense SPIRAL of small flames on the GROUND, rotating over time (the "spin").
    float fy = py + 0.5f;                                    // floor height
    int M=56; float turns=2.5f, Rmax=9.0f, spin=(float)frame*0.055f;
    for (int k=0;k<M;k++){
        float t = (float)k/(float)M;
        float ang = t*6.2831853f*turns + spin;               // winds around; +spin rotates it
        float rad = Rmax*(0.14f + 0.86f*t);                  // spirals outward from the player
        float fx = px + cosf(ang)*rad, fz = pz + sinf(ang)*rad;
        unsigned h=(unsigned)(k*2+1)*2654435761u ^ (unsigned)frame;
        h^=h<<13; h^=h>>17; h^=h<<5; float fl = 0.55f + 0.45f*((float)(h&0xFF)/255.0f);   // per-flame flicker
        float endT = t<0.08f ? t/0.08f : (t>0.92f ? (1.0f-t)/0.08f : 1.0f);              // soften the spiral ends
        float A = fade*fl*endT;
        float fh = 3.2f + fl*2.2f, fw = 1.0f + fl*0.5f;      // flame height / width (flicker)
        mm_flame(fx,fz,fy, Rx,Ry,Rz, fw*1.15f, fh*1.15f, MM_COL(255, 80,15,(int)(110*A)), MM_COL(180,20, 0,0));   // outer red-orange glow
        mm_flame(fx,fz,fy, Rx,Ry,Rz, fw*0.60f, fh*0.80f, MM_COL(255,200,90,(int)(150*A)), MM_COL(255,90,20,0));   // yellow body
        mm_flame(fx,fz,fy, Rx,Ry,Rz, fw*0.32f, fh*0.42f, MM_COL(255,240,190,(int)(160*A)), MM_COL(255,180,80,0)); // white-hot base
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// Procedural FIRE-PARTICLE sprite (generated once): a soft round radial gradient whose alpha is
// GUARANTEED zero at every quad edge (so stretched particles never show their card), broken up by
// noise that only DARKENS (never adds) so edges stay wispy. White RGB — the vertex colour tints it
// through the particle's heat ramp (white-yellow -> orange -> red) as it ages.
static GLuint s_fireTex = 0;
static void afn_fire_tex_init(void){
    if (s_fireTex) return;
    enum { S=64 }; static unsigned char px[S*S*4];
    for (int y=0;y<S;y++) for (int x=0;x<S;x++){
        float fx=((float)x+0.5f)/S*2.0f-1.0f, fy=((float)y+0.5f)/S*2.0f-1.0f;
        float r=sqrtf(fx*fx+fy*fy);
        float a=1.0f-r/0.92f; if(a<0.0f)a=0.0f; a=a*a;                            // quadratic falloff, hard 0 before the edge
        unsigned h1=(unsigned)((x>>2)*73856093u ^ (y>>2)*19349663u);              // coarse breakup
        unsigned h2=(unsigned)(x*83492791u ^ y*29849639u);                        // fine grain
        a *= 0.72f + 0.20f*(float)(h1&0xFF)/255.0f + 0.08f*(float)(h2&0xFF)/255.0f;
        if(a>1.0f)a=1.0f;
        int idx=(y*S+x)*4; px[idx]=255; px[idx+1]=255; px[idx+2]=255; px[idx+3]=(unsigned char)(a*255.0f);
    }
    glGenTextures(1,&s_fireTex);
    glBindTexture(GL_TEXTURE_2D,s_fireTex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,S,S,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
}
// Textured vertex (sets u,v — MMV zeroes them).
#define MMVT(I,X,Y,Z,U,V,C) do{ s_mmVB[I].u=(U); s_mmVB[I].v=(V); s_mmVB[I].color=(C); s_mmVB[I].x=(X); s_mmVB[I].y=(Y); s_mmVB[I].z=(Z);}while(0)

// ---------------------------------------------------------------------------
// Flame Wheel — HARDCODED prototype (fired on Select). A VERTICAL wheel of fire
// (ring plane = forward x up, i.e. a rolling tire aligned with travel) surrounds
// the player, who rides WITHIN it as it rolls forward. Glowing hoop bands + fire
// tufts licking outward around the rim, spinning to read as a rolling wheel.
// Additive. The ride override (movement block) carries the player. Migrate later.
// ---------------------------------------------------------------------------
static int   s_fw_active = 0, s_fw_life = 0;
static float s_fw_fx, s_fw_fz, s_fw_yaw;
#define FW_MAXLIFE 150        // ride ~2.5s
#define FW_SPEED   0.95f      // forward units/frame while riding
#define FW_RINGR   7.5f       // wheel radius (player sits within)
// Fire PARTICLE SYSTEM: real emitted+advected particles. Each spawns on the rim, inherits the spin
// (strong tangential velocity), drifts outward, rises with buoyancy, and cools white->yellow->
// orange->red as it ages. Rendered as velocity-STRETCHED soft sprites, so the licking tongues come
// from the particles' motion — not from authored shapes.
#define FW_PMAX 640
typedef struct { float x,y,z, vx,vy,vz, life, maxLife, size; } FwParticle;
static FwParticle s_fwP[FW_PMAX];
static int s_fwPN = 0;
static unsigned s_fwRng = 0x12345u;
static float fw_rand(void){ s_fwRng = s_fwRng*1664525u + 1013904223u; return (float)((s_fwRng>>8)&0xFFFF)/65535.0f; }
static void fw_emit(float cx,float cy,float cz, float Fx,float Fz, int count){
    float Ux=0.0f, Uy=1.0f, Uz=0.0f; float Fy=0.0f;
    for (int k=0;k<count && s_fwPN<FW_PMAX;k++){
        FwParticle* P=&s_fwP[s_fwPN++];
        float a=fw_rand()*6.2831853f, ca=cosf(a), sa=sinf(a);
        float ox=Fx*ca+Ux*sa, oy=Fy*ca+Uy*sa, oz=Fz*ca+Uz*sa;                    // outward radial (ring plane)
        float tx=-Fx*sa+Ux*ca, ty=-Fy*sa+Uy*ca, tz=-Fz*sa+Uz*ca;                 // tangent (spin drag)
        float rad=FW_RINGR*(0.96f + fw_rand()*0.10f);                             // spawn right on the band
        P->x=cx+ox*rad; P->y=cy+oy*rad; P->z=cz+oz*rad;
        // ALL sparks now: tiny hot flecks shed off the swirl strands (the strands themselves are
        // drawn as ribbons, not particles) — gentle tangential drift + outward fling.
        float tv = 0.25f+fw_rand()*0.35f;
        float rv = 0.12f+fw_rand()*0.50f;
        P->vx = tx*tv + ox*rv + (fw_rand()-0.5f)*0.08f + Fx*FW_SPEED*0.25f;
        P->vy = ty*tv + oy*rv + (fw_rand()-0.5f)*0.08f;
        P->vz = tz*tv + oz*rv + (fw_rand()-0.5f)*0.08f + Fz*FW_SPEED*0.25f;
        P->maxLife = P->life = 18.0f+fw_rand()*22.0f;
        P->size = 0.30f+fw_rand()*0.55f;
    }
}
// One luminous SWIRL STRAND — the light-painting streak the whole effect is built from. A soft
// ribbon following a WOBBLING arc around the wheel: radius undulates with two sine lobes (never a
// perfect circle), weaves slightly out of the wheel plane, and brightness CHASES around the arc so
// bright heads trail into dim tails. Drawn as two half-strips (bright spine -> alpha-0 edges).
// a0..a1 = arc range (full 2π = closed loop); radDrift adds radius across the arc (peel-away licks);
// fadeEnds ramps alpha to 0 at both ends for open arcs.
static void fw_strand(float cx,float cy,float cz, float Fx,float Fz,
                      float camFx,float camFy,float camFz,
                      float R, float a0,float a1,int K,
                      float wob1,float lob1,float ph1, float wob2,float lob2,float ph2,
                      float outAmp,float phOut, float radDrift,
                      float tme,float chase,float blobs,float ph4,
                      float hw, int cr,int cg,int cb, float baseA, int fadeEnds){
    float Ax=-Fz, Az=Fx;                                                          // wheel axis (weave dir)
    float PX[45],PY[45],PZ[45],PM[45];
    if(K>44)K=44; if(K<2)K=2;
    for(int j=0;j<=K;j++){ float s=(float)j/(float)K; float ang=a0+(a1-a0)*s;
        float rad=R*(1.0f + wob1*sinf(lob1*ang+ph1+tme*0.9f) + wob2*sinf(lob2*ang+ph2-tme*1.3f)) + radDrift*s;
        // FIRE FLICKER: fast jagged high-frequency perturbation (product of two chasing sines is
        // choppy, not silky) — this is what separates burning strands from smooth neon light trails.
        rad += R*0.016f*sinf(13.0f*ang+ph1*3.1f+tme*6.5f)*sinf(7.0f*ang-tme*10.3f+ph2*1.7f);
        float oop=outAmp*sinf(2.0f*ang+phOut+tme*0.7f);
        PX[j]=cx+Fx*cosf(ang)*rad+Ax*oop;
        PY[j]=cy+sinf(ang)*rad;
        PZ[j]=cz+Fz*cosf(ang)*rad+Az*oop;
        float m=0.5f+0.5f*sinf(blobs*ang+ph4-tme*chase); m*=m; m=0.25f+0.75f*m;   // chasing highlights
        m *= 0.68f + 0.32f*sinf(11.0f*ang+ph2*2.3f-tme*9.1f);                     // fast brightness sizzle
        if(fadeEnds){ float e=s<0.2f?s/0.2f:(s>0.8f?(1.0f-s)/0.2f:1.0f); m*=e; }
        PM[j]=m; }
    for(int half=0; half<2; half++){
        int n=0; float sgn = half ? -1.0f : 1.0f;
        for(int j=0;j<=K;j++){
            int jp=j>0?j-1:0, jn=j<K?j+1:K;
            float tx=PX[jn]-PX[jp], ty=PY[jn]-PY[jp], tz=PZ[jn]-PZ[jp];
            float wx=ty*camFz-tz*camFy, wy=tz*camFx-tx*camFz, wz=tx*camFy-ty*camFx;   // width billboards
            float wl=sqrtf(wx*wx+wy*wy+wz*wz); if(wl<0.0001f)wl=0.0001f; wx/=wl; wy/=wl; wz/=wl;
            int al=(int)(baseA*PM[j]); if(al>255)al=255; if(al<0)al=0;
            MMV(n, PX[j],PY[j],PZ[j], MM_COL(cr,cg,cb,al)); n++;
            MMV(n, PX[j]+wx*hw*sgn, PY[j]+wy*hw*sgn, PZ[j]+wz*hw*sgn, MM_COL(cr,cg,cb,0)); n++; }
        mm_flush(n, GL_TRIANGLE_STRIP);
    }
}
static void afn_flamewheel_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_fw_fx = sinf(yr); s_fw_fz = cosf(yr);      // forward (level) — the roll/travel direction
    s_fw_yaw = yaw;
    s_fw_life = FW_MAXLIFE; s_fw_active = 1;
    fw_emit(px, py + FW_RINGR*0.78f, pz, s_fw_fx, s_fw_fz, 50);                  // ignition spark burst
}
static void afn_flamewheel_step(void) { if (s_fw_active && !afn_paused) { if (--s_fw_life <= 0) s_fw_active = 0; } }
static void afn_flamewheel_render(const float* view, float px, float py, float pz) {
    if (!s_fw_active && s_fwPN==0) return;                                        // linger until every particle dies
    float Rx=view[0],Ry=view[4],Rz=view[8];                                      // camera right
    float Ucx=view[1],Ucy=view[5],Ucz=view[9];                                   // camera up
    float camFx=Ry*Ucz-Rz*Ucy, camFy=Rz*Ucx-Rx*Ucz, camFz=Rx*Ucy-Ry*Ucx;         // camera forward
    float fade = (s_fw_life < 18 && s_fw_active) ? (float)s_fw_life/18.0f : (s_fw_active?1.0f:0.0f);
    float Fx=s_fw_fx, Fz=s_fw_fz;
    float cx=px, cy=py + FW_RINGR*0.78f, cz=pz;                                   // wheel centre; player within

    // ---- SIM: advect + age every particle, then emit this frame's batch from the moving rim. ----
    if (!afn_paused) {
        for (int i=0;i<s_fwPN;){ FwParticle* P=&s_fwP[i];
            P->x+=P->vx; P->y+=P->vy; P->z+=P->vz;
            P->vx*=0.94f; P->vy=P->vy*0.94f+0.010f; P->vz*=0.94f;                 // drag + gentle buoyancy
            if ((P->life-=1.0f) <= 0.0f) { *P = s_fwP[--s_fwPN]; continue; }      // swap-remove dead
            i++; }
        if (s_fw_active && s_fw_life > 18) fw_emit(cx,cy,cz, Fx,Fz, 8);           // steady spark shed
    }

    afn_fire_tex_init();
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);                        // additive fire
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, s_fireTex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    // ---- RENDER: each particle is a soft sprite STRETCHED along its velocity (classic fire particle),
    //      tinted by age: white-yellow birth -> orange -> deep red death. Batched 21 quads per flush. ----
    int n=0;
    for (int i=0;i<s_fwPN;i++){ const FwParticle* P=&s_fwP[i];
        float t=1.0f - P->life/P->maxLife;                                        // age 0..1
        int g=(int)(215.0f - t*160.0f), b2=(int)(110.0f - t*100.0f);              // gold spark -> red cinder
        float aIn = t<0.12f ? t/0.12f : 1.0f;                                     // quick flash-in
        int al=(int)(190.0f*(1.0f-t)*aIn); if(al<2) continue;
        unsigned col=MM_COL(255,g,b2,al);
        float sz=P->size;
        float dx=P->vx, dy=P->vy, dz=P->vz;
        float sp=sqrtf(dx*dx+dy*dy+dz*dz);
        float hl;                                                                 // half-length along motion
        if (sp > 0.05f){ dx/=sp; dy/=sp; dz/=sp; hl=sz*0.6f + sp*1.4f; if(hl>2.2f)hl=2.2f; }
        else           { dx=Rx; dy=Ry; dz=Rz; hl=sz*0.55f; }
        float wx=dy*camFz-dz*camFy, wy=dz*camFx-dx*camFz, wz=dx*camFy-dy*camFx;   // width = dir x camFwd
        float wl=sqrtf(wx*wx+wy*wy+wz*wz); if(wl<0.001f){wx=Ucx;wy=Ucy;wz=Ucz;wl=1.0f;}
        wx/=wl; wy/=wl; wz/=wl;
        float hw=sz*0.5f;
        float ax=P->x-dx*hl, ay=P->y-dy*hl, az=P->z-dz*hl;                        // tail end
        float bx=P->x+dx*hl, by=P->y+dy*hl, bz=P->z+dz*hl;                        // head end
        if (n+6 > 126){ mm_flush(n, GL_TRIANGLES); n=0; }
        MMVT(n, ax-wx*hw, ay-wy*hw, az-wz*hw, 0.0f,0.0f, col); n++;
        MMVT(n, ax+wx*hw, ay+wy*hw, az+wz*hw, 0.0f,1.0f, col); n++;
        MMVT(n, bx-wx*hw, by-wy*hw, bz-wz*hw, 1.0f,0.0f, col); n++;
        MMVT(n, ax+wx*hw, ay+wy*hw, az+wz*hw, 0.0f,1.0f, col); n++;
        MMVT(n, bx+wx*hw, by+wy*hw, bz+wz*hw, 1.0f,1.0f, col); n++;
        MMVT(n, bx-wx*hw, by-wy*hw, bz-wz*hw, 1.0f,0.0f, col); n++;
    }
    if (n>0) mm_flush(n, GL_TRIANGLES);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisable(GL_TEXTURE_2D);       // back to plain geometry

    if (s_fw_active) {
        float tme=(float)(FW_MAXLIFE - s_fw_life)*0.06f;                          // strand anim clock
        // ---- SWIRL BUNDLE: 8 weaving light-streak strands — each an imperfect wobbling loop at its
        //      own radius/lobes/phase, glow pass + hot thin core pass. Their overlap and chasing
        //      brightness make the ring; there is NO perfect circle anywhere. ----
        for (int sI=0;sI<8;sI++){ unsigned h=(unsigned)(sI+1)*2654435761u ^ 0xB5297A4Du;
            float h1=(float)(h&0xFF)/255.0f, h2=(float)((h>>8)&0xFF)/255.0f, h3=(float)((h>>16)&0xFF)/255.0f, h4=(float)((h>>24)&0xFF)/255.0f;
            float Rs=FW_RINGR + (h1-0.5f)*1.8f;
            float wob1=0.030f+h2*0.045f, lob1=2.0f+(float)(h&3u);
            float wob2=0.018f+h3*0.028f, lob2=5.0f+(float)((h>>4)&3u);
            float outA=0.5f+h4*1.7f;
            float chase=1.2f+h2*2.2f, blobs=2.0f+(float)((h>>6)&2u);
            float rot=tme*(1.8f+h1*0.9f);                                          // each strand ROLLS (whole wheel spins)
            fw_strand(cx,cy,cz, Fx,Fz, camFx,camFy,camFz, Rs, rot,rot+6.2831853f,44,
                      wob1,lob1,h1*6.28f, wob2,lob2,h2*6.28f, outA,h3*6.28f, 0.0f,
                      tme,chase,blobs,h4*6.28f, 2.30f, 255,70,10, 95.0f*fade, 0);    // deep red-orange glow (thick)
            fw_strand(cx,cy,cz, Fx,Fz, camFx,camFy,camFz, Rs, rot,rot+6.2831853f,44,
                      wob1,lob1,h1*6.28f, wob2,lob2,h2*6.28f, outA,h3*6.28f, 0.0f,
                      tme,chase,blobs,h4*6.28f, 0.85f, 255,190,70, 200.0f*fade, 0);  // gold core (crossings flare white)
        }
        // ---- FLAME TEETH: many short sharp arcs peeling OUT off the ring, flickering in and out at
        //      spots that crawl around it — the jagged tongue fringe of the wheel. ----
        for (int sI=0;sI<14;sI++){ unsigned h=(unsigned)(sI+11)*2246822519u ^ 0x68E31DA4u;
            float h1=(float)(h&0xFF)/255.0f, h2=(float)((h>>8)&0xFF)/255.0f, h3=(float)((h>>16)&0xFF)/255.0f, h4=(float)((h>>24)&0xFF)/255.0f;
            float cyc=fmodf(h1 + tme*(0.22f+h3*0.18f), 1.0f);
            float vis=sinf(cyc*3.14159265f);                                       // flicker in -> out
            float aS=h2*6.2831853f + tme*(2.2f+h4*2.6f);                            // spots RACE around the rim (pronounced spin)
            float sweep=0.40f+h3*0.65f;                                            // longer, bolder teeth
            fw_strand(cx,cy,cz, Fx,Fz, camFx,camFy,camFz, FW_RINGR+0.2f, aS, aS+sweep, 12,
                      0.03f,5.0f,h4*6.28f, 0.0f,7.0f,0.0f, 0.3f+h4*0.7f,h1*6.28f, 1.8f+h2*3.0f,
                      tme,0.0f,1.0f,0.0f, 0.95f, 255,160,50, 230.0f*vis*fade, 1);
            // hot inner streak inside each tooth so they read as solid flame, not wisps
            fw_strand(cx,cy,cz, Fx,Fz, camFx,camFy,camFz, FW_RINGR+0.2f, aS, aS+sweep, 12,
                      0.03f,5.0f,h4*6.28f, 0.0f,7.0f,0.0f, 0.3f+h4*0.7f,h1*6.28f, 1.8f+h2*3.0f,
                      tme,0.0f,1.0f,0.0f, 0.40f, 255,225,120, 235.0f*vis*fade, 1);
        }
        mm_glow(cx, py+0.5f, cz, 1,0,0, 0,0,1, 7.5f,7.5f, MM_COL(255,140,40,(int)(95*fade)), MM_COL(255,90,20,0));
    }

    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// PHYSICAL CLASH — HARDCODED prototype. When the player's Quick Attack and the
// enemy's dash-in meet head-on, both lock into a PRESSURE STRUGGLE: a button
// prompt (Cross/Circle/Square/Triangle) floats over the fighters — hit the
// matching button to shove the meter toward the enemy; the AI shoves back on
// its own cadence. Prompts start SLOW near the middle and come faster and
// faster as the meter is pushed toward either side. Overflow a side to win:
// the loser eats damage + knockback. Migrate to nodes once the feel is right.
// ---------------------------------------------------------------------------
static int   s_pc_active=0, s_pc_cd=0, s_pc_t=0;
static float s_pc_pressure=0.5f;                 // 0 = player overwhelmed (loss) .. 1 = enemy overwhelmed (win)
static int   s_pc_cmd=0, s_pc_cmdT=0, s_pc_aiT=0, s_pc_knock=0, s_pc_won=0, s_pc_flash=0;
static float s_pc_mx,s_pc_my,s_pc_mz;            // struggle midpoint (bar/prompt anchor)
static float s_pc_pxl,s_pc_pzl, s_pc_exl,s_pc_ezl;   // locked player/enemy positions
static float s_pc_dirx,s_pc_dirz;                // player->enemy dir (facing + knockback axis)
static int   s_pc_pG=0, s_pc_eG=0;               // dash GRACE — frames since each side's dash was live
                                                 // (forgiving trigger: a bump right as a dash ends still clashes)
static unsigned s_pc_rng=0x51u;
static float pc_rand(void){ s_pc_rng=s_pc_rng*1664525u+1013904223u; return (float)((s_pc_rng>>8)&0xFFFF)/65535.0f; }
#define PC_MEET_R    24.0f    // dash-vs-dash contact radius that triggers the clash (roomy)
#define PC_PUSH      0.060f   // player shove per correct prompt
#define PC_MISS      0.025f   // bleed-back for hitting the WRONG face button
#define PC_AI_PUSH   0.048f   // AI shove per cadence tick
#define PC_DMG_E     12       // damage to the enemy on a win
#define PC_DMG_P     10       // damage to the player on a loss
#define PC_CD        150      // frames before another clash can trigger
// Cadence: slow near the centre, quickening toward the edges — BOTH the player's prompt
// window and the AI's shove interval tighten as the meter leaves the middle. The floor is
// kept humane (was 55->18 / 50->20 — too fast to read near the end).
// Past the 80% mark of either side (d>0.6), an EXTRA kick makes the final stretch noticeably quicker.
static int pc_window(void){ float d=fabsf(s_pc_pressure-0.5f)*2.0f; float w=55.0f - d*30.0f; if(d>0.6f) w-=(d-0.6f)*6.0f; return (int)w; }
static int pc_ai_wait(void){ float d=fabsf(s_pc_pressure-0.5f)*2.0f; float w=50.0f - d*27.0f; if(d>0.6f) w-=(d-0.6f)*5.0f; return (int)(w*(0.8f+pc_rand()*0.5f)); }
static int pc_enemy_npc(void){ for(int i=0;i<AFN_NPC_COUNT;i++) if((int)afn_npc_inst[i][7]==AFN_ENEMY_EIDX) return i; return -1; }
// (The struggle UI is drawn as a fullscreen 2D cut-in — physclash_render_2d, defined with the
//  other HUD-space renderers next to clash_render_2d — matching the beam clash's presentation.)

// ---------------------------------------------------------------------------
// Flash Cannon — HARDCODED prototype (fired on Select). A blinding silver-white
// BEAM BLAST: a dense bundle of near-white streaks racing down an expanding
// cone, a solid white core beam, steel-blue fringe, a pale-teal orb riding the
// beam head, and a big muzzle flash with curved wisps sweeping around it.
// Additive. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int   s_fc_active=0, s_fc_life=0;
static float s_fc_ox,s_fc_oy,s_fc_oz, s_fc_fx,s_fc_fz;
#define FC_MAXLIFE 80
#define FC_RANGE   62.0f
static void afn_flashcannon_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_fc_fx=sinf(yr); s_fc_fz=cosf(yr);
    s_fc_ox=px+s_fc_fx*4.0f; s_fc_oy=py+8.0f; s_fc_oz=pz+s_fc_fz*4.0f;           // chest-height muzzle
    s_fc_life=FC_MAXLIFE; s_fc_active=1;
}
static void afn_flashcannon_step(void){ if(s_fc_active && !afn_paused){ if(--s_fc_life<=0) s_fc_active=0; } }
static void afn_flashcannon_render(const float* view){
    if(!s_fc_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float camFx=Ry*Uz-Rz*Uy, camFy=Rz*Ux-Rx*Uz, camFz=Rx*Uy-Ry*Ux;               // camera forward (for wisps)
    float t=(float)(FC_MAXLIFE - s_fc_life);
    float fadeIn = t<4.0f ? t/4.0f : 1.0f;
    float fadeOut = s_fc_life<16 ? (float)s_fc_life/16.0f : 1.0f;
    float fade = fadeIn*fadeOut;
    float Fx=s_fc_fx, Fy=0.0f, Fz=s_fc_fz;
    float Axb=Fz, Azb=-Fx;                                                        // beam cross basis A (horizontal)
    float ox=s_fc_ox, oy=s_fc_oy, oz=s_fc_oz;
    float ext = t<9.0f ? t/9.0f : 1.0f; ext=1.0f-(1.0f-ext)*(1.0f-ext);          // beam extends fast (ease-out)
    float front = ext*FC_RANGE;
    float hx=ox+Fx*front, hy=oy, hz=oz+Fz*front;                                  // beam head
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);                        // additive light blast
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // ---- CORE BEAM: overlapping thick soft trails from muzzle to head — blinding white centre,
    //      steel-blue sheath; tapers thin back at the muzzle so it flares open toward the head. ----
    if (front > 1.0f){
        int aC=(int)(235*fade);
        mm_trail(hx,hy,hz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, front, 6.0f, MM_COL(150,200,255,(int)(aC*0.40f)), MM_COL(120,175,255,0));   // outer sheath
        mm_trail(hx,hy,hz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, front, 3.8f, MM_COL(215,238,255,(int)(aC*0.70f)), MM_COL(170,215,255,0));   // mid
        mm_trail(hx,hy,hz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, front, 2.0f, MM_COL(255,255,255,aC),              MM_COL(230,245,255,0)); } // white core

    // ---- STREAK BUNDLE: fast near-white streaks racing down an expanding cone around the core. ----
    int NS=36;
    for (int i=0;i<NS;i++){ unsigned h=(unsigned)(i+1)*2654435761u ^ 0x9E3779B9u;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc2=(float)((h>>16)&0xFF)/255.0f, fd=(float)((h>>24)&0xFF)/255.0f;
        float cyc=fmodf(fc2 + t*0.055f, 1.0f);                                    // race forward, staggered
        float dist=cyc*(front+5.0f); if(dist>FC_RANGE+6.0f) continue;
        float coneR=0.8f + 5.4f*(dist/FC_RANGE);                                  // cone widens with distance
        float ang=fa*6.2831853f + fb*2.1f;
        float rdx=Axb*cosf(ang), rdy=sinf(ang), rdz=Azb*cosf(ang);                // around the beam axis
        float R=coneR*(0.15f + fb*0.85f);
        float sx=ox+Fx*dist+rdx*R, sy=oy+rdy*R, sz=oz+Fz*dist+rdz*R;
        float av=sinf(cyc*3.14159265f);
        int a=(int)(205*fade*av); if(a<2) continue;
        float len=7.0f+fd*11.0f, w=0.30f+fd*0.75f;
        if ((h&3u)==0)
            mm_trail(sx,sy,sz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, len, w, MM_COL(255,255,255,a),      MM_COL(235,248,255,0));   // white streak
        else
            mm_trail(sx,sy,sz, Fx,Fy,Fz, Rx,Ry,Rz, Ux,Uy,Uz, len, w, MM_COL(195,228,255,a),      MM_COL(150,200,255,0)); } // steel-blue streak

    // ---- HEAD ORB: pale-teal sphere riding the front of the blast. ----
    if (front > 2.0f){ float pulse=1.0f+0.08f*sinf(t*0.5f); int aO=(int)(230*fade);
        mm_glow(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, 5.2f*pulse,5.2f*pulse, MM_COL(140,235,235,(int)(aO*0.5f)), MM_COL(110,210,220,0));
        mm_glow(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, 3.0f*pulse,3.0f*pulse, MM_COL(200,250,250,(int)(aO*0.8f)), MM_COL(160,235,235,0));
        mm_fill_oval(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, 1.6f*pulse,1.6f*pulse, MM_COL(240,255,255,aO)); }

    // ---- MUZZLE FLASH: hot white burst at the origin, strongest early, plus curved wisps
    //      (fw_strand arcs in the beam's vertical plane) sweeping around the muzzle. ----
    { float mf=1.0f - t/(float)FC_MAXLIFE; mf=0.35f+0.65f*mf*mf;
      float rad=6.0f+3.0f*sinf(t*0.4f)*0.3f;
      mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad*1.7f,rad*1.7f, MM_COL(190,225,255,(int)(120*mf*fade)), MM_COL(150,200,255,0));
      mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad,rad,           MM_COL(255,255,255,(int)(200*mf*fade)), MM_COL(230,245,255,0)); }
    for (int sI=0;sI<4;sI++){ unsigned h=(unsigned)(sI+3)*2246822519u ^ 0x68E31DA4u;
        float h1=(float)(h&0xFF)/255.0f, h2=(float)((h>>8)&0xFF)/255.0f, h3=(float)((h>>16)&0xFF)/255.0f;
        float tme=t*0.06f;
        float cyc=fmodf(h1 + tme*0.30f, 1.0f); float vis=sinf(cyc*3.14159265f);
        float aS=h2*6.2831853f + tme*1.6f;
        fw_strand(ox,oy,oz, Fx,Fz, camFx,camFy,camFz, 2.6f+h3*3.4f, aS, aS+1.6f+h1*1.2f, 14,
                  0.04f,3.0f,h2*6.28f, 0.0f,7.0f,0.0f, 0.5f+h3*0.9f,h1*6.28f, 1.2f+h2*2.0f,
                  tme,0.0f,1.0f,0.0f, 0.45f, 255,255,255, 160.0f*vis*fade, 1);
    }

    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Bubble Beam — HARDCODED prototype. A forward stream of TRANSLUCENT bubbles: a
// looping fleet of camera-facing bubbles (soft blue body + bright rim + a white
// highlight) that drift forward, spread into a cone, wobble, and pop at the end.
// Uses ALPHA blend (not additive) so the bubbles read as see-through. Migrate later.
// ---------------------------------------------------------------------------
static int   s_bb_active = 0, s_bb_life = 0;
static float s_bb_ox,s_bb_oy,s_bb_oz, s_bb_fx,s_bb_fy,s_bb_fz, s_bb_ax,s_bb_ay,s_bb_az, s_bb_bx,s_bb_by,s_bb_bz;
#define BB_MAXLIFE 120
static void afn_bubblebeam_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_bb_fx=sinf(yr); s_bb_fy=0.0f; s_bb_fz=cosf(yr);      // forward
    s_bb_ax=cosf(yr); s_bb_ay=0.0f; s_bb_az=-sinf(yr);     // right (cone spread basis)
    s_bb_bx=0.0f;     s_bb_by=1.0f; s_bb_bz=0.0f;          // up
    s_bb_ox=px+s_bb_fx*4.0f; s_bb_oy=py+7.0f; s_bb_oz=pz+s_bb_fz*4.0f;   // mouth height, just ahead
    s_bb_life=BB_MAXLIFE; s_bb_active=1;
}
static void afn_bubblebeam_step(void) { if (s_bb_active && !afn_paused) { if (--s_bb_life <= 0) s_bb_active = 0; } }
static void afn_bubblebeam_render(const float* view) {
    if (!s_bb_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float fade = (s_bb_life < 20) ? (float)s_bb_life/20.0f : 1.0f;
    static int frame=0; frame++;
    float ox=s_bb_ox,oy=s_bb_oy,oz=s_bb_oz;
    float Fx=s_bb_fx,Fy=s_bb_fy,Fz=s_bb_fz, Ax=s_bb_ax,Ay=s_bb_ay,Az=s_bb_az, Bx=s_bb_bx,By=s_bb_by,Bz=s_bb_bz;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // TRANSLUCENT (not additive)
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int N=26; float range=40.0f;
    float front = (float)(BB_MAXLIFE - s_bb_life) / 26.0f; if (front > 1.0f) front = 1.0f;   // stream extends forward over ~26 frames (ramp, not instant)
    for (int i=0;i<N;i++){ unsigned h=(unsigned)(i+1)*2654435761u ^ 0x9E3779B9u;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f;
        float ph=fmodf((float)frame*0.02f + fc, 1.0f);                    // travel phase (loops -> a steady stream)
        if (ph > front) continue;                                         // the stream front hasn't reached this bubble yet
        float dist=ph*range;
        float sAng=fa*6.2831853f, sR=(0.5f+fb*2.5f)*(0.3f+ph*1.6f);        // cone spread widens with distance
        float wob=sinf((float)frame*0.15f + (float)i)*0.7f;               // gentle wobble
        float cx=ox+Fx*dist+(Ax*cosf(sAng)+Bx*sinf(sAng))*sR+Ax*wob;
        float cy=oy+Fy*dist+(Ay*cosf(sAng)+By*sinf(sAng))*sR+Ay*wob;
        float cz=oz+Fz*dist+(Az*cosf(sAng)+Bz*sinf(sAng))*sR+Az*wob;
        float rad=0.6f+fb*1.1f;                                           // varied bubble sizes (smaller)
        float A=fade; if(ph>0.85f) A*=(1.0f-ph)/0.15f; if(ph<0.05f) A*=ph/0.05f;   // fade in / pop at the end
        mm_fill_oval(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, rad,rad, MM_COL(150,200,255,(int)(75*A)));          // translucent body
        mm_ring     (cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, rad,rad, rad*0.16f, MM_COL(215,235,255,(int)(185*A)));  // bright rim
        float hx=cx+Ux*rad*0.4f-Rx*rad*0.35f, hy=cy+Uy*rad*0.4f-Ry*rad*0.35f, hz=cz+Uz*rad*0.4f-Rz*rad*0.35f;
        mm_fill_oval(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, rad*0.24f,rad*0.24f, MM_COL(255,255,255,(int)(200*A))); // highlight
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Ice Beam — HARDCODED prototype. Three strands of ice DIAMONDS along a forward
// beam: a STRAIGHT light-blue centre line, plus TWO dark-blue side strands that
// wind around the axis in a helix (180 deg apart) and rotate over time. Translucent
// crystals. The beam shoots out (front ramps forward) then holds. Migrate later.
// ---------------------------------------------------------------------------
static int   s_ib_active = 0, s_ib_life = 0;
static float s_ib_ox,s_ib_oy,s_ib_oz, s_ib_fx,s_ib_fy,s_ib_fz, s_ib_ax,s_ib_ay,s_ib_az, s_ib_bx,s_ib_by,s_ib_bz;
#define IB_MAXLIFE 130
// A camera-facing DIAMOND (rhombus): center + up/right/down/left as a triangle fan.
static void mm_diamond(float cx,float cy,float cz, float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                       float w,float h, unsigned col) {
    int n=0;
    MMV(n,cx,cy,cz,col); n++;
    MMV(n, cx+Ux*h, cy+Uy*h, cz+Uz*h, col); n++;   // top
    MMV(n, cx+Rx*w, cy+Ry*w, cz+Rz*w, col); n++;   // right
    MMV(n, cx-Ux*h, cy-Uy*h, cz-Uz*h, col); n++;   // bottom
    MMV(n, cx-Rx*w, cy-Ry*w, cz-Rz*w, col); n++;   // left
    MMV(n, cx+Ux*h, cy+Uy*h, cz+Uz*h, col); n++;   // close back to top
    mm_flush(n, GL_TRIANGLE_FAN);
}
static unsigned mm_lerp_col(unsigned a, unsigned b, float t) {
    int ar=a&0xFF, ag=(a>>8)&0xFF, ab=(a>>16)&0xFF, aa=(a>>24)&0xFF;
    int br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF, ba=(b>>24)&0xFF;
    int r=ar+(int)((br-ar)*t), g=ag+(int)((bg-ag)*t), bl=ab+(int)((bb-ab)*t), al=aa+(int)((ba-aa)*t);
    return (unsigned)(r&0xFF)|((unsigned)(g&0xFF)<<8)|((unsigned)(bl&0xFF)<<16)|((unsigned)(al&0xFF)<<24);
}
// Faceted diamond: the 4 triangles (top-right, right-bottom, bottom-left, left-top) are drawn as
// FLAT separate triangles, each its own shade — so the crystal reads as 4 cut facets refracting
// light rather than a smooth blob.
static void mm_diamond_facet(float cx,float cy,float cz, float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                             float w,float h, unsigned c1, unsigned c2, unsigned c3, unsigned c4) {
    float Tx=cx+Ux*h, Ty=cy+Uy*h, Tz=cz+Uz*h;   // top
    float Px=cx+Rx*w, Py=cy+Ry*w, Pz=cz+Rz*w;   // right
    float Bx=cx-Ux*h, By=cy-Uy*h, Bz=cz-Uz*h;   // bottom
    float Lx=cx-Rx*w, Ly=cy-Ry*w, Lz=cz-Rz*w;   // left
    int n=0;
    MMV(n,cx,cy,cz,c1); n++; MMV(n,Tx,Ty,Tz,c1); n++; MMV(n,Px,Py,Pz,c1); n++;   // top-right facet
    MMV(n,cx,cy,cz,c2); n++; MMV(n,Px,Py,Pz,c2); n++; MMV(n,Bx,By,Bz,c2); n++;   // right-bottom
    MMV(n,cx,cy,cz,c3); n++; MMV(n,Bx,By,Bz,c3); n++; MMV(n,Lx,Ly,Lz,c3); n++;   // bottom-left
    MMV(n,cx,cy,cz,c4); n++; MMV(n,Lx,Ly,Lz,c4); n++; MMV(n,Tx,Ty,Tz,c4); n++;   // left-top
    mm_flush(n, GL_TRIANGLES);
}
// Gradient diamond: bright colHi toward the top/left facets, dark colLo toward bottom/right, so
// each crystal shades from a highlight to a dark edge (per-vertex colours).
static void mm_diamond_grad(float cx,float cy,float cz, float Rx,float Ry,float Rz, float Ux,float Uy,float Uz,
                            float w,float h, unsigned colHi, unsigned colLo) {
    unsigned colMid = mm_lerp_col(colHi, colLo, 0.5f);
    int n=0;
    MMV(n,cx,cy,cz,colMid); n++;
    MMV(n, cx+Ux*h, cy+Uy*h, cz+Uz*h, colHi); n++;   // top    (highlight)
    MMV(n, cx+Rx*w, cy+Ry*w, cz+Rz*w, colLo); n++;   // right  (dark)
    MMV(n, cx-Ux*h, cy-Uy*h, cz-Uz*h, colLo); n++;   // bottom (dark)
    MMV(n, cx-Rx*w, cy-Ry*w, cz-Rz*w, colHi); n++;   // left   (highlight)
    MMV(n, cx+Ux*h, cy+Uy*h, cz+Uz*h, colHi); n++;   // close
    mm_flush(n, GL_TRIANGLE_FAN);
}
static void afn_icebeam_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_ib_fx=sinf(yr); s_ib_fy=0.0f; s_ib_fz=cosf(yr);      // forward
    s_ib_ax=cosf(yr); s_ib_ay=0.0f; s_ib_az=-sinf(yr);     // helix cross-section basis
    s_ib_bx=0.0f;     s_ib_by=1.0f; s_ib_bz=0.0f;
    s_ib_ox=px+s_ib_fx*4.0f; s_ib_oy=py+7.0f; s_ib_oz=pz+s_ib_fz*4.0f;
    s_ib_life=IB_MAXLIFE; s_ib_active=1;
}
static void afn_icebeam_step(void) { if (s_ib_active && !afn_paused) { if (--s_ib_life <= 0) s_ib_active = 0; } }
static void afn_icebeam_render(const float* view) {
    if (!s_ib_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float fade = (s_ib_life < 18) ? (float)s_ib_life/18.0f : 1.0f;
    static int frame=0; frame++;
    float ox=s_ib_ox,oy=s_ib_oy,oz=s_ib_oz;
    float Fx=s_ib_fx,Fy=s_ib_fy,Fz=s_ib_fz, Ax=s_ib_ax,Ay=s_ib_ay,Az=s_ib_az, Bx=s_ib_bx,By=s_ib_by,Bz=s_ib_bz;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // translucent ice crystals
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int N=22; float range=46.0f, coils=3.0f, helixR=3.0f;
    float front = (float)(IB_MAXLIFE - s_ib_life)/20.0f; if (front > 1.0f) front = 1.0f;   // beam shoots out over ~20 frames
    int a=(int)(255*fade);
    // CENTER strand: straight down the beam axis (no spiral), LIGHT BLUE crystal.
    for (int i=0;i<N;i++){ float t=(float)i/(float)(N-1); if (t > front) continue;
        float dist=t*range; float cx=ox+Fx*dist, cy=oy+Fy*dist, cz=oz+Fz*dist;
        mm_diamond_grad(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 1.0f, 1.18f, MM_COL(230,244,255,(int)(150*fade)), MM_COL(120,185,250,(int)(150*fade)));   // light-blue gradient body (shorter)
        { int cc=(int)(0.9f*a);   // 4-facet refracting core (light-blue/white)
          mm_diamond_facet(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 0.57f,0.90f,
              MM_COL(250,253,255,cc), MM_COL(170,210,255,cc), MM_COL(125,182,250,cc), MM_COL(210,235,255,cc)); }
    }
    // TWO SIDE strands: wind around the axis in a helix (180 deg apart), DARK BLUE.
    for (int s=0;s<2;s++){ float ph0=(float)s*3.14159265f;
        for (int i=0;i<N;i++){ float t=(float)i/(float)(N-1); if (t > front) continue;
            float dist=t*range; float ang=t*coils*6.2831853f + (float)frame*0.12f + ph0;   // helix winds + rotates
            float cx=ox+Fx*dist+(Ax*cosf(ang)+Bx*sinf(ang))*helixR;
            float cy=oy+Fy*dist+(Ay*cosf(ang)+By*sinf(ang))*helixR;
            float cz=oz+Fz*dist+(Az*cosf(ang)+Bz*sinf(ang))*helixR;
            mm_diamond_grad(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 0.8f, 1.2f,  MM_COL(120,165,240,(int)(180*fade)), MM_COL(18,45,150,(int)(180*fade)));  // dark-blue gradient body
            { int ca=(int)(0.9f*a);   // 4-facet refracting core: each facet a distinct blue shade
              mm_diamond_facet(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 0.44f,0.70f,
                  MM_COL(215,232,255,ca), MM_COL(70,120,225,ca), MM_COL(28,62,175,ca), MM_COL(120,165,240,ca)); }
        }
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Icy Wind — HARDCODED prototype (fired on Select). A hazy ICE MIST cloud blows
// forward from the player; light-blue crystal diamonds (reused from Ice Beam)
// SHOOT through the mist on fast staggered cycles — same "streak within the
// corridor" trick as the Meteor Mash ovals. Two blend passes: soft translucent
// fog (alpha) + bright additive ice glints. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int   s_iw_active = 0, s_iw_life = 0;
static float s_iw_ox,s_iw_oy,s_iw_oz, s_iw_fx,s_iw_fy,s_iw_fz, s_iw_ax,s_iw_ay,s_iw_az, s_iw_bx,s_iw_by,s_iw_bz;
#define IW_MAXLIFE 110        // short life -> fast sweep
#define IW_RANGE   150.0f     // travels far across the map (projectile-like)
#define IW_BAND    34.0f      // depth of the traveling wall (the sheet's thickness)
static void afn_icywind_fire(float px, float py, float pz, float yaw) {
    float yr = yaw * (3.14159265f/180.0f);
    s_iw_fx=sinf(yr); s_iw_fy=0.0f; s_iw_fz=cosf(yr);      // forward (level)
    s_iw_ax=cosf(yr); s_iw_ay=0.0f; s_iw_az=-sinf(yr);     // right
    s_iw_bx=0.0f;     s_iw_by=1.0f; s_iw_bz=0.0f;          // up
    s_iw_ox=px+s_iw_fx*4.0f; s_iw_oy=py+8.0f; s_iw_oz=pz+s_iw_fz*4.0f;   // chest-height muzzle
    s_iw_life=IW_MAXLIFE; s_iw_active=1;
}
static void afn_icywind_step(void) { if (s_iw_active && !afn_paused) { if (--s_iw_life <= 0) s_iw_active = 0; } }
static void afn_icywind_render(const float* view) {
    if (!s_iw_active) return;
    float t = (float)(IW_MAXLIFE - s_iw_life);                                   // frames elapsed
    float fadeIn  = t < 8.0f ? t/8.0f : 1.0f;
    float fadeOut = s_iw_life < 20 ? (float)s_iw_life/20.0f : 1.0f;
    float fade = fadeIn * fadeOut;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];   // camera right / up
    float Fx=s_iw_fx,Fy=s_iw_fy,Fz=s_iw_fz;
    float Ax=s_iw_ax,Ay=s_iw_ay,Az=s_iw_az, Bx=s_iw_bx,By=s_iw_by,Bz=s_iw_bz;
    float ox=s_iw_ox, oy=s_iw_oy, oz=s_iw_oz;
    // Shard orientation is WORLD-anchored (no camera swing): the long axis IS the world travel dir F;
    // the width axis = F x cameraForward, so only the thin width billboards to face the camera.
    float camFx=Ry*Uz-Rz*Uy, camFy=Rz*Ux-Rx*Uz, camFz=Rx*Uy-Ry*Ux;             // camera forward = right x up
    float SPx=Fy*camFz-Fz*camFy, SPy=Fz*camFx-Fx*camFz, SPz=Fx*camFy-Fy*camFx;   // width dir (perp to F, faces cam)
    float spl=sqrtf(SPx*SPx+SPy*SPy+SPz*SPz); if(spl<0.001f)spl=0.001f; SPx/=spl; SPy/=spl; SPz/=spl;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    // A broad TALL sheet of icy wind that TRAVELS forward fast like a projectile: a fixed-depth
    // wall (`IW_BAND` thick) whose leading edge rides `frontDist`, sweeping across the map. Fog and
    // the ice glints are both placed within this band, so the whole sheet moves as one.
    float prog = t/(float)IW_MAXLIFE;
    float frontDist = prog * IW_RANGE;                                           // constant-speed sweep (projectile)
    float halfW=30.0f, wallH=26.0f;                                              // WIDE + tall -> a broad standing sheet

    // ---- WALL: fog band riding the moving front, thickest mid-band, soft at edges. ----
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);       // translucent mist
    int NH=170;
    for (int i=0;i<NH;i++){ unsigned h=(unsigned)(i+1)*2246822519u ^ 0x85EBCA6Bu;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f, fd=(float)((h>>24)&0xFF)/255.0f;
        float back=fc*IW_BAND;                                                    // distance trailing the leading edge
        float dist=frontDist - back;                                             // the whole band rides forward together
        if (dist < 0.0f) continue;                                               // hasn't emerged from the muzzle yet
        float lateral=(fa*2.0f-1.0f)*halfW;                                       // wide across the wall
        float vy=fd*wallH;                                                        // rises from the ground -> a curtain
        float curl=sinf(t*0.03f + fb*6.2831853f)*1.9f + sinf(dist*0.20f + t*0.05f)*1.3f;   // rolling billow
        float cx=ox+Fx*dist+Ax*lateral, cy=oy-3.0f+vy+curl, cz=oz+Fz*dist+Az*lateral;
        float lead=back/IW_BAND;                                                  // 0 at leading edge, 1 at trailing edge
        float env=1.0f - fabsf(lead-0.4f)*1.5f; if(env<0.0f)env=0.0f;             // fullest just behind the edge
        int a=(int)(52*fade*env); if(a<0)a=0;
        float pr=10.0f+fb*7.0f;
        mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, pr,pr, MM_COL(205,228,255,a), MM_COL(180,210,255,0)); }

    // ---- MUZZLE GUST: a bright cold burst at the mouth for the first ~third. ----
    if (t < IW_MAXLIFE*0.32f){ float f=1.0f - t/(IW_MAXLIFE*0.32f); float rad=5.0f+(1.0f-f)*10.0f;
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad,rad,           MM_COL(190,225,255,(int)(150*f)), MM_COL(160,205,255,0));
        mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, rad*0.5f,rad*0.5f, MM_COL(240,250,255,(int)(210*f)), MM_COL(240,250,255,0)); }

    // ---- ICE DIAMONDS: light-blue crystals rushing FORWARD through the traveling wall (meteor-oval
    //      trick). Each cycles from the band's tail up to its leading edge, so they ride the sweep. ----
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);                                           // additive glints

    // ---- SHARDS: thin very-extruded ice streaks firing forward through the wall, elongated along
    //      travel; each ENTERS and EXITS continually on its own fast cycle (Meteor Mash oval trick). ----
    int NS=34;
    for (int i=0;i<NS;i++){ unsigned h=(unsigned)(i+1)*2246822519u ^ 0x165667B1u;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f, fd=(float)((h>>24)&0xFF)/255.0f;
        float cyc=fmodf(fa + t*0.06f + fc, 1.0f);                                // fast forward cycle (enter->exit)
        float dist=(frontDist - IW_BAND) + cyc*(IW_BAND + 8.0f);                 // shoots through/just past the front
        if (dist < 0.0f) continue;
        float lateral=(fb*2.0f-1.0f)*halfW;                                       // across the full width
        float vy=fd*wallH;                                                        // up the curtain
        float cx=ox+Fx*dist+Ax*lateral, cy=oy-3.0f+vy, cz=oz+Fz*dist+Az*lateral;
        float av=sinf(cyc*3.14159265f);                                          // enter -> peak -> EXIT
        int a=(int)(200*fade*av); if(a<1) continue;
        float len=2.6f + fb*3.2f, wid=0.13f + fb*0.11f;                          // very extruded (long + thin)
        mm_fill_oval(cx,cy,cz, Fx,Fy,Fz, SPx,SPy,SPz, len, wid, MM_COL(205,230,255,(int)(a*0.85f)));   // pale-blue streak (world-aligned)
        mm_fill_oval(cx,cy,cz, Fx,Fy,Fz, SPx,SPy,SPz, len*0.6f, wid*0.6f, MM_COL(255,255,255,a)); }    // bright white core

    int ND=54;
    for (int i=0;i<ND;i++){ unsigned h=(unsigned)(i+1)*2654435761u ^ 0x27D4EB2Fu;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f, fd=(float)((h>>24)&0xFF)/255.0f;
        float cyc=fmodf(fa + t*0.05f + fc, 1.0f);                                // 0 = band tail -> 1 = leading edge, fast
        float dist=(frontDist - IW_BAND) + cyc*(IW_BAND + 6.0f);                 // shoots forward through/just past the front
        if (dist < 0.0f) continue;                                               // still behind the muzzle at launch
        float lateral=(fb*2.0f-1.0f)*halfW*0.9f;                                  // spread across the sheet width
        float vy=fd*wallH;                                                        // spread up the curtain
        float cx=ox+Fx*dist+Ax*lateral, cy=oy-3.0f+vy, cz=oz+Fz*dist+Az*lateral;
        float av=sinf(cyc*3.14159265f);                                          // appear -> peak -> VANISH across the band
        int a=(int)(230*fade*av); if(a<1) continue;
        // bucketed size: most glints small, a few medium, rare big (not all large)
        float sc = (fb < 0.60f) ? 0.6f + fb*0.55f                                // ~60% small   (0.60 .. 0.93)
                 : (fb < 0.88f) ? 1.1f + (fb-0.60f)*1.6f                         // ~28% medium  (1.10 .. 1.55)
                                : 2.0f + (fb-0.88f)*4.5f;                        // ~12% big     (2.00 .. 2.54)
        float rot=fa*6.2831853f + t*0.02f;                                       // slight per-glint tilt + drift
        // soft cold bloom behind the glint
        mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sc*1.3f,sc*1.3f, MM_COL(190,222,255,(int)(a*0.35f)), MM_COL(170,205,255,0));
        // main 4-point shine: white spikes fading to transparent tips, hot near-white core
        mm_shine4(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sc, rot,
                  MM_COL(200,228,255,(int)(a*0.55f)), MM_COL(255,255,255,a));
        // smaller, brighter cross rotated 45deg for the crisp twinkle centre
        mm_shine4(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sc*0.5f, rot+0.785398f,
                  MM_COL(225,240,255,(int)(a*0.7f)), MM_COL(255,255,255,a)); }

    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Sludge Bomb — HARDCODED prototype. Lobs a gooey purple sludge blob in an ARC
// toward a forward point (or the locked target); on impact it splats into a
// bubbling PUDDLE on the floor that lingers ~4 seconds, then fades. Poison-purple,
// alpha-blended (matte goo, not glow). Two phases: 0 = flying, 1 = puddle.
// ---------------------------------------------------------------------------
static int   s_sl_active=0, s_sl_phase=0, s_sl_timer=0;
static float s_sl_sx,s_sl_sy,s_sl_sz, s_sl_tx,s_sl_ty,s_sl_tz, s_sl_arc;
#define SL_FLY 44
#define SL_PUDDLE 240   // ~4s at 60fps
static void afn_sludge_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f); float land=30.0f;
    s_sl_sx=px+sinf(yr)*3.0f; s_sl_sy=py+9.0f; s_sl_sz=pz+cosf(yr)*3.0f;      // launch from the hand
    s_sl_tx=px+sinf(yr)*land; s_sl_ty=py+0.4f; s_sl_tz=pz+cosf(yr)*land;      // land forward on the floor
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
    if(afn_cam_lock_target>=0 && afn_ai_slot>=0){ s_sl_tx=s_npcX[afn_ai_slot]; s_sl_tz=s_npcZ[afn_ai_slot]; }   // toward the locked target
#endif
    s_sl_arc=18.0f; s_sl_phase=0; s_sl_timer=SL_FLY; s_sl_active=1;
}
static void afn_sludge_step(void){
    if(!s_sl_active||afn_paused) return;
    if(--s_sl_timer<=0){ if(s_sl_phase==0){ s_sl_phase=1; s_sl_timer=SL_PUDDLE; } else s_sl_active=0; }   // land -> puddle -> gone
}
static void afn_sludge_render(const float* view){
    if(!s_sl_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    static int frame=0; frame++;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // matte goo (alpha, not additive)
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if(s_sl_phase==0){
        float t=1.0f-(float)s_sl_timer/(float)SL_FLY;
        float bx=s_sl_sx+(s_sl_tx-s_sl_sx)*t, by=s_sl_sy+(s_sl_ty-s_sl_sy)*t + s_sl_arc*sinf(3.14159265f*t), bz=s_sl_sz+(s_sl_tz-s_sl_sz)*t;
        // trailing goo drips along the arc (fade back)
        for(int k=1;k<=5;k++){ float tk=t-(float)k*0.045f; if(tk<0.0f)continue;
            float dx=s_sl_sx+(s_sl_tx-s_sl_sx)*tk, dy=s_sl_sy+(s_sl_ty-s_sl_sy)*tk+s_sl_arc*sinf(3.14159265f*tk), dz=s_sl_sz+(s_sl_tz-s_sl_sz)*tk;
            float sz=1.5f*(1.0f-(float)k*0.16f); mm_fill_oval(dx,dy,dz, Rx,Ry,Rz, Ux,Uy,Uz, sz,sz, MM_COL(105,32,138,150-k*24)); }
        // droplets falling off the glob (gravity)
        for(int d=0;d<2;d++){ float dph=fmodf((float)frame*0.05f+(float)d*0.5f,1.0f);
            float ddx=bx+Rx*((float)d-0.5f)*1.6f, ddy=by-dph*dph*7.0f-1.0f, ddz=bz+Rz*((float)d-0.5f)*1.6f;
            float ds=0.5f*(1.0f-dph*0.5f); mm_fill_oval(ddx,ddy,ddz, Rx,Ry,Rz, Ux,Uy,Uz, ds,ds*1.35f, MM_COL(120,40,160,(int)(175*(1.0f-dph)))); }
        // gooey glob: soft dark shadow base -> wobbling lumps -> lit side -> specular -> toxic fleck
        mm_fill_oval(bx,by,bz, Rx,Ry,Rz, Ux,Uy,Uz, 2.7f,2.55f, MM_COL(45,12,65,205));                      // shadow base
        for(int j=0;j<6;j++){ float ja=(float)j*1.05f+(float)frame*0.16f; float off=1.3f;
            float ox=(Rx*cosf(ja)+Ux*sinf(ja))*off, oy=(Ry*cosf(ja)+Uy*sinf(ja))*off, oz=(Rz*cosf(ja)+Uz*sinf(ja))*off;
            float sz=1.3f+sinf((float)frame*0.25f+(float)j*1.3f)*0.3f;
            mm_fill_oval(bx+ox,by+oy,bz+oz, Rx,Ry,Rz, Ux,Uy,Uz, sz,sz, MM_COL(118,40,158,222)); }          // mid-purple goo
        mm_fill_oval(bx-Rx*0.7f+Ux*0.7f, by-Ry*0.7f+Uy*0.7f, bz-Rz*0.7f+Uz*0.7f, Rx,Ry,Rz, Ux,Uy,Uz, 1.5f,1.5f, MM_COL(172,72,205,205));   // lit side
        mm_fill_oval(bx-Rx*0.95f+Ux*0.95f, by-Ry*0.95f+Uy*0.95f, bz-Rz*0.95f+Uz*0.95f, Rx,Ry,Rz, Ux,Uy,Uz, 0.5f,0.5f, MM_COL(228,158,238,210));  // specular
        mm_fill_oval(bx+Rx*0.6f-Ux*0.25f, by+Ry*0.6f-Uy*0.25f, bz+Rz*0.6f-Uz*0.25f, Rx,Ry,Rz, Ux,Uy,Uz, 0.4f,0.4f, MM_COL(150,220,90,140));   // toxic green fleck
    } else {
        float t=1.0f-(float)s_sl_timer/(float)SL_PUDDLE;
        float fade=s_sl_timer<40?(float)s_sl_timer/40.0f:1.0f;
        float grow=t<0.06f?t/0.06f:1.0f;
        float tx=s_sl_tx,ty=s_sl_ty,tz=s_sl_tz; float pr=9.0f*grow;   // bigger mess
        // impact SPLAT burst — droplets fly out + arc down in the first moment
        if(t<0.14f){ float bt=t/0.14f;
            for(int j=0;j<9;j++){ float ja=(float)j*0.7f; float dist=pr*(0.6f+bt*1.5f), arc=sinf(bt*3.14159265f)*4.0f;
                float ds=0.6f*(1.0f-bt*0.4f);
                mm_fill_oval(tx+cosf(ja)*dist, ty+arc, tz+sinf(ja)*dist, Rx,Ry,Rz, Ux,Uy,Uz, ds,ds, MM_COL(135,48,170,(int)(205*(1.0f-bt)))); } }
        // flat ground puddle: dark rim -> irregular edge lumps -> lighter fill -> glossy sheen
        mm_fill_oval(tx,ty,tz, 1,0,0, 0,0,1, pr*1.05f,pr*0.95f, MM_COL(42,12,60,(int)(210*fade)));           // dark rim
        for(int j=0;j<8;j++){ float ja=(float)j*0.82f; float off=pr*0.6f;
            float ox=cosf(ja)*off, oz=sinf(ja*1.3f)*off*0.85f; float sz=pr*(0.34f+0.16f*sinf((float)j*2.1f));
            mm_fill_oval(tx+ox,ty,tz+oz, 1,0,0, 0,0,1, sz,sz*0.85f, MM_COL(105,35,140,(int)(200*fade))); }   // organic edge
        mm_fill_oval(tx,ty,tz, 1,0,0, 0,0,1, pr*0.6f,pr*0.55f, MM_COL(140,55,175,(int)(200*fade)));          // lighter fill
        mm_fill_oval(tx-pr*0.2f,ty,tz-pr*0.2f, 1,0,0, 0,0,1, pr*0.28f,pr*0.2f, MM_COL(182,98,208,(int)(110*fade)));   // glossy sheen
        // rising bubbles that pop (mostly purple, occasional toxic green)
        for(int j=0;j<7;j++){ unsigned bh=(unsigned)(j+1)*2654435761u; float bph=fmodf((float)frame*0.026f+(float)((bh>>4)&0xFF)/255.0f,1.0f);
            float ba=(float)(bh&0xFFFF)/65536.0f*6.2831853f, brr=pr*0.6f*((float)((bh>>8)&0xFF)/255.0f);
            float bxr=tx+cosf(ba)*brr, bzr=tz+sinf(ba)*brr, byr=ty+bph*2.4f;
            float bA=sinf(bph*3.14159265f), bs=0.35f+0.3f*(float)((bh>>16)&0xFF)/255.0f;
            unsigned bc = (j%3==0) ? MM_COL(150,215,90,(int)(150*bA*fade)) : MM_COL(168,78,202,(int)(150*bA*fade));
            mm_fill_oval(bxr,byr,bzr, Rx,Ry,Rz, Ux,Uy,Uz, bs,bs, bc); }
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Psybeam — HARDCODED prototype. A psychic ZAP: a zigzag bolt fired forward that
// shoots out fast, crackles (re-jitters each frame), and shimmers through an
// animated RAINBOW gradient (iridescent psychedelic). Additive glow ribbon +
// a white-hot core, muzzle flash + a rainbow head orb. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int   s_pb_active=0, s_pb_life=0;
static float s_pb_ox,s_pb_oy,s_pb_oz, s_pb_fx,s_pb_fy,s_pb_fz, s_pb_ax,s_pb_ay,s_pb_az, s_pb_bx,s_pb_by,s_pb_bz;
#define PB_MAXLIFE 84
static unsigned mm_rainbow(float hue, int alpha) {
    float h=hue*6.2831853f;
    int r=(int)((0.55f+0.45f*sinf(h))*255.0f);
    int g=(int)((0.55f+0.45f*sinf(h+2.0944f))*255.0f);
    int b=(int)((0.55f+0.45f*sinf(h+4.1888f))*255.0f);
    if(r<0)r=0; if(r>255)r=255; if(g<0)g=0; if(g>255)g=255; if(b<0)b=0; if(b>255)b=255;
    return MM_COL(r,g,b,alpha);
}
static void afn_psybeam_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_pb_fx=sinf(yr); s_pb_fy=0.0f; s_pb_fz=cosf(yr);
    s_pb_ax=cosf(yr); s_pb_ay=0.0f; s_pb_az=-sinf(yr);
    s_pb_bx=0.0f;     s_pb_by=1.0f; s_pb_bz=0.0f;
    s_pb_ox=px+s_pb_fx*4.0f; s_pb_oy=py+7.0f; s_pb_oz=pz+s_pb_fz*4.0f;
    s_pb_life=PB_MAXLIFE; s_pb_active=1;
}
static void afn_psybeam_step(void){ if(s_pb_active && !afn_paused){ if(--s_pb_life<=0) s_pb_active=0; } }
static void afn_psybeam_render(const float* view){
    if(!s_pb_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float fade=(s_pb_life<14)?(float)s_pb_life/14.0f:1.0f;
    static int frame=0; frame++;
    float ox=s_pb_ox,oy=s_pb_oy,oz=s_pb_oz;
    float Fx=s_pb_fx,Fy=s_pb_fy,Fz=s_pb_fz, Ax=s_pb_ax,Ay=s_pb_ay,Az=s_pb_az, Bx=s_pb_bx,By=s_pb_by,Bz=s_pb_bz;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive psychic glow
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int M=20; float range=46.0f, amp=4.2f, coils=5.0f, width=1.3f;
    float front=(float)(PB_MAXLIFE - s_pb_life)/8.0f; if(front>1.0f)front=1.0f;   // shoots out fast (~8 frames)
    float ct=(float)frame*0.06f;                                                  // rainbow animation
    static float PX[22],PY[22],PZ[22];
    unsigned rng=(unsigned)frame*2654435761u ^ 0xA11CE5EDu; int np=0;
    for(int i=0;i<=M;i++){ float t=(float)i/(float)M; if(t>front)break;
        float dist=t*range;
        float ph=t*coils, ff=ph-floorf(ph), tw=(ff<0.5f)?(ff*4.0f-1.0f):(3.0f-ff*4.0f);   // triangle-wave zigzag -1..1
        float endR=t<0.5f?t*2.0f:(1.0f-t)*2.0f; if(endR>1.0f)endR=1.0f;                    // taper at the ends
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5; float jx=((float)(rng&0xFFFF)/32768.0f-1.0f)*1.1f;
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5; float jy=((float)(rng&0xFFFF)/32768.0f-1.0f)*1.1f;
        float oxs=(tw*amp+jx)*endR, oys=jy*endR*0.55f;
        PX[np]=ox+Fx*dist+Ax*oxs+Bx*oys; PY[np]=oy+Fy*dist+Ay*oxs+By*oys; PZ[np]=oz+Fz*dist+Az*oxs+Bz*oys; np++;
    }
    if(np<2){ glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY); glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D); return; }
    for(int pass=0;pass<2;pass++){ float w=(pass==0)?width*2.0f:width*0.55f; int n=0;
        for(int i=0;i<np;i++){
            float dx,dy,dz;
            if(i==0){dx=PX[1]-PX[0];dy=PY[1]-PY[0];dz=PZ[1]-PZ[0];}
            else if(i==np-1){dx=PX[i]-PX[i-1];dy=PY[i]-PY[i-1];dz=PZ[i]-PZ[i-1];}
            else {dx=PX[i+1]-PX[i-1];dy=PY[i+1]-PY[i-1];dz=PZ[i+1]-PZ[i-1];}
            float ddr=dx*Rx+dy*Ry+dz*Rz, ddu=dx*Ux+dy*Uy+dz*Uz; float m=sqrtf(ddr*ddr+ddu*ddu); if(m<0.001f)m=0.001f; ddr/=m; ddu/=m;
            float wpx=Rx*(-ddu)+Ux*ddr, wpy=Ry*(-ddu)+Uy*ddr, wpz=Rz*(-ddu)+Uz*ddr;
            float t=(float)i/(float)(np-1);
            float at=(float)i/(float)M;                                  // absolute position along the full beam
            float wt=(at<0.22f?at/0.22f:1.0f);                           // taper the INTRO to a point at the mouth
            if(pass==0 && t>0.70f) wt*=(1.0f-t)/0.30f;                   // glow: taper the END into the head orb (ball)
            float wI=w*wt;
            unsigned col=(pass==0)?mm_rainbow(t*1.6f+ct,(int)(150*fade)):MM_COL(255,255,255,(int)(230*fade));   // rainbow glow / white core
            MMV(n, PX[i]-wpx*wI, PY[i]-wpy*wI, PZ[i]-wpz*wI, col); n++;
            MMV(n, PX[i]+wpx*wI, PY[i]+wpy*wI, PZ[i]+wpz*wI, col); n++;
        }
        mm_flush(n, GL_TRIANGLE_STRIP);
    }
    // muzzle flash (rainbow) + head orb at the front tip
    mm_glow(ox,oy,oz, Rx,Ry,Rz, Ux,Uy,Uz, 4.0f,4.0f, mm_rainbow(ct,(int)(160*fade)), mm_rainbow(ct,0));
    { float hx=PX[np-1],hy=PY[np-1],hz=PZ[np-1];
      mm_glow(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, 3.4f,3.4f, mm_rainbow(ct+0.5f,(int)(180*fade)), mm_rainbow(ct+0.5f,0));
      mm_fill_oval(hx,hy,hz, Rx,Ry,Rz, Ux,Uy,Uz, 1.1f,1.1f, MM_COL(255,255,255,(int)(230*fade))); }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Psychic — HARDCODED prototype. A funnel of 6 psychic rings along the forward
// axis, growing from a small ring near the player to a big one out front. They
// REVEAL in sequence (small -> big), then pulse + slowly rotate. Pink/magenta
// glow, additive. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int   s_ps_active=0, s_ps_life=0;
static float s_ps_ox,s_ps_oy,s_ps_oz, s_ps_fx,s_ps_fy,s_ps_fz, s_ps_ax,s_ps_ay,s_ps_az, s_ps_bx,s_ps_by,s_ps_bz;
#define PS_MAXLIFE 110
static void afn_psychic_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_ps_fx=sinf(yr); s_ps_fy=0.0f; s_ps_fz=cosf(yr);
    s_ps_ax=cosf(yr); s_ps_ay=0.0f; s_ps_az=-sinf(yr);       // ring-plane basis (perp to forward)
    s_ps_bx=0.0f;     s_ps_by=1.0f; s_ps_bz=0.0f;
    s_ps_ox=px+s_ps_fx*4.0f; s_ps_oy=py+7.0f; s_ps_oz=pz+s_ps_fz*4.0f;
    s_ps_life=PS_MAXLIFE; s_ps_active=1;
}
static void afn_psychic_step(void){ if(s_ps_active && !afn_paused){ if(--s_ps_life<=0) s_ps_active=0; } }
static void afn_psychic_render(const float* view){
    if(!s_ps_active) return;
    static int frame=0; frame++;
    float ox=s_ps_ox,oy=s_ps_oy,oz=s_ps_oz;
    float Fx=s_ps_fx,Fy=s_ps_fy,Fz=s_ps_fz, Ax=s_ps_ax,Ay=s_ps_ay,Az=s_ps_az, Bx=s_ps_bx,By=s_ps_by,Bz=s_ps_bz;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive psychic glow
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int NR=6; float spacing=5.5f, r0=1.6f, rGrow=2.3f;
    float elapsed=(float)(PS_MAXLIFE - s_ps_life);
    float fade=(s_ps_life<16)?(float)s_ps_life/16.0f:1.0f;
    for(int i=0;i<NR;i++){
        float appear=(float)i*5.0f; float age=elapsed-appear; if(age<0.0f)continue;   // reveal small -> big
        float ai=(age<8.0f)?age/8.0f:1.0f;
        float d=(float)i*spacing, pulse=1.0f+0.07f*sinf((float)frame*0.16f+(float)i*0.8f);
        float r=(r0+(float)i*rGrow)*pulse;
        float sp=(float)frame*0.03f+(float)i*0.5f, cs=cosf(sp), ss=sinf(sp);          // slow swirl per ring
        float RAx=Ax*cs+Bx*ss, RAy=Ay*cs+By*ss, RAz=Az*cs+Bz*ss;
        float RBx=-Ax*ss+Bx*cs, RBy=-Ay*ss+By*cs, RBz=-Az*ss+Bz*cs;
        float cx=ox+Fx*d, cy=oy+Fy*d, cz=oz+Fz*d, A2=fade*ai;
        mm_ring(cx,cy,cz, RAx,RAy,RAz, RBx,RBy,RBz, r,r, r*0.14f, MM_COL(205,70,225,(int)(150*A2)));            // outer glow
        mm_ring(cx,cy,cz, RAx,RAy,RAz, RBx,RBy,RBz, r*0.93f,r*0.93f, r*0.05f, MM_COL(255,180,255,(int)(220*A2))); // bright edge
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Surf — HARDCODED prototype. A wide translucent WAVE FRONT (perpendicular to the
// player's facing) that rises and SWEEPS forward across the arena: a blue water
// body with lower trailing rows (the wake), a white foam CREST on top, and SPRAY
// droplets flying off the crest. Alpha-blended (see-through water). Migrate later.
// ---------------------------------------------------------------------------
static int   s_su_active=0, s_su_life=0;
static float s_su_ox,s_su_oy,s_su_oz, s_su_fx,s_su_fy,s_su_fz, s_su_ax,s_su_ay,s_su_az, s_su_yaw;
#define SU_MAXLIFE 120
// Where along the wave the front + the ride sit at progress t (shared by render + the ride).
static float afn_surf_front(float t){ float ease=t*t*(3.0f-2.0f*t); return 6.0f+ease*40.0f; }
static void afn_surf_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_su_fx=sinf(yr); s_su_fy=0.0f; s_su_fz=cosf(yr);       // sweep direction
    s_su_ax=cosf(yr); s_su_ay=0.0f; s_su_az=-sinf(yr);      // width (right)
    s_su_ox=px; s_su_oy=py+0.4f; s_su_oz=pz; s_su_yaw=yaw;  // ground at the player
    s_su_life=SU_MAXLIFE; s_su_active=1;
}
static void afn_surf_step(void){ if(s_su_active && !afn_paused){ if(--s_su_life<=0) s_su_active=0; } }
// Water surface colour: deep blue at the base rising to cyan, whitening to foam near the crest.
static unsigned su_water_col(float hn, float zr, float ef){
    if(hn<0.0f)hn=0.0f; if(hn>1.0f)hn=1.0f;
    int r=(int)(30.0f+120.0f*hn), g=(int)(95.0f+120.0f*hn), b=(int)(180.0f+75.0f*hn);   // deep -> cyan by height
    float f=(zr>0.80f)?(zr-0.80f)/0.20f:0.0f;                                            // foam whitens the crest
    r+=(int)((255-r)*f); g+=(int)((255-g)*f); b+=(int)((255-b)*f);
    int a=(int)((150.0f+70.0f*zr+55.0f*f)*ef); if(a>255)a=255; if(a<0)a=0;               // more opaque toward the front
    return MM_COL(r,g,b,a);
}
static void afn_surf_render(const float* view){
    if(!s_su_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    static int frame=0; frame++;
    float ox=s_su_ox,oy=s_su_oy,oz=s_su_oz;
    float Fx=s_su_fx,Fy=s_su_fy,Fz=s_su_fz, Ax=s_su_ax,Ay=s_su_ay,Az=s_su_az;
    float t=(float)(SU_MAXLIFE - s_su_life)/(float)SU_MAXLIFE;
    float fd=afn_surf_front(t);                            // wave front advances forward (shared with the ride)
    float ef=(s_su_life<24)?(float)s_su_life/24.0f:1.0f;   // fade out at the end
    float riseIn=(t<0.15f)?t/0.15f:1.0f;                   // rise up at the start
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // translucent water
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int NX=16, NZ=10; float halfW=24.0f, crestH=8.5f*riseIn, waveDepth=20.0f;
    float backD=fd-waveDepth; if(backD<0.0f)backD=0.0f; float span=fd-backD; if(span<0.1f)span=0.1f;
    float invCH=1.0f/(crestH>0.2f?crestH:0.2f);
    // WAVE SURFACE: a rippled triangle-mesh sheet rising from the flat back up to the crest at the
    // front. Per-vertex colour gradient (deep blue -> cyan -> white foam) so it reads as water.
    for(int j=0;j<NZ-1;j++){ int n=0;
        for(int i=0;i<NX;i++){ float x=(-1.0f+2.0f*(float)i/(float)(NX-1))*halfW;
            for(int jj=0;jj<2;jj++){ float zr=(float)(j+jj)/(float)(NZ-1); float z=backD+zr*span;
                float rip=sinf(x*0.35f+(float)frame*0.14f)*0.55f + sinf(z*0.4f-(float)frame*0.18f)*0.55f;
                float h=crestH*powf(zr,1.4f)+rip*zr; float hn=h*invCH;
                MMV(n, ox+Ax*x+Fx*z, oy+h, oz+Az*x+Fz*z, su_water_col(hn,zr,ef)); n++; } }
        mm_flush(n, GL_TRIANGLE_STRIP);
    }
    // soft foam crest + spray riding the leading edge (z = fd)
    for(int i=0;i<NX;i++){ float x=(-1.0f+2.0f*(float)i/(float)(NX-1))*halfW;
        float cx=ox+Fx*fd+Ax*x, cz=oz+Fz*fd+Az*x, cy=oy+crestH+sinf((float)frame*0.15f+x)*0.6f;
        mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 3.0f,2.0f, MM_COL(255,255,255,(int)(150*ef)), MM_COL(210,235,255,0));   // soft foam
        for(int d=0;d<2;d++){ float dph=fmodf((float)frame*0.06f+(float)i*0.3f+(float)d*0.4f,1.0f);
            float dy=cy+dph*6.5f-dph*dph*9.0f; float dx=cx+Ax*((float)d-0.5f)*1.6f, dz=cz+Az*((float)d-0.5f)*1.6f;
            mm_glow(dx,dy,dz, Rx,Ry,Rz, Ux,Uy,Uz, 0.7f,0.7f, MM_COL(230,245,255,(int)(160*(1.0f-dph)*ef)), MM_COL(210,235,255,0)); } }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Flamethrower — HARDCODED prototype. A sustained forward JET of turbulent fire:
// a looping fleet of flame puffs that stream out of the mouth, widen into a cone,
// flicker/turbulate, and fade at the tips. White-hot at the mouth -> yellow ->
// orange-red out front, plus flying embers. Additive. Migrate to a preset later.
// ---------------------------------------------------------------------------
static int   s_fl_active=0, s_fl_life=0;
static float s_fl_ox,s_fl_oy,s_fl_oz, s_fl_fx,s_fl_fy,s_fl_fz, s_fl_ax,s_fl_ay,s_fl_az, s_fl_bx,s_fl_by,s_fl_bz;
#define FL_MAXLIFE 100
static void afn_flame_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_fl_fx=sinf(yr); s_fl_fy=0.0f; s_fl_fz=cosf(yr);
    s_fl_ax=cosf(yr); s_fl_ay=0.0f; s_fl_az=-sinf(yr);
    s_fl_bx=0.0f;     s_fl_by=1.0f; s_fl_bz=0.0f;
    s_fl_ox=px+s_fl_fx*4.0f; s_fl_oy=py+7.0f; s_fl_oz=pz+s_fl_fz*4.0f;   // from the mouth
    s_fl_life=FL_MAXLIFE; s_fl_active=1;
}
static void afn_flame_step(void){ if(s_fl_active && !afn_paused){ if(--s_fl_life<=0) s_fl_active=0; } }
static void afn_flame_render(const float* view){
    if(!s_fl_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float ef=(s_fl_life<16)?(float)s_fl_life/16.0f:1.0f;
    if(FL_MAXLIFE-s_fl_life<8) ef*=(float)(FL_MAXLIFE-s_fl_life)/8.0f;   // ease in
    static int frame=0; frame++;
    float ox=s_fl_ox,oy=s_fl_oy,oz=s_fl_oz;
    float Fx=s_fl_fx,Fy=s_fl_fy,Fz=s_fl_fz, Ax=s_fl_ax,Ay=s_fl_ay,Az=s_fl_az, Bx=s_fl_bx,By=s_fl_by,Bz=s_fl_bz;
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive fire
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    int N=46; float range=32.0f, coneR=6.5f;
    float front=(float)(FL_MAXLIFE - s_fl_life)/6.0f; if(front>1.0f)front=1.0f;   // jet reaches full length fast
    for(int i=0;i<N;i++){ unsigned h=(unsigned)(i+1)*2654435761u ^ 0x9E3779B9u;
        float fa=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fc=(float)((h>>16)&0xFF)/255.0f;
        float ph=fmodf((float)frame*0.032f+fc,1.0f); if(ph>front)continue;
        float dist=ph*range, sAng=fa*6.2831853f, sR=coneR*ph*(0.30f+fb*0.70f);
        float wa=sinf((float)frame*0.35f+(float)i*1.7f)*1.3f*ph, wb=cosf((float)frame*0.40f+(float)i*2.1f)*1.3f*ph;
        float ua=cosf(sAng)*sR+wa, ub=sinf(sAng)*sR+wb;
        float cx=ox+Fx*dist+Ax*ua+Bx*ub, cy=oy+Fy*dist+Ay*ua+By*ub, cz=oz+Fz*dist+Az*ua+Bz*ub;
        float av=(ph>0.82f)?(1.0f-ph)/0.18f:1.0f; if(av<0)av=0;
        float sz=1.3f+ph*2.7f+sinf((float)frame*0.3f+(float)i)*0.3f;
        unsigned outer=mm_lerp_col(MM_COL(255,110,20,0),MM_COL(190,35,0,0),ph);   // rgb only; alpha set below
        unsigned inner=mm_lerp_col(MM_COL(255,238,185,0),MM_COL(255,165,50,0),ph);
        outer=(outer&0x00FFFFFFu)|((unsigned)((int)(110*av*ef)&0xFF)<<24);
        inner=(inner&0x00FFFFFFu)|((unsigned)((int)(140*av*ef)&0xFF)<<24);
        mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sz*1.25f,sz*1.25f, outer, outer&0x00FFFFFFu);       // orange-red glow
        mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sz*0.72f,sz*0.72f, inner, inner&0x00FFFFFFu);       // yellow body
        if(ph<0.32f) mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, sz*0.4f,sz*0.4f, MM_COL(255,250,225,(int)(150*ef)), MM_COL(255,250,225,0)); }  // white-hot near the mouth
    // flying embers/sparks
    for(int j=0;j<14;j++){ float ph=fmodf((float)frame*0.05f+(float)j*0.13f,1.0f);
        float dist=ph*range*1.1f, sAng=(float)j*2.4f, sR=coneR*ph*((float)(j%3)*0.3f+0.5f);
        float arc=sinf(ph*3.14159265f)*2.5f-ph*ph*3.0f;
        float cx=ox+Fx*dist+(Ax*cosf(sAng)+Bx*sinf(sAng))*sR+Bx*arc;
        float cy=oy+Fy*dist+(Ay*cosf(sAng)+By*sinf(sAng))*sR+By*arc;
        float cz=oz+Fz*dist+(Az*cosf(sAng)+Bz*sinf(sAng))*sR+Bz*arc;
        mm_fill_oval(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, 0.35f,0.35f, MM_COL(255,205,95,(int)(200*(1.0f-ph)*ef))); }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// Aura Sphere — HARDCODED prototype. A glowing blue energy orb that travels
// forward: soft blue -> cyan halo, a white-hot pulsing core, three spinning
// aura rings, orbiting energy motes, and a comet trail. Additive. Migrate later.
// ---------------------------------------------------------------------------
static int   s_au_active=0, s_au_life=0;
static float s_au_ox,s_au_oy,s_au_oz, s_au_fx,s_au_fy,s_au_fz, s_au_ax,s_au_ay,s_au_az, s_au_bx,s_au_by,s_au_bz;
static int   s_au_cr=96, s_au_cg=176, s_au_cb=255;   // base tint (set from the kind-14 layer colour)
static int   s_au_tr=94, s_au_tg=162, s_au_tb=222;   // trail tint (set from the kind-14 layer trail colour)
#define AU_MAXLIFE 92
static void afn_aura_fire(float px,float py,float pz,float yaw){
    float yr=yaw*(3.14159265f/180.0f);
    s_au_fx=sinf(yr); s_au_fy=0.0f; s_au_fz=cosf(yr);
    s_au_ax=cosf(yr); s_au_ay=0.0f; s_au_az=-sinf(yr);
    s_au_bx=0.0f;     s_au_by=1.0f; s_au_bz=0.0f;
    s_au_ox=px+s_au_fx*4.0f; s_au_oy=py+8.0f; s_au_oz=pz+s_au_fz*4.0f;
    s_au_life=AU_MAXLIFE; s_au_active=1;
}
static void afn_aura_step(void){ if(s_au_active && !afn_paused){ if(--s_au_life<=0) s_au_active=0; } }
// Reusable aura ORB: layered coloured halo whitening into a hot core, sized by R (the core-glow
// radius; R=1.8 reproduces the standalone Aura Sphere's proportions). (cr,cg,cb) is the base tint;
// each inner layer is blended toward white so the core reads white-hot regardless of hue. Assumes
// additive blend set. Default blue tint (96,176,255) reproduces the original prototype colours.
static void afn_aura_orb_c(float Rx,float Ry,float Rz,float Ux,float Uy,float Uz, float cx,float cy,float cz, float R, float ef, int cr,int cg,int cb){
    if(cr+cg+cb < 24){ cr=96; cg=176; cb=255; }   // unset/black layer colour -> default blue (black is invisible on additive)
    #define AU_BR(c,t) ((int)((c) + (255-(c))*(t)))
    mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, R*3.00f,R*3.00f, MM_COL(cr,cg,cb,(int)(120*ef)), MM_COL(cr,cg,cb,0));
    mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, R*1.83f,R*1.83f, MM_COL(AU_BR(cr,0.42f),AU_BR(cg,0.34f),AU_BR(cb,0.20f),(int)(150*ef)), MM_COL(AU_BR(cr,0.30f),AU_BR(cg,0.24f),AU_BR(cb,0.14f),0));
    mm_glow(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, R*1.00f,R*1.00f, MM_COL(AU_BR(cr,0.70f),AU_BR(cg,0.62f),AU_BR(cb,0.50f),(int)(220*ef)), MM_COL(AU_BR(cr,0.60f),AU_BR(cg,0.52f),AU_BR(cb,0.42f),0));
    mm_fill_oval(cx,cy,cz, Rx,Ry,Rz, Ux,Uy,Uz, R*0.47f,R*0.47f, MM_COL(AU_BR(cr,0.82f),AU_BR(cg,0.76f),AU_BR(cb,0.66f),(int)(235*ef)));
    #undef AU_BR
}
static void afn_aura_render(const float* view){
    if(!s_au_active) return;
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    float t=(float)(AU_MAXLIFE - s_au_life)/(float)AU_MAXLIFE;
    float ef=(s_au_life<14)?(float)s_au_life/14.0f:1.0f; if(AU_MAXLIFE-s_au_life<8) ef*=(float)(AU_MAXLIFE-s_au_life)/8.0f;
    static int frame=0; frame++;
    float dist=t*40.0f;
    float cx=s_au_ox+s_au_fx*dist, cy=s_au_oy+s_au_fy*dist, cz=s_au_oz+s_au_fz*dist;
    float pulse=1.0f+0.09f*sinf((float)frame*0.35f);
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive aura glow
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    mm_trail(cx,cy,cz, s_au_fx,s_au_fy,s_au_fz, Rx,Ry,Rz, Ux,Uy,Uz, 11.0f, 3.4f, MM_COL(s_au_tr,s_au_tg,s_au_tb,(int)(0.7f*200*ef)), MM_COL((int)(s_au_tr*0.6f),(int)(s_au_tg*0.72f),(int)(s_au_tb*0.72f),0));
    afn_aura_orb_c(Rx,Ry,Rz,Ux,Uy,Uz, cx,cy,cz, 1.8f*pulse, ef, s_au_cr,s_au_cg,s_au_cb);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}
// Resolve the aura tint (ar,ag,ab) and trail tint (tr,tg,tb) from an instance's kind-14 layer.
// Unset/near-black aura -> default blue; dark/unset trail -> a bright trail derived from the aura.
#ifdef AFN_HAS_FX
static void afn_fx_aura_cols(int instIdx, int* ar,int* ag,int* ab, int* tr,int* tg,int* tb){
    int cr=96,cg=176,cb=255, xr=0,xg=0,xb=0;
    if(instIdx>=0 && instIdx<AFN_FX_COUNT){
        const AfnFxInstance* In=&afn_fx_instances[instIdx];
        for(int i=0;i<In->layerCount;i++){ const AfnFxLayer* L=&afn_fx_layers[In->layerStart+i];
            if(L->kind==14){ cr=(int)L->colr; cg=(int)L->colg; cb=(int)L->colb;
                             xr=(int)L->tcloudr; xg=(int)L->tcloudg; xb=(int)L->tcloudb; break; } }
    }
    if(cr+cg+cb<24){ cr=96; cg=176; cb=255; }
    int tmax=xr; if(xg>tmax)tmax=xg; if(xb>tmax)tmax=xb;
    if(tmax<90){ xr=(int)(cr*0.72f+26); xg=(int)(cg*0.80f+22); xb=(int)(cb*0.80f+18); }  // dark/unset -> bright from aura
    if(xr>255)xr=255; if(xg>255)xg=255; if(xb>255)xb=255;
    *ar=cr;*ag=cg;*ab=cb; *tr=xr;*tg=xg;*tb=xb;
}
#endif
// Draw an Effects-tab INSTANCE's orb-type layers at a fixed point (R = orb radius). Geometry only
// (additive blend must already be set). Currently supports the Aura Sphere layer (kind 14); add
// more sphere-type kinds here as they're built.
static void afn_fx_orb_at(float Rx,float Ry,float Rz,float Ux,float Uy,float Uz, int instIdx,
                          float cx,float cy,float cz, float R, float ef, float pulse){
#ifdef AFN_HAS_FX
    if(instIdx < 0 || instIdx >= AFN_FX_COUNT) return;
    const AfnFxInstance* In = &afn_fx_instances[instIdx];
    for(int i=0;i<In->layerCount;i++){
        const AfnFxLayer* L = &afn_fx_layers[In->layerStart + i];
        if(L->kind == 14)   // Aura Sphere — tinted by the layer's colour
            afn_aura_orb_c(Rx,Ry,Rz,Ux,Uy,Uz, cx,cy,cz, R*pulse, ef, (int)L->colr,(int)L->colg,(int)L->colb);
    }
#else
    (void)Rx;(void)Ry;(void)Rz;(void)Ux;(void)Uy;(void)Uz;(void)instIdx;(void)cx;(void)cy;(void)cz;(void)R;(void)ef;(void)pulse;
#endif
}
// Focus Blast orb drawn as the sub-sprite's assigned Effects INSTANCE — the charge ball (grows with
// afn_fb_scale = Min%->Max%) and each in-flight pooled shot (with a trail). Radius comes from the
// SAME sprite base*scale the focus_gfx quad used, so it honours Charge Shot's Min/Max Scale%.
static void afn_focusblast_aura_render(const float* view){
#if defined(AFN_HAS_SPRITES) && defined(AFN_HAS_SPR_EFFECT) && defined(AFN_HAS_FX)
    int fbi = resolve_focus_inst();   // NOT afn_fb_inst — that's only cached once the PLAYER charges;
                                      // the enemy orb needs the resolved instance too.
    if(fbi < 0 || afn_spr_effect[fbi] <= 0) return;   // 0 = no effect assigned
    int instIdx = afn_spr_effect[fbi] - 1;            // effectKind = instance index + 1
    int anyFlight=0; for(int k=0;k<AFN_FB_POOL;k++) if(s_fbPool[k].active){ anyFlight=1; break; }
    if(!afn_fb_charging && !anyFlight && !s_efbActive && !s_efbCharging) return;   // player OR enemy orb
    float Rx=view[0],Ry=view[4],Rz=view[8], Ux=view[1],Uy=view[5],Uz=view[9];
    static int frame=0; frame++; float pulse=1.0f+0.09f*sinf((float)frame*0.35f);
    float base=afn_spr_basesize[fbi];
    int ar,ag,ab,tr,tg,tb; afn_fx_aura_cols(instIdx,&ar,&ag,&ab,&tr,&tg,&tb);   // trail tint from the layer
    (void)ar;(void)ag;(void)ab;   // orb tint is read per-layer inside afn_fx_orb_at
    unsigned trCol=MM_COL(tr,tg,tb,140), trEdge=MM_COL((int)(tr*0.6f),(int)(tg*0.72f),(int)(tb*0.72f),0);
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if(afn_fb_charging){
        float R=base*afn_fb_scale*0.25f*0.6f; if(R<0.15f)R=0.15f;   // orb radius from the ball's world size
        afn_fx_orb_at(Rx,Ry,Rz,Ux,Uy,Uz, instIdx, afn_fb_x, afn_fb_y, afn_fb_z, R, 1.0f, pulse);
    }
    for(int k=0;k<AFN_FB_POOL;k++){ if(!s_fbPool[k].active) continue;
        float R=base*s_fbPool[k].scale*0.25f*0.6f; if(R<0.15f)R=0.15f;
        mm_trail(s_fbPool[k].x,s_fbPool[k].y,s_fbPool[k].z, s_fbPool[k].dirx,0.0f,s_fbPool[k].dirz, Rx,Ry,Rz, Ux,Uy,Uz, R*6.0f, R*1.9f, trCol, trEdge);
        afn_fx_orb_at(Rx,Ry,Rz,Ux,Uy,Uz, instIdx, s_fbPool[k].x,s_fbPool[k].y,s_fbPool[k].z, R, 1.0f, pulse);
    }
    // ENEMY orb (charge spot / in flight) — same focus_gfx effect, at its s_efb position.
    if(s_efbActive || s_efbCharging){
        float R=base*s_efbScale*0.25f*0.6f; if(R<0.15f)R=0.15f;
        if(s_efbActive && !s_efbCharging)
            mm_trail(s_efbX,s_efbY,s_efbZ, s_efbDirX,0.0f,s_efbDirZ, Rx,Ry,Rz, Ux,Uy,Uz, R*6.0f, R*1.9f, trCol, trEdge);
        afn_fx_orb_at(Rx,Ry,Rz,Ux,Uy,Uz, instIdx, s_efbX,s_efbY,s_efbZ, R, 1.0f, pulse);
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
#else
    (void)view;
#endif
}

// Beam pool: count each bolt's life down; expire at 0.
static void afn_beam_update(void) {
    if (afn_paused) return;
    for (int b = 0; b < AFN_BEAM_POOL; b++)
        if (s_beams[b].active && --s_beams[b].life <= 0) s_beams[b].active = 0;
}

// Cast a queued bolt: source = player chest, target = the lock-on enemy if one is
// locked, else a point `range` ahead along the player's facing. Pure vector setup.
static void afn_beam_resolve(float px, float py, float pz, float yawDeg) {
    if (!afn_beam_spawn) return;
    afn_beam_spawn = 0;
    int slot = -1;
    for (int b = 0; b < AFN_BEAM_POOL; b++) if (!s_beams[b].active) { slot = b; break; }
    if (slot < 0) slot = 0;   // pool full: recycle slot 0
    AfnBeam* bm = &s_beams[slot];
    bm->active = 1; bm->life = bm->maxLife = afn_beam_life;
    // Floor height: a slinky/bolt crawls ACROSS the floor, so run it just above the
    // ground (feet + 2) rather than chest. A coil then loops up off the floor by radius.
    bm->sx = px; bm->sy = py + 2.0f; bm->sz = pz;
    int gotTarget = 0;
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
    if (afn_cam_lock_target >= 0 && afn_ai_slot >= 0) {
        bm->tx = s_npcX[afn_ai_slot]; bm->ty = s_npcY[afn_ai_slot] + 2.0f; bm->tz = s_npcZ[afn_ai_slot];
        gotTarget = 1;
    }
#endif
    if (!gotTarget) {
        float yr = yawDeg * (3.14159265f / 180.0f);
        bm->tx = px + sinf(yr) * (float)afn_beam_range;
        bm->ty = py + 2.0f;
        bm->tz = pz + cosf(yr) * (float)afn_beam_range;
    }
    bm->width = afn_beam_width; bm->bow = afn_beam_bow; bm->jitter = afn_beam_jitter;
    bm->segs = afn_beam_segs; bm->col = afn_beam_col;
    bm->bounces = afn_beam_bounces; bm->decay = afn_beam_decay; bm->pulse = afn_beam_pulse;
    bm->spts = afn_beam_spline; bm->nspts = afn_beam_spline_n;
    bm->travel = afn_beam_travel; bm->travelBounces = afn_beam_travel_bounces; bm->travelPersist = afn_beam_travel_persist; bm->travelFade = afn_beam_travel_fade;
    bm->filaments = afn_beam_filaments < 1 ? 1 : (afn_beam_filaments > AFN_BEAM_FILAMENTS_MAX ? AFN_BEAM_FILAMENTS_MAX : afn_beam_filaments);
    bm->orbSize = afn_beam_orb;
    afn_beam_spline = 0; afn_beam_spline_n = 0; afn_beam_travel = 0; afn_beam_travel_bounces = 3; afn_beam_travel_persist = 0.30f; afn_beam_travel_fade = 0.35f;   // one-shot: don't leak into the next plain bolt
}

// Cast a bolt between two EXPLICIT world points (vs afn_beam_resolve which picks
// source/target from the player). Uses the current afn_beam_* params. For the Thunder
// strike: source = cloud above the reticle, target = the floor reticle.
static void afn_beam_cast(float sx, float sy, float sz, float tx, float ty, float tz) {
    int slot = -1;
    for (int b = 0; b < AFN_BEAM_POOL; b++) if (!s_beams[b].active) { slot = b; break; }
    if (slot < 0) slot = 0;
    AfnBeam* bm = &s_beams[slot];
    bm->active = 1; bm->life = bm->maxLife = afn_beam_life;
    bm->sx = sx; bm->sy = sy; bm->sz = sz; bm->tx = tx; bm->ty = ty; bm->tz = tz;
    bm->width = afn_beam_width; bm->bow = afn_beam_bow; bm->jitter = afn_beam_jitter;
    bm->segs = afn_beam_segs; bm->col = afn_beam_col;
    bm->bounces = afn_beam_bounces; bm->decay = afn_beam_decay; bm->pulse = afn_beam_pulse;
    bm->spts = 0; bm->nspts = 0;
    bm->travel = afn_beam_travel; bm->travelBounces = afn_beam_travel_bounces; bm->travelPersist = afn_beam_travel_persist; bm->travelFade = afn_beam_travel_fade;
    bm->filaments = afn_beam_filaments < 1 ? 1 : (afn_beam_filaments > AFN_BEAM_FILAMENTS_MAX ? AFN_BEAM_FILAMENTS_MAX : afn_beam_filaments);
    bm->orbSize = afn_beam_orb;
    afn_beam_travel = 0;
}

// Catmull-Rom sample of an authored effect spline (normalised x,y,th) at param s in
// 0..1 across all segments. Returns the interpolated x,y (and thickness th if want).
static void fx_spline_sample(const float (*p)[3], int n, float s, float* ox, float* oy, float* oth) {
    if (n <= 1) { *ox = n ? p[0][0] : 0.0f; *oy = n ? p[0][1] : 0.0f; if (oth) *oth = n ? p[0][2] : 1.0f; return; }
    if (s < 0) s = 0; if (s > 1) s = 1;
    float fs = s * (float)(n - 1); int i = (int)fs; if (i > n - 2) i = n - 2; if (i < 0) i = 0;
    float t = fs - (float)i, t2 = t*t, t3 = t2*t;
    int i0 = i-1 < 0 ? 0 : i-1, i1 = i, i2 = i+1, i3 = i+2 > n-1 ? n-1 : i+2;
    for (int c = 0; c < 3; c++) {
        float a0 = p[i0][c], a1 = p[i1][c], a2 = p[i2][c], a3 = p[i3][c];
        float v = 0.5f * ((2.0f*a1) + (-a0+a2)*t + (2.0f*a0-5.0f*a1+4.0f*a2-a3)*t2 + (-a0+3.0f*a1-3.0f*a2+a3)*t3);
        if (c == 0) *ox = v; else if (c == 1) *oy = v; else if (oth) *oth = v;
    }
}

// Render each bolt as a camera-facing triangle strip: walk the source->target line in
// N segments, push each point sideways (screen-perpendicular to the path) by the arc
// bow + per-frame random jitter (decaying to 0 at the ends), then emit two verts
// +/- side*halfWidth. Additive, no texture (the bright geometry glows on its own).
static void afn_beam_render(const float* view) {
    int any = 0;
    for (int b = 0; b < AFN_BEAM_POOL; b++) if (s_beams[b].active) { any = 1; break; }
    if (!any) return;
    float Rwx = view[0], Rwy = view[4], Rwz = view[8];   // camera right
    float Uwx = view[1], Uwy = view[5], Uwz = view[9];   // camera up
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D);
    glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    for (int b = 0; b < AFN_BEAM_POOL; b++) {
        AfnBeam* bm = &s_beams[b];
        if (!bm->active) continue;
        int nb = bm->bounces > 0 ? bm->bounces : 1;
        // When a spline is authored it tiles `travelBounces` times across the floor; otherwise
        // the parametric path makes `bounces` arches. Resolve enough segments for either.
        int tb = (bm->nspts >= 2 && bm->travelBounces > 0) ? bm->travelBounces : nb;
        int N = bm->segs; { int need = tb * 8; if (need > N) N = need; }   // ~8 segs per arch
        if (N < 2) N = 2; if (N > AFN_BEAM_MAX_SEGS) N = AFN_BEAM_MAX_SEGS;
        float dx = bm->tx - bm->sx, dy = bm->ty - bm->sy, dz = bm->tz - bm->sz;
        float len = sqrtf(dx*dx + dy*dy + dz*dz); if (len < 0.001f) len = 0.001f;
        float dirx = dx/len, diry = dy/len, dirz = dz/len;
        // Screen-perpendicular to the path (for the lightning jitter crackle).
        float pr = dirx*Rwx + diry*Rwy + dirz*Rwz, pu = dirx*Uwx + diry*Uwy + dirz*Uwz;
        float pl = sqrtf(pr*pr + pu*pu); if (pl < 0.001f) pl = 0.001f;
        float sscx = (-pu/pl)*Rwx + (pr/pl)*Uwx, sscy = (-pu/pl)*Rwy + (pr/pl)*Uwy, sscz = (-pu/pl)*Rwz + (pr/pl)*Uwz;
        // Path-frame UP (for the bounce arches to rise off the floor): up = (dir x worldUp) x dir.
        float wux=0,wuy=1,wuz=0; if (diry > 0.95f || diry < -0.95f) { wux=1; wuy=0; }
        float hsx = diry*wuz - dirz*wuy, hsy = dirz*wux - dirx*wuz, hsz = dirx*wuy - diry*wux;
        float hl = sqrtf(hsx*hsx+hsy*hsy+hsz*hsz); if (hl<0.001f) hl=0.001f; hsx/=hl; hsy/=hl; hsz/=hl;
        float hux = hsy*dirz - hsz*diry, huy = hsz*dirx - hsx*dirz, huz = hsx*diry - hsy*dirx;
        unsigned rng = ((unsigned)afn_frame_count * 2654435761u) ^ ((unsigned)b * 0x9E3779B9u) ^ 0xA53C9E1Du;
        // Pass 1: centerline — a spline of PARABOLIC bounce arcs. Each bounce is a hump
        // 4*lt*(1-lt) (0 at both contacts, peak at the middle, steep at the floor like a real
        // bounce), and each successive arch is `decay`x the previous height (energy loss). The
        // arch lifts along path-frame UP; a jagged crackle rides on top (clean at the anchors).
        float cxA[AFN_BEAM_MAX_SEGS+1], cyA[AFN_BEAM_MAX_SEGS+1], czA[AFN_BEAM_MAX_SEGS+1];
        float thA[AFN_BEAM_MAX_SEGS+1];
        float refY = (bm->nspts >= 2) ? bm->spts[0][1] : 0.0f;   // spline start = floor reference
        for (int i = 0; i <= N; i++) {
            float t = (float)i / (float)N;
            float along = t, arch, thk = 1.0f;
            if (bm->nspts >= 2) {
                // Authored shape tiled `tb` times across the floor: each tile replays the
                // spline (x -> along-path within its slot, refY-y -> height) at decay^tile
                // height, so the bolt bounces tb times, each shorter, then fizzles.
                float seg = t * (float)tb; int bi = (int)seg; if (bi >= tb) bi = tb-1;
                float lt = seg - (float)bi;
                float sx, sy, sth; fx_spline_sample(bm->spts, bm->nspts, lt, &sx, &sy, &sth);
                float decF = 1.0f; for (int k=0;k<bi;k++) decF *= bm->decay;
                along = ((float)bi + sx) / (float)tb;
                arch = (refY - sy) * bm->bow * decF; thk = sth;
            } else {
                float seg = t * (float)nb; int bi = (int)seg; if (bi >= nb) bi = nb-1;
                float lt = seg - (float)bi;                       // 0..1 within this bounce
                float hump = 4.0f * lt * (1.0f - lt);             // parabola: 0 at contacts, 1 at peak
                float decayF = 1.0f; for (int k=0;k<bi;k++) decayF *= bm->decay;   // each arch lower
                arch = bm->bow * decayF * hump;
            }
            // Smooth centerline (the arc/bounce path). The lightning crackle is added per
            // FILAMENT at draw time so the bundle reads as many distinct jagged strands.
            cxA[i] = bm->sx + dx*along + hux*arch;
            cyA[i] = bm->sy + dy*along + huy*arch;
            czA[i] = bm->sz + dz*along + huz*arch;
            thA[i] = thk;
        }
        float lifeF = bm->maxLife > 0 ? (float)bm->life / (float)bm->maxLife : 1.0f;
        // Travel bolts stay full-bright the whole crawl (no life dimming); their only fade is
        // the trailing tail, controlled per-point by travelFade below. Non-travel bolts use
        // the plain life flicker fade.
        float fadeMul = bm->travel ? 1.0f : lifeF;
        unsigned baseA = (unsigned)(((bm->col >> 24) & 0xFF) * fadeMul);
        unsigned rgb = bm->col & 0x00FFFFFFu;
        float hw = bm->width;
        // Per-centerline camera-facing perpendicular (filament width direction). Filaments
        // run roughly parallel to the centerline, so they share it.
        float lsxA[AFN_BEAM_MAX_SEGS+1], lsyA[AFN_BEAM_MAX_SEGS+1], lszA[AFN_BEAM_MAX_SEGS+1];
        for (int i = 0; i <= N; i++) {
            int ia = i>0?i-1:0, ib = i<N?i+1:N;
            float tx = cxA[ib]-cxA[ia], ty = cyA[ib]-cyA[ia], tz = czA[ib]-czA[ia];
            float tr = tx*Rwx+ty*Rwy+tz*Rwz, tu = tx*Uwx+ty*Uwy+tz*Uwz;
            float tl = sqrtf(tr*tr+tu*tu); if (tl<0.001f) tl=0.001f;
            lsxA[i]=(-tu/tl)*Rwx+(tr/tl)*Uwx; lsyA[i]=(-tu/tl)*Rwy+(tr/tl)*Uwy; lszA[i]=(-tu/tl)*Rwz+(tr/tl)*Uwz;
        }
        float headT = bm->travel ? (1.0f - lifeF) : 1.0f;
        float win = (bm->travelPersist > 0.01f ? bm->travelPersist : 0.01f) / (float)tb;
        float fw = hw * 0.55f; if (fw < 0.03f) fw = 0.03f;   // thin filament half-width

        // ---- the electric BUNDLE: thin crackling filaments around the centerline ----
        int nfil = bm->filaments < 1 ? 1 : bm->filaments;
        for (int k = 0; k < nfil; k++) {
            unsigned frng = rng ^ ((unsigned)(k+1) * 0x9E3779B9u);
            float ph = (float)k * (6.2831853f / (float)nfil);
            float cph = cosf(ph), sph = sinf(ph);
            AfnVertex fil[2 * (AFN_BEAM_MAX_SEGS + 1)];
            for (int i = 0; i <= N; i++) {
                float t = (float)i/(float)N;
                float aMul = 1.0f;
                if (bm->travel) {                 // crawling window (per-arc) reveals the bundle
                    float hT = headT*(1.0f+win); float d = hT - t;
                    if (t > hT || d > win || d < 0.0f) aMul = 0.0f;
                    else { float bb = 1.0f - d/win; bb *= bb; float fade = bm->travelFade; aMul = 1.0f - fade*(1.0f-bb); }
                }
                float edge = t<0.5f?t:(1.0f-t); float endRamp = edge*6.0f<1.0f?edge*6.0f:1.0f;
                float rad = bm->jitter * (0.35f + 0.65f*t) * endRamp;   // tube spreads toward the head
                frng ^= frng<<13; frng^=frng>>17; frng^=frng<<5; float c1=((float)(frng&0xFFFF)/32768.0f)-1.0f;
                frng ^= frng<<13; frng^=frng>>17; frng^=frng<<5; float c2=((float)(frng&0xFFFF)/32768.0f)-1.0f;
                float ox = cph*rad + c1*bm->jitter*0.8f*endRamp;   // screen-plane offset (tube + crackle)
                float oy = sph*rad + c2*bm->jitter*0.8f*endRamp;   // out-of-plane offset
                float fx2 = cxA[i] + sscx*ox + hsx*oy;
                float fy2 = cyA[i] + sscy*ox + hsy*oy;
                float fz2 = czA[i] + sscz*ox + hsz*oy;
                int a = (int)(baseA * aMul); if (a > 255) a = 255;
                unsigned col = rgb | ((unsigned)a << 24);
                fil[2*i].u=0; fil[2*i].v=0; fil[2*i].color=col;
                fil[2*i].x = fx2 - lsxA[i]*fw; fil[2*i].y = fy2 - lsyA[i]*fw; fil[2*i].z = fz2 - lszA[i]*fw;
                fil[2*i+1].u=1; fil[2*i+1].v=0; fil[2*i+1].color=col;
                fil[2*i+1].x = fx2 + lsxA[i]*fw; fil[2*i+1].y = fy2 + lsyA[i]*fw; fil[2*i+1].z = fz2 + lszA[i]*fw;
            }
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &fil->color);
            glVertexPointer(3, GL_FLOAT, sizeof(AfnVertex), &fil->x);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 2 * (N + 1));
        }

        // ---- bright HEAD ORB (the leading ball): a soft radial glow (blue halo + white core),
        // additive triangle fans so it's round without a texture ----
        if (bm->orbSize > 0.001f) {
            float ht = headT; if (ht>1.0f) ht=1.0f; if (ht<0.0f) ht=0.0f;
            float fi = ht*(float)N; int hi=(int)fi; if(hi>N-1)hi=N-1; if(hi<0)hi=0; int h2=hi+1<=N?hi+1:hi; float hf=fi-(float)hi;
            float ox = cxA[hi]+(cxA[h2]-cxA[hi])*hf, oy = cyA[hi]+(cyA[h2]-cyA[hi])*hf, oz = czA[hi]+(czA[h2]-czA[hi])*hf;
            float pls = 0.85f + 0.15f*sinf((float)afn_frame_count*0.7f);
            // two fans: large blue halo, then small white-hot core
            for (int pass = 0; pass < 2; pass++) {
                float orad = (pass==0 ? (hw*4.0f + bm->jitter*1.5f) : (hw*1.8f)) * pls * bm->orbSize;
                unsigned cc = (pass==0 ? rgb : 0x00FFFFFFu) | ((unsigned)baseA << 24);
                unsigned rim = (pass==0 ? rgb : 0x00FFFFFFu);   // alpha 0 at rim
                AfnVertex fan[11];
                fan[0].u=0.5f; fan[0].v=0.5f; fan[0].color=cc; fan[0].x=ox; fan[0].y=oy; fan[0].z=oz;
                for (int j = 0; j <= 8; j++) {
                    float ang = (float)j * (6.2831853f/8.0f); float ca=cosf(ang), sa=sinf(ang);
                    float rx=(Rwx*ca+Uwx*sa)*orad, ry=(Rwy*ca+Uwy*sa)*orad, rz=(Rwz*ca+Uwz*sa)*orad;
                    fan[1+j].u=0; fan[1+j].v=0; fan[1+j].color=rim; fan[1+j].x=ox+rx; fan[1+j].y=oy+ry; fan[1+j].z=oz+rz;
                }
                glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &fan->color);
                glVertexPointer(3, GL_FLOAT, sizeof(AfnVertex), &fan->x);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 10);
            }
        }
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// ---------------------------------------------------------------------------
// THUNDER spell (HARDCODED prototype, Select): hold to charge — a plane of dark
// rainclouds gathers overhead + a glowing reticle tracks the aim on the floor;
// release to call down a vertical lightning strike at the reticle. Pure-code:
// clouds = soft dark puff billboards, reticle = additive floor ring, strike = the
// lightning bundle (bow 0 = straight) via afn_beam_cast.
// ---------------------------------------------------------------------------
// Thunder tunables (defaults = the original hardcode; a Thunder effect layer overrides
// these via afn_thunder_apply()). The hardcode is preserved as these defaults.
float    afn_thunder_cloud_h      = 56.0f;   // cloud plane height above the floor
float    afn_thunder_aim          = 60.0f;   // reticle distance ahead of the player
int      afn_thunder_charge       = 90;      // frames to full charge
float    afn_thunder_cloud_spread = 26.0f;   // cloud disc radius
float    afn_thunder_cloud_size   = 12.0f;   // individual puff radius
int      afn_thunder_puffs        = 18;      // # cloud puffs
unsigned afn_thunder_cloud_col    = 0x00483028u; // dark blue-gray (0xAABBGGRR)
float    afn_thunder_reticle      = 4.0f;    // floor reticle radius
unsigned afn_thunder_reticle_col  = 0x00FFC060u; // reticle glow colour
float    afn_thunder_strike_w     = 0.8f;    // strike bolt width
float    afn_thunder_strike_jit   = 2.4f;    // strike crackle
int      afn_thunder_strike_fil   = 6;       // strike filaments
unsigned afn_thunder_strike_col   = 0xFFFFD0A0u; // strike bolt colour
int      afn_thunder_cam_pitch    = 40;      // cinematic camera UP-tilt while charging (degrees; 0 = none)
float    afn_thunder_cam_smooth   = 0.06f;   // ease-in rate of that tilt (lower = gentler)
int      afn_thunder_charging     = 0;       // 1 while charging — freezes player movement + the lock-pitch pull
static int   s_thCharging = 0, s_thCharge = 0, s_thStrike = 0;
static float s_thRetX = 0.0f, s_thRetZ = 0.0f;
int afn_thunder_charge_req = 0;   // Thunder Charge node sets this each frame it runs
int afn_thunder_strike_req = 0;   // Thunder Strike node sets this on release
// Floor Reticle node — set per frame by the node; afn_reticle_render draws + clears it.
int      afn_reticle_show = 0;
int      afn_reticle_dist = 60;
float    afn_reticle_size = 4.0f;
unsigned afn_reticle_col  = 0x00FFC060u;

// One soft radial billboard (bright/opaque centre -> transparent rim), camera-facing.
static void thunder_fan(float cx,float cy,float cz, float Rx,float Ry,float Rz,float Ux,float Uy,float Uz,
                        float rad, unsigned ctr, unsigned rim) {
    AfnVertex fan[10];
    fan[0].u=0.5f; fan[0].v=0.5f; fan[0].color=ctr; fan[0].x=cx; fan[0].y=cy; fan[0].z=cz;
    for (int j=0;j<=8;j++){ float a=(float)j*(6.2831853f/8.0f), ca=cosf(a), sa=sinf(a);
        float rx=(Rx*ca+Ux*sa)*rad, ry=(Ry*ca+Uy*sa)*rad, rz=(Rz*ca+Uz*sa)*rad;
        fan[1+j].u=0; fan[1+j].v=0; fan[1+j].color=rim; fan[1+j].x=cx+rx; fan[1+j].y=cy+ry; fan[1+j].z=cz+rz; }
    glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(AfnVertex),&fan->color);
    glVertexPointer(3,GL_FLOAT,sizeof(AfnVertex),&fan->x);
    glDrawArrays(GL_TRIANGLE_FAN,0,10);
}

// Stick free-aim state (declared before afn_thunder_apply, which sets the speeds from the layer).
int   afn_aim_stick_req = 0;
float afn_aim_speed     = 2.0f;   // reticle distance units / frame at full stick (back-and-forth)
int   afn_aim_orbit     = 500;    // camera orbit brad / frame while L2/R2 held
int   afn_aim_active    = 0;
float afn_aim_dist      = 60.0f;  // reticle distance ahead of the player (left stick adjusts)
float afn_aim_freeX = 0.0f, afn_aim_freeZ = 0.0f;

#ifdef AFN_HAS_FX
// Drive the Thunder globals from an authored Thunder (kind 2) effect layer — the editor's
// 3D preview mirrors this exactly. Strike + reticle reuse the layer's bolt fields.
static void afn_thunder_apply(const AfnFxLayer* L) {
    afn_thunder_cloud_h = L->tCloudH; afn_thunder_aim = L->tAim; afn_thunder_charge = (int)L->tCharge;
    afn_thunder_cloud_spread = L->tSpread; afn_thunder_cloud_size = L->tCloudSize;
    afn_thunder_puffs = L->tPuffs; afn_thunder_reticle = L->tReticle;
    afn_thunder_cloud_col   = 0xFF000000u | ((unsigned)L->tcloudb<<16) | ((unsigned)L->tcloudg<<8) | (unsigned)L->tcloudr;
    afn_thunder_reticle_col = 0xFF000000u | ((unsigned)L->colb<<16)    | ((unsigned)L->colg<<8)    | (unsigned)L->colr;
    afn_thunder_strike_col  = afn_thunder_reticle_col;
    afn_thunder_strike_w = L->bWidth; afn_thunder_strike_jit = L->bJitter; afn_thunder_strike_fil = L->filaments;
    afn_thunder_cam_pitch = (int)L->tCamPitch; afn_thunder_cam_smooth = L->tCamSmooth;
    afn_aim_speed = L->tAimSpeed; afn_aim_orbit = (int)L->tAimOrbit;   // reticle slide + orbit speed (Effects panel)
}
#endif

// Stick free-aim (node-driven; used while charging Thunder). The Aim Stick node sets the
// request each frame it runs; the LEFT STICK slides the floor reticle around (camera-
// relative); L2/R2 orbit. afn_thunder_step uses afn_aim_free* while active. (Globals above.)
static void afn_aim_step(float px, float py, float pz, float yawDeg) {
    if (!afn_aim_stick_req) { afn_aim_active = 0; return; }
    afn_aim_stick_req = 0;
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
    if (afn_cam_lock_target >= 0) { afn_aim_active = 0; return; }   // locked on: reticle snaps to target, no free-aim
#endif
    if (!afn_aim_active) { afn_aim_dist = afn_thunder_aim; afn_aim_active = 1; }
    // LEFT STICK alone: forward/back slides the aim distance (back-and-forth), left/right orbits
    // the camera (the reticle sweeps around with it). The player is frozen while charging, so the
    // stick is free for aiming.
    float sx = (float)(afn_stick_mag[3] - afn_stick_mag[2]) / 256.0f;   // right - left
    float sy = (float)(afn_stick_mag[0] - afn_stick_mag[1]) / 256.0f;   // up - down
    afn_aim_dist += sy * afn_aim_speed;
    if (afn_aim_dist < 4.0f) afn_aim_dist = 4.0f;
    orbit_angle -= (int)(sx * (float)afn_aim_orbit);   // stick left/right orbits (negate to flip)
    float camAngle = (float)orbit_angle * (6.2831853f / 65536.0f);     // reticle rides the camera forward at `dist`
    afn_aim_freeX = px + sinf(camAngle) * afn_aim_dist;
    afn_aim_freeZ = pz + cosf(camAngle) * afn_aim_dist;
}

static void afn_thunder_step(const float* view, float px, float py, float pz, float yawDeg) {
#ifdef AFN_HAS_FX
    for (int i=0;i<AFN_FX_LAYER_COUNT;i++) if (afn_fx_layers[i].kind==2) { afn_thunder_apply(&afn_fx_layers[i]); break; }
#endif
    // Node-driven: the Thunder Charge node sets afn_thunder_charge_req each frame it runs
    // (drive On Key Held); the Thunder Strike node sets afn_thunder_strike_req on release.
#if 0   // dev test: Select also charges/strikes Thunder — DISABLED so Select casts Meteor Mash.
    if (afn_keys_held & KEY_SELECT) afn_thunder_charge_req = 1;
    if (afn_keys_released & KEY_SELECT) afn_thunder_strike_req = 1;
#endif
    int charge = afn_thunder_charge_req; afn_thunder_charge_req = 0;
    int strike = afn_thunder_strike_req; afn_thunder_strike_req = 0;
    float yr = yawDeg * (3.14159265f/180.0f);
    // Aim: snap dead onto the lock-on target if one is locked, else a fixed distance ahead.
    float retX = px + sinf(yr)*afn_thunder_aim, retZ = pz + cosf(yr)*afn_thunder_aim;
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
    if (afn_cam_lock_target >= 0 && afn_ai_slot >= 0) { retX = s_npcX[afn_ai_slot]; retZ = s_npcZ[afn_ai_slot]; }
#endif
    if (afn_aim_active) { retX = afn_aim_freeX; retZ = afn_aim_freeZ; }   // stick free-aim overrides
    if (charge) {
        if (!s_thCharging) { s_thCharging = 1; s_thCharge = 0; }
        s_thCharge++; s_thRetX = retX; s_thRetZ = retZ;
    }
    if (strike) {
        if (!s_thCharging) { s_thRetX = retX; s_thRetZ = retZ; }   // bare strike: aim at current facing
        s_thCharging = 0;
        // STRIKE: straight (bow 0) jagged bundle, cloud -> reticle.
        afn_beam_bounces = 1; afn_beam_bow = 0.0f; afn_beam_width = afn_thunder_strike_w; afn_beam_jitter = afn_thunder_strike_jit;
        afn_beam_segs = 18; afn_beam_decay = 1.0f; afn_beam_pulse = 0.0f; afn_beam_life = 12;
        afn_beam_travel = 0; afn_beam_col = afn_thunder_strike_col; afn_beam_filaments = afn_thunder_strike_fil; afn_beam_orb = 0.5f;
        afn_beam_cast(s_thRetX, py+afn_thunder_cloud_h, s_thRetZ, s_thRetX, py+1.0f, s_thRetZ);
        // impact sparks
        afn_part_frame=-1; afn_part_blend=1; afn_part_speed=2.4f; afn_part_spread=1.3f;
        afn_part_life=26; afn_part_grav=0.06f; afn_part_size0=0.5f; afn_part_size1=0.0f;
        afn_part_col0=0xFFFFE0B0u; afn_part_col1=0x00FFFFFFu; afn_part_spawn=26;
        afn_particles_emit(s_thRetX, py+1.0f, s_thRetZ);
        s_thStrike = 16;
    } else if (!charge) {
        s_thCharging = 0;   // released/cancelled without a strike → drop the clouds
    }
    afn_thunder_charging = s_thCharging;   // freezes player movement + drives the LOCKED camera up-tilt
                                           // (done in the camera block so it converges and holds cleanly)
    if (!s_thCharging && s_thStrike <= 0) return;

    float Rwx=view[0],Rwy=view[4],Rwz=view[8], Uwx=view[1],Uwy=view[5],Uwz=view[9];
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);

    if (s_thCharging) {
        float cdiv = afn_thunder_charge>1?(float)afn_thunder_charge:1.0f;
        float chg = (float)s_thCharge/cdiv; if (chg>1.0f) chg=1.0f;   // ramps to full charge
        int npuff = afn_thunder_puffs<1?1:(afn_thunder_puffs>48?48:afn_thunder_puffs);
        unsigned ccol = afn_thunder_cloud_col & 0x00FFFFFFu;
        // raincloud mass: dark soft puffs over the reticle (alpha-blended)
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int i=0;i<npuff;i++){
            unsigned h = (unsigned)((i+1)*2654435761u) ^ 0x9E3779B9u;
            float ang = (float)(h & 0xFFFF)/65536.0f * 6.2831853f;
            float rr  = 6.0f + (float)((h>>16)&0xFF)/255.0f * afn_thunder_cloud_spread;
            float drift = sinf((float)afn_frame_count*0.02f + (float)i)*2.0f;
            float cx=s_thRetX+cosf(ang)*rr+drift, cy=py+afn_thunder_cloud_h+((float)((h>>8)&7)-3.0f), cz=s_thRetZ+sinf(ang)*rr;
            unsigned a = (unsigned)(150.0f*chg)+30u;
            thunder_fan(cx,cy,cz, Rwx,Rwy,Rwz,Uwx,Uwy,Uwz, afn_thunder_cloud_size, ccol|(a<<24), ccol);
        }
        // internal charge flickers (additive) building toward the strike
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        if (((afn_frame_count>>2) & 3)==0) {
            int fi=(afn_frame_count/4)%npuff; unsigned h=(unsigned)((fi+1)*2654435761u)^0x9E3779B9u;
            float ang=(float)(h&0xFFFF)/65536.0f*6.2831853f, rr=6.0f+(float)((h>>16)&0xFF)/255.0f*afn_thunder_cloud_spread;
            unsigned a=(unsigned)(190.0f*chg);
            thunder_fan(s_thRetX+cosf(ang)*rr, py+afn_thunder_cloud_h, s_thRetZ+sinf(ang)*rr, Rwx,Rwy,Rwz,Uwx,Uwy,Uwz, afn_thunder_cloud_size*0.75f, 0x00FFE0B0u|(a<<24), 0x00FFE0B0u);
        }
        // floor reticle: glowing additive ring (XZ plane) + pulsing centre
        unsigned rcol = afn_thunder_reticle_col & 0x00FFFFFFu;
        float ry=py+0.3f, r0=afn_thunder_reticle+chg*1.0f, r1=r0+1.3f, spin=(float)afn_frame_count*0.03f;
        AfnVertex ring[2*25];
        for (int j=0;j<=24;j++){ float a=spin+(float)j*(6.2831853f/24.0f), ca=cosf(a), sa=sinf(a);
            unsigned col=rcol|(200u<<24);
            ring[2*j].u=0; ring[2*j].v=0; ring[2*j].color=col; ring[2*j].x=s_thRetX+ca*r0; ring[2*j].y=ry; ring[2*j].z=s_thRetZ+sa*r0;
            ring[2*j+1].u=1; ring[2*j+1].v=0; ring[2*j+1].color=col; ring[2*j+1].x=s_thRetX+ca*r1; ring[2*j+1].y=ry; ring[2*j+1].z=s_thRetZ+sa*r1; }
        glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(AfnVertex),&ring->color);
        glVertexPointer(3,GL_FLOAT,sizeof(AfnVertex),&ring->x);
        glDrawArrays(GL_TRIANGLE_STRIP,0,2*25);
        float pls=0.5f+0.5f*sinf((float)afn_frame_count*0.2f);
        thunder_fan(s_thRetX,ry,s_thRetZ, Rwx,Rwy,Rwz,Uwx,Uwy,Uwz, 1.5f+pls, 0x00FFE0A0u|((120u+(unsigned)(100.0f*pls))<<24), 0x00FFE0A0u);
    }
    if (s_thStrike>0) {
        s_thStrike--;
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        float f=(float)s_thStrike/16.0f; unsigned a=(unsigned)(230.0f*f);
        thunder_fan(s_thRetX, py+1.0f, s_thRetZ, Rwx,Rwy,Rwz,Uwx,Uwy,Uwz, 7.0f*(1.2f-f), 0x00FFFFFFu|(a<<24), 0x00FFC080u);
    }
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

// Floor Reticle node: draw a spinning additive ring + pulsing centre on the floor a set
// distance ahead of the player's facing, when afn_reticle_show was set this frame (then
// clear it). Reuses thunder_fan for the soft centre glow.
void afn_reticle_render(const float* view, float px, float py, float pz, float yawDeg) {
    if (!afn_reticle_show) return;
    afn_reticle_show = 0;
    float yr = yawDeg * (3.14159265f/180.0f);
    float rx = px + sinf(yr)*(float)afn_reticle_dist, rz = pz + cosf(yr)*(float)afn_reticle_dist;
    float Rwx=view[0],Rwy=view[4],Rwz=view[8], Uwx=view[1],Uwy=view[5],Uwz=view[9];
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDepthMask(GL_FALSE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    unsigned rc = afn_reticle_col & 0x00FFFFFFu;
    float ry=py+0.3f, r0=afn_reticle_size, r1=r0+1.3f, spin=(float)afn_frame_count*0.03f;
    AfnVertex ring[2*25];
    for (int j=0;j<=24;j++){ float a=spin+(float)j*(6.2831853f/24.0f), ca=cosf(a), sa=sinf(a);
        unsigned col=rc|(200u<<24);
        ring[2*j].u=0; ring[2*j].v=0; ring[2*j].color=col; ring[2*j].x=rx+ca*r0; ring[2*j].y=ry; ring[2*j].z=rz+sa*r0;
        ring[2*j+1].u=1; ring[2*j+1].v=0; ring[2*j+1].color=col; ring[2*j+1].x=rx+ca*r1; ring[2*j+1].y=ry; ring[2*j+1].z=rz+sa*r1; }
    glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(AfnVertex),&ring->color);
    glVertexPointer(3,GL_FLOAT,sizeof(AfnVertex),&ring->x);
    glDrawArrays(GL_TRIANGLE_STRIP,0,2*25);
    float pls=0.5f+0.5f*sinf((float)afn_frame_count*0.2f);
    thunder_fan(rx,ry,rz, Rwx,Rwy,Rwz,Uwx,Uwy,Uwz, afn_reticle_size*0.4f+pls, rc|((120u+(unsigned)(100.0f*pls))<<24), rc);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D);
}

#ifdef AFN_HAS_FX
// Spawn one authored effect LAYER at the player. kind 0 = particle burst (its emitter
// params), kind 1 = lightning bolt that follows the layer's authored spline
// (afn_fx_pts[splineStart..]). Pixel-space editor params -> world (width*0.1, bow*0.15,
// jitter*0.1).
static void afn_fx_play_layer(const AfnFxLayer* L, float px, float py, float pz, float yawDeg) {
    if (L->kind == 0) {
        afn_part_spawn = L->pCount > 0 ? L->pCount : 1;
        afn_part_frame = -1; afn_part_blend = 1;
        afn_part_speed = L->pSpeed; afn_part_spread = L->pSpread; afn_part_life = L->pLife;
        afn_part_grav  = L->pGrav;  afn_part_size0 = L->pSize * 0.1f; afn_part_size1 = 0.0f;
        afn_part_col0 = 0xFFFFFFFFu; afn_part_col1 = 0x00FFFFFFu;
        afn_particles_emit(px, py + 14.0f, pz);
    } else if (L->kind == 3) {
        afn_meteor_fire(px, py, pz, yawDeg);   // Meteor Mash projectile (tuned prototype)
    } else if (L->kind == 4) {
        // Wild Charge: reuse the Quick Attack dash, flagged for the electric visuals (yellow
        // trail + light-blue sparks). The movement block picks up the trigger next frame.
        afn_qa_trigger = 1; afn_wild_charge = 1; afn_qa_tgt = -1;
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
        if (afn_cam_lock_target >= 0) afn_qa_tgt = afn_cam_lock_target;   // rush the locked target
#endif
    } else if (L->kind == 5) {
        afn_electroweb_fire(px, py, pz, yawDeg);   // Electroweb snare lattice (status move)
    } else if (L->kind == 6) {
        afn_firespin_fire();                       // Fire Spin vortex on the player (follows live pos)
    } else if (L->kind == 7) {
        afn_bubblebeam_fire(px, py, pz, yawDeg);   // Bubble Beam forward stream
    } else if (L->kind == 8) {
        afn_icebeam_fire(px, py, pz, yawDeg);      // Ice Beam (3-strand diamond helix)
    } else if (L->kind == 9) {
        afn_sludge_fire(px, py, pz, yawDeg);       // Sludge Bomb (arc lob + floor puddle)
    } else if (L->kind == 10) {
        afn_psybeam_fire(px, py, pz, yawDeg);      // Psybeam (rainbow zigzag zap)
    } else if (L->kind == 11) {
        afn_psychic_fire(px, py, pz, yawDeg);      // Psychic (funnel of 6 rings)
    } else if (L->kind == 12) {
        afn_surf_fire(px, py, pz, yawDeg);         // Surf (wave sweeps forward + the player rides it)
    } else if (L->kind == 13) {
        afn_flame_fire(px, py, pz, yawDeg);        // Flamethrower (forward fire jet)
    } else if (L->kind == 14) {
        // Orb tint from the layer's Aura colour; trail tint from its Trail colour (both with fallbacks).
        s_au_cr=(int)L->colr; s_au_cg=(int)L->colg; s_au_cb=(int)L->colb;
        if(s_au_cr+s_au_cg+s_au_cb < 24){ s_au_cr=96; s_au_cg=176; s_au_cb=255; }
        { int tmax=(int)L->tcloudr; if((int)L->tcloudg>tmax)tmax=(int)L->tcloudg; if((int)L->tcloudb>tmax)tmax=(int)L->tcloudb;
          if(tmax<90){ s_au_tr=(int)(s_au_cr*0.72f+26); s_au_tg=(int)(s_au_cg*0.80f+22); s_au_tb=(int)(s_au_cb*0.80f+18); }
          else { s_au_tr=(int)L->tcloudr; s_au_tg=(int)L->tcloudg; s_au_tb=(int)L->tcloudb; }
          if(s_au_tr>255)s_au_tr=255; if(s_au_tg>255)s_au_tg=255; if(s_au_tb>255)s_au_tb=255; }
        afn_aura_fire(px, py, pz, yawDeg);         // Aura Sphere (traveling energy orb)
    } else if (L->kind == 15) {
        afn_icywind_fire(px, py, pz, yawDeg);      // Icy Wind (wide ice-mist wall sweeping forward)
    } else if (L->kind == 16) {
        afn_flamewheel_fire(px, py, pz, yawDeg);   // Flame Wheel (fire ring the player rides forward)
    } else if (L->kind == 17) {
        afn_flashcannon_fire(px, py, pz, yawDeg);  // Flash Cannon (silver-white beam blast)
    } else {
        // Lightning bundle — all params are WORLD units straight from the layer (the editor's
        // 3D sim authors in the same units, so it mirrors this exactly). No px scaling.
        afn_beam_width   = L->bWidth;
        afn_beam_bow     = L->bBow;
        afn_beam_jitter  = L->bJitter;
        afn_beam_segs    = L->bSegs > 1 ? L->bSegs : 14;
        afn_beam_bounces = L->bBounces > 0 ? L->bBounces : 1;
        afn_beam_decay   = L->bDecay; afn_beam_pulse = L->bPulse;
        afn_beam_life    = (L->travel && (int)L->bTravelLife > 5) ? (int)L->bTravelLife
                                                                  : ((int)L->pLife > 5 ? (int)L->pLife : 18);
        // Forward reach = arc length x bounces (the SAME arc repeats N times across the map).
        { float al = L->bArcLen > 1.0f ? L->bArcLen : 1.0f;
          afn_beam_range = (int)(al * afn_beam_bounces); if (afn_beam_range < 2) afn_beam_range = 2; }
        afn_beam_col = 0xFF000000u | ((unsigned)L->colb << 16) | ((unsigned)L->colg << 8) | (unsigned)L->colr;
        afn_beam_filaments = L->filaments > 0 ? L->filaments : 1;
        afn_beam_orb = L->orbSize;
        afn_beam_travel = L->travel; afn_beam_travel_persist = L->bTravelPersist > 0.01f ? L->bTravelPersist : 0.01f;
        afn_beam_travel_fade = L->bTravelFade;
        afn_beam_spline = 0; afn_beam_spline_n = 0;   // parametric bounce bundle
        afn_beam_spawn = 1;
        afn_beam_resolve(px, py, pz, yawDeg);
    }
}

// Play Effect node: trigger an authored effect INSTANCE (index into afn_fx_instances)
// at the player — plays ALL of that instance's composited layers at once.
int afn_fx_play_req = 0;   // 0 = none; else (instance index + 1) queued by the node this frame
static void afn_fx_play(int idx, float px, float py, float pz, float yawDeg) {
    if (idx < 0 || idx >= AFN_FX_COUNT) return;
    const AfnFxInstance* In = &afn_fx_instances[idx];
    for (int i = 0; i < In->layerCount; i++)
        afn_fx_play_layer(&afn_fx_layers[In->layerStart + i], px, py, pz, yawDeg);
}
#endif // AFN_HAS_FX

// Node primitive (Step Enemy Beam): advance the enemy's fired projectile. Driven
// once per frame by the node graph, independent of the enemy's life/visibility, so
// a shot always completes (hits the player or expires) even if the enemy dies
// mid-flight. Reads the player position from player_x/y/z (1-frame lag is fine).
void afn_enemy_beam_step(void) {
    if (afn_paused) return;   // scene pause: freeze the in-flight enemy projectile
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
    if (afn_paused) return;   // scene pause: freeze in-flight player Focus Blasts
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
    if (afn_enemy_frozen || afn_paused) {   // cutscene freeze OR scene pause: no decisions
        // Don't stand a DEAD enemy back up. A victory cutscene (Freeze Enemy) or pause
        // would otherwise stomp the die-collapse (the BP's Hold Skel Clip, held via
        // s_npcClipHold) with the idle pose — "the enemy standing up in the background."
        // Only a LIVE enemy with no held clip snaps to idle while frozen.
        if (afn_hp[eidx] > 0 && !s_npcClipHold[i]) s_npcClip[i] = afn_aic_idle;
        return;
    }
#if defined(AFN_HAS_HUD) && defined(AFN_HAS_SPRITE_IDX)
    if (afn_hud_visible[AFN_CLASH_ELEM]) return;   // beam clash owns the enemy — freeze the AI
#endif
    if (s_pc_active) return;                       // PHYSICAL clash owns the enemy — no decisions/attacks mid-QTE
    if (s_aiAtkCD > 0) s_aiAtkCD--;
    if (s_aiDodgeCD > 0) s_aiDodgeCD--;
    if (afn_ai_state != AI_CHARGE) { s_efbCharging = 0; s_eChgDodgeFrames = 0; }   // never leave a charge orb hanging; drop any in-charge dodge so it can't resume on the next charge

    if (afn_ai_state != AI_BLOCK) { afn_ai_blocking = 0; s_eBlockFrames = 0; }   // only the BLOCK state keeps the guard up
    // A dodge/block interrupted by a state change (or the beam clash) must not leave
    // its frame counter stuck > 0: afn_ai_blast_incoming gates on s_eDodgeFrames /
    // s_eBlockFrames, so a stuck value would PERMANENTLY stop the enemy dodging.
    if (afn_ai_state != AI_DODGE) s_eDodgeFrames = 0;

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
        afn_ai_lose_ready = (++s_aiLoseT >= afn_ait_lose_frames) ? 1 : 0;
    } else { s_aiLoseT = 0; afn_ai_lose_ready = 0; }

    // Face the player every combat frame (the lock-on, no reticle).
    if (afn_ai_state != AI_ROAM) {
        float toYaw = atan2f(dx, dz) * (180.0f / 3.14159265f);
        float diff = toYaw - s_npcYaw[i]; while (diff > 180.0f) diff -= 360.0f; while (diff < -180.0f) diff += 360.0f;
        s_npcYaw[i] += diff * (afn_ait_yaw_ease_m / 100.0f);
    }

    // (Dodge-ready is now a node: the Is Blast Incoming gate calls
    // afn_ai_blast_incoming() — see below. AI Sense no longer rolls it.)
    afn_ai_can_fire = (s_aiAtkCD == 0 && !s_efbActive && !s_efbCharging) ? 1 : 0;
}

// ROAM: nav drives motion; just pick walk/idle clip.
void afn_ai_roam(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
#ifdef AFN_HAS_NAVMESH
    s_npcClip[i] = s_npcNavMoving[i] ? afn_aic_move : afn_aic_idle;   // Move : Idle (name-resolved via AI Clips)
#endif
}

// CHASE: close toward the player; set afn_ai_reached at the strafe radius.
void afn_ai_chase(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    s_npcClip[i] = afn_aic_move;   // Move (name-resolved via AI Clips)
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
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    // 8-dir strafe walk clips. 44-anim glTF map (atk_phs variants shift +3, "charge"
    // at 23 shifts the rest +1 more): Move 36, strafeL 37, strafeLD 38, strafeLDFW 39,
    // strafeR 40, strafeRD 41, strafeRDFW 42, backpeddle 21. Same direction order.
    int eStrafe[8] = { afn_aic_move, afn_aic_strafe_ldfw, afn_aic_strafe_l, afn_aic_strafe_rd,
                       afn_aic_backpeddle, afn_aic_strafe_ld, afn_aic_strafe_r, afn_aic_strafe_rdfw };
    if (--s_aiStrafeLeg <= 0) { s_aiStrafeDir = ai_chance(0.5f) ? 1 : -1; s_aiStrafeLeg = afn_ait_strafe_leg; }
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
    if (s_efbCharging) return 0;   // while charging, the in-charge sidestep (afn_ai_charge_step) handles dodging WITHOUT a state switch, so the node-driven dodge can't cancel the charge
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
    if (s_efbCharging) return 0;   // while charging, the in-charge sidestep handles the threat — don't block (a block state-switch would cancel the charge)
    if (s_eDodgeFrames != 0 || s_eBlockFrames != 0 || afn_ai_state == AI_BLOCK || !afn_fb_active) return 0;
    float d2 = afn_fb_nearest_sq(s_npcX[i], s_npcZ[i]);
    float dtr = (float)afn_ai_dodge_trig;
    if (d2 < 0.0f || d2 > dtr*dtr) return 0;
    return ai_chance(afn_ai_block_prob * 0.01f) ? 1 : 0;
}

// BLOCK begin: raise the guard (Block clip 22, shared rig) for a short window.
void afn_ai_block_begin(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_eBlockFrames = 24; afn_ai_blocking = 1; afn_ai_block_done = 0;
    s_npcClip[i] = afn_aic_block;
}

// BLOCK step: hold the guard + clip; set afn_ai_block_done when the window ends.
void afn_ai_block_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    afn_ai_blocking = 1; s_npcClip[i] = afn_aic_block;
    afn_ai_block_done = (--s_eBlockFrames <= 0) ? 1 : 0;
}

// DODGE begin: set up the side-roll (called once on the dodge-ready edge). Mirrors
// the PLAYER's dodge: same committed side burst, same dodge clips (Dodge node's
// afn_dodge_clip_l/r, fallback to the rig's DodgeL/R 28/29) — but L/R swapped, since
// the enemy faces the player so its screen-left/right is mirrored.
void afn_ai_dodge_begin(void) {
    int i = afn_ai_slot; if (i < 0) return;
    float px = (float)player_x, pz = (float)player_z;
    float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
    float pl = afn_ai_dist > 1e-3f ? afn_ai_dist : 1.0f, fx = dx/pl, fz = dz/pl, rx = fz, rz = -fx;
    // Lock mirrors the PLAYER's lock-strafe (parity with the player's own dodge): only while
    // the player is Z-targeting does anyone strafe. Out of lock there's NO sideways dodge —
    // only back/forth along the player axis.
    int locked = (afn_lock_strafe && afn_cam_lock_target >= 0);
    if (locked) {
        // LOCK-ON strafe dodge: perpendicular to the player, random L/R; clip from the move
        // vector projected onto the enemy's actual render facing (its OWN DodgeL/R clips).
        int side = ai_chance(0.5f) ? 1 : -1;
        s_eDodgeDX = rx * side; s_eDodgeDZ = rz * side;
        float ey = s_npcYaw[i] * DEG2RAD;
        float dotR = s_eDodgeDX * cosf(ey) - s_eDodgeDZ * sinf(ey);
        s_eDodgeClip = (dotR > 0.0f) ? afn_ai_dodge_clip_l : afn_ai_dodge_clip_r;
    } else {
        // OUT OF LOCK: NEVER strafe — back/forth along the player axis only. Random forward
        // (toward the player, DodgeFW) or back (away, DodgeBWD); the enemy faces the player so
        // forward/back is unambiguous (no L/R parity to break).
        if (ai_chance(0.5f)) { s_eDodgeDX = -fx; s_eDodgeDZ = -fz; s_eDodgeClip = afn_ai_dodge_clip_b; }   // back, away
        else                 { s_eDodgeDX =  fx; s_eDodgeDZ =  fz; s_eDodgeClip = afn_ai_dodge_clip_f; }   // forward, in
    }
    s_eDodgeFrames = s_eDodgeTotal = afn_ait_dodge_frames; s_aiDodgeCD = afn_ait_dodge_cd; afn_ai_dodge_done = 0;
}

// DODGE step: integrate the roll; set afn_ai_dodge_done when finished. Mirrors the
// player's roll — speed from the Dodge node (afn_dodge_speed, default 70 — the old
// enemy 9 barely moved), quadratic ease-IN over afn_dodge_ramp and ease-OUT over
// afn_dodge_falloff, with the same sub-stepped wall collision so the fast roll can't
// tunnel through a wall.
void afn_ai_dodge_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    if (s_eDodgeFrames > 0) {
        int total = s_eDodgeTotal, frames = s_eDodgeFrames;
        int ramp = afn_ait_dodge_ramp    >= 0 ? afn_ait_dodge_ramp    : (afn_dodge_ramp  > 0 ? afn_dodge_ramp  : 6);
        int fall = afn_ait_dodge_falloff >= 0 ? afn_ait_dodge_falloff : (afn_dodge_falloff > 0 ? afn_dodge_falloff : 6);
        if (ramp > total) ramp = total; if (fall > total) fall = total;
        float env = 1.0f;
        if (ramp > 0) { float t = (float)(total - frames + 1) / (float)ramp; if (t > 1.0f) t = 1.0f; if (t*t < env) env = t*t; }
        if (fall > 0) { float u = (float)frames / (float)fall; if (u > 1.0f) u = 1.0f; if (u*u < env) env = u*u; }
        int spd = afn_ait_dodge_speed >= 0 ? afn_ait_dodge_speed : (afn_dodge_speed > 0 ? afn_dodge_speed : 70);
        float sp = spd * 0.08f * env;
        float ix = s_eDodgeDX * sp, iz = s_eDodgeDZ * sp;
        int sub = (int)(sp / 3.0f) + 1;
        for (int st = 0; st < sub; st++) { s_npcX[i] += ix/sub; s_npcZ[i] += iz/sub; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
        s_npcClip[i] = s_eDodgeClip;   // clip chosen at dodge_begin (strafe L/R while locked, forward when de-aggro'd)
        afn_ai_dodge_done = (--s_eDodgeFrames == 0) ? 1 : 0;
    } else afn_ai_dodge_done = 1;
}

// HARDCODED enemy melee reflex (prototype): runs each frame for the enemy NPC AFTER
// the node BP has moved it (so it overrides position/clip while active). Two parts:
//   1) Jump-evade — when the player's Quick Attack is dashing straight at this enemy
//      (afn_qa_phase==1, afn_qa_tgt==eidx), roll once on the dash's rising edge to hop
//      (s_npcVY launch; the per-NPC gravity integrator arcs + lands it). Clips 31/32.
//   2) Quick Attack — occasional dash-in melee mirroring the player's: lunge (34) at
//      ENEMY_QA_SPEED toward the player, contact damage to afn_health within QA_STOP.
//      On CONNECT the dash ends on the hit; only a WHIFF plays the decelerating skid
//      (35) recovery. Suppresses BP motion by overwriting X/Z/Yaw/clip.
// To migrate: expose the tunables as node pins + a SetAiState(QuickAttack/Jump) path.
static void afn_ai_melee_reflex(int i, int eidx, float px, float pz) {
    if (afn_ai_state == AI_DEAD) { s_eqaPhase = 0; s_ePrevPlayerQA = 0; return; }

    // --- Jump-evade ---------------------------------------------------------
    int playerDashAtMe = (afn_qa_phase == 1 && afn_qa_tgt == eidx);
    if (s_eJumpCD > 0) s_eJumpCD--;
    // ALWAYS jump-evade a player Quick Attack (no chance roll). Works mid-charge too:
    // the condition doesn't gate on the charge state, and the gravity integrator below
    // arcs the launch regardless of what the enemy is doing.
    if (playerDashAtMe && !s_ePrevPlayerQA && s_npcGround[i] && s_eqaPhase == 0
        && s_eDodgeFrames == 0 && s_eJumpCD == 0) {
        s_npcVY[i] = afn_eqa_jump_vel_m / 100.0f; s_npcGround[i] = 0; s_eJumpCD = afn_eqa_jump_cd;
    }
    s_ePrevPlayerQA = playerDashAtMe;

    // --- Quick Attack dash-in ----------------------------------------------
    if (s_eqaCD > 0) s_eqaCD--;
    if (s_eqaPhase == 0 && s_eqaCD == 0 && s_npcGround[i] && !s_pc_active
        && (afn_ai_state == AI_STRAFE || afn_ai_state == AI_CHASE)   // only from neutral movement
        && s_eDodgeFrames == 0 && s_eBlockFrames == 0) {
        float dx = px - s_npcX[i], dz = pz - s_npcZ[i], d2 = dx*dx + dz*dz;
        if (d2 <= (float)afn_eqa_range*afn_eqa_range && ai_chance(afn_eqa_chance_m * 0.001f)) {
            float d = sqrtf(d2); if (d < 1e-3f) d = 1.0f;
            s_eqaDirX = dx/d; s_eqaDirZ = dz/d;
            s_eqaYaw = atan2f(s_eqaDirX, s_eqaDirZ) * (180.0f/3.14159265f);
            s_eqaPhase = 1; s_eqaFrames = ENEMY_QA_MAX; s_eqaDealt = 0;
            afn_play_sfx_inst_gain(afn_ai_sfx_whoosh, afn_enemy_sfx_gain(afn_ai_dist));   // dash whoosh (Ai Quick Attack -> Whoosh SFX pin)
        }
    }
    if (s_eqaPhase == 1) {
        float sp = afn_eqa_speed * 0.08f;
        int ramp = 4; { float t = (float)(ENEMY_QA_MAX - s_eqaFrames + 1) / (float)ramp; if (t > 1.0f) t = 1.0f; sp *= t*t; }
        float ix = s_eqaDirX*sp, iz = s_eqaDirZ*sp; int sub = (int)(sp/3.0f) + 1;
        for (int st = 0; st < sub; st++) { s_npcX[i] += ix/sub; s_npcZ[i] += iz/sub; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
        s_npcYaw[i] = s_eqaYaw; s_npcClip[i] = afn_aic_lunge;   // lunge
        if (!s_eqaDealt) {
            float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
            // Grounded only — the player can JUMP over the enemy's Quick Attack, the
            // same way the enemy jump-evades the player's (the dash whiffs underneath).
            // MUTUAL DASH: if the player is dashing too (or just was — grace), don't deal
            // damage or end the dash; let the physical clash trigger instead.
            if (player_on_ground && s_pc_pG == 0 && dx*dx + dz*dz <= (float)afn_eqa_stop*afn_eqa_stop) {
                int dmg = afn_player_blocking ? (afn_eqa_dmg * afn_block_pct) / 100 : afn_eqa_dmg;
                afn_health -= dmg; if (afn_health < 0) afn_health = 0;
                // CONNECT: end the dash on the hit — no skid (skid is the overshoot
                // recovery, only for a whiff). Stops it sliding INTO the player.
                s_eqaDealt = 1; s_eqaPhase = 0; s_eqaCD = afn_eqa_cd;
            }
        }
        if (s_eqaPhase == 1 && --s_eqaFrames <= 0) { s_eqaPhase = 2; s_eqaFrames = ENEMY_QA_SKID; }   // WHIFF -> skid recovery
    } else if (s_eqaPhase == 2) {
        float dec = (ENEMY_QA_SKID > 0) ? (float)s_eqaFrames / (float)ENEMY_QA_SKID : 0.0f;
        float sp = afn_eqa_speed * 0.08f * dec * 0.5f;
        float ix = s_eqaDirX*sp, iz = s_eqaDirZ*sp; int sub = (int)(sp/3.0f) + 1;
        for (int st = 0; st < sub; st++) { s_npcX[i] += ix/sub; s_npcZ[i] += iz/sub; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
        s_npcYaw[i] = s_eqaYaw; s_npcClip[i] = afn_aic_skid;   // skid
        if (--s_eqaFrames <= 0) { s_eqaPhase = 0; s_eqaCD = afn_eqa_cd; }
    }
    // Airborne (jump-evade) clip when not mid-QA: rising = jump (31), falling = jump_fall (32).
    if (s_eqaPhase == 0 && !s_npcGround[i]) s_npcClip[i] = (s_npcVY[i] > 0.0f) ? afn_aic_jump : afn_aic_jumpfall;
}

// NODE entry (Ai Quick Attack): run the melee reflex for the enemy AI slot. The Ai
// Quick Attack node sets the afn_qa_* tunables above, then calls this each frame from
// the enemy BP's On Update chain (placed AFTER the movement/state nodes so the reflex
// overrides pose/position). Suppressed while the beam-clash struggle backdrop is up —
// the enemy is locked in the side-view mash and must NOT Quick-Attack / damage the
// player mid-clash. Replaces the old hardcoded call in the NPC update loop.
void afn_ai_quick_attack(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    if (afn_hud_visible[AFN_CLASH_ELEM]) return;
    afn_ai_melee_reflex(i, AFN_ENEMY_EIDX, (float)player_x, (float)player_z);
}

// CHARGE begin: roll charge-vs-tap, start the wind-up (called once on entry).
void afn_ai_charge_begin(void) {
    s_aiChargeShot = ai_chance(afn_ai_chargeprob * 0.01f);
    // Same charge time as the player: full charge takes afn_fb_max frames (Focus Blast).
    s_aiTimer = s_aiChargeShot ? afn_fb_max : afn_ait_tap_windup;
    s_efbScale = afn_ai_orb_min; s_efbCharging = 1; afn_ai_charge_done = 0;   // seed at Min Scale%; charge_step grows it
    // Charge wind-up SFX — track OUR voice so stopping it can't cut the player's
    // chargefocus (same shared instance/sample). Shield it from the player's
    // sample-wide chargefocus StopSound (fired when the player launches the blast
    // the enemy is charge-dodging) so the enemy loop carries on instead of resetting.
    if (s_aiChargeShot) { s_eChargeVoice = afn_play_sfx_inst_gain(afn_ai_sfx_charge, afn_enemy_sfx_gain(afn_ai_dist)); afn_sfx_protect_voice = s_eChargeVoice; }
}

// CHARGE step: hold the charge pose, grow the orb at the muzzle; set
// afn_ai_charge_done when the wind-up elapses (the FireBeam node then launches).
void afn_ai_charge_step(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    s_npcClip[i] = afn_aic_charge; s_efbCharging = 1;   // atk_spc_chg (charge pose)
    // Keep OUR charge voice alive — if it was cut, restart it. Check our specific
    // voice (not any sample-4 voice) so the player's chargefocus doesn't fool it.
    if (s_aiChargeShot && !afn_inst_voice_active(s_eChargeVoice, afn_ai_sfx_charge)) {
        s_eChargeVoice = afn_play_sfx_inst_gain(afn_ai_sfx_charge, afn_enemy_sfx_gain(afn_ai_dist));
        afn_sfx_protect_voice = s_eChargeVoice;
    }
    float mx, my, mz; enemy_muzzle(i, &mx, &my, &mz);
    s_efbX = mx; s_efbY = my; s_efbZ = mz;
    // Grow the orb LINEARLY across the whole wind-up (like the player's level-based
    // growth), so the charge visibly fills instead of snapping to full in a few
    // frames (old exp lerp). frac = 0..1 over the wind-up.
    float tgt = s_aiChargeShot ? afn_ai_orb_max : afn_ai_orb_min;   // tap stays at Min Scale% (no wind-up growth)
    int total = s_aiChargeShot ? afn_fb_max : afn_ait_tap_windup;   // match player charge time
    float frac = total > 0 ? (1.0f - (float)s_aiTimer / (float)total) : 1.0f;
    if (frac < 0.0f) frac = 0.0f; if (frac > 1.0f) frac = 1.0f;
    s_efbScale = afn_ai_orb_min + (tgt - afn_ai_orb_min) * frac;
    afn_ai_charge_done = (--s_aiTimer <= 0) ? 1 : 0;

    // --- Charge-dodge: sidestep an incoming blast WITHOUT dropping the charge. The orb
    // keeps growing (above) the whole time; we only add a lateral roll + swap the charge
    // pose for the charge-dodge clip. Reuses the dodge tunables (range/chance/speed/CD). ---
    if (s_aiChgDodgeCD > 0) s_aiChgDodgeCD--;
    if (s_eChgDodgeFrames == 0 && s_aiChgDodgeCD == 0 && afn_fb_active) {
        float d2 = afn_fb_nearest_sq(s_npcX[i], s_npcZ[i]);
        float dtr = (float)afn_ai_dodge_trig;
        if (d2 >= 0.0f && d2 <= dtr*dtr && ai_chance(afn_ai_dodgeprob * 0.01f)) {
            float px = (float)player_x, pz = (float)player_z;
            float dx = px - s_npcX[i], dz = pz - s_npcZ[i];
            float pl = afn_ai_dist > 1e-3f ? afn_ai_dist : sqrtf(dx*dx + dz*dz); if (pl < 1e-3f) pl = 1.0f;
            float fx = dx/pl, fz = dz/pl, rx = fz, rz = -fx;
            int side = ai_chance(0.5f) ? 1 : -1;
            s_eChgDodgeDX = rx * side; s_eChgDodgeDZ = rz * side;
            // Clip from the move vector projected onto the enemy's actual render facing
            // (s_npcYaw) — robust to the facing lag/drift, same as the standard dodge.
            float cdy = s_npcYaw[i] * DEG2RAD;
            float cdDotR = s_eChgDodgeDX * cosf(cdy) - s_eChgDodgeDZ * sinf(cdy);
            s_eChgDodgeClip = (cdDotR > 0.0f) ? afn_ai_chgdodge_clip_l : afn_ai_chgdodge_clip_r;
            s_eChgDodgeFrames = s_eChgDodgeTotal = afn_ait_dodge_frames; s_aiChgDodgeCD = afn_ait_dodge_cd;
        }
    }
    if (s_eChgDodgeFrames > 0) {
        int total2 = s_eChgDodgeTotal, frames = s_eChgDodgeFrames;
        int ramp = afn_ait_dodge_ramp    >= 0 ? afn_ait_dodge_ramp    : (afn_dodge_ramp  > 0 ? afn_dodge_ramp  : 6);
        int fall = afn_ait_dodge_falloff >= 0 ? afn_ait_dodge_falloff : (afn_dodge_falloff > 0 ? afn_dodge_falloff : 6);
        if (ramp > total2) ramp = total2; if (fall > total2) fall = total2;
        float env = 1.0f;
        if (ramp > 0) { float t = (float)(total2 - frames + 1) / (float)ramp; if (t > 1.0f) t = 1.0f; if (t*t < env) env = t*t; }
        if (fall > 0) { float u = (float)frames / (float)fall; if (u > 1.0f) u = 1.0f; if (u*u < env) env = u*u; }
        int spd = afn_ait_dodge_speed >= 0 ? afn_ait_dodge_speed : (afn_dodge_speed > 0 ? afn_dodge_speed : 70);
        float sp = spd * 0.08f * env;
        float ix = s_eChgDodgeDX * sp, iz = s_eChgDodgeDZ * sp;
        int sub = (int)(sp / 3.0f) + 1;
        for (int st = 0; st < sub; st++) { s_npcX[i] += ix/sub; s_npcZ[i] += iz/sub; collide_walls(&s_npcX[i], &s_npcZ[i], s_npcY[i]); }
        s_npcClip[i] = s_eChgDodgeClip;   // charge-dodge anim overrides the static charge pose
        s_eChgDodgeFrames--;
    }
}

// FIRE: launch the projectile from the muzzle toward the player; start recovery.
void afn_ai_fire_beam(void) {
    int i = afn_ai_slot; if (i < 0) return;
    if (afn_enemy_frozen || afn_paused) return;   // cutscene freeze OR scene pause
    float px = (float)player_x, pz = (float)player_z;
    float mx, my, mz; enemy_muzzle(i, &mx, &my, &mz);
    float ddx = px - mx, ddz = pz - mz, dl = sqrtf(ddx*ddx + ddz*ddz); if (dl < 1e-3f) dl = 1.0f;
    s_efbDirX = ddx/dl; s_efbDirZ = ddz/dl;
    s_efbX = mx + s_efbDirX*8.0f; s_efbZ = mz + s_efbDirZ*8.0f; s_efbY = my;
    if (s_aiChargeShot) { s_efbDmg = ENEMY_CHG_DMG; s_efbSpeed = afn_ai_chg_speed_t / 10.0f; s_efbScale = afn_ai_orb_max; s_efbHoming = ENEMY_CHG_HOMING; s_ebBeamFull = 1; }
    else                { s_efbDmg = ENEMY_TAP_DMG; s_efbSpeed = afn_ai_tap_speed_t / 10.0f; s_efbScale = afn_ai_orb_min; s_efbHoming = 0.0f; s_ebBeamFull = 0; }   // tap = Min Scale%
    s_efbActive = 1; s_efbCharging = 0; s_efbLife = ENEMY_SHOT_LIFE;
    // Charged shot always charges to FULL, so it plays the big 'shoot' SFX (what the
    // player plays at >=95% charge), not midblast. Tap shots stay smallblast.
    afn_play_sfx_inst_gain(s_aiChargeShot ? afn_ai_sfx_shoot : afn_ai_sfx_tap,
                           afn_enemy_sfx_gain(afn_ai_dist));   // beam launch SFX (same as player)
    s_aiAtkCD = afn_ai_atkcd; s_aiTimer = afn_ait_fire_recover;
}

// FIRE recover: hold the launch clip; set afn_ai_fire_done when recovery elapses.
void afn_ai_fire_recover(void) {
    int i = afn_ai_slot; if (i < 0) return;
    s_npcClip[i] = afn_aic_launch;   // atk_spc_lnc (launch)
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
    // NOTE on pause: we run the FULL graph even while paused so per-frame HUD upkeep in the
    // update (lock-on reticle anchor, enemy HP bar afn_hpbar_*) keeps refreshing — otherwise
    // it lapses and the reticle falls to its corner. The SIM is frozen instead at the source:
    // the enemy AI steps + both projectile steps early-out on afn_paused, and the freeze block
    // (after script_tick) zeros every player action. So nodes still run, nothing moves.
    afn_emitted_script_update();
    afn_emitted_script_key_released();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_released();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
    // On Hit: fires for objects/player that took damage (flags computed by the
    // HP-drop detector just before this call). Last so it sees this frame's flags.
    afn_emitted_script_on_hit();
    afn_bp_dispatch_on_hit();
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

// Thick 2D line segment in HUD coords (arbitrary angle — hud_solid_quad is axis-aligned only).
static void hud_seg(float ax,float ay, float bx,float by, float hw, unsigned col){
    float dx=bx-ax, dy=by-ay; float l=sqrtf(dx*dx+dy*dy); if(l<0.001f)l=0.001f;
    float wx=-dy/l*hw, wy=dx/l*hw;
    AfnVertex q[4]={ {0,0,col,ax+wx,ay+wy,0},{0,0,col,ax-wx,ay-wy,0},{0,0,col,bx-wx,by-wy,0},{0,0,col,bx+wx,by+wy,0} };
    AfnVertex* v=q;
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4,GL_UNSIGNED_BYTE,sizeof(AfnVertex),&v->color);
    glVertexPointer(3,GL_FLOAT,sizeof(AfnVertex),&v->x);
    glDrawArrays(GL_TRIANGLE_FAN,0,4);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_TEXTURE_2D);
}
static void hud_ring2d(float cx,float cy,float r,float hw,unsigned col){
    float px2=cx+r, py2=cy;
    for(int j=1;j<=16;j++){ float a=(float)j/16.0f*6.2831853f;
        float nx=cx+cosf(a)*r, ny=cy+sinf(a)*r;
        hud_seg(px2,py2,nx,ny,hw,col); px2=nx; py2=ny; }
}
// PHYSICAL CLASH 2D cut-in (HARDCODED prototype): the beam clash's presentation language —
// fullscreen speed-line backdrop — but tinted RED, with impact flashes popping here and there
// in the background, and the pressure bar + button prompt drawn on top.
static void physclash_render_2d(void){
    const float W=960.0f, H=544.0f;
    float t=(float)s_pc_t;
    hud_solid_quad(0,0,W,H, MM_COL(30,4,8,240));                                   // deep-red base wash
    // BLUR STREAKS: bucketed motion-blur lines racing right->left across the backdrop — most thin
    // and faint, some medium, a few thick/long/bright; each streak is layered (wide dim halo +
    // brighter core + hot leading tip) so it reads as a blur, not a hard bar.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    for (int i2=0;i2<26;i2++){ unsigned h=(unsigned)(i2+1)*2654435761u ^ 0x85EBCA6Bu;
        float fy=(float)(h&0xFF)/255.0f, fb=(float)((h>>8)&0xFF)/255.0f, fp=(float)((h>>16)&0xFF)/255.0f;
        float len, th, sp; int aB;
        if      (fb < 0.60f){ len=140.0f+fb*220.0f; th=2.0f; sp=170.0f; aB=70;  }   // ~60% thin faint
        else if (fb < 0.88f){ len=260.0f+fb*240.0f; th=3.6f; sp=235.0f; aB=110; }   // ~28% medium
        else                { len=430.0f+fb*260.0f; th=6.2f; sp=310.0f; aB=165; }   // ~12% thick bright
        float y=fy*(H-40.0f)+20.0f;
        float x=W - fmodf(t*sp + fp*(W+len), W+len);                               // head sweeps right->left, wraps
        hud_solid_quad(x, y-th*1.7f, x+len, y+th*1.7f, MM_COL(255, 70, 40, aB/3));   // wide dim halo
        hud_solid_quad(x, y-th*0.6f, x+len, y+th*0.6f, MM_COL(255,140, 90, aB));     // core
        hud_solid_quad(x, y-th*0.5f, x+len*0.16f, y+th*0.5f, MM_COL(255,225,185, aB+45)); }   // hot leading tip
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // IMPACT FLASHES: white-hot starbursts popping at random spots in the background (additive).
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    for(int i2=0;i2<4;i2++){
        int Pi=34+i2*11;
        int ph=(s_pc_t+i2*17)%Pi;
        if(ph<16){ float f=1.0f-(float)ph/16.0f;
            unsigned hb=(unsigned)((s_pc_t+i2*17)/Pi+1)*2654435761u ^ (unsigned)((i2+1)*0x9E3779B9u);
            float ix=(float)(hb&0x3FF)/1023.0f*(W-160.0f)+80.0f, iy=(float)((hb>>10)&0x3FF)/1023.0f*(H-200.0f)+130.0f;
            float s2=26.0f+(float)((hb>>20)&0x3F);
            float g=s2*(1.6f-f*0.6f);                                              // expands as it fades
            int aC=(int)(230*f), aH=(int)(110*f);
            hud_solid_quad(ix-g,iy-g*0.26f,ix+g,iy+g*0.26f, MM_COL(255,110,55,aH));   // halo bars
            hud_solid_quad(ix-g*0.26f,iy-g,ix+g*0.26f,iy+g, MM_COL(255,110,55,aH));
            hud_seg(ix-g,iy, ix+g,iy, 2.2f, MM_COL(255,235,215,aC));                  // starburst arms
            hud_seg(ix,iy-g, ix,iy+g, 2.2f, MM_COL(255,235,215,aC));
            hud_seg(ix-g*0.55f,iy-g*0.55f, ix+g*0.55f,iy+g*0.55f, 1.6f, MM_COL(255,190,150,aC));
            hud_seg(ix-g*0.55f,iy+g*0.55f, ix+g*0.55f,iy-g*0.55f, 1.6f, MM_COL(255,190,150,aC));
            hud_solid_quad(ix-4,iy-4,ix+4,iy+4, MM_COL(255,255,255,aC)); }
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // PRESSURE BAR: tug-of-war from the centre — gold pushing the enemy (right), red losing (left).
    { float bx0=170.0f,bx1=790.0f, cy=96.0f, hh=12.0f; float mid=(bx0+bx1)*0.5f;
      hud_solid_quad(bx0-5,cy-hh-5,bx1+5,cy+hh+5, MM_COL(235,235,245,235));        // frame
      hud_solid_quad(bx0-2,cy-hh-2,bx1+2,cy+hh+2, MM_COL(22,16,26,248));           // back
      float dev=(s_pc_pressure-0.5f)*2.0f; float fw2=dev*(bx1-bx0)*0.5f;
      unsigned fc2 = dev>=0.0f ? MM_COL(255,205,70,255) : MM_COL(255,70,45,255);
      if (fw2>=0.0f) hud_solid_quad(mid,cy-hh,mid+fw2,cy+hh, fc2);
      else           hud_solid_quad(mid+fw2,cy-hh,mid,cy+hh, fc2);
      hud_solid_quad(mid-2,cy-hh-5,mid+2,cy+hh+5, MM_COL(255,255,255,255));        // centre tick
      hud_solid_quad(bx0-18,cy-hh,bx0-8,cy+hh, MM_COL(255,80,55,255));             // your cap (red)
      hud_solid_quad(bx1+8,cy-hh,bx1+18,cy+hh, MM_COL(255,210,80,255)); }          // enemy cap (gold)
    // BUTTON PROMPT: PSV-coloured shape mid-screen; grows + dims as its window expires; white
    // ring flash on a correct press.
    { float icx=480.0f, icy=232.0f;
      float wdw=(float)pc_window(); if(wdw<1.0f)wdw=1.0f;
      float urg=1.0f-(float)s_pc_cmdT/wdw; if(urg<0.0f)urg=0.0f; if(urg>1.0f)urg=1.0f;
      float sc=42.0f*(1.0f+0.06f*sinf(t*0.55f)+0.28f*urg);
      int ia=245-(int)(130*urg);
      if (s_pc_cmd==0){ unsigned c=MM_COL(95,150,255,ia);                          // CROSS (blue X)
          hud_seg(icx-sc*0.7071f,icy-sc*0.7071f, icx+sc*0.7071f,icy+sc*0.7071f, 8.0f, c);
          hud_seg(icx-sc*0.7071f,icy+sc*0.7071f, icx+sc*0.7071f,icy-sc*0.7071f, 8.0f, c); }
      else if (s_pc_cmd==1){                                                       // CIRCLE (red O)
          hud_ring2d(icx,icy, sc*0.9f, 7.5f, MM_COL(255,90,70,ia)); }
      else if (s_pc_cmd==2){ unsigned c=MM_COL(255,120,205,ia); float s2=sc*0.78f; // SQUARE (pink)
          hud_seg(icx-s2,icy-s2, icx+s2,icy-s2, 7.5f, c);
          hud_seg(icx-s2,icy+s2, icx+s2,icy+s2, 7.5f, c);
          hud_seg(icx-s2,icy-s2, icx-s2,icy+s2, 7.5f, c);
          hud_seg(icx+s2,icy-s2, icx+s2,icy+s2, 7.5f, c); }
      else { unsigned c=MM_COL(120,235,145,ia);                                    // TRIANGLE (green)
          float vTy=icy-sc, vBy=icy+sc*0.55f;
          hud_seg(icx,vTy, icx-sc*0.9f,vBy, 7.5f, c);
          hud_seg(icx-sc*0.9f,vBy, icx+sc*0.9f,vBy, 7.5f, c);
          hud_seg(icx+sc*0.9f,vBy, icx,vTy, 7.5f, c); }
      if (s_pc_flash>0){ float ff=(float)s_pc_flash/8.0f;
          glBlendFunc(GL_SRC_ALPHA, GL_ONE);
          hud_ring2d(icx,icy, sc*(1.35f+(1.0f-ff)*0.9f), 5.0f, MM_COL(255,255,255,(int)(210*ff)));
          glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    }
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

    // PHYSICAL CLASH (HARDCODED prototype): fullscreen RED cut-in (speed-line backdrop +
    // background impact flashes + pressure bar/prompt) UNDER the normal HUD elements.
    if (s_pc_active) physclash_render_2d();

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
    if (!afn_sfx_active_inst(AFN_SND_MASH)) afn_play_sfx_inst_gain(AFN_SND_MASH, 0);
    afn_set_sfx_pitch_inst(AFN_SND_MASH, 50 + (int)(afn_clash_balance * 100.0f));
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
    afn_hud_visible[AFN_PAUSE_ELEM] = 0; afn_paused = 0;   // pause overlay stays hidden until Start
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
    // ANALOG_WIDE + the Ext2 buffer surfaces L2/R2/L3/R3 (the base Vita maps these to the
    // REAR TOUCH PANEL); the plain buffer never reports them. Front buttons are unchanged.
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
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
        sceCtrlPeekBufferPositiveExt2(0, &pad, 1);   // Ext2 = includes L2/R2/L3/R3 (rear touch)
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
        // Paused: ignore EVERY input except the pause/resume key. afn_pause_key is captured by
        // the Toggle Pause node from whatever On Key Pressed drove it (Select, Start, ...), so the
        // resume key is node-configurable — not hardcoded here. With the full graph still running
        // for HUD upkeep, masking to that one key means no other node fires from a button — no
        // lock-on toggle, no ability/energy spend, no movement, no camera orbit — only the Toggle
        // Pause can resume. (Combined with the AI/projectile/anim gates below = total freeze.)
        if (afn_paused) { afn_keys_held &= afn_pause_key; afn_keys_pressed &= afn_pause_key; afn_keys_released &= afn_pause_key; }
#ifdef AFN_HAS_CAM_ANIM
        // Cutscene input freeze (Play Camera Anim "Freeze Input" pin): while the path is
        // ANIMATING (active + not done), mask EVERY button so no node fires — no ability,
        // lock-on, charge, dodge, or movement during the shot. Gated on !done so a Hold-Last
        // cut (e.g. the victory cam) releases input the instant the path completes, letting
        // the results menu take over. The cut ends/eases on its own timer.
        if (afn_cam_cut_active && !afn_cam_cut_done && afn_cam_cut_freeze_input) {
            afn_keys_held = 0; afn_keys_pressed = 0; afn_keys_released = 0;
        }
#endif
        // Lock Player Functions (game-over / menu screens): mask only the HELD keys so
        // On-Key-Held abilities (Charge Up's energy fill + aura, Focus Blast charge) never
        // run, while On-Key-Pressed menu navigation (cursor Up/Down, confirm) still fires.
        // Uses last frame's flag (the node sets it during script_tick); reset just before
        // the tick below, so it auto-clears the frame the node stops running.
        if (afn_lock_functions) afn_keys_held = 0;
        // Pause is NODE-DRIVEN now (Toggle Pause node, On Key Pressed(Start) in the BP): it
        // flips afn_paused + shows/hides the overlay + plays the SFX. The runtime just RESPECTS
        // afn_paused — script_tick runs only the key-pressed graph while paused, the freeze
        // block zeros input, the NPC physics loop is skipped, and rigs_render holds animation.
        // On Hit detector: flag the player / any object that LOST hp since last
        // frame, so the On Hit node fires for every damage source (node or
        // hardcoded combat). Read by the On Hit dispatcher inside script_tick().
        {
            static int s_hitInit = 0, s_prevHealth = 0, s_prevHp[NUM_SPRITES];
            if (!s_hitInit) { s_prevHealth = afn_health; for (int i = 0; i < NUM_SPRITES; i++) s_prevHp[i] = afn_hp[i]; s_hitInit = 1; }
            afn_any_hit = 0;
            if (afn_health < s_prevHealth) afn_any_hit = 1;
            s_prevHealth = afn_health;
            for (int i = 0; i < NUM_SPRITES; i++) {
                afn_obj_hit[i] = (afn_hp[i] < s_prevHp[i]) ? 1 : 0;
                if (afn_obj_hit[i]) afn_any_hit = 1;
                s_prevHp[i] = afn_hp[i];
            }
        }
        afn_charging_up = 0;   // ChargeUp node re-asserts this each frame it runs (held)
        afn_lock_functions = 0;   // Lock Player Functions node re-asserts each frame it runs (the held-mask above already consumed last frame's value)
        script_tick();   // runs even while paused (HUD upkeep); AI + projectiles self-gate on afn_paused, freeze block (below) zeros player actions
        // Lock Player Functions: clear every per-frame ability trigger the graph queued, so
        // pressed/released abilities (Dodge, Quick Attack, Focus Blast fire, Block) and the
        // charge aura don't fire on a menu screen. The HELD mask above stops the energy fill
        // at its source; this stops the rest. Runs BEFORE the charge-aura render below.
        if (afn_lock_functions) {
            afn_charging_up = 0;
            afn_fb_charge_req = 0; afn_fb_fire_req = 0;
            afn_qa_trigger = 0; afn_dodge_trigger = 0;
            afn_player_blocking = 0;
            // The charge SOUND is on On-Key-PRESSED (Down) — the SAME key the menu's
            // CursorDown uses — so the held-mask can't stop it without killing menu nav.
            // It's a LOOPING sfx, so silence the loops here (after the graph queued it,
            // before audio renders). One-shot nav beeps + music are untouched.
            afn_stop_looping_sfx();
        }
        // Charge aura: every HIDDEN attached-model mesh instance is shown while a
        // ChargeUp node ran this frame (button held), hidden otherwise.
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_SPRITE_START_HIDDEN)
        for (int csi = 0; csi < afn_sprite_count; csi++) {
            int ceix = afn_mesh_inst_sprite[csi];
            if (ceix >= 0 && ceix < NUM_SPRITES && afn_sprite_start_hidden[ceix])
                afn_sprite_visible[ceix] = afn_charging_up ? 1 : 0;
        }
#endif
#ifdef AFN_HAS_PLAYER_RIG
        // Cutscene / scripted freeze (Play Camera Anim or any Freeze Player node): hold
        // the player COMPLETELY still — kill movement, jump, dodge, Quick Attack, Focus
        // Blast, block and charge before the action blocks below consume them, so NO
        // ability can fire while frozen. Mirrors the charge lock-down below.
        if (afn_player_frozen || afn_paused) {
            afn_input_fwd = 0; afn_input_right = 0;        // no movement / strafe
            afn_qa_trigger = 0; afn_dodge_trigger = 0;     // no Quick Attack / Dodge
            player_vy = 0;                                 // no Jump
            afn_fb_charge_req = 0; afn_fb_fire_req = 0;     // no Focus Blast charge/fire
            afn_charging_up = 0;                           // no Charge Up
            afn_player_blocking = 0;                       // drop Block
            afn_qa_phase = 0; afn_qa_active = 0;            // cancel an in-progress Quick Attack (else its dash facing lingers)
            afn_dodge_frames = 0;                          // cancel an in-progress dodge
        }
#ifdef AFN_HAS_CAM_ANIM
        // Play Camera Anim "Snap Player": teleport the player to the scene-start pose the
        // camera path was authored around, so a cutscene triggered MID-FIGHT (e.g. the
        // victory cam) frames the player correctly instead of animating at the authored
        // spot while the player is somewhere else. Fires once at cut start.
        if (afn_cam_cut_snap) {
            // Snap spot: the wired Snap X/Z (world units) if given, else the scene spawn.
            // Y stays at the spawn ground (cutscene snaps are ground-level).
            playerX = afn_cam_cut_snap_has_pos ? (float)afn_cam_cut_snap_x : AFN_PLAYER_START_X;
            playerZ = afn_cam_cut_snap_has_pos ? (float)afn_cam_cut_snap_z : AFN_PLAYER_START_Z;
            playerY = AFN_PLAYER_START_Y;
            playerVY = 0.0f; grounded = 1;
            // Face the wired Face Angle (degrees) if given, else the SCENE-DEFAULT heading
            // the camera path was authored around — NOT the live gameplay orbit_angle (the
            // player's last 3rd-person camera yaw), which made the victory cam show the
            // player's BACK when you won facing away.
            float snapFaceRad = afn_cam_cut_snap_has_face
                ? (float)afn_cam_cut_snap_face_deg * (3.14159265f / 180.0f)
                : afn_cam_slots[0][0];
            afn_active_camera = 0;
            orbit_angle = (int)(snapFaceRad * (65536.0f / 6.2831853f));
            afn_player_heading = orbit_angle;
            afn_cam_lock_target = -1; afn_lock_strafe = 0; afn_tank_camera = 0;   // clear lock-on/tank so rotation matches the authored spawn pose
            afn_qa_phase = 0; afn_qa_active = 0; afn_dodge_frames = 0;             // cancel any in-progress Quick Attack / dodge that would hold the facing
            afn_cam_cut_face_lock = 1;          // hold this facing for the whole cut (heading alone re-syncs to the rotated gameplay orbit)
            afn_cam_reinit = 1;                 // re-seed the follow cam + orbit accumulator to the snapped pose
            afn_cam_cut_snap = 0;
        }
#endif
        // Charging locks the player down: stand still, no other actions, hold the
        // charge pose. afn_charging_up is set by the ChargeUp node (D-pad Down held);
        // we cancel everything the node graph queued THIS frame before the movement/
        // action blocks below consume it, so the button does nothing but charge.
        if (afn_charging_up) {
            if (player_vy != 0) {
                // Jump CANCELS the charge: a Jump mid-charge drops the charge and lets
                // the jump through (don't eat the input + leave a dead, energy-draining
                // press). Suppress only the Focus Blast so the same press can't also fire.
                afn_charging_up = 0;
                afn_fb_charge_req = 0; afn_fb_fire_req = 0;
            } else {
                afn_input_fwd = 0; afn_input_right = 0;        // stand still (kills movement + IsMoving + strafe anim)
                afn_qa_trigger = 0; afn_dodge_trigger = 0;     // no Quick Attack / Dodge
                player_vy = 0;                                 // no Jump this frame
                afn_fb_charge_req = 0; afn_fb_fire_req = 0;     // no Focus Blast charge/fire
                if (afn_charge_clip >= 0) afn_rig_clip = afn_charge_clip;   // play the charge anim
            }
        }
        // No blocking in the air: the Block node sets afn_player_blocking (and it
        // persists across frames), so force it off whenever the player is airborne.
        if (!player_on_ground) afn_player_blocking = 0;
#endif
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
            playerVY = 0.0f; grounded = 1;
            // Reset the camera orbit + tank heading to the SCENE DEFAULT, NOT the stale
            // gameplay orbit_angle the player last rotated the camera to. orbit_angle /
            // afn_player_heading are only seeded from the default at BOOT; across a restart
            // they persist, so the old code's `afn_player_heading = orbit_angle` baked in
            // the last camera yaw -> in tank mode the intro player faced wherever you'd
            // rotated. Seed both from the default slot so restart == fresh boot.
            afn_active_camera = 0;
            orbit_angle = (int)(afn_cam_slots[0][0] * (65536.0f / 6.2831853f));
            afn_player_heading = orbit_angle;
            afn_cam_reinit = 1;   // re-seed follow-cam eye + clear stale orbit accumulator / facing statics
            afn_cam_lock_target = -1; afn_lock_strafe = 0; afn_tank_camera = 0;   // OnStart re-establishes
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
            s_eqaPhase = 0; s_eqaCD = 0; s_eJumpCD = 0; s_ePrevPlayerQA = 0;   // clear melee reflexes
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
            afn_stop_sfx_inst(AFN_SND_STRUGGLE);   // never leave the struggle/mash loops ringing across a swap
            afn_stop_sfx_inst(AFN_SND_MASH);
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
        // Slot Yaw as a RELATIVE offset from the scene-default camera (slot 0). While
        // locked-on the lock assist owns yaw (it faces the target), so a Set Camera's
        // absolute yaw never applied — switching after orbiting looked inconsistent
        // (a 180 orbit flipped it). Feeding this offset into the lock-facing instead
        // makes a slot rotate the framing by its authored angle RELATIVE to the
        // current facing, consistently regardless of orbit. Slot 0 -> 0 (faces target).
        s_camLookYaw += (S[5] - s_camLookYaw) * 0.125f;     // ease the AIM pan toward the active slot's lookYaw
        s_camHOffset += (S[7] - s_camHOffset) * 0.125f;     // ease the lateral framing offset toward the slot's hOffset
        s_camDepth   += (S[8] - s_camDepth)   * 0.125f;     // ease the forward/back dolly toward the slot's depthOffset
        s_camLookPitch += (S[9] - s_camLookPitch) * 0.125f; // ease the AIM tilt toward the active slot's lookPitch
        s_camVOffset += (S[10] - s_camVOffset) * 0.125f;    // ease the vertical framing offset toward the slot's vOffset
        float camDistTgt   = S[1] > 1.0f ? S[1] : camDist;  // keep manual zoom unless slot overrides
        float camHeightTgt = S[2];
        camDist   += (camDistTgt   - camDist)   * 0.125f;
        camHeight += (camHeightTgt - camHeight) * 0.125f;
        {
            static int s_prevCamSlot = 0, s_camYawEasing = 0, s_prevLock = -1;
            if (afn_active_camera != s_prevCamSlot) { s_prevCamSlot = afn_active_camera; s_camYawEasing = 1; }
            // Just UNLOCKED (lock target -> -1): re-ease the slot's yaw/pitch so the
            // camera resets to the slot's framing — otherwise it stays at the lock
            // assist's pitch (reads as a tilt). Absolute target = stable, converges.
            if (s_prevLock >= 0 && afn_cam_lock_target < 0) s_camYawEasing = 1;
            s_prevLock = afn_cam_lock_target;
            // While locked on, the lock assist (below) owns yaw + pitch — it eases the
            // orbit to frame the target every frame; a Set Camera only blends distance/
            // height (zoom), no yaw snap.
            if (afn_cam_lock_target >= 0) s_camYawEasing = 0;
            if (s_camYawEasing) {
                // KEEP the current orbit YAW on a switch / unlock — re-house in the
                // direction you're already looking, don't flip to the slot's world
                // angle. Only re-settle the PITCH (the lock assist took it over while
                // locked). Slot pitch in column 4 (deg); 0 = auto from height/distance.
                float slotPitchDeg = S[4];
                int pitchTgt;
                if (slotPitchDeg != 0.0f)
                    pitchTgt = (int)(slotPitchDeg * (65536.0f / 360.0f));
                else
                    pitchTgt = (int)(atan2f(camHeightTgt > 0.0f ? camHeightTgt : 8.0f, camDistTgt) * (65536.0f / 6.2831853f));
                int pd = pitchTgt - orbit_pitch;
                orbit_pitch += pd >> 3;
                if (pd > -300 && pd < 300) { orbit_pitch = pitchTgt; s_camYawEasing = 0; }
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
                    // Slot 0 (the usual/default camera) AND Lock-On Aware slots (col 6)
                    // face the lock TARGET — the normal lock-on. A non-aware preset
                    // holds its own absolute authored yaw instead.
                    int slotLockAware = (afn_active_camera == 0) || (S[6] != 0.0f);
                    if (slotLockAware) {
                        // Aware (incl. slot 0 / usual lock): ease the yaw to FACE the target.
                        float desired = atan2f(ldx, ldz);
                        float cur = orbit_angle * (6.2831853f / 65536.0f);
                        float diff = desired - cur;
                        while (diff >  3.14159265f) diff -= 6.2831853f;
                        while (diff < -3.14159265f) diff += 6.2831853f;
                        orbit_angle = (int)(uint16_t)(orbit_angle
                                     + (int)(diff * (65536.0f / 6.2831853f) * 0.10f));   // ease toward
                    }
                    // Non-aware slot: keep the current yaw (don't lock-track) — no flip.
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
                    if (!afn_thunder_charging)   // Thunder charge owns the pitch (see the override after the clamp)
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
            // Scene (re)start: snap the accumulator to the freshly-reset orbit_angle so the
            // MAX_DELTA clamp below doesn't slow-swing the camera from the stale gameplay yaw.
            if (afn_cam_reinit) { s_prevOrbit = orbit_angle; s_prevOrbitP = orbit_pitch; }
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

        // Thunder cinematic up-tilt (LOCKED-only): the FINAL say on pitch this frame, so nothing
        // else fights it. Eases toward the up-angle (smooth-in) then SNAPS + holds — no creep.
        if (afn_thunder_charging && afn_thunder_cam_pitch != 0) {
            int thLocked = 0;
#if defined(AFN_HAS_SPRITE_IDX) && defined(AFN_HAS_CAM_LOCK)
            thLocked = (afn_cam_lock_target >= 0);
#endif
            if (thLocked) {
                int tgt = -(int)(afn_thunder_cam_pitch * (65536.0f/360.0f));   // negative = look up
                float sm = afn_thunder_cam_smooth; if (sm < 0.005f) sm = 0.005f; if (sm > 1.0f) sm = 1.0f;
                int d = tgt - orbit_pitch, step = (int)((float)d * sm);
                if (step == 0) step = (d > 0) ? 1 : (d < 0 ? -1 : 0);   // guarantee arrival (no asymptote)
                orbit_pitch += step;
                if ((d > 0 && orbit_pitch > tgt) || (d < 0 && orbit_pitch < tgt)) orbit_pitch = tgt;   // arrive + hold
            }
        }

        // Camera yaw + pitch (radians) from the node/manual orbit_angle/pitch (brad).
        float camAngle = orbit_angle * (6.2831853f / 65536.0f);
        float pitch    = orbit_pitch * (6.2831853f / 65536.0f);

        // --- Player movement: reads the node-set move intent (afn_input_fwd/right,
        //     256 = full) in camera space, scaled by afn_move_speed (or the walk
        //     default when no script). Ported from PSP scene_update. ---
        // 8-Way Stick node: snap the analog move vector to the nearest 45 deg
        // octant (magnitude preserved) so movement AND the Strafe Anim clip pick
        // read a crisp 8-way direction instead of fighting analog drift.
        // Thunder charge: the left stick aims (not move), so drop the move intent — the player
        // stands IDLE (no walk anim / facing churn) instead of acting on the stick.
        if (afn_thunder_charging) { afn_input_fwd = 0; afn_input_right = 0; }
        // Electroweb snare: stepping into a live web immobilises the player for ~2s (rising edge).
        if (s_ew_active) {
            float ex = playerX - s_ew_x, ez = playerZ - s_ew_z; int inside = (ex*ex + ez*ez) < s_ew_r*s_ew_r;
            if (inside && !s_ew_inPrev) afn_player_stuck = 120;
            s_ew_inPrev = inside;
        }
        if (afn_player_stuck > 0) { afn_input_fwd = 0; afn_input_right = 0; afn_player_stuck--; }
        if (s_su_active) { afn_input_fwd = 0; afn_input_right = 0; }   // Surf: the wave carries the player (no walk)
        if (s_fw_active) { afn_input_fwd = 0; afn_input_right = 0; }   // Flame Wheel: the wheel carries the player (no walk)
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
        // Camera-relative movement follows the camera's AIM (camAngle + the slot's
        // H Rotation pan), so "up" is always away from the camera ON SCREEN. Using
        // the bare camAngle let any leftover pan walk the player diagonally.
        float moveAngle = lockStrafing ? lockAngle
                        : (afn_tank_camera && afn_tank_move)
                          ? (afn_player_heading * (6.2831853f/65536.0f)) : (camAngle + s_camLookYaw);
        float fwdX = sinf(moveAngle), fwdZ = cosf(moveAngle);
        float rgtX = cosf(moveAngle), rgtZ = -sinf(moveAngle);
        float mvX = fAmt*fwdX + rAmt*rgtX;
        float mvZ = fAmt*fwdZ + rAmt*rgtZ;
        // Move only when the node graph asks: a movement node sets afn_input_fwd/
        // right AND afn_move_speed. No walk-speed fallback — purely node-driven.
        int facedByMove = 0;
        static float sTankRelFace = 0.0f;   // tank drive: facing offset from the heading (deg)
        // Scene (re)start: drop the stale strafe-relative facing so a cutscene (e.g. the
        // intro) faces the clean spawn pose instead of wherever the player last turned/
        // strafed. Without this the player model came up rotated on restart. afn_cam_reinit
        // is still set here (the camera-eye block clears it later this frame).
        if (afn_cam_reinit) sTankRelFace = 0.0f;
        if ((mvX*mvX + mvZ*mvZ > 0.0001f) && afn_move_speed > 0 && !afn_player_frozen && afn_qa_phase == 0 && !afn_thunder_charging) {
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
        // Thunder charge: force the idle clip so a walk clip selected just before charging
        // doesn't keep looping in place (zeroing the move intent stops RE-selection but the
        // last-selected clip would otherwise hold and keep animating).
        if (afn_thunder_charging) afn_rig_clip = AFN_PLAYER_DEFAULT_CLIP;
        // Tank heading owns the facing whenever camera-relative Direction
        // Facing isn't steering it. In tank drive the persisted relative
        // facing (sTankRelFace) rides on top — so after walking "down" the
        // model stays turned around at idle (no release snap) and TurnPlayer
        // visibly rotates the whole frame. When a camera-relative walk ENDS,
        // bake its final facing into the heading (no snap there either).
        static int sWasFacedByMove = 0;
        if (afn_cam_reinit) sWasFacedByMove = 0;   // (re)start: don't bake a stale move-facing into the heading
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

        // Quick Attack (dash-in melee). Triggered by the Quick Attack node. Phase 1
        // (dash): lunge toward the captured lock target (or straight forward) with a
        // quick ease-in, wall-collide, and on reaching Stop Range deal damage once +
        // punch the camera. A whiff overshoots until the Max Frames budget runs out.
        // Phase 2 (skid): a short decelerating slide that holds the skid pose, then
        // hands the rig back. Normal movement is suppressed while phase != 0.
#if defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
        afn_qa_hit = 0;   // 1 only on the frame contact lands (Quick Attack Hit gate)
        afn_qa_started = 0;   // 1 only on the frame the dash begins (Quick Attack Started gate)
        // Wild Charge is node-driven now (Play Effect -> kind=4 layer -> afn_qa_trigger + afn_wild_charge).
        if (afn_qa_trigger && afn_qa_phase == 0 && afn_dodge_frames <= 0 && !afn_player_frozen) {
            float dirx = 0.0f, dirz = 1.0f; int haveDir = 0;
            if (afn_qa_tgt >= 0) {
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_qa_tgt) {
                        float tx = s_npcX[n] - playerX, tz = s_npcZ[n] - playerZ;
                        float l = sqrtf(tx*tx + tz*tz);
                        if (l > 0.001f) { dirx = tx/l; dirz = tz/l; haveDir = 1; }
                        break;
                    }
            }
            if (!haveDir) { float fy = playerYaw * (3.14159265f/180.0f); dirx = sinf(fy); dirz = cosf(fy); }  // no target -> forward
            s_qaDirX = dirx; s_qaDirZ = dirz;
            s_qaYaw  = atan2f(dirx, dirz) * (180.0f/3.14159265f);   // face the lunge
            afn_qa_phase = 1; afn_qa_frames = afn_qa_max; s_qaTotal = afn_qa_max;
            s_qaDealt = 0; afn_qa_active = 1; afn_qa_started = 1;   // dash begins this frame
        }
        afn_qa_trigger = 0;
        if (afn_qa_phase == 1 && !afn_player_frozen) {
            float sp = afn_qa_speed * 0.08f;
            int ramp = 4; if (ramp > s_qaTotal) ramp = s_qaTotal;   // brief ease-in out of the windup
            if (ramp > 0) { float t = (float)(s_qaTotal - afn_qa_frames + 1) / (float)ramp; if (t > 1.0f) t = 1.0f; sp *= t*t; }
            float ix = s_qaDirX*sp, iz = s_qaDirZ*sp;
            int sub = (int)(sp/3.0f) + 1;
            for (int st = 0; st < sub; st++) { playerX += ix/sub; playerZ += iz/sub; collide_walls(&playerX, &playerZ, playerY); }
            playerYaw = s_qaYaw;
            if (afn_qa_clip_lunge >= 0) afn_rig_clip = afn_qa_clip_lunge;
            if (afn_qa_tgt >= 0 && !s_qaDealt) {   // contact check vs the target
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_qa_tgt) {
                        float dx = s_npcX[n] - playerX, dz = s_npcZ[n] - playerZ;
                        // Contact is 2D (X/Z); an airborne enemy (jump-evade) has hopped
                        // over the dash — the lunge whiffs under it. Grounded only.
                        // MUTUAL DASH: if the enemy is dashing too (or just was — grace),
                        // don't deal damage / skid; let the physical clash trigger instead.
                        if (s_npcGround[n] && s_pc_eG == 0 && dx*dx + dz*dz <= (float)afn_qa_range*afn_qa_range) {
                            int dmg = afn_ai_blocking ? (afn_qa_dmg * afn_block_pct) / 100 : afn_qa_dmg;   // enemy Block reduces it
                            if (afn_qa_tgt < NUM_SPRITES) { afn_hp[afn_qa_tgt] -= dmg; if (afn_hp[afn_qa_tgt] < 0) afn_hp[afn_qa_tgt] = 0; }
                            s_qaDealt = 1; afn_qa_hit = 1;
                            s_qaCamPunch = afn_qa_punch * 0.01f; if (s_qaCamPunch > 0.6f) s_qaCamPunch = 0.6f;   // camera punch-in
                            afn_qa_phase = 2; afn_qa_frames = afn_qa_skid;   // -> skid
                        }
                        break;
                    }
            }
            if (afn_qa_phase == 1 && --afn_qa_frames <= 0) { afn_qa_phase = 2; afn_qa_frames = afn_qa_skid; }   // whiff -> skid
        } else if (afn_qa_phase == 2 && !afn_player_frozen) {
            float dec = (afn_qa_skid > 0) ? (float)afn_qa_frames / (float)afn_qa_skid : 0.0f;   // decelerating slide
            float sp = afn_qa_speed * 0.08f * dec * 0.5f;
            float ix = s_qaDirX*sp, iz = s_qaDirZ*sp;
            int sub = (int)(sp/3.0f) + 1;
            for (int st = 0; st < sub; st++) { playerX += ix/sub; playerZ += iz/sub; collide_walls(&playerX, &playerZ, playerY); }
            playerYaw = s_qaYaw;
            if (afn_qa_clip_skid >= 0) afn_rig_clip = afn_qa_clip_skid;
            if (--afn_qa_frames <= 0) {
                afn_qa_phase = 0; afn_qa_active = 0;
                if (afn_qa_clip_idle >= 0 && afn_input_fwd == 0 && afn_input_right == 0) afn_rig_clip = afn_qa_clip_idle;
            }
        }
        if (afn_qa_cd > 0) afn_qa_cd--;   // spam-gate countdown
        if (s_qaCamPunch > 0.001f) s_qaCamPunch -= s_qaCamPunch * 0.12f; else s_qaCamPunch = 0.0f;   // ease the punch back out
        // Wild Charge visuals: crackle short BLUE electric bolts around the player each dash frame
        // (via the beam pool). The yellow trail is handled in rigs_render's afterimage.
        if (afn_wild_charge) {
            if (afn_qa_active && !afn_player_frozen) {
                static unsigned wcs = 0x1234567u;
                for (int k = 0; k < 2; k++) {
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float a1=(float)(wcs&0xFFFF)/65536.0f*6.2831853f;
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float r1=1.0f+(float)(wcs&0xFF)/255.0f*3.5f;
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float h1=(float)(wcs&0xFF)/255.0f*11.0f;
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float a2=(float)(wcs&0xFFFF)/65536.0f*6.2831853f;
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float r2=1.0f+(float)(wcs&0xFF)/255.0f*3.5f;
                    wcs^=wcs<<13; wcs^=wcs>>17; wcs^=wcs<<5; float h2=(float)(wcs&0xFF)/255.0f*11.0f;
                    afn_beam_width=0.22f; afn_beam_bow=0.0f; afn_beam_jitter=1.5f; afn_beam_segs=8;
                    afn_beam_decay=1.0f; afn_beam_pulse=0.0f; afn_beam_life=4; afn_beam_travel=0;
                    afn_beam_col=0xFFFFC878u; afn_beam_filaments=2; afn_beam_orb=0.0f;   // light blue (0xAABBGGRR: B=FF,G=C8,R=78)
                    afn_beam_cast(playerX+cosf(a1)*r1, playerY+h1, playerZ+sinf(a1)*r1,
                                  playerX+cosf(a2)*r2, playerY+h2, playerZ+sinf(a2)*r2);
                }
            } else { afn_wild_charge = 0; }   // dash ended -> stop the sparks/trail
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
        // SURF RIDE: while a wave is sweeping, the player rides just behind its crest — carried
        // forward with the front + lifted onto the water (overrides gravity/floor for the ride).
        if (s_su_active) {
            float st=(float)(SU_MAXLIFE - s_su_life)/(float)SU_MAXLIFE;
            float rideD=afn_surf_front(st) - 3.0f;                                   // sit just behind the crest
            playerX = s_su_ox + s_su_fx*rideD; playerZ = s_su_oz + s_su_fz*rideD;
            float riseIn=(st<0.15f)?st/0.15f:1.0f, fallOut=(st>0.85f)?(1.0f-st)/0.15f:1.0f;
            playerY = s_su_oy + 5.0f*riseIn*fallOut;                                 // ride on the wave, settle at the end
            playerVY = 0.0f; grounded = 1; playerYaw = s_su_yaw;                     // face the sweep, no gravity
        }
        // FLAME WHEEL RIDE: the wheel rolls forward and carries the player within it, straight along
        // the fired direction (no steering) — translate forward each frame, face the roll.
        if (s_fw_active) {
            playerX += s_fw_fx * FW_SPEED; playerZ += s_fw_fz * FW_SPEED;
            playerYaw = s_fw_yaw;
        }
#ifdef AFN_HAS_PLAYER_RIG
        // PHYSICAL CLASH (HARDCODED prototype): sense the player's Quick Attack meeting the enemy's
        // dash-in head-on; lock both in place and run the pressure struggle (prompts + AI cadence).
        if (s_pc_cd > 0) s_pc_cd--;
        if (afn_qa_phase == 1) s_pc_pG = 12; else if (s_pc_pG > 0) s_pc_pG--;   // refresh/bleed the grace windows
        if (s_eqaPhase == 1)   s_pc_eG = 12; else if (s_pc_eG > 0) s_pc_eG--;
        if (!s_pc_active && s_pc_cd == 0 && s_pc_pG > 0 && s_pc_eG > 0 && afn_hp[AFN_ENEMY_EIDX] > 0) {
            int eN = pc_enemy_npc();
            if (eN >= 0) {
                float dx = s_npcX[eN]-playerX, dz = s_npcZ[eN]-playerZ;
                if (dx*dx + dz*dz <= PC_MEET_R*PC_MEET_R) {
                    float d = sqrtf(dx*dx + dz*dz); if (d < 1e-3f) d = 1.0f;
                    s_pc_dirx = dx/d; s_pc_dirz = dz/d;
                    afn_qa_phase = 0; afn_qa_active = 0; afn_qa_trigger = 0; afn_qa_frames = 0;   // cancel both dashes
                    afn_wild_charge = 0;
                    s_eqaPhase = 0; s_eqaCD = afn_eqa_cd; s_eqaDealt = 1;
                    s_pc_pxl = playerX; s_pc_pzl = playerZ; s_pc_exl = s_npcX[eN]; s_pc_ezl = s_npcZ[eN];
                    s_pc_mx = (playerX+s_npcX[eN])*0.5f; s_pc_my = playerY + 16.0f; s_pc_mz = (playerZ+s_npcZ[eN])*0.5f;
                    s_pc_pressure = 0.5f; s_pc_t = 0; s_pc_knock = 0; s_pc_flash = 0;
                    s_pc_cmd = (int)(pc_rand()*3.999f); s_pc_cmdT = pc_window(); s_pc_aiT = pc_ai_wait();
                    s_pc_active = 1;
                    afn_clash_suppress_beams();   // kill any in-flight projectiles (both sides)
#ifdef AFN_SND_CLASH
                    afn_play_sfx_inst_gain(AFN_SND_CLASH, 256);
#endif
                }
            }
        }
        if (s_pc_active) {
            int eN = pc_enemy_npc();
            // pin both fighters nose-to-nose + freeze the player's movement/abilities (nav keys still
            // read); keep both sides' projectiles dead so nothing chips anyone mid-QTE.
            afn_player_frozen = 1; afn_lock_functions = 1;
            afn_clash_suppress_beams();
            playerX = s_pc_pxl; playerZ = s_pc_pzl; playerVY = 0.0f;
            playerYaw = atan2f(s_pc_dirx, s_pc_dirz) * (180.0f/3.14159265f);
            if (eN >= 0) { s_npcX[eN]=s_pc_exl; s_npcZ[eN]=s_pc_ezl;
                s_npcYaw[eN] = atan2f(-s_pc_dirx, -s_pc_dirz) * (180.0f/3.14159265f);
                s_npcClip[eN] = afn_aic_lunge; }
            if (!afn_paused) {
                s_pc_t++;
                if (s_pc_flash > 0) s_pc_flash--;
                // player prompt: correct button shoves toward the enemy; wrong one bleeds back
                static const unsigned pcKeys[4] = { KEY_A, KEY_B, KEY_X, KEY_Y };   // Cross/Circle/Square/Triangle
                if (key_hit(pcKeys[s_pc_cmd])) {
                    s_pc_pressure += PC_PUSH; s_pc_flash = 8;
                    s_pc_cmd = (int)(pc_rand()*3.999f); s_pc_cmdT = pc_window();
                } else {
                    for (int k2=0;k2<4;k2++) if (k2!=s_pc_cmd && key_hit(pcKeys[k2])) { s_pc_pressure -= PC_MISS; break; }
                }
                if (--s_pc_cmdT <= 0) { s_pc_cmd = (int)(pc_rand()*3.999f); s_pc_cmdT = pc_window(); }   // window missed -> new prompt
                // AI shove cadence (also tightens as the meter leaves the middle)
                if (--s_pc_aiT <= 0) { s_pc_pressure -= PC_AI_PUSH*(0.8f+pc_rand()*0.4f); s_pc_aiT = pc_ai_wait(); }
                // resolve: NO timeout — the struggle runs until one side's meter overflows
                int done=0, won=0;
                if      (s_pc_pressure >= 1.0f) { done=1; won=1; }
                else if (s_pc_pressure <= 0.0f) { done=1; won=0; }
                if (done) {
                    s_pc_active = 0; s_pc_cd = PC_CD; s_pc_won = won; s_pc_knock = 14;
                    afn_player_frozen = 0;
                    if (won) { afn_hp[AFN_ENEMY_EIDX] -= PC_DMG_E; if (afn_hp[AFN_ENEMY_EIDX] < 0) afn_hp[AFN_ENEMY_EIDX] = 0;
                               if (eN >= 0) { s_npcVY[eN] = 1.1f; s_npcGround[eN] = 0; } }
                    else     { afn_health -= PC_DMG_P; if (afn_health < 0) afn_health = 0; playerVY = 1.1f; }
#ifdef AFN_SND_WIN_CLASH
                    afn_play_sfx_inst_gain(AFN_SND_WIN_CLASH, 256);
#endif
                }
            }
        }
        if (s_pc_knock > 0) {                                                    // loser is shoved back
            s_pc_knock--;
            float sp = 1.7f * ((float)s_pc_knock/14.0f);
            if (s_pc_won) { int eN = pc_enemy_npc();
                if (eN >= 0) { s_npcX[eN] += s_pc_dirx*sp; s_npcZ[eN] += s_pc_dirz*sp; collide_walls(&s_npcX[eN], &s_npcZ[eN], s_npcY[eN]); } }
            else { playerX -= s_pc_dirx*sp; playerZ -= s_pc_dirz*sp; }
        }
#endif
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
                if (afn_paused) continue;   // pause: freeze NPC physics (gravity/nav/landing) too — script_tick gating alone misses this loop
                // Enemy combat AI is NODE-DRIVEN now (enemy BP: On Update -> Ai Sense +
                // state dispatch). It runs in script_tick(), not here.
                // True box: half-extents (hx,hy,hz) + center offset (cx,cy,cz),
                // all in world px relative to the NPC origin. Y rests the box
                // bottom (cy-hy) on the floor.
                float hx = afn_npc_col[i][0], hy = afn_npc_col[i][1], hz = afn_npc_col[i][2];
                float cx = afn_npc_col[i][3], cy = afn_npc_col[i][4], cz = afn_npc_col[i][5];
                float nbottom = cy - hy;

                // Melee reflex (enemy Quick Attack + jump-evade) is NODE-DRIVEN now:
                // the enemy BP's On Update chain calls the Ai Quick Attack node (after
                // its movement/state nodes) inside script_tick(), which ran above. It
                // still lands after the BP move and before the gravity integrator below,
                // so the dash overrides motion and a jump-evade launch arcs this frame.
                // See afn_ai_quick_attack() / afn_ai_melee_reflex().

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
        // Scene (re)start: drop the init flag so the eye/pitch/Y statics below SNAP to the
        // player's spawn pose this frame instead of easing in from the previous run's stale
        // camera (which read as a weird rotate, especially into the intro cutscene).
        if (afn_cam_reinit) { s_camEyeInit = 0; afn_cam_reinit = 0; }

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

        // Quick Attack camera punch-in: snap the camera closer on the smack frame
        // (s_qaCamPunch set there) and ease it back out over the skid for impact.
        if (s_qaCamPunch > 0.001f) effDist *= (1.0f - s_qaCamPunch);

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
        // Camera AIM rotation (slot lookYaw / lookPitch): rotate ONLY the look
        // direction, leaving the eye at the orbit position, so the subject sits
        // off-center (pan) and higher/lower in frame (tilt). aimPitch subtracts
        // s_camLookPitch so + = tilt UP (pitch convention: + = look down).
        float aimAngle = camAngle + s_camLookYaw;
        float aimPitch = pitch - s_camLookPitch;
        float cpA = cosf(aimPitch);
        float fwdVX = sinf(aimAngle) * cpA;
        float fwdVY = -sinf(aimPitch);
        float fwdVZ = cosf(aimAngle) * cpA;
        // H Offset: slide the camera sideways along its right axis. Depth Offset:
        // dolly straight toward/away the PLAYER (not the aim angle) so the zoom is
        // ALWAYS along the camera->player axis — consistent in/out regardless of the
        // slot's orbit angle or pan. Both translate the eye + look point together.
        float dpx = playerX - ex, dpz = playerZ - ez;
        float dpl = sqrtf(dpx*dpx + dpz*dpz); if (dpl < 1e-3f) dpl = 1.0f; dpx /= dpl; dpz /= dpl;
        // H Offset slides the eye along the FIXED orbit-frame right axis (camAngle),
        // NOT the panned aim (aimAngle). Using the aim made the offset eye sweep
        // around as H Rotation panned — H Rotation then read as an orbit. In the
        // orbit frame the offset is a static lateral slide and H Rotation pivots the
        // aim ON THE SPOT around it. Depth still dollies straight toward the player.
        float ofx = cosf(camAngle) * s_camHOffset + dpx * s_camDepth;
        float ofz = -sinf(camAngle) * s_camHOffset + dpz * s_camDepth;
        // V Offset: slide the eye AND look point up/down in world Y by the same amount,
        // so the whole shot translates vertically without tilting the aim.
        float ofy = s_camVOffset;
#ifdef AFN_HAS_CAM_ANIM
        // Cutscene takeover (Play Camera Anim node): advance the path playhead, then
        // feed the scripted eye + look straight into look_at, bypassing the orbit.
        if (afn_cam_cut_active) {
            int an = afn_cam_cut_anim; if (an < 0 || an >= AFN_CAM_ANIM_COUNT) an = 0;
            int cnt = afn_cam_anim_count[an];
            int last = cnt > 0 ? afn_cam_anim_kf[afn_cam_anim_start[an] + cnt - 1].frame : 0;
            // Base frames-per-tick from the path fps (step/speed), warped by the
            // current segment's per-keyframe speed multiplier.
            float fpt = (afn_cam_anim_speed[an] > 0)
                        ? ((float)afn_cam_anim_step[an] / (float)afn_cam_anim_speed[an]) : 1.0f;
            if (!afn_cam_anim_smooth[an]) fpt *= afn_cam_seg_speed(an, afn_cam_cut_fframe);   // smooth path ignores per-keyframe Speed
            afn_cam_cut_fframe += fpt;
            if (afn_cam_cut_fframe >= (float)last) {
                if (afn_cam_cut_loop) { while (last > 0 && afn_cam_cut_fframe >= (float)last) afn_cam_cut_fframe -= (float)last; }
                else { afn_cam_cut_fframe = (float)last; afn_cam_cut_done = 1;
                       if (!afn_cam_cut_hold) { afn_cam_cut_active = 0; afn_player_frozen = 0; afn_enemy_frozen = 0; afn_cam_cut_face_lock = 0; } }
            }
            afn_cam_cut_frame = (int)afn_cam_cut_fframe;
#ifdef AFN_HAS_PLAYER_RIG
            // Player Clip pin: force the player rig onto the scripted clip (e.g. a 'winner'
            // pose) for the whole cut. This runs AFTER script_tick + the locomotion blocks
            // and BEFORE rigs_render (below), so it overrides the idle/move state machine
            // and the blueprint's idle anim until the path ends, then play resumes.
            if (afn_cam_cut_player_clip >= 0) afn_rig_clip = afn_cam_cut_player_clip;
            // Snap facing lock: hold the player at the snapped facing for the whole cut,
            // overriding the tank heading (which re-syncs to the rotated gameplay orbit each
            // frame and otherwise spun the player's back to the victory cam). Uses the wired
            // Face Angle if given, else the scene-default facing.
            if (afn_cam_cut_face_lock)
                playerYaw = afn_cam_cut_snap_has_face
                    ? (float)afn_cam_cut_snap_face_deg
                    : afn_cam_slots[0][0] * (180.0f / 3.14159265f);
#endif
            float cex, cey, cez, cfx, cfy, cfz;
            afn_cam_anim_sample(an, afn_cam_cut_fframe, &cex, &cey, &cez, &cfx, &cfy, &cfz);
            ex = cex; ey = cey; ez = cez;
            fwdVX = cfx; fwdVY = cfy; fwdVZ = cfz;
            ofx = ofy = ofz = 0.0f;
            // Seed the eased follow-cam statics so when the cut ENDS the camera glides
            // back from the cutscene's final pose instead of snapping.
            s_camEyeX = cex; s_camEyeZ = cez;
            if (effDist > 1.0f) { float ph = (cey - targetY) / effDist; if (ph < -1.0f) ph = -1.0f; if (ph > 1.0f) ph = 1.0f; s_camPosPitch = asinf(ph); }
            g_camEyeX = ex; g_camEyeZ = ez;
        }
#endif
        look_at(view, ex+ofx, ey+ofy, ez+ofz, ex+ofx + fwdVX, ey+ofy + fwdVY, ez+ofz + fwdVZ, 0.0f, 1.0f, 0.0f);
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
#ifdef AFN_HAS_LIGHTS
        lights_setup(view);   // eye-space scene lights for this frame's view
#endif
        for (int si = 0; si < afn_sprite_count; si++) {
            int mi = afn_sprites[si].meshIdx;
            if (mi < 0 || mi >= afn_mesh_count) continue;
#ifdef AFN_HAS_SPRITE_IDX
            int eidx = afn_mesh_inst_sprite[si];
            if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
            const AfnSpriteInst* sp = &afn_sprites[si];
            float mx = sp->x, my = sp->y, mz = sp->z;
#if defined(AFN_HAS_MESH_INST_ATTACH) && defined(AFN_HAS_PLAYER_RIG) && defined(AFN_HAS_SPRITE_IDX)
            // Attached MODEL: ride the parent's LIVE transform + offset. These arrays
            // are PARALLEL TO afn_sprites (indexed by the mesh-instance si) — NOT the
            // billboard afn_spr_* arrays (different index space; mixing them re-anchored
            // the static level mesh to a phantom parent -> floor/camera went haywire).
            if (afn_mesh_inst_parent[si] >= 0) {
                int found = 0;
                for (int n = 0; n < AFN_NPC_COUNT; n++)
                    if ((int)afn_npc_inst[n][7] == afn_mesh_inst_parent[si]) {
                        mx = s_npcX[n] + afn_mesh_inst_poff[si][0];
                        my = s_npcY[n] + afn_mesh_inst_poff[si][1];
                        mz = s_npcZ[n] + afn_mesh_inst_poff[si][2];
                        found = 1; break;
                    }
                if (!found) { mx = playerX + afn_mesh_inst_poff[si][0]; my = playerY + afn_mesh_inst_poff[si][1]; mz = playerZ + afn_mesh_inst_poff[si][2]; }
            }
            // Bone-driven: ride the player rig joint cached during rig_draw.
            if (afn_mesh_inst_bone[si] >= 0) {
                int b = afn_mesh_inst_bone[si];
                mx = s_player_bone_world[b][0] + afn_mesh_inst_poff[si][0];
                my = s_player_bone_world[b][1] + afn_mesh_inst_poff[si][1];
                mz = s_player_bone_world[b][2] + afn_mesh_inst_poff[si][2];
            }
#endif
            // 128 units ~= one PSP D16 depth quantum (2^-16 ~= 128 float-depth
            // ULPs near z=1) — the PSP proves that step is invisible yet big
            // enough to separate these faces.
            glPolygonOffset(0.0f, (float)((si + 1) * 128));   // later instances sit a hair deeper
            glLoadMatrixf(view);
            glTranslatef(mx, my, mz);
            if (sp->rotZ != 0.0f) glRotatef(sp->rotZ, 0,0,1);
            if (sp->rotX != 0.0f) glRotatef(sp->rotX, 1,0,0);
            if (sp->rotY != 0.0f) glRotatef(sp->rotY, 0,1,0);
            glScalef(sp->scale, sp->scale, sp->scale);
            int blended = afn_meshes[mi].blend;   // attached-model soft alpha
            if (blended) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
            draw_mesh(mi);
            if (blended) { glDepthMask(GL_TRUE); glDisable(GL_BLEND); }
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
        // Cut the looping charge wind-up SFX the instant the enemy stops charging
        // (fired, interrupted, died) — detect the s_efbCharging 1->0 edge.
        { static int s_prevEfbCharging = 0;
          if (s_prevEfbCharging && !s_efbCharging) { afn_stop_inst_voice(s_eChargeVoice, afn_ai_sfx_charge); s_eChargeVoice = -1; afn_sfx_protect_voice = -1; }
          s_prevEfbCharging = s_efbCharging; }
        enemy_orb_render(view);   // HARDCODED: enemy projectile orb (charge spot / in flight)
#endif
        // Particle system: integrate the pool, emit any pending burst at the player,
        // then billboard them. (Spawn Particles node fills afn_part_spawn in script_tick.)
        afn_particles_update();
        afn_particles_emit(playerX + afn_part_ox, playerY + afn_part_oy, playerZ + afn_part_oz);
        afn_particles_render(view);
        // Beam/lightning ribbons: tick life, cast any queued bolt, draw the strips.
        afn_beam_update();
        // BACKUP (UNHOOKED): the original hardcoded Pikachu-jolt cast — these are now the
        // Effects-tab lightning DEFAULTS, so the layer/Play Effect node reproduces it. Kept
        // here as a reference; flip `#if 0` to `#if 1` to re-bind it to the Select button.
#if 0
        if (key_hit(KEY_SELECT)) {
            afn_beam_bounces = 12;          // parabolic arches across the floor
            afn_beam_range   = 156;         // total reach (~13 units / bounce)
            afn_beam_bow     = 7.0f;        // arch height off the floor
            afn_beam_width   = 0.55f;       // filament thickness
            afn_beam_jitter  = 1.3f;        // bundle spread + crackle (world units)
            afn_beam_decay   = 0.97f;       // each bounce slightly lower
            afn_beam_pulse   = 0.0f;
            afn_beam_life    = 150;         // frames to crawl across
            afn_beam_segs    = 14;
            afn_beam_travel  = 1;           // crawl a head across, bundle trailing
            afn_beam_travel_persist = 0.55f;
            afn_beam_travel_fade    = 0.30f;
            afn_beam_col     = 0xFFFFB060u; // light blue (0xAABBGGRR: B=FF,G=B0,R=60)
            afn_beam_filaments = 5;         // bundled crackling strands
            afn_beam_orb     = 1.0f;        // head-orb radius multiplier
            afn_beam_spline  = 0; afn_beam_spline_n = 0;   // parametric bounce (no authored spline)
            afn_beam_spawn   = 1;
        }
#endif
#ifdef AFN_HAS_FX
        // Play Effect node queued an authored effect instance this frame.
        if (afn_fx_play_req > 0) { afn_fx_play(afn_fx_play_req - 1, playerX, playerY, playerZ, playerYaw); afn_fx_play_req = 0; }
#endif
        // HARDCODED TEST: hold Select to charge the Thunder spell (rainclouds + floor reticle),
        // release to strike. Casts a vertical bolt at the reticle, drawn by afn_beam_render below.
        afn_aim_step(playerX, playerY, playerZ, playerYaw);              // Aim Stick node (free-aim + orbit)
        afn_thunder_step(view, playerX, playerY, playerZ, playerYaw);
        afn_reticle_render(view, playerX, playerY, playerZ, playerYaw);   // Floor Reticle node
        afn_beam_resolve(playerX, playerY, playerZ, playerYaw);
        afn_beam_render(view);

        // Meteor Mash is now node-driven (Play Effect -> kind=3 layer -> afn_meteor_fire); Select
        // is reused by Wild Charge. Keep rendering/stepping the projectile whenever one is live.
        afn_meteor_render(view);
        afn_meteor_step();
        // Electroweb is node-driven now (Play Effect -> kind=5 layer -> afn_electroweb_fire).
        afn_electroweb_render(view);
        afn_electroweb_step();
        // Fire Spin is node-driven now (Play Effect -> kind=6 layer -> afn_firespin_fire); it
        // follows the player's live position, so keep rendering/stepping whenever one is active.
        afn_firespin_render(view, playerX, playerY, playerZ);
        afn_firespin_step();
        // Bubble Beam is node-driven now (Play Effect -> kind=7 layer -> afn_bubblebeam_fire).
        afn_bubblebeam_render(view);
        afn_bubblebeam_step();
        // Ice Beam is node-driven now (Play Effect -> kind=8 layer -> afn_icebeam_fire).
        afn_icebeam_render(view);
        afn_icebeam_step();
        // Sludge Bomb is node-driven now (Play Effect -> kind=9 layer -> afn_sludge_fire).
        afn_sludge_render(view);
        afn_sludge_step();
        // Psybeam is node-driven now (Play Effect -> kind=10 layer -> afn_psybeam_fire).
        afn_psybeam_render(view);
        afn_psybeam_step();
        // Psychic is node-driven now (Play Effect -> kind=11 layer -> afn_psychic_fire).
        afn_psychic_render(view);
        afn_psychic_step();
        // Surf is node-driven now (Play Effect -> kind=12 layer -> afn_surf_fire); the ride override
        // in the movement block keys off s_su_active, so it carries the player either way.
        afn_surf_render(view);
        afn_surf_step();
        // Flamethrower is node-driven now (Play Effect -> kind=13 layer -> afn_flame_fire).
        afn_flame_render(view);
        afn_flame_step();
        // Aura Sphere is node-driven now (Play Effect -> kind=14 layer -> afn_aura_fire).
        afn_aura_render(view);
        afn_aura_step();
        afn_focusblast_aura_render(view);   // Focus Blast orb rendered as an aura sphere (charge + flight)
        // Icy Wind is node-driven now (Play Effect -> kind=15 layer -> afn_icywind_fire); it sweeps
        // forward on its own, so keep rendering/stepping the wall whenever one is active.
        afn_icywind_render(view);
        afn_icywind_step();
        // Flame Wheel is node-driven now (Play Effect -> kind=16 layer -> afn_flamewheel_fire); the
        // ride override keys off s_fw_active, so it carries the player either way. Render/step
        // whenever a wheel or its dying sparks are live.
        afn_flamewheel_render(view, playerX, playerY, playerZ);
        afn_flamewheel_step();
        // Flash Cannon is node-driven now (Play Effect -> kind=17 layer -> afn_flashcannon_fire).
        afn_flashcannon_render(view);
        afn_flashcannon_step();

        }   // end 3D world (skipped in 2D menu mode)

#if defined(AFN_HAS_HUD) && defined(AFN_HAS_CAM_LOCK)
        // Pause: the lock-on reticle's anchor AND its spin layers are only (re)set by the L
        // lock-toggle's Show HUD node, so while the graph is paused they lapse — the marker
        // drops to its authored corner and the spin keyframe layer can go inactive (freezing
        // the spin). Re-pin the anchor to the live lock target AND re-assert the reticle
        // element's keyframe layers active each paused frame so the reticle keeps spinning.
        // (Don't reset frame/tick — that would stutter the spin; the layer-advance in
        // hud_render keeps looping it.) Anchor == afn_cam_lock_target, both set on the toggle.
        if (afn_paused && afn_cam_lock_target >= 0) {
            afn_hud_anchor_sprite[AFN_TARGET_ELEM] = afn_cam_lock_target;
#ifdef AFN_HAS_HUD_ANIM
            const AfnHudElem* _rel = &afn_hud_elems[AFN_TARGET_ELEM];
            for (int _rk = 0; _rk < _rel->pieceCount; _rk++) {
                int _rl = afn_hud_piece_layer[_rel->pieceStart + _rk];
                if (_rl >= 0) afn_hud_layer_active[_rl] = 1;   // keep spinning; frame/tick untouched
            }
#endif
        }
#endif
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

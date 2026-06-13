// Affinity visual-script node codegen — node graph -> C. SHARED by the NDS and
// PS Vita exporters (extracted verbatim from nds_package.cpp so both emit the
// SAME C). The emitted code is platform-neutral (afn_* vars, KEY_*); each
// runtime defines the variables it consumes and leaves the rest inert.
#include "node_script_emit.h"
#include "../gba/gba_package.h"
#include <vector>
#include <set>
#include <string>
#include <cstring>
#include <functional>
#include <algorithm>

namespace Affinity {

void EmitNodeScriptBodies(std::ostream& f,
                          const GBAScriptExport& script,
                          const std::vector<GBABlueprintExport>& blueprints,
                          const std::vector<GBABlueprintInstanceExport>& bpInstances,
                          const std::vector<GBASpriteExport>& sprites,
                          const std::vector<GBASoundInstanceExport>& soundInstances) {
    bool hasAnyScript = !script.nodes.empty() || !blueprints.empty();
    if (hasAnyScript) {
        // curScript = which graph (inline scene or a blueprint) the lambdas
        // operate on; swapped before each blueprint emit pass below.
        const GBAScriptExport* curScript = &script;
        auto findNode = [&](int id) -> const GBAScriptNodeExport* {
            for (auto& n : curScript->nodes) if (n.id == id) return &n;
            return nullptr;
        };
        auto findExecOuts = [&](int nodeId, int pinIdx) -> std::vector<int> {
            std::vector<int> targets;
            for (auto& l : curScript->links)
                if (l.fromNodeId == nodeId && l.fromPinType == 0 && l.fromPinIdx == pinIdx)
                    targets.push_back(l.toNodeId);
            return targets;
        };
        auto findDataIn = [&](int nodeId, int pinIdx) -> const GBAScriptNodeExport* {
            for (auto& l : curScript->links)
                if (l.toNodeId == nodeId && l.toPinType == 3 && l.toPinIdx == pinIdx)
                    return findNode(l.fromNodeId);
            return nullptr;
        };
        auto resolveInt = [&](const GBAScriptNodeExport* dn) -> int {
            if (!dn) return 0;
            if (dn->type == GBAScriptNodeType::Animation) return dn->paramInt[1];
            if (dn->type == GBAScriptNodeType::SkelAnim) return dn->paramInt[1]; // clip index
            return dn->paramInt[0];
        };
        auto resolveFloat = [&](const GBAScriptNodeExport* dn) -> float {
            if (!dn) return 0.0f;
            // Integer nodes store their value in paramInt[0] as a raw int.
            // Bit-casting that as float gives a denormal (≈ 0), so when the
            // user wires an Integer literal to a Float pin we must convert
            // rather than reinterpret. Float nodes already stored their
            // value bit-cast in paramInt[0] so the memcpy path is correct.
            if (dn->type == GBAScriptNodeType::Integer)
                return (float)dn->paramInt[0];
            float fv;
            memcpy(&fv, &dn->paramInt[0], sizeof(float));
            return fv;
        };
        auto keyName = [](int k) -> const char* {
            static const char* keys[] = { "KEY_A","KEY_B","KEY_L","KEY_R",
                                          "KEY_START","KEY_SELECT",
                                          "KEY_UP","KEY_DOWN","KEY_LEFT","KEY_RIGHT",
                                          "KEY_X","KEY_Y",
                                          "KEY_LSTICK_UP","KEY_LSTICK_DOWN","KEY_LSTICK_LEFT","KEY_LSTICK_RIGHT",
                                          "KEY_RSTICK_UP","KEY_RSTICK_DOWN","KEY_RSTICK_LEFT","KEY_RSTICK_RIGHT" };
            return (k >= 0 && k < 20) ? keys[k] : "KEY_A";
        };

        // Collect chains per event type (BFS from each event node through exec links).
        // Re-runnable: blueprint emit passes call buildChains() after pointing
        // curScript at the blueprint's graph.
        struct Chain { const GBAScriptNodeExport* event; std::vector<const GBAScriptNodeExport*> actions; };
        std::vector<Chain> chains;
        auto buildChains = [&]() {
            chains.clear();
            for (auto& n : curScript->nodes) {
                auto et = n.type;
                if (et != GBAScriptNodeType::OnUpdate &&
                    et != GBAScriptNodeType::OnStart &&
                    et != GBAScriptNodeType::OnKeyHeld &&
                    et != GBAScriptNodeType::OnKeyPressed &&
                    et != GBAScriptNodeType::OnKeyReleased &&
                    et != GBAScriptNodeType::OnCollision &&
                    et != GBAScriptNodeType::OnCollision2D)
                    continue;
                Chain ch; ch.event = &n;
                std::vector<int> frontier = findExecOuts(n.id, 0);
                std::vector<bool> seen(10000, false);
                int safety = 0;
                while (!frontier.empty() && safety < 256) {
                    int nid = frontier.front();
                    frontier.erase(frontier.begin());
                    if (nid < 0 || nid >= (int)seen.size() || seen[nid]) continue;
                    seen[nid] = true;
                    safety++;
                    auto* an = findNode(nid);
                    if (!an) continue;
                    ch.actions.push_back(an);
                    // Don't follow exec outs of CheckFlag (dual-pin), OnRise
                    // (edge-detect wrapper), FlipFlop (toggles between two
                    // exec pins), or IsFlagSet (single-pin gate) — those
                    // recurse into their own branches inside emitOne so
                    // siblings don't bleed across pins.
                    if (an->type == GBAScriptNodeType::CheckFlag ||
                        an->type == GBAScriptNodeType::OnRise ||
                        an->type == GBAScriptNodeType::FlipFlop ||
                        an->type == GBAScriptNodeType::IsFlagSet) continue;
                    for (int t : findExecOuts(an->id, 0)) frontier.push_back(t);
                }
                if (!ch.actions.empty()) chains.push_back(ch);
            }
        };
        buildChains();

        // Mirrors GBA: track whether emitAction is currently inside an
        // IsJumping / IsFalling gate so PlayAnim can lock with afn_anim_prio.
        bool inJumpGate = false;
        int  gateDepth = 0;   // >0 while emitting inside any gate (Is Moving etc.)

        // Per-action emit. Subset of GBA's switch — covers the common
        // movement / animation / state nodes. Unsupported types fall through
        // to a comment so we know what's missing on NDS.
        auto emitAction = [&](const GBAScriptNodeExport* a) {
            switch (a->type) {
            case GBAScriptNodeType::Walk: {
                // Walk is the LOW-priority speed. If a Sprint already set the
                // speed this frame (afn_speed_prio), don't clobber it — so
                // holding B (sprint) while also holding a direction (which
                // fires Walk) stays at sprint speed regardless of node order.
                auto* d = findDataIn(a->id, 0);
                if (d) f << "    if (!afn_speed_prio) afn_move_speed = " << (int)(resolveInt(d) * 37.0f / 35.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::Sprint: {
                // Sprint is HIGH priority — sets the speed and locks out Walk
                // for the rest of this frame.
                auto* d = findDataIn(a->id, 0);
                if (d) f << "    afn_move_speed = " << (int)(resolveInt(d) * 37.0f / 35.0f) << "; afn_speed_prio = 1;\n";
                break;
            }
            case GBAScriptNodeType::Jump: {
                auto* d = findDataIn(a->id, 0);
                float force = d ? resolveFloat(d) : 2.0f;
                f << "    if (player_on_ground) player_vy = " << (int)(force * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetGravity: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.09f;
                f << "    afn_gravity = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetMaxFall: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 6.0f;
                f << "    afn_terminal_vel = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::AutoOrbit: {
                auto* d = findDataIn(a->id, 0);
                f << "    afn_auto_orbit_speed = " << (d ? resolveInt(d) : 205) << ";\n";
                break;
            }
            case GBAScriptNodeType::OrbitCamera: {
                auto* dirData = findDataIn(a->id, 0);
                auto* speedData = findDataIn(a->id, 1);
                int dir   = dirData   ? dirData->paramInt[0] : 1;
                int speed = speedData ? resolveInt(speedData) : 512;
                speed /= 2;  // halve orbit speed on NDS
                // Orbit when this exec runs — the chain (e.g. On Key Held)
                // decides when. Direction picks the axis/sign: Left/Right rotate
                // the yaw (orbit_angle); Up/Down tilt the pitch (orbit_pitch).
                // Speed scales by afn_key_mag (256 for buttons; 0..256 stick
                // deflection on analog runtimes) so stick-bound orbit ramps.
                if (dir == 0)      f << "    orbit_angle += (" << speed << " * afn_key_mag) >> 8;\n";  // Left
                else if (dir == 1) f << "    orbit_angle -= (" << speed << " * afn_key_mag) >> 8;\n";  // Right
                else if (dir == 2) f << "    orbit_pitch += (" << speed << " * afn_key_mag) >> 8;\n";  // Up
                else if (dir == 3) f << "    orbit_pitch -= (" << speed << " * afn_key_mag) >> 8;\n";  // Down
                break;
            }
            case GBAScriptNodeType::MovePlayer: {
                auto* dirData = findDataIn(a->id, 0);
                int dir = dirData ? dirData->paramInt[0] : 0;
                // Apply the movement in the chosen Direction when this exec
                // runs. WHEN it runs is the chain's job — wire it after an
                // On Key Held(key) to bind it to a button. The old code
                // re-checked the matching DPAD key here, double-gating:
                // On Key Held(A) -> Move Player(Up) required BOTH A and Up.
                // afn_key_mag is the gating key's strength (256 for buttons;
                // 0..256 stick deflection on analog runtimes), set by the key
                // dispatcher at chain entry — slight push = slight move.
                static const char* dirVars[] = { "afn_input_right -= afn_key_mag","afn_input_right += afn_key_mag",
                                                 "afn_input_fwd += afn_key_mag","afn_input_fwd -= afn_key_mag" };
                if (dir >= 0 && dir < 4)
                    f << "    if (!afn_player_frozen) " << dirVars[dir] << ";\n";
                // Facing switch (node paramInt[0], left-click property):
                // 1 = Consistent Facing — the rig keeps its current yaw while
                // moving (strafe/moonwalk) instead of turning toward the
                // movement direction. Reset each tick; runtimes skip their
                // face-movement update while it's set.
                if (a->paramInt[0] == 1)
                    f << "    afn_face_lock = 1;\n";
                break;
            }
            case GBAScriptNodeType::PlayAnim: {
                auto* d = findDataIn(a->id, 0);
                int idx = d ? resolveInt(d) : 0;
                // Inside an IsJumping/IsFalling gate, PlayAnim wins and
                // claims priority; outside, it defers to whatever already
                // set afn_play_anim this frame (afn_anim_prio gates it).
                // Always honour afn_player_frozen — when a menu freezes the
                // player, cursor nav (DPAD up/down) shouldn't restart the
                // walk anim from a still-firing OnKeyHeld → PlayAnim chain.
                // Tiered animation priority (afn_anim_prio is a LEVEL, not a flag):
                //   0 = ungated (walk),  1 = inside a normal gate (sprint behind
                //   Is Moving),  2 = inside a jump/fall gate (airborne anims).
                // A PlayAnim plays only if its level >= the level already claimed
                // this frame, then claims its own level — so jump beats sprint
                // beats walk regardless of node emission order.
                int animLvl = inJumpGate ? 2 : (gateDepth > 0 ? 1 : 0);
                f << "    if (!afn_player_frozen && " << animLvl << " >= afn_anim_prio) { afn_play_anim = "
                  << idx << "; afn_anim_prio = " << animLvl << "; }\n";
                break;
            }
            case GBAScriptNodeType::PlaySkelAnim: {
                // Set the skeletal (glTF/DSMA) clip the player rig plays in Mode 4.
                // The clip index comes from a wired Skeletal Animation data node.
                auto* d = findDataIn(a->id, 0);
                int clip = d ? resolveInt(d) : 0;
                f << "    afn_rig_clip = " << clip << ";\n";
                break;
            }
            case GBAScriptNodeType::SetSkelAnim: {
                // Set the skeletal clip on a specific rigged NPC (by sprite index).
                // Mirrors SetSpriteAnim: a single per-frame override slot the
                // NPC rig renderer applies to the matching instance.
                auto* objData  = findDataIn(a->id, 0);
                auto* clipData = findDataIn(a->id, 1);
                int obj  = objData  ? resolveInt(objData)  : 0;
                int clip = clipData ? resolveInt(clipData) : 0;
                f << "    afn_skel_anim_obj = " << obj << "; afn_skel_anim_clip = " << clip << ";\n";
                break;
            }
            case GBAScriptNodeType::FreezePlayer:
                f << "    afn_player_frozen = 1; afn_play_anim = -1;\n";
                break;
            case GBAScriptNodeType::UnfreezePlayer:
                f << "    afn_player_frozen = 0;\n";
                break;
            case GBAScriptNodeType::CursorUp:
                f << "    if (afn_cursor_stop > 0) afn_cursor_stop--;\n";
                f << "    else afn_cursor_stop = afn_stop_count - 1;\n";
                break;
            case GBAScriptNodeType::CursorDown:
                f << "    afn_cursor_stop++;\n";
                f << "    if (afn_cursor_stop >= afn_stop_count) afn_cursor_stop = 0;\n";
                break;
            case GBAScriptNodeType::FollowLink:
                f << "    { int link = afn_stop_links[afn_cursor_stop];\n";
                f << "      if (link >= 0) { afn_hud_visible[afn_elem_idx] = 0; afn_hud_visible[link] = 1; afn_active_element = link; } }\n";
                break;
            case GBAScriptNodeType::SetVisible: {
                auto* sprData = findDataIn(a->id, 0);
                auto* visData = findDataIn(a->id, 1);
                int sIdx = sprData ? resolveInt(sprData) : a->paramInt[0];
                int vis  = visData ? resolveInt(visData) : a->paramInt[1];
                f << "    if ((unsigned)" << sIdx << " < NUM_SPRITES) afn_sprite_visible["
                  << sIdx << "] = " << (vis ? 1 : 0) << ";\n";
                break;
            }
            case GBAScriptNodeType::DestroyObject: {
                auto* d = findDataIn(a->id, 0);
                // In BP context with no Object wired, default to the instance's
                // own sprite (afn_bp_cur_spr_idx). Scene scripts fall back to
                // the node's literal paramInt[0]. Mirrors GBA semantics.
                std::string sIdx;
                if (d) sIdx = std::to_string(resolveInt(d));
                else if (curScript != &script) sIdx = "afn_bp_cur_spr_idx";
                else sIdx = std::to_string(a->paramInt[0]);
                f << "    if ((unsigned)" << sIdx << " < NUM_SPRITES) {\n";
                f << "        afn_sprite_visible[" << sIdx << "] = 0;\n";
                f << "        afn_collision_enabled[" << sIdx << "] = 0;\n";
                f << "    }\n";
                break;
            }
            case GBAScriptNodeType::SetFlag: {
                auto* flagData = findDataIn(a->id, 0);
                auto* valData  = findDataIn(a->id, 1);
                int flag = flagData ? resolveInt(flagData) : a->paramInt[0];
                int val  = valData  ? resolveInt(valData)  : a->paramInt[1];
                if (val) f << "    afn_flags |=  (1u << " << flag << ");\n";
                else     f << "    afn_flags &= ~(1u << " << flag << ");\n";
                break;
            }
            case GBAScriptNodeType::ToggleFlag:
                f << "    afn_flags ^= (1u << " << a->paramInt[0] << ");\n";
                break;
            case GBAScriptNodeType::ScreenShake: {
                auto* d0 = findDataIn(a->id, 0);
                auto* d1 = findDataIn(a->id, 1);
                f << "    afn_shake_intensity = " << (d0 ? resolveInt(d0) : 4) << ";\n";
                f << "    afn_shake_frames    = " << (d1 ? resolveInt(d1) : 20) << ";\n";
                break;
            }
            case GBAScriptNodeType::DampenJump: {
                auto* d = findDataIn(a->id, 0);
                float factor = d ? resolveFloat(d) : 0.75f;
                f << "    if (player_vy > 0) player_vy = (player_vy * " << (int)(factor*256.0f) << ") >> 8;\n";
                break;
            }
            case GBAScriptNodeType::StartGrind:
                // Capture the rail we collided with — the runtime reads its
                // mesh axis to lock the grind direction along the rail.
                f << "    afn_grinding = 1; afn_grind_rail = afn_collided_sprite;\n"; break;
            case GBAScriptNodeType::StopGrind:
                f << "    afn_grinding = 0;\n"; break;
            case GBAScriptNodeType::GrindPower: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 24.0f;
                f << "    afn_grind_power = " << (int)v << ";\n"; break;
            }
            case GBAScriptNodeType::GrindBoost: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_grind_boost = " << (int)v << ";\n"; break;
            }
            case GBAScriptNodeType::GrindBleed: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 6.0f;
                f << "    afn_grind_bleed = " << (int)v << ";\n"; break;
            }
            case GBAScriptNodeType::GrindCatch: {
                auto* dy = findDataIn(a->id, 0);
                auto* dx = findDataIn(a->id, 1);
                float vy = dy ? resolveFloat(dy) : 0.0f;
                float vx = dx ? resolveFloat(dx) : 0.0f;
                // Editor px -> 16.8 fixed world units (256 = 1 px), matching positions.
                f << "    afn_grind_catch_y = " << (int)(vy * 256.0f) << ";\n";
                f << "    afn_grind_catch_x = " << (int)(vx * 256.0f) << ";\n"; break;
            }
            // --- Gate nodes (open a brace; closed by the chain's tail logic) ---
            // Read the physics-VALIDATED grind flag (afn_grinding_active), not
            // the raw afn_grinding intent: StartGrind (On Collision) sets
            // afn_grinding=1 in the script pass even while you're airborne over
            // the rail, and the physics zeroes it later the same frame. Reading
            // that intent makes On Rise see a spurious "grinding" during the
            // approach so the real landing produces no fresh edge (grind SFX
            // failing to retrigger). afn_grinding_active mirrors the validated
            // state from the previous physics tick.
            case GBAScriptNodeType::IsGrinding:
                f << "    if (afn_grinding_active) {\n"; break;
            case GBAScriptNodeType::IsNotGrinding:
                f << "    if (!afn_grinding_active) {\n"; break;
            case GBAScriptNodeType::IsMoving:
                f << "    if (player_moving) {\n"; break;
            case GBAScriptNodeType::IsLockedOn:
                // Lock On target active (PSV camera lock; inert -1 elsewhere).
                f << "    if (afn_cam_lock_target >= 0) {\n"; break;
            case GBAScriptNodeType::IsInView: {
                // Gate: target on-screen (camera FOV). Target pin = Object
                // or Attached Sprite (self).
                auto* tv = findDataIn(a->id, 0);
                if (tv && tv->type == GBAScriptNodeType::AttachedSprite)
                    f << "    if (afn_in_view(afn_bp_cur_spr_idx)) {\n";
                else
                    f << "    if (afn_in_view(" << (tv ? resolveInt(tv) : -1) << ")) {\n";
                break;
            }
            case GBAScriptNodeType::IsNotLockedOn:
                f << "    if (afn_cam_lock_target < 0) {\n"; break;
            case GBAScriptNodeType::DashToTarget: {
                // Bullet-punch lunge (PSV, AFN_HAS_CAM_LOCK): capture the
                // current lock target and burst toward it for N frames.
                auto* spD = findDataIn(a->id, 0);
                auto* frD = findDataIn(a->id, 1);
                int sp = spD ? resolveInt(spD) : 120;
                int fr = frD ? resolveInt(frD) : 10;
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_dash_speed = " << sp << ";\n";
                f << "    afn_dash_frames = " << fr << ";\n";
                f << "    afn_dash_target = afn_cam_lock_target;\n";
                f << "#endif\n";
                break;
            }
            case GBAScriptNodeType::StrafeAnim: {
                // 8-way directional clip picker (PSV lock-strafe): pick the
                // clip matching the stick direction relative to facing the
                // target. Pins 0..7 = Fwd,Fwd-R,Right,Back-R,Back,Back-L,Left,Fwd-L.
                int sc[8]; bool set[8]; int anySet = 0;
                for (int k = 0; k < 8; k++) {
                    auto* cd = findDataIn(a->id, k);
                    set[k] = (cd != nullptr);
                    sc[k] = cd ? resolveInt(cd) : 0;
                    if (set[k]) anySet = 1;
                }
                // Fill unwired octants from the NEAREST wired one (circular
                // search ±1,±2,...): wire just the 4 cardinals and diagonals
                // lean to a neighbor; wire only Fwd and everything is Fwd.
                if (anySet) {
                    for (int k = 0; k < 8; k++) {
                        if (set[k]) continue;
                        for (int d = 1; d <= 4; d++) {
                            int a1 = (k + d) & 7, a2 = (k - d + 8) & 7;
                            if (set[a1]) { sc[k] = sc[a1]; break; }
                            if (set[a2]) { sc[k] = sc[a2]; break; }
                        }
                    }
                }
                if (anySet) {
                    // Register the clips + flag; the runtime movement block
                    // picks the octant AFTER input is final (this runs on
                    // OnUpdate, before MovePlayer sets afn_input_fwd/right).
                    f << "#ifdef AFN_HAS_CAM_LOCK\n";
                    for (int k = 0; k < 8; k++)
                        f << "    afn_strafe_clip[" << k << "] = " << sc[k] << ";\n";
                    f << "    afn_strafe_anim = 1;\n";
                    f << "#endif\n";
                }
                break;
            }
            case GBAScriptNodeType::SnapStick8: {
                // Gate the left stick to 8 directions (PSV). The runtime movement
                // block snaps afn_input_fwd/right to the nearest octant; set once
                // from On Start. NDS lacks the analog stick path -> compiled out.
                f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                f << "    afn_stick_8way = 1;\n";
                f << "#endif\n";
                break;
            }
            case GBAScriptNodeType::IsOnGround:
                f << "    if (player_on_ground) {\n"; break;
            case GBAScriptNodeType::IsJumping:
                // Matches GBA: just "rising," no airborne guard. Sounds wrong
                // vs the editor tooltip but the BP idioms (e.g. jump SFX on
                // the same frame Jump sets player_vy while still grounded)
                // depend on this looser check.
                f << "    if (player_vy > 0) {\n"; inJumpGate = true; break;
            case GBAScriptNodeType::IsFalling:
                f << "    if (!player_on_ground && player_vy <= 0) {\n"; inJumpGate = true; break;
            case GBAScriptNodeType::IsNear2D:
                // Mirrors GBA: fires when the player just collided with
                // THIS BP's tm_object. Combined with OnKeyPressed(A) this
                // is the "press A near NPC" idiom.
                f << "    if (afn_collided_tm_obj == afn_bp_cur_tm_obj && afn_bp_cur_tm_obj >= 0) {\n"; break;
            case GBAScriptNodeType::IsFollowMoving:
                f << "    if (tm_fol_moving) {\n"; break;
            // CheckFlag is handled specially in emitChain (dual-pin Set/Clear branches).
            case GBAScriptNodeType::SetVelocityY: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    player_vy = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetVelocityX: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_player_vx_world = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetVelocityZ: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_player_vz_world = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::VelocityFalloff: {
                auto* d = findDataIn(a->id, 0);
                int frames = d ? resolveInt(d) : 0;
                if (frames < 1) frames = 1;
                f << "    afn_velocity_falloff = " << frames << ";\n";
                break;
            }
            case GBAScriptNodeType::BoostForward: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_pending_boost_fwd = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::HaltMomentum: {
                f << "    afn_player_vx_world = 0;\n";
                f << "    afn_player_vz_world = 0;\n";
                f << "    afn_pending_boost_fwd = 0;\n";
                f << "    afn_velocity_falloff = 0;\n";
                f << "    player_vy = 0;\n";
                break;
            }
            case GBAScriptNodeType::SetPlayerHeight: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 1.0f;
                f << "    afn_player_height = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetPlayerWidth: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 3.0f;
                f << "    afn_player_width = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case GBAScriptNodeType::SetSpriteAnim: {
                auto* objData  = findDataIn(a->id, 0);
                auto* animData = findDataIn(a->id, 1);
                int obj  = objData  ? resolveInt(objData)  : 0;
                int anim = animData ? resolveInt(animData) : 0;
                // Mode 4: per-sprite override via afn_sprite_anim_spr/val.
                // Mode 0 tm_object: write directly to its mutable anim slot
                // (the FollowPlayer / AIFollow BPs rely on this to switch
                // a follower between idle and walk sprites).
                f << "    if (afn_current_mode == 1) { extern int tm_obj_anim_idx[]; extern int tm_obj_anim_play[]; tm_obj_anim_idx[" << obj << "] = " << anim << "; tm_obj_anim_play[" << obj << "] = 1; }\n";
                f << "    else { afn_sprite_anim_spr = " << obj << "; afn_sprite_anim_val = " << anim << "; }\n";
                break;
            }
            case GBAScriptNodeType::FollowPlayer: {
                auto* objData   = findDataIn(a->id, 0);
                auto* distData  = findDataIn(a->id, 1);
                auto* speedData = findDataIn(a->id, 2);
                int obj   = objData   ? resolveInt(objData)   : 0;
                int dist  = distData  ? resolveInt(distData)  : 0;
                int speed = speedData ? resolveInt(speedData) : 0;
                f << "    if (!tm_fol_active) {\n";
                f << "      tm_fol_obj = " << obj << ";\n";
                f << "      extern int tm_fol_prev_ptx, tm_fol_prev_pty;\n";
                f << "      tm_fol_prev_ptx = tm_player_tx;\n";
                f << "      tm_fol_prev_pty = tm_player_ty;\n";
                f << "      extern int tm_fol_trail_count, tm_fol_trail_head;\n";
                f << "      tm_fol_trail_count = 0; tm_fol_trail_head = 0;\n";
                f << "      tm_fol_active = 1;\n";
                f << "    }\n";
                f << "    tm_fol_dist = " << dist << ";\n";
                f << "    tm_fol_speed = " << speed << ";\n";
                break;
            }
            case GBAScriptNodeType::SetFollowFacing:
                f << "    if (tm_fol_active && tm_fol_obj >= 0)\n";
                f << "      tm_obj_facing[tm_fol_obj] = tm_fol_facing;\n";
                break;
            case GBAScriptNodeType::PlaySound: {
                auto* d = findDataIn(a->id, 0);
                int sId = d ? resolveInt(d) : 0;
                if (sId >= 0 && sId < (int)soundInstances.size() && soundInstances[sId].isSfx) {
                    f << "    afn_play_sfx(" << soundInstances[sId].sfxSampleIdx
                      << ", " << soundInstances[sId].mixerGain
                      << ", " << soundInstances[sId].fifoChannel << ");\n";
                } else {
                    f << "    afn_play_sound(" << sId << ");\n";
                }
                break;
            }
            case GBAScriptNodeType::StopSound: {
                // With a Sound Instance wired, stop ONLY that SFX's voices so
                // BGM keeps playing (e.g. kill a looping grind SFX when you
                // leave the rail). Unwired -> stop everything (legacy).
                auto* sd = findDataIn(a->id, 0);
                if (sd) {
                    int sId = resolveInt(sd);
                    if (sId >= 0 && sId < (int)soundInstances.size() && soundInstances[sId].isSfx)
                        f << "    afn_stop_sfx_sample(" << soundInstances[sId].sfxSampleIdx << ");\n";
                    else
                        f << "    afn_stop_sound();\n";
                } else {
                    f << "    afn_stop_sound();\n";
                }
                break;
            }
            case GBAScriptNodeType::Respawn:
                f << "    player_x = afn_start_x; player_y = afn_start_y; player_z = afn_start_z;\n";
                f << "    player_vy = 0;\n";
                break;
            case GBAScriptNodeType::ChangeScene: {
                auto* scData = findDataIn(a->id, 0);
                int scIdx = scData ? resolveInt(scData) : a->paramInt[0];
                int scMode = a->paramInt[1]; // 0 = 3D / Mode 4
                f << "    afn_scene_start_transition(" << scIdx << ", " << scMode << ", 15);\n";
                break;
            }
            case GBAScriptNodeType::ReloadScene:
                f << "    afn_scene_start_transition(afn_current_scene, afn_current_mode, 15);\n";
                break;
            case GBAScriptNodeType::SetHudValue: {
                auto* valData  = findDataIn(a->id, 0);
                auto* slotData = findDataIn(a->id, 1);
                int val  = valData  ? resolveInt(valData)  : 0;
                int slot = slotData ? resolveInt(slotData) : 0;
                if (slot < 0) slot = 0; if (slot > 3) slot = 3;
                f << "    afn_hud_value[" << slot << "] += " << val << ";\n";
                break;
            }
            case GBAScriptNodeType::ShowHUD: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : a->paramInt[0];
                if (slot < 0) slot = 0; if (slot > 3) slot = 3;
                f << "    afn_hud_visible[" << slot << "] = 1;\n";
                // World anchoring (PSV only, AFN_HAS_HUD_ANCHOR): the Anchor
                // pin (data input 1) pins the element's content to that
                // sprite's attached-sprite world position projected to screen.
                // Wire an Attached Sprite node for the BP owner ("self"), or
                // an Object node for a specific sprite. Unwired = the normal
                // authored screen position.
                f << "#ifdef AFN_HAS_HUD_ANCHOR\n";
                {
                    auto* anchorData = findDataIn(a->id, 1);
                    if (anchorData && anchorData->type == GBAScriptNodeType::AttachedSprite) {
                        // Only instances that HAVE an owner anchor; ownerless
                        // instances (element-linked BPs run with spr_idx -1)
                        // must not stomp an anchor set by the sprite instance.
                        f << "    if (afn_bp_cur_spr_idx >= 0) afn_hud_anchor_sprite[" << slot << "] = afn_bp_cur_spr_idx;\n";
                        // Max/Min Size + Near/Far (Attached Sprite node). Size
                        // percent; Near/Far in EDITOR units (->/4 world px).
                        // Min at/under Near, Max at/over Far — proximity scale
                        // by PLAYER->target distance (shrinks as you approach).
                        // Min==Max = flat size, no scaling.
                        int mx = anchorData->paramInt[0] > 0 ? anchorData->paramInt[0] : 100;
                        int mn = anchorData->paramInt[1] > 0 ? anchorData->paramInt[1] : 100;
                        int nr = anchorData->paramInt[2];
                        int fr = anchorData->paramInt[3];
                        f << "    afn_hud_anchor_min[" << slot << "] = " << mn << ";\n";
                        f << "    afn_hud_anchor_max[" << slot << "] = " << mx << ";\n";
                        if (nr > 0) f << "    afn_hud_anchor_near[" << slot << "] = " << (nr / 4) << ";\n";
                        if (fr > 0) f << "    afn_hud_anchor_far[" << slot << "] = " << (fr / 4) << ";\n";
                    } else if (anchorData) {
                        f << "    afn_hud_anchor_sprite[" << slot << "] = " << resolveInt(anchorData) << ";\n";
                    } else {
                        f << "    afn_hud_anchor_sprite[" << slot << "] = -1;\n";
                    }
                }
                f << "#endif\n";
                // Mirror GBA: showing a menu element with cursor stops
                // freezes the player + primes the cursor nav state so
                // CursorUp/Down/FollowLink have something to walk.
                f << "    afn_elem_idx = " << slot << ";\n";
                f << "    afn_active_element = " << slot << ";\n";
                f << "    afn_cursor_stop = 0;\n";
                f << "    afn_stop_count = afn_hud_elems[" << slot << "].stopCount;\n";
                f << "    if (afn_stop_count > 0) {\n";
                f << "      afn_player_frozen = 1;\n";
                f << "      afn_play_anim = -1;\n";
                f << "      afn_move_speed = 0;\n";
                f << "      { int si; for (si = 0; si < afn_stop_count && si < 8; si++) afn_stop_links[si] = afn_hud_stops[afn_hud_elems[" << slot << "].stopStart + si].link; }\n";
                f << "    }\n";
                break;
            }
            case GBAScriptNodeType::HideHUD: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : a->paramInt[0];
                if (slot < 0) slot = 0; if (slot > 3) slot = 3;
                f << "    afn_hud_visible[" << slot << "] = 0;\n";
                // Hiding a menu auto-unfreezes / clears the anim hold so
                // gameplay resumes without needing an explicit UnfreezePlayer
                // wire. Matches GBA HideHUD semantics.
                f << "    afn_player_frozen = 0;\n";
                f << "    afn_play_anim = 0;\n";
                break;
            }
            case GBAScriptNodeType::PlayHudAnim: {
                int li = a->paramInt[0];
                f << "    afn_hud_layer_frame[" << li << "] = 0;\n";
                f << "    afn_hud_layer_tick[" << li << "] = 0;\n";
                f << "    afn_hud_layer_active[" << li << "] = 1;\n";
                break;
            }
            case GBAScriptNodeType::StopHudAnim: {
                int li = a->paramInt[0];
                f << "    afn_hud_layer_active[" << li << "] = 0;\n";
                break;
            }
            case GBAScriptNodeType::SetHudAnimSpeed: {
                int li = a->paramInt[0];
                auto* sd = findDataIn(a->id, 0);
                int spd = sd ? resolveInt(sd) : 1;
                f << "    afn_hud_layer_speed_override[" << li << "] = " << spd << ";\n";
                break;
            }
            case GBAScriptNodeType::UpdateRespawnPos: {
                auto* objData = findDataIn(a->id, 0);
                std::string obj;
                if (objData) obj = std::to_string(resolveInt(objData));
                else if (curScript != &script) obj = "afn_bp_cur_spr_idx";
                else obj = std::to_string(a->paramInt[0]);
                f << "    afn_start_x = afn_sprite_data[" << obj << "][0];\n";
                f << "    afn_start_y = afn_sprite_data[" << obj << "][1];\n";
                f << "    afn_start_z = afn_sprite_data[" << obj << "][2];\n";
                break;
            }
            case GBAScriptNodeType::SetCamera: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : 0;
                f << "    afn_active_camera = " << slot << ";\n";
                break;
            }
            case GBAScriptNodeType::TankCamera: {
                auto* onData = findDataIn(a->id, 0);
                int on = onData ? resolveInt(onData) : 0;
                f << "    afn_tank_camera = " << on << ";\n";
                break;
            }
            case GBAScriptNodeType::TurnPlayer: {
                auto* dirData = findDataIn(a->id, 0);
                auto* speedData = findDataIn(a->id, 1);
                int dir   = dirData   ? dirData->paramInt[0] : 0;
                int speed = speedData ? resolveInt(speedData) : 512;
                const char* sign = (dir == 0) ? "+" : "-";   // Left=0 turns +, Right=1 turns -
                f << "    afn_tank_camera = 1;\n";            // Turn Player implies tank controls
                // Movement toggle (node paramInt[0], left-click property):
                // 0 = Tank (Heading) — MovePlayer axes follow the heading
                // (classic tank: after turning around, "up" walks toward the
                // camera). 1 = Camera Relative — TurnPlayer only steers the
                // facing; movement stays camera-relative.
                f << "    afn_tank_move = " << (a->paramInt[0] == 1 ? 0 : 1) << ";\n";
                // Turn rate scales by afn_key_mag like OrbitCamera — stick-
                // bound turning ramps with deflection, buttons stay full rate.
                f << "    afn_player_heading " << sign << "= (" << speed << " * afn_key_mag) >> 8;\n";
                break;
            }
            case GBAScriptNodeType::LockOnTarget: {
                // Lock-on camera assist (PSV, AFN_HAS_CAM_LOCK): sets the lock
                // target. Gate with Is In View if you only want to lock things
                // on-screen. Target pin: Attached Sprite = the BP owner ("self",
                // guarded so ownerless instances don't stomp it), Object = a
                // specific sprite; unwired in a BP also means self.
                auto* tgt = findDataIn(a->id, 0);
                // Framing params (left-click): paramInt[0] zoom %, paramInt[1]
                // side offset in EDITOR units (->/4 world px). 0 = tuned default.
                int lkZoom = a->paramInt[0] > 0 ? a->paramInt[0] : 18;
                int lkSide = a->paramInt[1] > 0 ? a->paramInt[1] : 32;
                int lkHeight = a->paramInt[3] > 0 ? a->paramInt[3] : 32;   // editor units (->/4 world px)
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                if (tgt && tgt->type != GBAScriptNodeType::AttachedSprite)
                    f << "    afn_cam_lock_target = " << resolveInt(tgt) << ";\n";
                else if (curScript != &script || (tgt && tgt->type == GBAScriptNodeType::AttachedSprite))
                    f << "    if (afn_bp_cur_spr_idx >= 0) afn_cam_lock_target = afn_bp_cur_spr_idx;\n";
                else
                    f << "    afn_cam_lock_target = -1;\n";   // scene script, no target wired
                f << "    afn_lock_zoom = " << lkZoom << ";\n";
                f << "    afn_lock_side = " << (lkSide / 4) << ";\n";
                // paramInt[2] is a bitfield: bit0 = zoom in/out, bit1 = no look-down.
                f << "    afn_lock_zoom_in = " << (a->paramInt[2] & 1) << ";\n";
                f << "    afn_lock_height = " << (lkHeight / 4) << ";\n";
                f << "    afn_lock_no_lookdown = " << ((a->paramInt[2] >> 1) & 1) << ";\n";
                f << "#endif\n";
                break;
            }
            case GBAScriptNodeType::ReleaseLockOn: {
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_cam_lock_target = -1;\n";
                f << "    afn_lock_strafe = 0;\n";   // dropping the lock drops Z-targeting movement
                f << "#endif\n";
                break;
            }
            case GBAScriptNodeType::LockStrafe: {
                // Z-targeting movement: only does anything while a Lock On
                // target is active (afn_cam_lock_target >= 0).
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_lock_strafe = 1;\n";
                f << "#endif\n";
                break;
            }
            case GBAScriptNodeType::CastEffect: {
                auto* objData = findDataIn(a->id, 0);
                std::string obj;
                if (objData) obj = std::to_string(resolveInt(objData));
                else if (curScript != &script) obj = "afn_bp_cur_spr_idx";   // self (blueprint owner)
                else obj = "0";
                // Show every hidden attached sprite parented to the target; the
                // runtime restarts its anim on show and auto-hides at one-shot end.
                f << "    { int _i; for (_i = 0; _i < NUM_SPRITES; _i++)\n";
                f << "        if (afn_sprite_data[_i][12] == " << obj << " && afn_sprite_start_hidden[_i]) afn_sprite_visible[_i] = 1; }\n";
                break;
            }
            default:
                f << "    /* TODO: emit node type " << (int)a->type << " */\n";
                break;
            }
        };

        // Recursive emit: walk exec links from a node as a tree, not a flat
        // list. Each gate/CheckFlag/OnRise scopes its own downstream subtree;
        // siblings (multiple exec-out targets from same pin) emit as separate
        // top-level blocks. Visited-set prevents repeat emission when two
        // parents wire to the same action.
        std::set<int> emitVisited;
        std::function<void(const GBAScriptNodeExport*)> emitOne;
        // Sibling exec targets fan out in link declaration order, but gates
        // (IsJumping, IsOnGround, ...) READ state that sibling action nodes
        // (Jump, SetVelocityY, ...) WRITE on the same frame. Walking gates
        // first means they see stale state — jump SFX gated by IsJumping
        // never fires because Jump hasn't run yet. Sort action nodes before
        // gate nodes so writers run before readers.
        auto isGateType = [](GBAScriptNodeType t) -> bool {
            return t == GBAScriptNodeType::IsMoving ||
                   t == GBAScriptNodeType::IsOnGround ||
                   t == GBAScriptNodeType::IsJumping ||
                   t == GBAScriptNodeType::IsFalling ||
                   t == GBAScriptNodeType::IsNear2D ||
                   t == GBAScriptNodeType::IsFollowMoving ||
                   t == GBAScriptNodeType::CheckFlag ||
                   t == GBAScriptNodeType::IsFlagSet ||
                   t == GBAScriptNodeType::FlipFlop ||
                   t == GBAScriptNodeType::OnRise ||
                   t == GBAScriptNodeType::Countdown ||
                   t == GBAScriptNodeType::IsLockedOn ||
                   t == GBAScriptNodeType::IsNotLockedOn ||
                   t == GBAScriptNodeType::IsInView;
        };
        auto walkExec = [&](int nodeId, int pinIdx) {
            auto targets = findExecOuts(nodeId, pinIdx);
            std::stable_partition(targets.begin(), targets.end(), [&](int t) {
                auto* n = findNode(t);
                return n && !isGateType(n->type);
            });
            for (int t : targets) {
                if (emitVisited.count(t)) continue;
                emitVisited.insert(t);
                auto* n = findNode(t);
                if (n) emitOne(n);
            }
        };
        emitOne = [&](const GBAScriptNodeExport* a) {
            if (a->type == GBAScriptNodeType::CheckFlag) {
                auto* fd = findDataIn(a->id, 0);
                int flag = fd ? resolveInt(fd) : a->paramInt[0];
                f << "    if (afn_flags & (1u << " << flag << ")) {\n";
                walkExec(a->id, 0);
                auto clearTargets = findExecOuts(a->id, 1);
                if (!clearTargets.empty()) {
                    f << "    } else {\n";
                    walkExec(a->id, 1);
                }
                f << "    }\n";
                return;
            }
            if (a->type == GBAScriptNodeType::IsFlagSet) {
                // Gate-only (single exec out). True branch walks pin 0.
                auto* fd = findDataIn(a->id, 0);
                int flag = fd ? resolveInt(fd) : a->paramInt[0];
                f << "    if (afn_flags & (1u << " << flag << ")) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == GBAScriptNodeType::FlipFlop) {
                // Per-node static toggle: alternates A/B exec on each call.
                // Without this the editor's "toggle menu open/close" pattern
                // never reaches the B (HideHUD/UnfreezePlayer) branch.
                f << "    { static int afn_ff_" << a->id << " = 0;\n";
                f << "      afn_ff_" << a->id << " = !afn_ff_" << a->id << ";\n";
                f << "      if (afn_ff_" << a->id << ") {\n";
                walkExec(a->id, 0);
                f << "      } else {\n";
                walkExec(a->id, 1);
                f << "      } }\n";
                return;
            }
            if (a->type == GBAScriptNodeType::OnRise) {
                f << "    if (afn_rise_" << a->id << " >= afn_frame_count - 1) { afn_rise_" << a->id << " = afn_frame_count; }\n";
                f << "    else { afn_rise_" << a->id << " = afn_frame_count;\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == GBAScriptNodeType::Countdown) {
                // Per-node static counter; downstream fires when it hits 0
                // (then auto-resets so the gate repeats).
                auto* cntData = findDataIn(a->id, 0);
                int cnt = cntData ? resolveInt(cntData) : 60;
                f << "    { static int afn_cd_" << a->id << " = " << cnt << ";\n";
                f << "      if (--afn_cd_" << a->id << " <= 0) { afn_cd_" << a->id << " = " << cnt << ";\n";
                walkExec(a->id, 0);
                f << "    } }\n";
                return;
            }
            bool isGate = (a->type == GBAScriptNodeType::IsMoving ||
                           a->type == GBAScriptNodeType::IsOnGround ||
                           a->type == GBAScriptNodeType::IsJumping ||
                           a->type == GBAScriptNodeType::IsFalling ||
                           a->type == GBAScriptNodeType::IsNear2D ||
                           a->type == GBAScriptNodeType::IsFollowMoving ||
                           a->type == GBAScriptNodeType::IsGrinding ||
                           a->type == GBAScriptNodeType::IsNotGrinding ||
                           a->type == GBAScriptNodeType::IsLockedOn ||
                           a->type == GBAScriptNodeType::IsNotLockedOn ||
                           a->type == GBAScriptNodeType::IsInView);
            if (isGate) {
                bool wasJump = inJumpGate;
                gateDepth++;
                emitAction(a);  // emits "if (cond) {\n" and updates inJumpGate
                walkExec(a->id, 0);
                f << "    }\n";
                gateDepth--;
                inJumpGate = wasJump;
                return;
            }
            emitAction(a);
            walkExec(a->id, 0);
        };

        auto emitChain = [&](const Chain& c) {
            inJumpGate = false;
            gateDepth = 0;
            emitVisited.clear();
            walkExec(c.event->id, 0);
        };

        // Emit each dispatcher function with the matching chains inlined.
        // Key events may have multiple key data inputs wired (e.g. "any of
        // LEFT/RIGHT/UP/DOWN"); OR them together when present, fall back to
        // event->paramInt[0] otherwise. Mirrors gba_package.cpp resolveEventKey.
        auto emitDispatcher = [&](const char* fname, GBAScriptNodeType evType,
                                  const char* keyCheck) {
            f << "static void " << fname << "(void) {\n";
            for (auto& c : chains) {
                if (c.event->type != evType) continue;
                if (keyCheck) {
                    std::vector<int> keys;
                    for (auto& l : curScript->links)
                        if (l.toNodeId == c.event->id && l.toPinType == 3 && l.toPinIdx == 0) {
                            auto* dn = findNode(l.fromNodeId);
                            if (dn) keys.push_back(dn->paramInt[0]);
                        }
                    if (keys.empty()) keys.push_back(c.event->paramInt[0]);
                    f << "    if (";
                    for (size_t ki = 0; ki < keys.size(); ki++) {
                        if (ki) f << " || ";
                        f << keyCheck << "(" << keyName(keys[ki]) << ")";
                    }
                    f << ") {\n";
                    // Chain-entry key magnitude: stick keys pass their analog
                    // deflection (afn_stick_mag, 0..256) to MovePlayer et al;
                    // buttons are full-on. Guarded like afn_stick_sens so
                    // stick-less runtimes (NDS) compile the chain unchanged
                    // (their afn_key_mag stays 256).
                    // (Not for key_released: the stick is back under the
                    // threshold by then, so its magnitude reads 0 — released
                    // chains run full-on.)
                    bool anyStick = false;
                    for (int kk : keys) if (kk >= 12 && kk <= 19) anyStick = true;
                    if (anyStick && strcmp(keyCheck, "key_released") != 0) {
                        f << "#ifdef AFN_HAS_STICK_SENS\n";
                        if (keys.size() == 1) {
                            f << "        afn_key_mag = afn_stick_mag[" << (keys[0] - 12) << "];\n";
                        } else {
                            // Multiple keys OR'd: strongest held source wins.
                            f << "        afn_key_mag = 0;\n";
                            for (int kk : keys) {
                                if (kk >= 12 && kk <= 19)
                                    f << "        if (afn_stick_mag[" << (kk - 12)
                                      << "] > afn_key_mag) afn_key_mag = afn_stick_mag[" << (kk - 12) << "];\n";
                                else
                                    f << "        if (key_is_down(" << keyName(kk) << ")) afn_key_mag = 256;\n";
                            }
                        }
                        f << "#endif\n";
                    } else {
                        f << "        afn_key_mag = 256;\n";
                    }
                    emitChain(c);
                    f << "    }\n";
                } else if (evType == GBAScriptNodeType::OnCollision) {
                    // Optional Radius pin (pin 0): per-bp axis-aligned gate
                    // tighter than the outer 24px afn_collided_sprite trigger.
                    auto* radData = findDataIn(c.event->id, 0);
                    if (radData) {
                        int rad = resolveInt(radData);
                        f << "    if (afn_collided_sprite >= 0) {\n";
                        f << "        int _dx = player_x - afn_sprite_data[afn_collided_sprite][0];\n";
                        f << "        int _dz = player_z - afn_sprite_data[afn_collided_sprite][2];\n";
                        f << "        if (_dx < 0) _dx = -_dx; if (_dz < 0) _dz = -_dz;\n";
                        f << "        if ((_dx >> 8) < " << rad << " && (_dz >> 8) < " << rad << ") {\n";
                        emitChain(c);
                        f << "        }\n";
                        f << "    }\n";
                    } else {
                        emitChain(c);
                    }
                } else {
                    emitChain(c);
                }
            }
            f << "}\n";
        };

        f << "\n// ---- Generated script code from visual node graph ----\n";
        // Stick-direction sensitivity (Key node "Sensitivity" slider, stored in
        // paramInt[1]; 0 = unset -> runtime default 48). Collected across the
        // scene script and all blueprints, emitted into the init hook guarded
        // by AFN_HAS_STICK_SENS so only runtimes with analog sticks (PSV)
        // compile it — NDS never defines the macro. The emitted value is the
        // trip threshold on the 0..127 axis range (lower = more sensitive);
        // if two Key nodes tune the same direction, the last one wins.
        f << "static void afn_emitted_script_init(void)         {\n";
        f << "#ifdef AFN_HAS_STICK_SENS\n";
        {
            auto emitSens = [&](const GBAScriptNodeExport& n) {
                if (n.type != GBAScriptNodeType::Key) return;
                int key = n.paramInt[0], sens = n.paramInt[1], str = n.paramInt[2];
                if (key < 12 || key > 19) return;
                if (sens > 0) {
                    if (sens > 100) sens = 100;
                    int thr = 8 + ((100 - sens) * 112) / 100;   // sens 64% -> 48 (old fixed deadzone)
                    f << "    afn_stick_sens[" << (key - 12) << "] = " << thr
                      << ";   // " << keyName(key) << " sensitivity " << sens << "%\n";
                }
                // Strength slider (paramInt[2], 0 = unset -> 100%): scales the
                // analog ramp output, so a full push moves at strength% speed.
                if (str > 0) {
                    if (str > 100) str = 100;
                    f << "    afn_stick_strength[" << (key - 12) << "] = " << (str * 256) / 100
                      << ";   // " << keyName(key) << " strength " << str << "%\n";
                }
            };
            for (const auto& n : script.nodes) emitSens(n);
            for (const auto& bp : blueprints)
                for (const auto& n : bp.script.nodes) emitSens(n);
        }
        f << "#endif\n";
        f << "}\n";
        emitDispatcher("afn_emitted_script_start",        GBAScriptNodeType::OnStart,        nullptr);
        emitDispatcher("afn_emitted_script_update",       GBAScriptNodeType::OnUpdate,       nullptr);
        emitDispatcher("afn_emitted_script_key_held",     GBAScriptNodeType::OnKeyHeld,      "key_is_down");
        emitDispatcher("afn_emitted_script_key_pressed",  GBAScriptNodeType::OnKeyPressed,   "key_hit");
        emitDispatcher("afn_emitted_script_key_released", GBAScriptNodeType::OnKeyReleased,  "key_released");
        emitDispatcher("afn_emitted_script_collision",    GBAScriptNodeType::OnCollision,    nullptr);
        emitDispatcher("afn_emitted_script_collision2d",  GBAScriptNodeType::OnCollision2D,  nullptr);

        // Blueprint event handlers — one set of named functions per blueprint.
        // Per-instance state (current sprite/tm-obj) lives in afn_bp_cur_*.
        for (size_t bi = 0; bi < blueprints.size(); bi++) {
            curScript = &blueprints[bi].script;
            buildChains();
            char fn[64];
            snprintf(fn, sizeof(fn), "afn_bp%zu_start",        bi);
            emitDispatcher(fn, GBAScriptNodeType::OnStart, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_update",       bi);
            emitDispatcher(fn, GBAScriptNodeType::OnUpdate, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_held",     bi);
            emitDispatcher(fn, GBAScriptNodeType::OnKeyHeld, "key_is_down");
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_pressed",  bi);
            emitDispatcher(fn, GBAScriptNodeType::OnKeyPressed, "key_hit");
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_released", bi);
            emitDispatcher(fn, GBAScriptNodeType::OnKeyReleased, "key_released");
            snprintf(fn, sizeof(fn), "afn_bp%zu_collision",    bi);
            emitDispatcher(fn, GBAScriptNodeType::OnCollision, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_collision2d",  bi);
            emitDispatcher(fn, GBAScriptNodeType::OnCollision2D, nullptr);
        }
        curScript = &script;  // restore so any later helpers behave

        // Blueprint instance table + dispatchers. Each dispatcher iterates
        // instances, sets afn_bp_cur_spr_idx (so emitted code can reference
        // its owning sprite), and calls the matching bp's handler.
        f << "\n#define AFN_BP_COUNT "     << (int)blueprints.size() << "\n";
        f << "#define AFN_BP_INSTANCE_COUNT " << (int)bpInstances.size() << "\n";
        if (!bpInstances.empty()) {
            // Row: { bpIdx, spriteIdx, tmObjIdx, sceneMode, sceneMask }.
            // tmObjIdx: -1 if instance is a 3D sprite, else index into the
            // scene's tm_objects (used by collision2d dispatch).
            // sceneMode: 0 = Mode 4 (3D), 1 = Mode 0 (tilemap), 2 = Mode 7.
            // sceneMask: bit N set = instance lives in scene N (0xFFFFFFFF = all).
            f << "static const unsigned int afn_bp_instances[" << (int)bpInstances.size() << "][5] = {\n";
            for (const auto& inst : bpInstances)
                f << "    { " << inst.blueprintIdx << ", " << (unsigned)(int)inst.spriteIdx
                  << ", " << (unsigned)(int)inst.tmObjIdx
                  << ", " << inst.sceneMode << ", 0x" << std::hex << inst.sceneMask << "u" << std::dec << " },\n";
            f << "};\n";
        }
        // Per-event bp dispatcher. Emits a switch on bpIdx so each instance
        // calls into the right blueprint's handler with cur_spr_idx set.
        // sceneGate: true → skip instances that don't match the current
        // (afn_current_mode, afn_current_scene). gateExpr is an additional
        // C expression evaluated per-instance.
        auto emitBpDispatcher = [&](const char* fname, const char* suffix,
                                    const char* gateExpr, bool sceneGate) {
            f << "static void " << fname << "(void) {\n";
            if (!bpInstances.empty()) {
                f << "    extern int afn_current_mode;\n";
                f << "    extern int afn_current_scene;\n";
                f << "    for (int i = 0; i < AFN_BP_INSTANCE_COUNT; i++) {\n";
                if (sceneGate) {
                    f << "        int instMode = (int)afn_bp_instances[i][3];\n";
                    f << "        if (instMode >= 0 && instMode != afn_current_mode) continue;\n";
                    f << "        unsigned int mask = afn_bp_instances[i][4];\n";
                    f << "        if (mask != 0xFFFFFFFFu && !(mask & (1u << afn_current_scene))) continue;\n";
                }
                if (gateExpr) f << "        if (!(" << gateExpr << ")) continue;\n";
                f << "        int bpIdx = afn_bp_instances[i][0];\n";
                f << "        afn_bp_cur_spr_idx = (int)afn_bp_instances[i][1];\n";
                f << "        afn_bp_cur_tm_obj  = (int)afn_bp_instances[i][2];\n";
                f << "        switch (bpIdx) {\n";
                for (size_t bi = 0; bi < blueprints.size(); bi++)
                    f << "            case " << bi << ": afn_bp" << bi << "_" << suffix << "(); break;\n";
                f << "        }\n";
                f << "    }\n";
                f << "    afn_bp_cur_spr_idx = -1;\n";
                f << "    afn_bp_cur_tm_obj  = -1;\n";
            }
            f << "}\n";
        };
        // OnStart: scene-gated so re-firing on scene swap only triggers BPs
        // that belong to the new scene (e.g. a scene-1 song doesn't start
        // playing during the scene-0 splash).
        emitBpDispatcher("afn_bp_dispatch_start",        "start",        nullptr, true);
        // Per-frame / input / collision events: also scene-gated so menu
        // BPs don't react to keys outside their scene, etc.
        emitBpDispatcher("afn_bp_dispatch_update",       "update",       nullptr, true);
        emitBpDispatcher("afn_bp_dispatch_key_held",     "key_held",     nullptr, true);
        emitBpDispatcher("afn_bp_dispatch_key_pressed",  "key_pressed",  nullptr, true);
        emitBpDispatcher("afn_bp_dispatch_key_released", "key_released", nullptr, true);
        emitBpDispatcher("afn_bp_dispatch_collision",    "collision",
                         "(int)afn_bp_instances[i][1] == afn_collided_sprite", true);
        // Mode 0 collision: gate on this instance's tmObjIdx matching the
        // tm_object the player just walked into (set by mode0 movement).
        emitBpDispatcher("afn_bp_dispatch_collision2d",  "collision2d",
                         "(int)afn_bp_instances[i][2] == afn_collided_tm_obj", true);
    } else {
        // No scripts in this build — empty stubs keep script_glue.c linkable.
        f << "\n// Script dispatchers — no scripts in this build.\n";
        f << "static inline void afn_emitted_script_init(void)         {}\n";
        f << "static inline void afn_emitted_script_start(void)        {}\n";
        f << "static inline void afn_emitted_script_update(void)       {}\n";
        f << "static inline void afn_emitted_script_key_held(void)     {}\n";
        f << "static inline void afn_emitted_script_key_pressed(void)  {}\n";
        f << "static inline void afn_emitted_script_key_released(void) {}\n";
        f << "static inline void afn_emitted_script_collision(void)    {}\n";
        f << "static inline void afn_emitted_script_collision2d(void)  {}\n";
        f << "static inline void afn_bp_dispatch_start(void)           {}\n";
        f << "static inline void afn_bp_dispatch_update(void)          {}\n";
        f << "static inline void afn_bp_dispatch_key_held(void)        {}\n";
        f << "static inline void afn_bp_dispatch_key_pressed(void)     {}\n";
        f << "static inline void afn_bp_dispatch_key_released(void)    {}\n";
        f << "static inline void afn_bp_dispatch_collision(void)       {}\n";
        f << "static inline void afn_bp_dispatch_collision2d(void)     {}\n";
    }
}

} // namespace Affinity

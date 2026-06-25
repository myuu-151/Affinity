// Affinity visual-script node codegen — node graph -> C. SHARED by the NDS and
// PS Vita exporters (extracted verbatim from nds_package.cpp so both emit the
// SAME C). The emitted code is platform-neutral (afn_* vars, KEY_*); each
// runtime defines the variables it consumes and leaves the rest inert.
#include "node_script_emit.h"
#include "afn_export_ir.h"
#include <vector>
#include <set>
#include <string>
#include <cstring>
#include <functional>
#include <algorithm>

namespace Affinity {

void EmitNodeScriptBodies(std::ostream& f,
                          const AfnScriptExport& script,
                          const std::vector<AfnBlueprintExport>& blueprints,
                          const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                          const std::vector<AfnSpriteExport>& sprites,
                          const std::vector<AfnSoundInstanceExport>& soundInstances,
                          const std::vector<int>& hudLayerRemap,
                          const std::vector<int>& hudLayerCount) {
    // Translate a node's flat editor-layer index to the runtime afn_hud_layer[]
    // range [first, first+count). Identity (count 1) when no remap is supplied.
    auto remapLayer = [&](int F) -> int {
        if (F >= 0 && F < (int)hudLayerRemap.size()) return hudLayerRemap[F];
        return F;
    };
    auto remapCount = [&](int F) -> int {
        if (F >= 0 && F < (int)hudLayerCount.size()) return hudLayerCount[F];
        return 1;
    };
    bool hasAnyScript = !script.nodes.empty() || !blueprints.empty();
    if (hasAnyScript) {
        // OnRise edge-detect state — one int per OnRise node across the scene
        // script AND every blueprint (file-scope, persists between frames). Must be
        // declared before the emitted functions reference afn_rise_<id>. The GBA/
        // NDS exporters declare these in their own package files; the PSV/PSP path
        // routes through this shared emitter, so declare them here.
        {
            std::set<int> riseIds;
            for (auto& n : script.nodes)
                if (n.type == AfnScriptNodeType::OnRise) riseIds.insert(n.id);
            for (auto& bp : blueprints)
                for (auto& n : bp.script.nodes)
                    if (n.type == AfnScriptNodeType::OnRise) riseIds.insert(n.id);
            for (int rid : riseIds)
                f << "static int afn_rise_" << rid << " = -2;\n";
            if (!riseIds.empty()) f << "\n";
        }
        // curScript = which graph (inline scene or a blueprint) the lambdas
        // operate on; swapped before each blueprint emit pass below.
        const AfnScriptExport* curScript = &script;
        auto findNode = [&](int id) -> const AfnScriptNodeExport* {
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
        auto findDataIn = [&](int nodeId, int pinIdx) -> const AfnScriptNodeExport* {
            for (auto& l : curScript->links)
                if (l.toNodeId == nodeId && l.toPinType == 3 && l.toPinIdx == pinIdx)
                    return findNode(l.fromNodeId);
            return nullptr;
        };
        auto resolveInt = [&](const AfnScriptNodeExport* dn) -> int {
            if (!dn) return 0;
            if (dn->type == AfnScriptNodeType::Animation) return dn->paramInt[1];
            if (dn->type == AfnScriptNodeType::SkelAnim) return dn->paramInt[1]; // clip index
            return dn->paramInt[0];
        };
        auto resolveFloat = [&](const AfnScriptNodeExport* dn) -> float {
            if (!dn) return 0.0f;
            // Integer nodes store their value in paramInt[0] as a raw int.
            // Bit-casting that as float gives a denormal (≈ 0), so when the
            // user wires an Integer literal to a Float pin we must convert
            // rather than reinterpret. Float nodes already stored their
            // value bit-cast in paramInt[0] so the memcpy path is correct.
            if (dn->type == AfnScriptNodeType::Integer)
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

        // Recursive runtime-expression generator for the logic/flow nodes
        // (Branch, Is True, Is False, Switch on Int). Turns a data-node subtree
        // into a C expression string referencing LIVE runtime globals — unlike
        // resolveInt, which bakes a constant data node to a single int at export
        // time. Only the PSV/PSP runtime globals listed below are referenceable;
        // any unknown data source falls back to its stored constant. This is
        // what makes Compare / And / Or / Not / Select actually do something on
        // PSV (they were GBA-era data nodes with no runtime consumer here).
        std::function<std::string(const AfnScriptNodeExport*)> emitIntExpr;
        // Expression for a data INPUT pin: the wired child subtree if connected,
        // else the node's own inline literal stored in paramInt[pin].
        auto argExpr = [&](const AfnScriptNodeExport* n, int pin) -> std::string {
            const AfnScriptNodeExport* c = findDataIn(n->id, pin);
            if (c) return emitIntExpr(c);
            return "(" + std::to_string(n->paramInt[pin]) + ")";
        };
        emitIntExpr = [&](const AfnScriptNodeExport* dn) -> std::string {
            if (!dn) return "0";
            using T = AfnScriptNodeType;
            switch (dn->type) {
                // constant leaves
                case T::Integer: return "(" + std::to_string(dn->paramInt[0]) + ")";
                case T::Bool:    return dn->paramInt[0] ? "1" : "0";
                case T::Float:   return "(" + std::to_string((int)resolveFloat(dn)) + ")";
                // runtime value sources (PSV/PSP globals)
                case T::GetPlayerX: return "player_x";
                case T::GetPlayerY: return "player_y";
                case T::GetPlayerZ: return "player_z";
                case T::GetScore:   return "afn_score";
                case T::GetEnergy:  return "afn_energy";
                case T::GetHealth:  return "afn_health";
                case T::GetChargePct: return "((afn_fb_max > 0) ? (int)(afn_fb_level * 100.0f / afn_fb_max) : 0)";
                case T::GetCursorStop: return "afn_cursor_stop";
                case T::GetTime:    return "afn_frame_count";
                case T::GetLastKey: return "afn_last_key";
                case T::GetRandom:  return "((int)(afn_rng & 0xFF))";
                case T::GetHP:
                case T::GetHP2:     return "afn_hp[" + argExpr(dn, 0) + "]";
                case T::GetFlag:    return "((afn_flags >> " + argExpr(dn, 0) + ") & 1u)";
                case T::IsKeyDown: {
                    const AfnScriptNodeExport* kc = findDataIn(dn->id, 0);
                    int ki = kc ? kc->paramInt[0] : dn->paramInt[0];
                    return std::string("key_is_down(") + keyName(ki) + ")";
                }
                // math operators
                case T::AddMath:      return "(" + argExpr(dn,0) + " + " + argExpr(dn,1) + ")";
                case T::SubtractMath: return "(" + argExpr(dn,0) + " - " + argExpr(dn,1) + ")";
                case T::MultiplyMath: return "(" + argExpr(dn,0) + " * " + argExpr(dn,1) + ")";
                case T::Divide:    { std::string b=argExpr(dn,1); return "((" + b + ")!=0 ? " + argExpr(dn,0) + "/" + b + " : 0)"; }
                case T::ModuloMath:{ std::string b=argExpr(dn,1); return "((" + b + ")!=0 ? " + argExpr(dn,0) + "%" + b + " : 0)"; }
                case T::NegateMath:  return "(-" + argExpr(dn,0) + ")";
                case T::AbsMath:   { std::string a=argExpr(dn,0); return "((" + a + ")<0 ? -" + a + " : " + a + ")"; }
                case T::MinMath:   { std::string a=argExpr(dn,0),b=argExpr(dn,1); return "((" + a + ")<(" + b + ") ? " + a + " : " + b + ")"; }
                case T::MaxMath:   { std::string a=argExpr(dn,0),b=argExpr(dn,1); return "((" + a + ")>(" + b + ") ? " + a + " : " + b + ")"; }
                case T::Average:     return "((" + argExpr(dn,0) + " + " + argExpr(dn,1) + ")/2)";
                case T::SignMath:  { std::string a=argExpr(dn,0); return "((" + a + ")>0 ? 1 : ((" + a + ")<0 ? -1 : 0))"; }
                case T::ClampMath: { std::string v=argExpr(dn,0),lo=argExpr(dn,1),hi=argExpr(dn,2);
                                     return "((" + v + ")<(" + lo + ") ? " + lo + " : ((" + v + ")>(" + hi + ") ? " + hi + " : " + v + "))"; }
                // comparison / boolean logic
                case T::CompareInt: { const char* ops[] = { "==","!=","<",">","<=",">=" };
                                      int op = dn->paramInt[0]; if (op < 0 || op > 5) op = 0;
                                      return "((" + argExpr(dn,0) + ") " + ops[op] + " (" + argExpr(dn,1) + "))"; }
                case T::AndLogic:  return "((" + argExpr(dn,0) + ") && (" + argExpr(dn,1) + "))";
                case T::OrLogic:   return "((" + argExpr(dn,0) + ") || (" + argExpr(dn,1) + "))";
                case T::NotLogic:  return "(!(" + argExpr(dn,0) + "))";
                case T::Xor:       return "((!!(" + argExpr(dn,0) + ")) ^ (!!(" + argExpr(dn,1) + ")))";
                case T::Select:    return "((" + argExpr(dn,0) + ") ? (" + argExpr(dn,1) + ") : (" + argExpr(dn,2) + "))";
                default:           return "(" + std::to_string(resolveInt(dn)) + ")";
            }
        };

        // Collect chains per event type (BFS from each event node through exec links).
        // Re-runnable: blueprint emit passes call buildChains() after pointing
        // curScript at the blueprint's graph.
        struct Chain { const AfnScriptNodeExport* event; std::vector<const AfnScriptNodeExport*> actions; };
        std::vector<Chain> chains;
        auto buildChains = [&]() {
            chains.clear();
            for (auto& n : curScript->nodes) {
                auto et = n.type;
                if (et != AfnScriptNodeType::OnUpdate &&
                    et != AfnScriptNodeType::OnStart &&
                    et != AfnScriptNodeType::OnKeyHeld &&
                    et != AfnScriptNodeType::OnKeyPressed &&
                    et != AfnScriptNodeType::OnKeyReleased &&
                    et != AfnScriptNodeType::OnCollision &&
                    et != AfnScriptNodeType::OnCollision2D)
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
                    if (an->type == AfnScriptNodeType::CheckFlag ||
                        an->type == AfnScriptNodeType::OnRise ||
                        an->type == AfnScriptNodeType::FlipFlop ||
                        an->type == AfnScriptNodeType::IsFlagSet ||
                        an->type == AfnScriptNodeType::Branch ||
                        an->type == AfnScriptNodeType::IsTrue ||
                        an->type == AfnScriptNodeType::IsFalse ||
                        an->type == AfnScriptNodeType::SwitchInt) continue;
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
        auto emitAction = [&](const AfnScriptNodeExport* a) {
            switch (a->type) {
            case AfnScriptNodeType::Walk: {
                // Walk is the LOW-priority speed. If a Sprint already set the
                // speed this frame (afn_speed_prio), don't clobber it — so
                // holding B (sprint) while also holding a direction (which
                // fires Walk) stays at sprint speed regardless of node order.
                auto* d = findDataIn(a->id, 0);
                if (d) f << "    if (!afn_speed_prio) afn_move_speed = " << (int)(resolveInt(d) * 37.0f / 35.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::Sprint: {
                // Sprint is HIGH priority — sets the speed and locks out Walk
                // for the rest of this frame.
                auto* d = findDataIn(a->id, 0);
                if (d) f << "    afn_move_speed = " << (int)(resolveInt(d) * 37.0f / 35.0f) << "; afn_speed_prio = 1;\n";
                break;
            }
            case AfnScriptNodeType::Jump: {
                auto* d = findDataIn(a->id, 0);
                float force = d ? resolveFloat(d) : 2.0f;
                f << "    if (player_on_ground) player_vy = " << (int)(force * 256.0f) << ";\n";
                // Anime-jump shaping (PSV): Fall Force pin = extra downward accel
                // past the apex; node sliders paramInt[1]=Rise Float % (gravity
                // removed while rising) and paramInt[0]=Fall Smooth (ease-in
                // frames). Guarded so NDS/PSP/GBA (no globals) are unaffected.
                auto* ff = findDataIn(a->id, 1);
                int fallForce = ff ? (int)(resolveFloat(ff) * 256.0f) : 0;
                int smooth    = a->paramInt[0];
                int riseFloat = a->paramInt[1];
                if (fallForce || smooth || riseFloat) {
                    f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                    f << "    afn_fall_force = " << fallForce << ";\n";
                    f << "    afn_rise_float = " << riseFloat << ";\n";
                    f << "    afn_fall_smooth = " << smooth << ";\n";
                    f << "#endif\n";
                }
                break;
            }
            case AfnScriptNodeType::SetGravity: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.09f;
                f << "    afn_gravity = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::SetMaxFall: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 6.0f;
                f << "    afn_terminal_vel = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::AutoOrbit: {
                auto* d = findDataIn(a->id, 0);
                f << "    afn_auto_orbit_speed = " << (d ? resolveInt(d) : 205) << ";\n";
                break;
            }
            case AfnScriptNodeType::OrbitCamera: {
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
            case AfnScriptNodeType::MovePlayer: {
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
            case AfnScriptNodeType::PlayAnim: {
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
            case AfnScriptNodeType::PlaySkelAnim: {
                // Set the skeletal (glTF/DSMA) clip the player rig plays in Mode 4.
                // The clip index comes from a wired Skeletal Animation data node.
                auto* d = findDataIn(a->id, 0);
                int clip = d ? resolveInt(d) : 0;
                f << "    afn_rig_clip = " << clip << ";\n";
                break;
            }
            case AfnScriptNodeType::SetSkelAnim: {
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
            case AfnScriptNodeType::HoldSkelClip: {
                // Like SetSkelAnim, but the NPC plays the clip ONCE and freezes the
                // last frame (a die-collapse). Object = an Object, or Attached Sprite
                // (self). Wire On Update + Is HP Zero(self) to it.
                auto* objData  = findDataIn(a->id, 0);
                auto* clipData = findDataIn(a->id, 1);
                int clip = clipData ? resolveInt(clipData) : 0;
                if (objData && objData->type == AfnScriptNodeType::AttachedSprite)
                    f << "    afn_skel_anim_obj = afn_bp_cur_spr_idx;";
                else
                    f << "    afn_skel_anim_obj = " << (objData ? resolveInt(objData) : 0) << ";";
                f << " afn_skel_anim_clip = " << clip << "; afn_skel_anim_hold = 1;\n";
                break;
            }
            case AfnScriptNodeType::FreezePlayer:
                f << "    afn_player_frozen = 1; afn_play_anim = -1;\n";
                break;
            case AfnScriptNodeType::UnfreezePlayer:
                f << "    afn_player_frozen = 0;\n";
                break;
            case AfnScriptNodeType::CursorUp:
                f << "    if (afn_cursor_stop > 0) afn_cursor_stop--;\n";
                f << "    else afn_cursor_stop = afn_stop_count - 1;\n";
                break;
            case AfnScriptNodeType::CursorDown:
                f << "    afn_cursor_stop++;\n";
                f << "    if (afn_cursor_stop >= afn_stop_count) afn_cursor_stop = 0;\n";
                break;
            case AfnScriptNodeType::FollowLink:
                f << "    { int link = afn_stop_links[afn_cursor_stop];\n";
                f << "      if (link >= 0) { afn_hud_visible[afn_elem_idx] = 0; afn_hud_visible[link] = 1; afn_active_element = link; } }\n";
                break;
            case AfnScriptNodeType::SetVisible: {
                auto* sprData = findDataIn(a->id, 0);
                auto* visData = findDataIn(a->id, 1);
                int sIdx = sprData ? resolveInt(sprData) : a->paramInt[0];
                int vis  = visData ? resolveInt(visData) : a->paramInt[1];
                f << "    if ((unsigned)" << sIdx << " < NUM_SPRITES) afn_sprite_visible["
                  << sIdx << "] = " << (vis ? 1 : 0) << ";\n";
                break;
            }
            case AfnScriptNodeType::DestroyObject: {
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
            case AfnScriptNodeType::SetFlag: {
                auto* flagData = findDataIn(a->id, 0);
                auto* valData  = findDataIn(a->id, 1);
                int flag = flagData ? resolveInt(flagData) : a->paramInt[0];
                int val  = valData  ? resolveInt(valData)  : a->paramInt[1];
                if (val) f << "    afn_flags |=  (1u << " << flag << ");\n";
                else     f << "    afn_flags &= ~(1u << " << flag << ");\n";
                break;
            }
            case AfnScriptNodeType::ToggleFlag:
                f << "    afn_flags ^= (1u << " << a->paramInt[0] << ");\n";
                break;
            case AfnScriptNodeType::ScreenShake: {
                auto* d0 = findDataIn(a->id, 0);
                auto* d1 = findDataIn(a->id, 1);
                f << "    afn_shake_intensity = " << (d0 ? resolveInt(d0) : 4) << ";\n";
                f << "    afn_shake_frames    = " << (d1 ? resolveInt(d1) : 20) << ";\n";
                break;
            }
            case AfnScriptNodeType::DampenJump: {
                auto* d = findDataIn(a->id, 0);
                float factor = d ? resolveFloat(d) : 0.75f;
                f << "    if (player_vy > 0) player_vy = (player_vy * " << (int)(factor*256.0f) << ") >> 8;\n";
                break;
            }
            case AfnScriptNodeType::StartGrind:
                // Capture the rail we collided with — the runtime reads its
                // mesh axis to lock the grind direction along the rail.
                f << "    afn_grinding = 1; afn_grind_rail = afn_collided_sprite;\n"; break;
            case AfnScriptNodeType::StopGrind:
                f << "    afn_grinding = 0;\n"; break;
            case AfnScriptNodeType::GrindPower: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 24.0f;
                f << "    afn_grind_power = " << (int)v << ";\n"; break;
            }
            case AfnScriptNodeType::GrindBoost: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_grind_boost = " << (int)v << ";\n"; break;
            }
            case AfnScriptNodeType::GrindBleed: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 6.0f;
                f << "    afn_grind_bleed = " << (int)v << ";\n"; break;
            }
            case AfnScriptNodeType::GrindCatch: {
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
            case AfnScriptNodeType::IsGrinding:
                f << "    if (afn_grinding_active) {\n"; break;
            case AfnScriptNodeType::IsNotGrinding:
                f << "    if (!afn_grinding_active) {\n"; break;
            case AfnScriptNodeType::IsMoving:
                f << "    if (player_moving) {\n"; break;
            case AfnScriptNodeType::IsLockedOn:
                // Lock On target active (PSV camera lock; inert -1 elsewhere).
                f << "    if (afn_cam_lock_target >= 0) {\n"; break;
            case AfnScriptNodeType::IsInView: {
                // Gate: target on-screen (camera FOV). Target pin = Object
                // or Attached Sprite (self).
                auto* tv = findDataIn(a->id, 0);
                if (tv && tv->type == AfnScriptNodeType::AttachedSprite)
                    f << "    if (afn_in_view(afn_bp_cur_spr_idx)) {\n";
                else
                    f << "    if (afn_in_view(" << (tv ? resolveInt(tv) : -1) << ")) {\n";
                break;
            }
            case AfnScriptNodeType::IsNotLockedOn:
                f << "    if (afn_cam_lock_target < 0) {\n"; break;
            case AfnScriptNodeType::DoOnce:
                // Fire downstream only the FIRST time exec reaches here this session.
                // A per-node static latch (zero-initialized at program start), set on
                // the first pass; the isGate path closes the brace after the downstream.
                f << "    static int afn_do_" << a->id << " = 0;\n";
                f << "    if (!afn_do_" << a->id << " && (afn_do_" << a->id << " = 1)) {\n";
                break;
            case AfnScriptNodeType::IsHealthZero:
                f << "    if (afn_health <= 0) {\n"; break;
            case AfnScriptNodeType::FadeInHudElement: {
                // Show a HUD element and crossfade its alpha in over N frames.
                auto* elemD = findDataIn(a->id, 0);
                auto* frD   = findDataIn(a->id, 1);
                int elem = elemD ? resolveInt(elemD) : 0;
                int fr   = frD   ? resolveInt(frD)   : 60;
                f << "    afn_hud_visible[" << elem << "] = 1; afn_hud_elem_fade[" << elem << "] = 0;\n";
                f << "    afn_hud_fade_len[" << elem << "] = " << fr << "; afn_hud_fade_dur[" << elem << "] = " << fr << ";\n";
                break;
            }
            case AfnScriptNodeType::IsHPZero: {
                // Gate: the target sprite's HP <= 0. Object pin = an Object, or
                // Attached Sprite (self) — guarded so an unresolved self never
                // indexes afn_hp[-1].
                auto* tv = findDataIn(a->id, 0);
                if (tv && tv->type == AfnScriptNodeType::AttachedSprite)
                    f << "    if (afn_bp_cur_spr_idx >= 0 && afn_hp[afn_bp_cur_spr_idx] <= 0) {\n";
                else {
                    int o = tv ? resolveInt(tv) : -1;
                    f << "    if (" << o << " >= 0 && afn_hp[" << o << "] <= 0) {\n";
                }
                break;
            }
            case AfnScriptNodeType::DashToTarget: {
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
            case AfnScriptNodeType::ChargeShot: {
                // Focus blast charge (PSV): assert charge each held frame; the
                // runtime grows the player's hidden effect sub-sprite (the ball).
                auto* mxD = findDataIn(a->id, 0);
                auto* mnsD = findDataIn(a->id, 1);
                auto* mxsD = findDataIn(a->id, 2);
                int mx  = mxD  ? resolveInt(mxD)  : 180;
                int mns = mnsD ? resolveInt(mnsD) : 5;
                int mxs = mxsD ? resolveInt(mxsD) : 70;
                std::string self = (curScript != &script) ? "afn_bp_cur_spr_idx" : "0";
                f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                f << "    afn_fb_charge_req = 1;\n";
                f << "    afn_fb_parent     = " << self << ";\n";
                f << "    afn_fb_max        = " << mx << ";\n";
                f << "    afn_fb_min_scale  = " << mns << " / 100.0f;\n";
                f << "    afn_fb_max_scale  = " << mxs << " / 100.0f;\n";
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::FireChargeShot: {
                // Focus blast release (PSV): request launch; runtime snapshots the
                // charged ball into a homing projectile aimed at the lock target.
                auto* dmgD = findDataIn(a->id, 0);
                auto* spD  = findDataIn(a->id, 1);
                auto* hrD  = findDataIn(a->id, 2);
                auto* hmD  = findDataIn(a->id, 3);
                auto* ciD  = findDataIn(a->id, 4);
                int dmg = dmgD ? resolveInt(dmgD) : 30;
                int sp  = spD  ? resolveInt(spD)  : 60;   // tenths of px/frame (60 = 6.0)
                int hr  = hrD  ? resolveInt(hrD)  : 4;    // hit slop (world px)
                int hm  = hmD  ? resolveInt(hmD)  : 12;   // homing % (12 = 0.12 ease/frame; 100 = perfect)
                int ci  = ciD  ? resolveInt(ciD)  : 0;    // circle home (1 = orbit; 0 = fly off once passed)
                f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                f << "    afn_fb_fire_req = 1;\n";
                f << "    afn_fb_dmg_max  = " << dmg << ";\n";
                f << "    afn_fb_speed    = " << sp << " / 10.0f;\n";
                f << "    afn_fb_hit_r    = " << hr << "; afn_fb_homing = " << hm << " / 100.0f; afn_fb_circle = " << (ci ? 1 : 0) << ";\n";
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_fb_tgt      = afn_cam_lock_target;\n";
                f << "#else\n";
                f << "    afn_fb_tgt      = -1;\n";
                f << "#endif\n";
                f << "#endif\n";
                break;
            }
            // ---- Energy resource (player) ----
            case AfnScriptNodeType::SetEnergy: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "0";
                f << "    afn_energy = " << v << ";\n";
                f << "    if (afn_energy < 0) afn_energy = 0; if (afn_energy > afn_energy_max) afn_energy = afn_energy_max;\n";
                break;
            }
            case AfnScriptNodeType::AddEnergy: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "1";
                f << "    afn_energy += " << v << "; if (afn_energy > afn_energy_max) afn_energy = afn_energy_max;\n";
                break;
            }
            case AfnScriptNodeType::SpendEnergy: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "1";
                f << "    afn_energy -= " << v << "; if (afn_energy < 0) afn_energy = 0;\n";
                break;
            }
            case AfnScriptNodeType::SpendChargeEnergy: {
                // Spend energy scaled by charge level: Min% at a tap, Max% at full charge.
                auto* dmn = findDataIn(a->id, 0); std::string mn = dmn ? emitIntExpr(dmn) : "4";
                auto* dmx = findDataIn(a->id, 1); std::string mx = dmx ? emitIntExpr(dmx) : "33";
                f << "    { int _mn = " << mn << ", _mx = " << mx << ";\n";
                f << "      int _cp = (afn_fb_max > 0) ? (int)(afn_fb_level * 100.0f / afn_fb_max) : 0;\n";
                f << "      int _amt = _mn + (_mx - _mn) * _cp / 100;\n";
                f << "      afn_energy -= _amt; if (afn_energy < 0) afn_energy = 0; }\n";
                break;
            }
            case AfnScriptNodeType::SetMaxEnergy: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "100";
                f << "    afn_energy_max = " << v << "; if (afn_energy_max < 0) afn_energy_max = 0;\n";
                f << "    if (afn_energy > afn_energy_max) afn_energy = afn_energy_max;\n";
                break;
            }
            case AfnScriptNodeType::HasEnergy: {
                // Gate: opens the brace; the isGate path closes it.
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "1";
                f << "    if (afn_energy >= " << v << ") {\n";
                break;
            }
            // ---- Player Health resource ----
            case AfnScriptNodeType::SetHealth: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "0";
                f << "    afn_health = " << v << ";\n";
                f << "    if (afn_health < 0) afn_health = 0; if (afn_health > afn_health_max) afn_health = afn_health_max;\n";
                break;
            }
            case AfnScriptNodeType::DamageHealth: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "1";
                f << "    afn_health -= " << v << "; if (afn_health < 0) afn_health = 0;\n";
                break;
            }
            case AfnScriptNodeType::HealHealth: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "1";
                f << "    afn_health += " << v << "; if (afn_health > afn_health_max) afn_health = afn_health_max;\n";
                break;
            }
            case AfnScriptNodeType::SetMaxHealth: {
                auto* d = findDataIn(a->id, 0); std::string v = d ? emitIntExpr(d) : "100";
                f << "    afn_health_max = " << v << "; if (afn_health_max < 0) afn_health_max = 0;\n";
                f << "    if (afn_health > afn_health_max) afn_health = afn_health_max;\n";
                break;
            }
            case AfnScriptNodeType::StrafeAnim: {
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
            case AfnScriptNodeType::SnapStick8: {
                // Gate the left stick to 8 directions (PSV). The runtime movement
                // block snaps afn_input_fwd/right to the nearest octant; set once
                // from On Start. NDS lacks the analog stick path -> compiled out.
                f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                f << "    afn_stick_8way = 1;\n";
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::Dodge: {
                // One-button pure left/right side roll (PSV, AFN_HAS_PLAYER_RIG):
                // set speed/frames + the L/R clips and raise the trigger so the
                // movement block picks the side from the live stick's horizontal
                // component next frame (input is final there) and commits.
                auto* spD = findDataIn(a->id, 0);
                auto* frD = findDataIn(a->id, 1);
                auto* lcD = findDataIn(a->id, 2);
                auto* rcD = findDataIn(a->id, 3);
                auto* icD = findDataIn(a->id, 4);
                auto* rpD = findDataIn(a->id, 5);
                auto* foD = findDataIn(a->id, 6);
                auto* cdD = findDataIn(a->id, 7);
                auto* fcD = findDataIn(a->id, 8);
                auto* bcD = findDataIn(a->id, 9);
                auto* ecD = findDataIn(a->id, 10);
                int sp = spD ? resolveInt(spD) : 70;
                int fr = frD ? resolveInt(frD) : 14;
                int lc = lcD ? resolveInt(lcD) : 0;
                int rc = rcD ? resolveInt(rcD) : 0;
                int ic = icD ? resolveInt(icD) : -1;   // -1 = no auto-return to idle
                int rp = rpD ? resolveInt(rpD) : 6;    // speed ease-in frames (0 = instant)
                int fo = foD ? resolveInt(foD) : 6;    // speed ease-out frames (0 = hard stop)
                int cd = cdD ? resolveInt(cdD) : 0;    // spam-gate lockout frames (0 = none)
                int fc = fcD ? resolveInt(fcD) : -1;   // forward clip (-1 = unwired -> lateral roll)
                int bc = bcD ? resolveInt(bcD) : -1;   // back clip    (-1 = unwired -> lateral roll)
                int ec = ecD ? resolveInt(ecD) : 0;    // energy cost (0 = free); also gates the fire on affording it
                // Gate the whole trigger on the cooldown AND on affording the
                // energy cost, so a press during the lockout (or with too little
                // energy) sets nothing. The runtime counts afn_dodge_cd down.
                f << "#ifdef AFN_HAS_PLAYER_RIG\n";
                f << "    if (afn_dodge_cd <= 0 && afn_energy >= " << ec << ") {\n";
                f << "        afn_dodge_speed = " << sp << ";\n";
                f << "        afn_dodge_frames = " << fr << ";\n";
                f << "        afn_dodge_clip_l = " << lc << ";\n";
                f << "        afn_dodge_clip_r = " << rc << ";\n";
                f << "        afn_dodge_clip_f = " << fc << ";\n";
                f << "        afn_dodge_clip_b = " << bc << ";\n";
                f << "        afn_dodge_idle = " << ic << ";\n";
                f << "        afn_dodge_ramp = " << rp << ";\n";
                f << "        afn_dodge_falloff = " << fo << ";\n";
                f << "        afn_dodge_cd = " << cd << ";\n";
                f << "        afn_dodge_trigger = 1;\n";
                if (ec != 0) {
                    f << "        afn_energy -= " << ec << "; if (afn_energy < 0) afn_energy = 0; // dodge cost\n";
                }
                f << "    }\n";
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::IsDodging:
                f << "    if (afn_dodge_frames > 0) {\n"; break;
            case AfnScriptNodeType::IsNotDodging:
                f << "    if (afn_dodge_frames <= 0) {\n"; break;
            case AfnScriptNodeType::IsOnGround:
                f << "    if (player_on_ground) {\n"; break;
            case AfnScriptNodeType::IsAirborne:
                f << "    if (!player_on_ground) {\n"; break;
            case AfnScriptNodeType::IsJumping:
                // PSV exposes the ACTUAL vertical velocity (player_vy_now), so
                // "rising" is accurate the whole ascent. Other targets keep the
                // legacy look (player_vy, the Jump impulse) for the BP idioms
                // (e.g. jump SFX on the same frame Jump sets player_vy).
                f << "#ifdef AFN_HAS_PLAYER_RIG\n"
                     "    if (player_vy_now > 0) {\n"
                     "#else\n"
                     "    if (player_vy > 0) {\n"
                     "#endif\n"; inJumpGate = true; break;
            case AfnScriptNodeType::IsFalling:
                // PSV: true only while genuinely descending (player_vy_now < 0).
                f << "#ifdef AFN_HAS_PLAYER_RIG\n"
                     "    if (!player_on_ground && player_vy_now < 0) {\n"
                     "#else\n"
                     "    if (!player_on_ground && player_vy <= 0) {\n"
                     "#endif\n"; inJumpGate = true; break;
            case AfnScriptNodeType::IsLanding:
                f << "    if (afn_land_timer > 0) {\n"; break;
            case AfnScriptNodeType::IsNotLanding:
                f << "    if (afn_land_timer <= 0) {\n"; break;
            case AfnScriptNodeType::IsCharging:
                f << "    if (afn_fb_charging) {\n"; break;
            case AfnScriptNodeType::IsNotCharging:
                f << "    if (!afn_fb_charging) {\n"; break;
            case AfnScriptNodeType::IsFiring:
                f << "    if (afn_fb_fire_timer > 0) {\n"; break;
            case AfnScriptNodeType::IsNear2D:
                // Mirrors GBA: fires when the player just collided with
                // THIS BP's tm_object. Combined with OnKeyPressed(A) this
                // is the "press A near NPC" idiom.
                f << "    if (afn_collided_tm_obj == afn_bp_cur_tm_obj && afn_bp_cur_tm_obj >= 0) {\n"; break;
            case AfnScriptNodeType::IsFollowMoving:
                f << "    if (tm_fol_moving) {\n"; break;
            // CheckFlag is handled specially in emitChain (dual-pin Set/Clear branches).
            case AfnScriptNodeType::SetVelocityY: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    player_vy = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::SetVelocityX: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_player_vx_world = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::SetVelocityZ: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_player_vz_world = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::VelocityFalloff: {
                auto* d = findDataIn(a->id, 0);
                int frames = d ? resolveInt(d) : 0;
                if (frames < 1) frames = 1;
                f << "    afn_velocity_falloff = " << frames << ";\n";
                break;
            }
            case AfnScriptNodeType::BoostForward: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 0.0f;
                f << "    afn_pending_boost_fwd = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::HaltMomentum: {
                f << "    afn_player_vx_world = 0;\n";
                f << "    afn_player_vz_world = 0;\n";
                f << "    afn_pending_boost_fwd = 0;\n";
                f << "    afn_velocity_falloff = 0;\n";
                f << "    player_vy = 0;\n";
                break;
            }
            case AfnScriptNodeType::SetPlayerHeight: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 1.0f;
                f << "    afn_player_height = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::SetPlayerWidth: {
                auto* d = findDataIn(a->id, 0);
                float v = d ? resolveFloat(d) : 3.0f;
                f << "    afn_player_width = " << (int)(v * 256.0f) << ";\n";
                break;
            }
            case AfnScriptNodeType::SetSpriteAnim: {
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
            case AfnScriptNodeType::FollowPlayer: {
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
            case AfnScriptNodeType::SetFollowFacing:
                f << "    if (tm_fol_active && tm_fol_obj >= 0)\n";
                f << "      tm_obj_facing[tm_fol_obj] = tm_fol_facing;\n";
                break;
            case AfnScriptNodeType::PlaySound: {
                auto* d = findDataIn(a->id, 0);
                int sId = d ? resolveInt(d) : 0;
                auto* lk = findDataIn(a->id, 1);
                int link = lk ? resolveInt(lk) : 0;
                // Persist Link > 0 => persistent music: route through afn_play_sound
                // (the link-gated carry/swap logic lives there) for SFX *and* MIDI.
                // Link 0 => normal play: SFX takes the direct fast path.
                if (link > 0) {
                    f << "    afn_play_sound(" << sId << ", " << link << ");\n";
                } else if (sId >= 0 && sId < (int)soundInstances.size() && soundInstances[sId].isSfx) {
                    f << "    afn_play_sfx(" << soundInstances[sId].sfxSampleIdx
                      << ", " << soundInstances[sId].mixerGain
                      << ", " << soundInstances[sId].fifoChannel << ");\n";
                } else {
                    f << "    afn_play_sound(" << sId << ", 0);\n";
                }
                break;
            }
            case AfnScriptNodeType::StopSound: {
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
            case AfnScriptNodeType::Respawn:
                f << "    player_x = afn_start_x; player_y = afn_start_y; player_z = afn_start_z;\n";
                f << "    player_vy = 0;\n";
                break;
            case AfnScriptNodeType::ChangeScene: {
                auto* scData = findDataIn(a->id, 0);
                int scIdx = scData ? resolveInt(scData) : a->paramInt[0];
                int scMode = a->paramInt[1]; // 0 = 3D / Mode 4
                // Optional Delay pin (frames): hold the current scene for N frames
                // before the switch. Armed once (guarded so OnUpdate/Held re-fires
                // don't restart the countdown). AFN_HAS_SCENE_DELAY targets (PSV)
                // count it down; others fall back to an instant switch.
                auto* dlData = findDataIn(a->id, 1);
                int delay = dlData ? resolveInt(dlData) : 0;
                // Transition (frames) pin: duration of the fade / crossfade (default 15).
                auto* tfData = findDataIn(a->id, 2);
                int tf = tfData ? resolveInt(tfData) : 15; if (tf < 1) tf = 15;
                if (delay > 0) {
                    f << "#ifdef AFN_HAS_SCENE_DELAY\n";
                    f << "    if (afn_scene_delay <= 0 && afn_scene_phase == 0) { afn_scene_delay = " << delay
                      << "; afn_scene_delay_scene = " << scIdx << "; afn_scene_delay_mode = " << scMode
                      << "; afn_scene_delay_frames = " << tf << "; }\n";
                    f << "#else\n";
                    f << "    afn_scene_start_transition(" << scIdx << ", " << scMode << ", " << tf << ");\n";
                    f << "#endif\n";
                } else {
                    f << "    afn_scene_start_transition(" << scIdx << ", " << scMode << ", " << tf << ");\n";
                }
                break;
            }
            case AfnScriptNodeType::ReloadScene:
                f << "    afn_scene_start_transition(afn_current_scene, afn_current_mode, 15);\n";
                break;
            case AfnScriptNodeType::SetHudValue: {
                auto* valData  = findDataIn(a->id, 0);
                auto* slotData = findDataIn(a->id, 1);
                int val  = valData  ? resolveInt(valData)  : 0;
                int slot = slotData ? resolveInt(slotData) : 0;
                if (slot < 0) slot = 0;   // element index (afn_hud_visible/elems are ELEM_COUNT-sized)
                f << "    afn_hud_value[" << slot << "] += " << val << ";\n";
                break;
            }
            case AfnScriptNodeType::CycleHudValue: {
                // Slot/Delta/Count are live expressions (so Slot can be Get Cursor Stop,
                // i.e. cycle whichever player the cursor is on). Wrap to [0,Count).
                auto* slotData  = findDataIn(a->id, 0);
                auto* deltaData = findDataIn(a->id, 1);
                auto* cntData   = findDataIn(a->id, 2);
                std::string slotE  = slotData  ? emitIntExpr(slotData)  : "0";
                std::string deltaE = deltaData ? emitIntExpr(deltaData) : "1";
                std::string cntE   = cntData   ? emitIntExpr(cntData)   : "1";
                f << "    { int _s = (" << slotE << "); if (_s < 0) _s = 0; if (_s > 3) _s = 3;\n";
                f << "      int _c = (" << cntE << "); if (_c > 0) { int _v = (afn_hud_value[_s] + (" << deltaE << ")) % _c;"
                  << " if (_v < 0) _v += _c; afn_hud_value[_s] = _v; } }\n";
                break;
            }
            case AfnScriptNodeType::ShowHUD: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : a->paramInt[0];
                if (slot < 0) slot = 0;   // element index (afn_hud_visible/elems are ELEM_COUNT-sized)
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
                    if (anchorData && anchorData->type == AfnScriptNodeType::AttachedSprite) {
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
            case AfnScriptNodeType::HideHUD: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : a->paramInt[0];
                if (slot < 0) slot = 0;   // element index (afn_hud_visible/elems are ELEM_COUNT-sized)
                f << "    afn_hud_visible[" << slot << "] = 0;\n";
                // Hiding a menu auto-unfreezes / clears the anim hold so
                // gameplay resumes without needing an explicit UnfreezePlayer
                // wire. Matches GBA HideHUD semantics.
                f << "    afn_player_frozen = 0;\n";
                f << "    afn_play_anim = 0;\n";
                break;
            }
            case AfnScriptNodeType::PlayHudAnim: {
                int li = remapLayer(a->paramInt[0]), cnt = remapCount(a->paramInt[0]);
                if (li < 0) break;   // target editor-layer has no runtime track
                for (int k = 0; k < cnt; k++) {   // drive every per-item track of the layer
                    f << "    afn_hud_layer_frame[" << (li + k) << "] = 0;\n";
                    f << "    afn_hud_layer_tick[" << (li + k) << "] = 0;\n";
                    f << "    afn_hud_layer_active[" << (li + k) << "] = 1;\n";
                }
                break;
            }
            case AfnScriptNodeType::SuppressBeams:
                f << "    afn_clash_suppress_beams();\n";
                break;
            case AfnScriptNodeType::ClashHitEnemy: {
                // Clash win: deal Clash Dmg % of the PLAYER's full attack (afn_fb_dmg_max)
                // to the object — instead of an instant KO. Object = Object or self.
                auto* tv = findDataIn(a->id, 0);
                std::string o = (tv && tv->type == AfnScriptNodeType::AttachedSprite)
                                ? "afn_bp_cur_spr_idx" : std::to_string(tv ? resolveInt(tv) : -1);
                f << "    if ((" << o << ") >= 0) { afn_hp[" << o << "] -= (afn_fb_dmg_max * afn_clash_dmg_pct) / 100;"
                  << " if (afn_hp[" << o << "] < 0) afn_hp[" << o << "] = 0; }\n";
                break;
            }
            case AfnScriptNodeType::ClashHitPlayer:
                // Clash loss: deal Clash Dmg % of the ENEMY's full attack (ENEMY_CHG_DMG) to the player.
                f << "    afn_health -= (ENEMY_CHG_DMG * afn_clash_dmg_pct) / 100; if (afn_health < 0) afn_health = 0;\n";
                break;
            case AfnScriptNodeType::SetAiState: {
                auto* sd = findDataIn(a->id, 0); int st = sd ? resolveInt(sd) : 0;
                f << "    afn_ai_state = " << st << ";\n";
                break;
            }
            case AfnScriptNodeType::AiSense:       f << "    afn_ai_sense();\n"; break;
            case AfnScriptNodeType::AiRoam:        f << "    afn_ai_roam();\n"; break;
            case AfnScriptNodeType::AiChase:       f << "    afn_ai_chase();\n"; break;
            case AfnScriptNodeType::AiStrafe:      f << "    afn_ai_strafe();\n"; break;
            case AfnScriptNodeType::AiDodgeBegin:  f << "    afn_ai_dodge_begin();\n"; break;
            case AfnScriptNodeType::AiDodgeStep:   f << "    afn_ai_dodge_step();\n"; break;
            case AfnScriptNodeType::AiChargeBegin: f << "    afn_ai_charge_begin();\n"; break;
            case AfnScriptNodeType::AiChargeStep:  f << "    afn_ai_charge_step();\n"; break;
            case AfnScriptNodeType::AiFireBeam:    f << "    afn_ai_fire_beam();\n"; break;
            case AfnScriptNodeType::AiFireRecover: f << "    afn_ai_fire_recover();\n"; break;
            case AfnScriptNodeType::AiBlockBegin:  f << "    afn_ai_block_begin();\n"; break;
            case AfnScriptNodeType::AiBlockStep:   f << "    afn_ai_block_step();\n"; break;
            case AfnScriptNodeType::SetBlock: {
                auto* d = findDataIn(a->id, 0); int on = d ? resolveInt(d) : 0;
                auto* cd = findDataIn(a->id, 1); int cost = cd ? resolveInt(cd) : 0;
                if (on)
                    f << "    afn_player_blocking = 1; afn_block_energy = " << cost << ";\n";   // Energy Cost = energy spent per BLOCKED hit
                else
                    f << "    afn_player_blocking = 0;\n";
                break;
            }
            case AfnScriptNodeType::OrbitCamStep:
                f << "    afn_cam_orbit_timer++;   // advance the node-driven orbit\n";
                break;
            case AfnScriptNodeType::StopOrbitCam:
                f << "    afn_cam_orbit_active = 0;   // end the orbit cam\n";
                break;
            case AfnScriptNodeType::StepEnemyBeam:
                f << "    afn_enemy_beam_step();   // advance the enemy projectile (flight + hit)\n";
                break;
            case AfnScriptNodeType::StepFocusBlast:
                f << "    afn_focus_blast_step();  // advance the player Focus Blast (flight + hit)\n";
                break;
            case AfnScriptNodeType::ShowHPBar: {
                // Raise the floating HP bar for an object this frame (per-frame flag).
                auto* od = findDataIn(a->id, 0); auto* md = findDataIn(a->id, 1);
                int mx = md ? resolveInt(md) : 100;
                f << "    afn_hpbar_active = 1; afn_hpbar_max = " << mx << ";\n";
                if (od && od->type == AfnScriptNodeType::AttachedSprite)
                    f << "    afn_hpbar_obj = afn_bp_cur_spr_idx;\n";
                else
                    f << "    afn_hpbar_obj = " << (od ? resolveInt(od) : -1) << ";\n";
                break;
            }
            case AfnScriptNodeType::EnemyAI: {
                // Enable the enemy AI + feed tunables. Defaults match the #defines.
                auto* d0=findDataIn(a->id,0); auto* d1=findDataIn(a->id,1); auto* d2=findDataIn(a->id,2);
                auto* d3=findDataIn(a->id,3); auto* d4=findDataIn(a->id,4); auto* d5=findDataIn(a->id,5);
                auto* d6=findDataIn(a->id,6); auto* d7=findDataIn(a->id,7);
                auto* d8=findDataIn(a->id,8); auto* d9=findDataIn(a->id,9);
                auto* d10=findDataIn(a->id,10); auto* d11=findDataIn(a->id,11);
                f << "    afn_ai_enabled = 1;\n";
                f << "    afn_ai_detect_r = " << (d0?resolveInt(d0):60) << "; afn_ai_lose_r = " << (d1?resolveInt(d1):95)
                  << "; afn_ai_pref_r = " << (d2?resolveInt(d2):22) << ";\n";
                f << "    afn_ai_atkcd = " << (d3?resolveInt(d3):80) << "; afn_ai_chargeprob = " << (d4?resolveInt(d4):40)
                  << "; afn_ai_dodgeprob = " << (d5?resolveInt(d5):70) << "; afn_ai_movespd_m = " << (d6?resolveInt(d6):800) << ";\n";
                f << "    afn_ai_dodge_trig = " << (d7?resolveInt(d7):24)
                  << "; afn_ai_block_prob = " << (d8?resolveInt(d8):30)
                  << "; afn_block_pct = " << (d9?resolveInt(d9):20) << ";\n";
                f << "    afn_ai_chg_speed_t = " << (d10?resolveInt(d10):20)
                  << "; afn_ai_tap_speed_t = " << (d11?resolveInt(d11):25) << ";\n";
                break;
            }
            case AfnScriptNodeType::ClashBegin:
                f << "    afn_clash_begin();\n";
                break;
            case AfnScriptNodeType::ClashPush:
                // Player Cross tap -> push the balance toward the enemy. Uses the
                // Beam Clash node's Player Push tunable (afn_clash_push_m, x1000).
                f << "    afn_clash_balance += afn_clash_push_m * 0.001f;\n";
                break;
            case AfnScriptNodeType::ClashAiStep:
                f << "    afn_clash_ai_step();\n";
                break;
            case AfnScriptNodeType::SetHP: {
                // afn_hp[obj] = HP. Object pin = an Object or Attached Sprite (self).
                auto* tv = findDataIn(a->id, 0);
                auto* hv = findDataIn(a->id, 1);
                std::string hp = hv ? emitIntExpr(hv) : "0";
                if (tv && tv->type == AfnScriptNodeType::AttachedSprite)
                    f << "    if (afn_bp_cur_spr_idx >= 0) afn_hp[afn_bp_cur_spr_idx] = " << hp << ";\n";
                else {
                    int o = tv ? resolveInt(tv) : -1;
                    f << "    if (" << o << " >= 0) afn_hp[" << o << "] = " << hp << ";\n";
                }
                break;
            }
            case AfnScriptNodeType::DamageHP: {
                // afn_hp[obj] -= Amount (clamped >= 0). Object = an Object or Attached Sprite (self).
                auto* tv = findDataIn(a->id, 0);
                auto* av = findDataIn(a->id, 1);
                std::string amt = av ? emitIntExpr(av) : "1";
                if (tv && tv->type == AfnScriptNodeType::AttachedSprite)
                    f << "    if (afn_bp_cur_spr_idx >= 0) { afn_hp[afn_bp_cur_spr_idx] -= (" << amt << "); if (afn_hp[afn_bp_cur_spr_idx] < 0) afn_hp[afn_bp_cur_spr_idx] = 0; }\n";
                else {
                    int o = tv ? resolveInt(tv) : -1;
                    f << "    if (" << o << " >= 0) { afn_hp[" << o << "] -= (" << amt << "); if (afn_hp[" << o << "] < 0) afn_hp[" << o << "] = 0; }\n";
                }
                break;
            }
            case AfnScriptNodeType::BeamClash: {
                // Enable the beam-clash mechanic and feed its tunables. The mechanic
                // (detect both full beams meeting -> 2D struggle -> mash vs AI ->
                // resolve to enemy KO / player death) stays in the runtime clash_tick;
                // this just flips it on + sets the feel knobs each frame. Push values
                // are x1000, Full Charge is a %, so the int data pins carry fractions.
                auto* d0 = findDataIn(a->id, 0); auto* d1 = findDataIn(a->id, 1);
                auto* d2 = findDataIn(a->id, 2); auto* d3 = findDataIn(a->id, 3);
                auto* d4 = findDataIn(a->id, 4); auto* d5 = findDataIn(a->id, 5);
                auto* d6 = findDataIn(a->id, 6); auto* d7 = findDataIn(a->id, 7);
                auto* d8 = findDataIn(a->id, 8); auto* d9 = findDataIn(a->id, 9);
                int fullPct = d0 ? resolveInt(d0) : 85;
                int pPush   = d1 ? resolveInt(d1) : 60;
                int aiPush  = d2 ? resolveInt(d2) : 50;
                int aiMin   = d3 ? resolveInt(d3) : 6;
                int meetR   = d4 ? resolveInt(d4) : 18;
                int dmgPct  = d5 ? resolveInt(d5) : 150;
                int airFb   = d6 ? resolveInt(d6) : 90;
                int aiJit   = d7 ? resolveInt(d7) : 1;
                int fumPct  = d8 ? resolveInt(d8) : 1;
                int fumLen  = d9 ? resolveInt(d9) : 6;
                f << "    afn_clash_enabled = 1;\n";
                f << "    afn_clash_full_pct = " << fullPct << "; afn_clash_push_m = " << pPush
                  << "; afn_clash_ai_push_m = " << aiPush << ";\n";
                f << "    afn_clash_ai_min = " << aiMin << "; afn_clash_meet_r = " << meetR
                  << "; afn_clash_dmg_pct = " << dmgPct << "; afn_clash_air_fb = " << airFb << ";\n";
                f << "    afn_clash_ai_jit = " << aiJit << "; afn_clash_fumble_pct = " << fumPct
                  << "; afn_clash_fumble_len = " << fumLen << ";\n";
                break;
            }
            case AfnScriptNodeType::StopMusic:
                // Stop ONLY the persistent music track; one-shot SFX keep ringing
                // (e.g. a clash 'win_clash' under the victory fanfare).
                f << "    afn_stop_music();\n";
                break;
            case AfnScriptNodeType::LoopHudAnim: {
                // Keep a HUD element's anim layers active + looping. Driven every
                // frame (On Update -> Is Hud Visible -> Loop Hud Anim): re-arms each
                // layer and rewinds it once it passes its length, so an authored
                // layer that isn't flagged Loop still blinks continuously (menu cursor).
                auto* ed = findDataIn(a->id, 0);
                int elem = ed ? resolveInt(ed) : 0;
                f << "#ifdef AFN_HAS_HUD_ANIM\n";
                f << "    { const AfnHudElem* _le = &afn_hud_elems[" << elem << "];\n";
                f << "      for (int _lk = 0; _lk < _le->pieceCount; _lk++) {\n";
                f << "        int _ll = afn_hud_piece_layer[_le->pieceStart + _lk]; if (_ll < 0) continue;\n";
                f << "        afn_hud_layer_active[_ll] = 1;\n";
                f << "        if (afn_hud_layer[_ll].length > 0 && afn_hud_layer_frame[_ll] >= afn_hud_layer[_ll].length) {\n";
                f << "          afn_hud_layer_frame[_ll] = 0; afn_hud_layer_tick[_ll] = 0; } } }\n";
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::StopHudAnim: {
                int li = remapLayer(a->paramInt[0]), cnt = remapCount(a->paramInt[0]);
                if (li < 0) break;
                for (int k = 0; k < cnt; k++)
                    f << "    afn_hud_layer_active[" << (li + k) << "] = 0;\n";
                break;
            }
            case AfnScriptNodeType::SetHudAnimSpeed: {
                int li = remapLayer(a->paramInt[0]), cnt = remapCount(a->paramInt[0]);
                if (li < 0) break;
                auto* sd = findDataIn(a->id, 0);
                int spd = sd ? resolveInt(sd) : 1;
                for (int k = 0; k < cnt; k++)
                    f << "    afn_hud_layer_speed_override[" << (li + k) << "] = " << spd << ";\n";
                break;
            }
            case AfnScriptNodeType::UpdateRespawnPos: {
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
            case AfnScriptNodeType::SetCamera: {
                auto* slotData = findDataIn(a->id, 0);
                int slot = slotData ? resolveInt(slotData) : 0;
                f << "    afn_active_camera = " << slot << ";\n";
                break;
            }
            case AfnScriptNodeType::TankCamera: {
                auto* onData = findDataIn(a->id, 0);
                int on = onData ? resolveInt(onData) : 0;
                f << "    afn_tank_camera = " << on << ";\n";
                break;
            }
            case AfnScriptNodeType::TurnPlayer: {
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
            case AfnScriptNodeType::LockOnTarget: {
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
                if (tgt && tgt->type != AfnScriptNodeType::AttachedSprite)
                    f << "    afn_cam_lock_target = " << resolveInt(tgt) << ";\n";
                else if (curScript != &script || (tgt && tgt->type == AfnScriptNodeType::AttachedSprite))
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
            case AfnScriptNodeType::ReleaseLockOn: {
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_cam_lock_target = -1;\n";
                f << "    afn_lock_strafe = 0;\n";   // dropping the lock drops Z-targeting movement
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::OrbitCameraOnObject: {
                // KO/death cinematic: orbit the camera around the Target (an Object,
                // or Attached Sprite = the BP owner "self"; unwired in a BP = self).
                // Runs until the scene swaps (or a future Release Camera node).
                auto* tgt = findDataIn(a->id, 0);
                f << "#ifdef AFN_HAS_SPRITE_IDX\n";
                if (tgt && tgt->type != AfnScriptNodeType::AttachedSprite)
                    f << "    afn_cam_orbit_obj = " << resolveInt(tgt) << ";\n";
                else if (curScript != &script || (tgt && tgt->type == AfnScriptNodeType::AttachedSprite))
                    f << "    afn_cam_orbit_obj = afn_bp_cur_spr_idx;\n";
                else
                    f << "    afn_cam_orbit_obj = -1;\n";   // no target -> orbit the player
                f << "    afn_cam_orbit_angle0 = orbit_angle * (6.2831853f / 65536.0f);\n";
                f << "    afn_cam_orbit_timer = 0;\n";
                f << "    afn_cam_orbit_active = 1;\n";
                // Tunable data pins (defaults preserve the KO feel): Zoom % of cam
                // distance, Orbit Speed (milli-rad/frame), Pitch (centi-rad).
                { auto* z = findDataIn(a->id, 1); auto* sp = findDataIn(a->id, 2); auto* pi = findDataIn(a->id, 3);
                  auto* ez = findDataIn(a->id, 4); auto* lh = findDataIn(a->id, 5);
                  f << "    afn_cam_orbit_zoom_pct = " << (z  ? resolveInt(z)  : 45) << ";\n";
                  f << "    afn_cam_orbit_rate_mr  = " << (sp ? resolveInt(sp) : 12) << ";\n";
                  f << "    afn_cam_orbit_pitch_cr = " << (pi ? resolveInt(pi) : 32) << ";\n";
                  f << "    afn_cam_orbit_ease_pm  = " << (ez ? resolveInt(ez) : 60) << ";\n";
                  f << "    afn_cam_orbit_lookh    = " << (lh ? resolveInt(lh) : 0)  << ";\n"; }
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::LockStrafe: {
                // Z-targeting movement: only does anything while a Lock On
                // target is active (afn_cam_lock_target >= 0).
                f << "#ifdef AFN_HAS_CAM_LOCK\n";
                f << "    afn_lock_strafe = 1;\n";
                f << "#endif\n";
                break;
            }
            case AfnScriptNodeType::CastEffect: {
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
        std::function<void(const AfnScriptNodeExport*)> emitOne;
        // Sibling exec targets fan out in link declaration order, but gates
        // (IsJumping, IsOnGround, ...) READ state that sibling action nodes
        // (Jump, SetVelocityY, ...) WRITE on the same frame. Walking gates
        // first means they see stale state — jump SFX gated by IsJumping
        // never fires because Jump hasn't run yet. Sort action nodes before
        // gate nodes so writers run before readers.
        auto isGateType = [](AfnScriptNodeType t) -> bool {
            return t == AfnScriptNodeType::IsMoving ||
                   t == AfnScriptNodeType::IsOnGround ||
                   t == AfnScriptNodeType::IsJumping ||
                   t == AfnScriptNodeType::IsFalling ||
                   t == AfnScriptNodeType::IsNear2D ||
                   t == AfnScriptNodeType::IsFollowMoving ||
                   t == AfnScriptNodeType::CheckFlag ||
                   t == AfnScriptNodeType::IsFlagSet ||
                   t == AfnScriptNodeType::FlipFlop ||
                   t == AfnScriptNodeType::OnRise ||
                   t == AfnScriptNodeType::Countdown ||
                   t == AfnScriptNodeType::IsLockedOn ||
                   t == AfnScriptNodeType::IsNotLockedOn ||
                   t == AfnScriptNodeType::IsDodging ||
                   t == AfnScriptNodeType::IsNotDodging ||
                   t == AfnScriptNodeType::IsAirborne ||
                   t == AfnScriptNodeType::IsLanding ||
                   t == AfnScriptNodeType::IsNotLanding ||
                   t == AfnScriptNodeType::IsCharging ||
                   t == AfnScriptNodeType::IsNotCharging ||
                   t == AfnScriptNodeType::IsFiring ||
                   t == AfnScriptNodeType::HasEnergy ||
                   t == AfnScriptNodeType::IsHPZero ||
                   t == AfnScriptNodeType::IsHealthZero ||
                   t == AfnScriptNodeType::DoOnce ||
                   t == AfnScriptNodeType::IsInView;
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
        emitOne = [&](const AfnScriptNodeExport* a) {
            if (a->type == AfnScriptNodeType::CheckFlag) {
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
            if (a->type == AfnScriptNodeType::IsFlagSet) {
                // Gate-only (single exec out). True branch walks pin 0.
                auto* fd = findDataIn(a->id, 0);
                int flag = fd ? resolveInt(fd) : a->paramInt[0];
                f << "    if (afn_flags & (1u << " << flag << ")) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::FlipFlop) {
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
            if (a->type == AfnScriptNodeType::OnRise) {
                f << "    if (afn_rise_" << a->id << " >= afn_frame_count - 1) { afn_rise_" << a->id << " = afn_frame_count; }\n";
                f << "    else { afn_rise_" << a->id << " = afn_frame_count;\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::Countdown) {
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
            if (a->type == AfnScriptNodeType::Delay) {
                // Non-blocking one-shot: fires downstream once, <Frames> frames after
                // the upstream gate (re)opens. Self-rearming via frame-count latching
                // (same idiom as OnRise) so it needs NO external reset — while the gate
                // keeps exec reaching it the start stays latched; when the gate closes
                // for >1 frame and later reopens (e.g. the next battle) it relatches and
                // fires fresh. Drive it from a persistent gate (IsTrue/IsHPZero/...), not
                // a one-frame edge like OnRise.
                auto* dData = findDataIn(a->id, 0);
                int frames = dData ? resolveInt(dData) : 60;
                // _last = last frame this node was reached, _start = frame the current
                // open-streak began, _fired = one-shot latch. A gap of >1 frame since
                // the last reach means the gate closed and reopened (next battle) -> new
                // streak + re-arm. Fires once when the streak reaches <frames> (>=, so a
                // dropped frame can't skip past it). NOTE: _start must be SEPARATE from
                // _last — a single var conflates them and the count never accumulates.
                f << "    { static int afn_dll_" << a->id << " = -1000000; static int afn_dls_" << a->id << " = 0; static int afn_dlf_" << a->id << " = 0;\n";
                f << "      if (afn_frame_count - afn_dll_" << a->id << " > 1) { afn_dls_" << a->id << " = afn_frame_count; afn_dlf_" << a->id << " = 0; }\n";
                f << "      afn_dll_" << a->id << " = afn_frame_count;\n";
                f << "      if (!afn_dlf_" << a->id << " && afn_frame_count - afn_dls_" << a->id << " >= " << frames << ") { afn_dlf_" << a->id << " = 1;\n";
                walkExec(a->id, 0);
                f << "    } }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::Branch) {
                // If/Else: condition data input -> True (pin 0) / False (pin 1).
                auto* cd = findDataIn(a->id, 0);
                std::string cond = cd ? emitIntExpr(cd) : (a->paramInt[0] ? "1" : "0");
                f << "    if (" << cond << ") {\n";
                walkExec(a->id, 0);
                if (!findExecOuts(a->id, 1).empty()) {
                    f << "    } else {\n";
                    walkExec(a->id, 1);
                }
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsTrue) {
                // Gate: pass exec while the condition data input is non-zero.
                auto* cd = findDataIn(a->id, 0);
                std::string cond = cd ? emitIntExpr(cd) : (a->paramInt[0] ? "1" : "0");
                f << "    if (" << cond << ") {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsClashReady) {
                f << "    if (afn_clash_ready) {\n";   // runtime clash_sense set this
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsClashWon) {
                f << "    if (afn_clash_balance >= 1.0f) {\n";   // pushed to the enemy
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsClashLost) {
                f << "    if (afn_clash_balance <= 0.0f) {\n";   // pushed into your zone
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsAiState) {
                auto* sd = findDataIn(a->id, 0); int st = sd ? resolveInt(sd) : 0;
                f << "    if (afn_ai_state == " << st << ") {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsPlayerWithin) {
                auto* rd = findDataIn(a->id, 0); int r = rd ? resolveInt(rd) : 60;
                f << "    if (afn_ai_dist <= " << r << ".0f) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsPlayerBeyond) {
                auto* rd = findDataIn(a->id, 0); int r = rd ? resolveInt(rd) : 95;
                f << "    if (afn_ai_dist > " << r << ".0f) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsBlastIncoming) {
                f << "    if (afn_ai_blast_incoming()) {\n";   // player blast in dodge range (+chance)
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::ShouldAiBlock) {
                f << "    if (afn_ai_blast_block()) {\n";   // blast in range + block-chance roll
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsAiFlag) {
                // Flag selector (wired Int): 0=lose_ready 1=dodge_ready 2=can_fire
                // 3=charge_done 4=dodge_done 5=fire_done 6=reached.
                static const char* flagN[8] = { "lose_ready","dodge_ready","can_fire","charge_done","dodge_done","fire_done","reached","block_done" };
                auto* fd = findDataIn(a->id, 0); int fi = fd ? resolveInt(fd) : 0; if (fi < 0 || fi > 7) fi = 0;
                f << "    if (afn_ai_" << flagN[fi] << ") {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsHudVisible) {
                // Gate: pass exec while the given HUD element slot is visible. Used to
                // scope menu cursor-nav/confirm to the frames the menu is actually up.
                auto* sd = findDataIn(a->id, 0);
                int slot = sd ? resolveInt(sd) : 0;
                f << "    if (afn_hud_visible[" << slot << "]) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::IsFalse) {
                // Gate: pass exec while the condition data input is zero (if-not).
                auto* cd = findDataIn(a->id, 0);
                std::string cond = cd ? emitIntExpr(cd) : (a->paramInt[0] ? "1" : "0");
                f << "    if (!(" << cond << ")) {\n";
                walkExec(a->id, 0);
                f << "    }\n";
                return;
            }
            if (a->type == AfnScriptNodeType::SwitchInt) {
                // Route exec by integer value: pins 0..3 = cases 0..3, pin 4 = default.
                auto* vd = findDataIn(a->id, 0);
                std::string val = vd ? emitIntExpr(vd) : std::to_string(a->paramInt[0]);
                f << "    switch (" << val << ") {\n";
                for (int cse = 0; cse < 4; cse++) {
                    if (findExecOuts(a->id, cse).empty()) continue;
                    f << "    case " << cse << ": {\n";
                    walkExec(a->id, cse);
                    f << "    } break;\n";
                }
                if (!findExecOuts(a->id, 4).empty()) {
                    f << "    default: {\n";
                    walkExec(a->id, 4);
                    f << "    } break;\n";
                }
                f << "    }\n";
                return;
            }
            bool isGate = (a->type == AfnScriptNodeType::IsMoving ||
                           a->type == AfnScriptNodeType::IsOnGround ||
                           a->type == AfnScriptNodeType::IsJumping ||
                           a->type == AfnScriptNodeType::IsFalling ||
                           a->type == AfnScriptNodeType::IsNear2D ||
                           a->type == AfnScriptNodeType::IsFollowMoving ||
                           a->type == AfnScriptNodeType::IsGrinding ||
                           a->type == AfnScriptNodeType::IsNotGrinding ||
                           a->type == AfnScriptNodeType::IsLockedOn ||
                           a->type == AfnScriptNodeType::IsNotLockedOn ||
                           a->type == AfnScriptNodeType::IsDodging ||
                           a->type == AfnScriptNodeType::IsNotDodging ||
                           a->type == AfnScriptNodeType::IsAirborne ||
                           a->type == AfnScriptNodeType::IsLanding ||
                           a->type == AfnScriptNodeType::IsNotLanding ||
                           a->type == AfnScriptNodeType::IsCharging ||
                           a->type == AfnScriptNodeType::IsNotCharging ||
                           a->type == AfnScriptNodeType::IsFiring ||
                           a->type == AfnScriptNodeType::HasEnergy ||
                           a->type == AfnScriptNodeType::IsHPZero ||
                           a->type == AfnScriptNodeType::IsHealthZero ||
                           a->type == AfnScriptNodeType::DoOnce ||
                           a->type == AfnScriptNodeType::IsInView);
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
        auto emitDispatcher = [&](const char* fname, AfnScriptNodeType evType,
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
                } else if (evType == AfnScriptNodeType::OnCollision) {
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
            auto emitSens = [&](const AfnScriptNodeExport& n) {
                if (n.type != AfnScriptNodeType::Key) return;
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
        emitDispatcher("afn_emitted_script_start",        AfnScriptNodeType::OnStart,        nullptr);
        emitDispatcher("afn_emitted_script_update",       AfnScriptNodeType::OnUpdate,       nullptr);
        emitDispatcher("afn_emitted_script_key_held",     AfnScriptNodeType::OnKeyHeld,      "key_is_down");
        emitDispatcher("afn_emitted_script_key_pressed",  AfnScriptNodeType::OnKeyPressed,   "key_hit");
        emitDispatcher("afn_emitted_script_key_released", AfnScriptNodeType::OnKeyReleased,  "key_released");
        emitDispatcher("afn_emitted_script_collision",    AfnScriptNodeType::OnCollision,    nullptr);
        emitDispatcher("afn_emitted_script_collision2d",  AfnScriptNodeType::OnCollision2D,  nullptr);

        // Blueprint event handlers — one set of named functions per blueprint.
        // Per-instance state (current sprite/tm-obj) lives in afn_bp_cur_*.
        for (size_t bi = 0; bi < blueprints.size(); bi++) {
            curScript = &blueprints[bi].script;
            buildChains();
            char fn[64];
            snprintf(fn, sizeof(fn), "afn_bp%zu_start",        bi);
            emitDispatcher(fn, AfnScriptNodeType::OnStart, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_update",       bi);
            emitDispatcher(fn, AfnScriptNodeType::OnUpdate, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_held",     bi);
            emitDispatcher(fn, AfnScriptNodeType::OnKeyHeld, "key_is_down");
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_pressed",  bi);
            emitDispatcher(fn, AfnScriptNodeType::OnKeyPressed, "key_hit");
            snprintf(fn, sizeof(fn), "afn_bp%zu_key_released", bi);
            emitDispatcher(fn, AfnScriptNodeType::OnKeyReleased, "key_released");
            snprintf(fn, sizeof(fn), "afn_bp%zu_collision",    bi);
            emitDispatcher(fn, AfnScriptNodeType::OnCollision, nullptr);
            snprintf(fn, sizeof(fn), "afn_bp%zu_collision2d",  bi);
            emitDispatcher(fn, AfnScriptNodeType::OnCollision2D, nullptr);
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

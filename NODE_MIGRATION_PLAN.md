# Node Migration Plan — hardcoded → nodes (PSV / pokemon_arena)

Port every `// HARDCODED` system in `psv_runtime/main.c` to node-driven behavior,
housed in the relevant blueprints. **Hybrid grain**: each system becomes a small
set of meaningful nodes (not one monolith, not micro-primitives).

## Node-driven rule (the invariant)
The runtime keeps the heavy per-frame work (camera-orbit math, menu render,
projectile flight, AI movement), but **triggers + parameters come from variables/
flags the nodes set**. `main.c` never hardcodes the trigger condition. So porting a
feature = (1) add node(s) that set flags, (2) make the runtime respect those flags,
(3) delete the `// HARDCODED` trigger.

## Per-new-node checklist (CLAUDE.md rules)
- `VsNodeType` (frame_loop.cpp) and `GBAScriptNodeType` (gba_package.h) must match
  EXACTLY; **append before `COUNT`** — never insert mid-enum. If a middle insert
  ships, add version migration at the 4 node-load sites + bump save version.
- Add `sVsNodeDefs` entry (pins/labels), node desc, and the `setActionFunc` body
  (the REAL generated C + the runtime-consumption chain).
- Codegen: `node_script_emit.cpp` (shared NDS/PSV emitter). GBA: `gba_package.cpp`.
- Runtime: `main.c` reads the node-set flags; remove the matching HARDCODED trigger.
- Exporter + runtime + `setActionFunc` updated in the SAME pass.

## Blueprint housing
| BP | houses |
|----|----|
| `ch_controller` | player charge/lock/dodge (done) + **player death cinematic**, **beam-clash player side** |
| `enemy` | **enemy AI, projectile, KO cinematic** (currently ~empty — the big port) |
| `splash` / `ch_select` / `stg_select` | menus (already node-driven) |
| (new) `results` BP *or* fold into `ch_controller` | win/lose → results menu |

---

## Milestone 1 — Cinematics + Results menu (pilot)

### New nodes
1. **OrbitCameraOnObject** (action) — orbit the camera around an object. Params:
   Object, Zoom%, Orbit Speed, Pitch. Sets the camera-override flags the runtime
   camera block reads. Replaces the hardcoded `s_koActive` / `s_pkoActive` override.
2. **HoldSkelClip** (action) — play a rig clip once and freeze the last frame (the
   die collapse). *Or* add a "hold last frame" flag to `SetSkelAnim`/`PlaySkelAnim`.
3. **FadeInHudElement** (action) — crossfade a HUD element's alpha 0→full over N
   frames (`afn_hud_elem_fade[elem]`). For the results-menu fade-in.

Reuse existing for the rest: `OnDeath`, `ShowHUD`/`HideHUD`, `CursorUp`/`CursorDown`,
`FollowLink`, `ChangeScene`/`ResetScene`, `PlaySound`, `FreezePlayer`, `Wait`/`Delay`,
`OnKeyPressed`, `GetHealth`/`IsHPZero`. (Verify each at impl.)

### BP wiring
- **enemy**: `OnDeath(self)` → HoldSkelClip(die) + OrbitCameraOnObject(self) +
  DestroyAfter(self, ~3.5 s).
- **ch_controller** (player death): on player health == 0 → HoldSkelClip(die) +
  OrbitCameraOnObject(player) + ReleaseLockOn (so the body doesn't rotate).
- **results**: on enemy dead OR player dead → Wait(180) → ShowHUD(die) +
  FadeInHudElement + FreezePlayer → CursorUp/Down (beep) → OnKeyPressed(A) →
  PlaySound(select) → stop 0 ? ChangeScene(title) : ResetScene. PlaySound(victory)
  fanfare on the outcome.

### Runtime changes (`main.c`)
- Delete the hardcoded `results_tick` win/lose **trigger** (keep the menu render,
  now driven by the node-set ShowHUD/fade/cursor flags).
- Delete the hardcoded KO/death camera-override **conditions**; OrbitCameraOnObject
  sets the same flags.
- Keep: camera-orbit math, die-clip freeze, HUD render.

---

## Roadmap (after M1)

- **M2 — Beam clash.** Nodes: `OnBeamsMeet` (trigger when both full beams collide),
  `RunClashStruggle` (the 2D struggle: backdrop/mash/beams + human AI masher +
  resolve). Clash audio via `PlaySound` + new pitch/loop helper nodes
  (`afn_set_sfx_pitch`/`afn_sfx_active` already exist as runtime primitives). Housed
  in `ch_controller` (+ maybe a `clash` BP).
- **M3 — Enemy combat AI** (the big one). Either a coarse `RunEnemyAI` node, or a
  small state-machine set: `FollowNavmeshToTarget`, `StrafeAroundTarget`,
  `FireProjectileFromBone`, `DodgeIncoming`. Housed in the `enemy` BP. Warrants its
  own sub-plan.
- **M4 — Enemy projectile + HP bar / reticle.** `DrawBar` exists; reticle via an
  anchored `ShowHUD`.

## Status
- ✅ **Enemy KO cinematic — PORTED + verified on hardware.** New nodes OrbitCameraOnObject + HoldSkelClip; implemented the previously-stubbed PSV nodes **Is HP Zero** and **Do Once** (codegen + gate registration); wired the graph into the `enemy` BP directly in the .afnproj; stripped the hardcoded `s_koActive` camera. Enemy HP seeded at boot (BP runs in all scenes, so Is HP Zero would fire in menus otherwise).
- ✅ KO re-fires every battle: swapped `Do Once` → `On Rise` (re-arms when HP refills).
- ✅ **Player death cinematic — PORTED + verified.** New gate `Is Health Zero` (afn_health<=0); extended Hold Skel Clip to the player rig (`s_playerClipHold`, AFN_PLAYER_SPRITE_IDX path); wired into ch_controller (On Update→Is Health Zero→On Rise→Hold Skel Clip(self,die)+Orbit Cam On Obj(self)); stripped hardcoded `s_pkoActive` camera.
- ⬜ Results menu (win/lose → fade-in menu → cursor → restart/title) + FadeInHudElement node; strip hardcoded results_tick.
- ⬜ M2 beam clash · ⬜ M3 enemy AI · ⬜ M4 projectile/HUD

### BP graph encoding (for authoring .afnproj graphs)
- `bpVsNode=ID,TYPE,X,Y,P0,P1,P2,P3,P4` (TYPE = VsNodeType int; P0–P4 params)
- `bpVsLink=srcID,pinClass,idx|dstID,pinClass,idx` — pinClass 0=exec-out, 1=exec-in, 2=data-out, 3=data-in
- keep `bpVsNodeCount` / `bpVsLinkCount` / `bpVsNextId` in sync
- Attached Sprite = self; resolve via `afn_bp_cur_spr_idx` in codegen (NOT resolveInt, which yields 0)
- **Watch for stubbed nodes**: several PSV nodes emit `/* TODO: emit node type N */` and silently no-op — check the generated psv_script.h when a graph misbehaves.

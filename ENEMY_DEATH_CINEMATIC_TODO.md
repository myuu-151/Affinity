# TODO — Enemy death cinematic + clip-index fix (PSV)

Handoff for the hardcoded enemy NPC combat AI in `psv_runtime/main.c` (all blocks
tagged `// HARDCODED`, gated to NPC **editor sprite index 3** = `AFN_ENEMY_EIDX`).
Project: pokemon_arena (`3dblitz11.afnproj`), rig **r8.glb** (riolu), shared by
player + enemy (rig slot 0).

## Goal (user request)
When the enemy is beaten (HP hits 0): **play the `die` clip**, and **zoom the
camera in + slowly orbit around the enemy** (a KO cinematic), then despawn.

---

## ⚠️ FIRST: fix shifted clip indices (combat anims are currently wrong)

The user added a `die` animation. The editor keeps clips **name-sorted
(case-insensitive)**, so adding `die` (now index **21**) shifted every clip after
it by **+1**. The enemy AI's hardcoded clip indices in `npc_ai_tick` are now wrong
for movement clips.

**Current authoritative order (r8.glb, 38 clips, name-sorted):**
```
0 atk_phs        10 atk_spc_chg_L      20 crouch       30 Move
1 atk_spc_chg    11 atk_spc_chg_LD     21 die  (NEW)   31 strafeL
2 atk_spc_chg_air 12 atk_spc_chg_LDFW  22 DodgeBWD     32 strafeLD
3 atk_spc_chg_BWD 13 atk_spc_chg_R     23 DodgeFW      33 strafeLDFW
4 ..dodge_BWD     14 atk_spc_chg_RD    24 DodgeL       34 strafeR
5 ..dodge_fwd     15 atk_spc_chg_RDFW  25 DodgeR       35 strafeRD
6 ..dodge_L       16 atk_spc_lnc       26 Idle         36 strafeRDFW
7 ..dodge_R       17 atk_spc_lnc_air   27 jump         37 TPOSE
8 ..dodge_R.001   18 backpeddle        28 jump_fall
9 atk_spc_chg_fwd 19 Block             29 land
```
(Re-derive any time with: `python` parse of `pokemon_arena/ftr/riolu/r8.glb` JSON
chunk → `animations[]` sorted by `name.lower()`.)

**Edits in `npc_ai_tick` (all in `psv_runtime/main.c`):**
| Where | OLD | NEW |
|---|---|---|
| ROAM walk/idle | `s_npcNavMoving[i] ? 29 : 25` | `? 30 : 26` (Move / Idle) |
| CHASE clip | `s_npcClip[i] = 29` | `= 30` (Move) |
| STRAFE `eStrafe[8]` | `{29,32,30,34,18,31,33,35}` | `{30,33,31,35,18,32,34,36}` |
| Dodge clip | `side>0 ? 24 : 23` | `side>0 ? 25 : 24` (DodgeR/DodgeL) |
| CHARGE clip `1`, FIRE clip `16` | — | unchanged (before the insertion) |

> Hardcoded indices are fragile: any future rig re-import re-sorts + shifts them.
> Longer-term fix = export clip NAMES to the runtime and look up by name.

---

## THEN: the death cinematic (not yet implemented)

In `npc_ai_tick`, the **death branch** currently does:
`s_aiState = AI_DEAD; afn_sprite_visible[eidx] = 0; s_efbCharging = 0; return;`
(immediate despawn). Replace with a dying/KO sequence:

1. Add state: `s_koActive` + `s_koTimer` (+ maybe `s_koAngle0`, `s_koDist`). On death,
   set `s_koActive = 1`, `s_koTimer = 0`, **keep the enemy visible**, set
   `s_aiState = AI_DEAD`, kill any orb (`s_efbActive = s_efbCharging = 0`).
2. While dying: `s_npcClip[slot] = 21` (die). (die is 24 frames; it's flagged Loop —
   decide whether to let it loop or hold the last frame for the KO.)
3. **Camera cinematic** — the orbit camera is at `psv_runtime/main.c` ~**3180–3345**.
   It computes `targetX/targetY/targetZ` (look-at), `camAngle` (yaw), `effDist`
   (distance), `s_camPosPitch`/`pitch` BEFORE the eye is computed at ~**3295**.
   Inject an override there when `s_koActive`:
   - `targetX = s_npcX[slot]`, `targetZ = s_npcZ[slot]`,
     `targetY = s_npcY[slot] + afn_npc_col[slot][4]` (enemy box center).
   - `camAngle = startAngle + slowRate * s_koTimer` (slow orbit, e.g. ~0.01 rad/frame).
   - `effDist`: ease toward a zoomed-in distance (e.g. `camDist * 0.45`).
   - `pitch`: a fixed gentle downward angle.
   (Find where `targetX/camAngle/camDist/pitch` are first set, just above ~3250, and
   override right after, before the eye math at 3295.)
4. After `s_koTimer` reaches the cinematic length (~3–4 s = die anim + orbit), set
   `afn_sprite_visible[eidx] = 0` (despawn) and `s_koActive = 0` (camera returns to
   the player normally).

`slot` = the NPC array index where `(int)afn_npc_inst[i][7] == AFN_ENEMY_EIDX (3)`.

---

## Code anchors (`psv_runtime/main.c`)
- Enemy AI state + `ENEMY_*` tuning `#define`s: ~line **77**.
- `npc_ai_tick` (death branch + ROAM/CHASE/STRAFE/CHARGE/FIRE, clip indices to fix): ~**1414+**.
- `enemy_projectile_tick`, `enemy_orb_render`, `enemy_muzzle`, `resolve_focus_inst`: ~**1297–1413**.
- Orbit camera (inject cinematic override here): ~**3180–3345**; eye computed ~**3295**.
- `npc_ai_tick` call site: NPC physics loop ~**2975**.

## Build + run
- Build PSV: `/c/devkitPro/msys2/usr/bin/bash.exe -lc 'export VITASDK=/c/vitasdk; export PATH=$VITASDK/bin:$PATH; cd /c/Users/NoSig/Documents/gbadev/Affinity/psv_runtime/build && make'` → `affinity_psv.vpk`.
- Run: `cd /c/Users/NoSig/Documents/gbadev/vita3k && ./Vita3K.exe "<repo>/psv_runtime/build/affinity_psv.vpk"` (installs + runs; title `AFNT00002`).
- Enemy config in `psv_runtime/include/psv_rig.h`: `AFN_NPC_COUNT 1`, `afn_npc_inst[0]` col 7 = **3** (eidx), col 6 = **0** (rig slot), `afn_npc_col[0]` center offset `cy = 3.75`.

## Status
- ✅ Enemy AI working (roam → lock-on strafe → charge/tap projectiles → dodge → despawn + HP bar), pushed to master, archived in `myuu-151/pokemon-arena`.
- ✅ Ground-aware camera shipped in **v0.9.3**.
- ✅ Clip-index fix (above) — re-mapped all movement/dodge clips for the inserted `die` (index 21).
- ✅ Death cinematic — `die` (clip 21, non-loop, holds last frame) + zoom-in/slow-orbit around the enemy box center, despawn after `ENEMY_KO_FRAMES` (210). State: `s_koActive/s_koTimer/s_koSlot/s_koAngle0`; camera override injected before the eye math (~3295).
- ⬜ **Name-based clip lookup** — hardcoded indices re-broke when `die` shifted everything. Export clip NAMES → resolve indices by name at runtime init so a rig re-import/re-sort never breaks the AI again.

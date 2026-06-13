# PSV Camera System

How the PS Vita runtime camera works (`psv_runtime/main.c`). It's the most
layered subsystem in the runtime: several independent features all read and
write the **same two orbit globals** in a fixed per-frame order, and the trick
is that they must not fight each other. This documents the order, each layer,
and the invariants that keep tweaks from breaking other layers.

> Editor-facing note: the camera is **node-driven**. The runtime only reacts to
> globals that nodes set (`orbit_angle`, `afn_active_camera`, `afn_cam_lock_target`,
> `afn_tank_camera`, …). It never hardcodes orbit input.

---

## Core state

Two globals are the single source of truth; everything ultimately steers these:

| Global | Meaning | Units |
| --- | --- | --- |
| `orbit_angle` | camera yaw | brad (65536 = 360°) |
| `orbit_pitch` | camera pitch | brad (+ = look **down**) |

Camera **forward** is `+(sin, cos)(camAngle)` in XZ, `-sin(pitch)` in Y. Facing a
world point therefore means `yaw = atan2(dx, dz)` — *not* the negated form (that
cost a device test once; see `project-psv-lockon`).

`camAngle` / `pitch` (radians) are derived from the brad globals **once per
frame, after all the yaw/pitch layers below have run**, then consumed by movement
and the eye solve.

---

## Camera slots

`afn_cam_slots[][5]` — slot 0 is the scene-start camera (Camera Properties),
slots 1..N are SetCamera presets. Columns:

```
{ yaw(rad), orbit dist(world px), camera height(world px), horizon(editor px), pitch(deg, 0=auto) }
```

- `AFN_CAM_SLOT_COUNT` sizes the array; `afn_active_camera` selects the row
  (clamped to range — an out-of-range index would read past the array and crash).
- Emitted by the shared exporter (`src/platform/psp/psp_package.cpp`,
  `GeneratePSPMapData`), so PSP and PSV share the layout. **NDS uses column 3
  (horizon); PSV uses column 4 (pitch, degrees).**
- Pitch authoring is unified: slot Pitch and Camera Properties Pitch are the same
  degrees convention (`0 = auto`, derive tilt from height/distance).

---

## Per-frame order

Within the main `while (1)` loop, the camera touches `orbit_angle/orbit_pitch`
in this sequence. **Order matters** — later layers correct or override earlier
ones.

1. **`script_tick`** — nodes run first: OrbitCamera writes `orbit_angle/pitch`,
   SetCamera sets `afn_active_camera`, Lock On sets `afn_cam_lock_target`, etc.
2. **Slot blend + switch ease** (≈ main.c:1597) — `camDist`/`camHeight` ease
   toward the active slot every frame (~⅛ step). Yaw/pitch are a **one-shot**
   ease that fires only when the slot index changes, then releases.
   *While a lock target is active this yaw/pitch ease is skipped* (the lock
   assist owns them — otherwise a Set Camera would snap the orbit toward the
   slot's absolute yaw before the lock pulls it back).
3. **Lock-on assist** (≈ main.c:1644, `#ifdef AFN_HAS_PLAYER_RIG`) — while
   `afn_cam_lock_target >= 0`, ease both axes toward the target, 10%/frame:
   - yaw → `atan2(dx, dz)`
   - pitch → `heightP + trackP`, where
     `heightP = atan2(afn_lock_height, orbitDist)` (constant elevated framing,
     referenced to orbit distance so it doesn't steepen on approach) and
     `trackP = atan2(playerY - targetY, horiz)` (up on a jump, down on a low
     target). **No Look-Down** clamps only `trackP`'s down side.
4. **Orbit-rate clamp + "orbiting?" detect** (≈ main.c:1672) — `AFN_ORBIT_MAX_DELTA`
   caps this frame's `orbit_angle` change so the position chase can keep up;
   `orbitingNow` records whether the angle moved (selects the ease rate later).
5. **Pitch hard clamp** — `orbit_pitch` to ≈ ±80° (±14563 brad).
6. **Derive radians** — `camAngle`, `pitch`. Movement reads `camAngle` from here.

The **eye solve** (render block, ≈ main.c:2167) runs later in the same frame and
only *reads* `camAngle/pitch`.

---

## Camera-delay system (position vs view decoupling)

This is the subtlest part and the easiest to break. NDS parity: the view
**direction snaps** with `orbit_angle/pitch` every frame, but the camera
**position lags**.

- **Y follow** — `s_camFollowY` eases toward `playerY` at `AFN_JUMP_CAM_LAND`
  (grounded) or `AFN_JUMP_CAM_AIR` (airborne), so the camera trails a jump's apex.
  Look target is `playerY + camHeight*0.5` (torso, not feet).
- **Ease rate** (`camEase`) is picked per frame from what the player's doing:
  walk vs sprint (`afn_speed_prio`), ease-in while moving / ease-out while still,
  and bumped to the orbit ease rate when `orbitingNow`.
- **Position pitch lag** — `s_camPosPitch` eases toward `pitch` at `camEase`, while
  the **view** pitch uses `pitch` directly. So pitching slides the player
  off-center vertically, then re-centers as the chase catches up.
- **XZ chase + circle re-projection** — `s_camEyeX/Z` ease toward the ideal orbit
  point, then are **re-projected onto the orbit circle** (radius `horizR`). The
  raw lerp cuts across the chord as the target sweeps the circle, which would read
  as a zoom-in/out; snapping the radius back keeps the angular glide at constant
  distance.
- **The view is built with `look_at` aimed *along* `camAngle/pitch` from the eased
  eye** — NOT `look_at(player)`. A look-at-player would re-aim every frame and
  kill the visible delay. When the chase is fully caught up, the two are identical.

**Tuning knobs** (Camera Properties → emitted `#define`s): `AFN_WALK_EASE_IN/OUT`,
`AFN_SPRINT_EASE_IN/OUT`, `AFN_ORBIT_EASE_IN/OUT`, `AFN_ORBIT_MAX_DELTA`,
`AFN_JUMP_CAM_LAND/AIR` (all x/256 catch-up per frame).

---

## Lock-on framing (over-the-shoulder)

Separate from the yaw/pitch assist, the eye solve blends an over-the-shoulder
frame while locked (`#ifdef AFN_HAS_CAM_LOCK`, ≈ main.c:2188):

- `s_lockFrame` eases 0→1 on lock / 1→0 on unlock (blends the whole effect).
- **Lateral shift** — the *look point* is pushed along camera-right by
  `afn_lock_side`, so eye+target pan together (a pure lateral offset, not a
  rotation). The side **auto-switches** to keep the player off the target, with a
  0.15 rad hysteresis band and an eased offset (`s_lockSideEased`) to smooth flips.
- **Zoom** — `effDist = camDist * (1 ± afn_lock_zoom)` (sign from `afn_lock_zoom_in`),
  clamped to ≥ 25% so a big zoom-in can't collapse onto the player.

Lock On node sliders → `afn_lock_zoom`, `afn_lock_side`, `afn_lock_zoom_in`,
`afn_lock_height`, `afn_lock_no_lookdown` (the last two are packed into
`paramInt[2]` bit1 + `paramInt[3]`).

`afn_in_view(spr)` (before the `psv_script.h` include) measures a target's angle
from the **camera eye** (`g_camEyeX/Z`, set during the eye solve) against a ~57°
half-FOV — used by the Is In View gate.

---

## Tank camera

`afn_tank_camera` freezes the camera's world yaw so orbit input turns the
*player* (`afn_player_heading`) instead. Movement axes then follow the heading
(`afn_tank_move`) rather than `camAngle`. Lock Strafe overrides both (axes follow
the player→target line).

---

## Invariants (don't break these)

1. **All yaw/pitch layers write `orbit_angle/orbit_pitch` in the order above**,
   then radians are derived once. Inserting a write out of order (e.g. after the
   clamp) desyncs movement from the view.
2. **View direction snaps; position lags.** Never replace the
   `look_at(eye, eye+forward)` with `look_at(player)` — it silently removes the
   camera delay.
3. **Don't let two layers ease the same axis simultaneously.** The slot-switch
   ease and the lock assist both target yaw/pitch; the lock case is resolved by
   skipping the slot ease while locked. Any new auto-camera must pick an owner.
4. **Re-project the eased eye onto the orbit circle** after the XZ lerp, or
   orbiting reads as a zoom wobble.
5. **`afn_active_camera` is clamped before indexing** `afn_cam_slots`.
6. Camera math against editor-authored content uses **GBA units (240×160)**, not
   the Vita's resolution (see root `CLAUDE.md`).

---

## Quick map

| Concern | Location (`psv_runtime/main.c`) |
| --- | --- |
| Orbit globals | `orbit_angle`, `orbit_pitch` (≈ :763) |
| Slot table | `afn_cam_slots` (generated, `psv_mapdata.h`) |
| Slot blend + switch ease | ≈ :1597 |
| Lock-on yaw/pitch assist | ≈ :1644 |
| Orbit-rate clamp / `orbitingNow` | ≈ :1672 |
| Pitch clamp + radians | ≈ :1690 |
| Movement (reads `camAngle`) | ≈ :1700 |
| Y follow / ease rate | ≈ :2167 |
| Lock framing (zoom + side) | ≈ :2188 |
| Position-pitch lag | ≈ :2245 |
| Eye XZ chase + re-projection | ≈ :2256 |
| `look_at` (snapped direction) | ≈ :2314 |

Line numbers drift — search the named globals/comments if they've moved.

# Node Implementation Convention

> **Looking for what each node does?** See [`NODE_REFERENCE.md`](NODE_REFERENCE.md) — a per-node catalog of all ~249 nodes with descriptions and pins. This document is about *implementing* nodes, not using them.

This doc is the cold-start guide for adding, modifying, and debugging Visual Script nodes in Affinity. Read this first if a session was compacted, you've never touched the node system, or you're about to add a node and want to do it right the first time.

The system is small but it touches a lot of files. Get any one of them wrong and the symptom is usually "node visible in editor, does nothing at runtime" or "node compiles but bricks every saved project from before the change." Both are fixable but both waste a build cycle. Read the relevant section before editing.

---

## Table of contents

1. [Philosophy](#philosophy)
2. [Mental model: editor → exporter → runtime](#mental-model)
3. [File map](#file-map)
4. [Anatomy of a node](#anatomy-of-a-node)
5. [Pin types and the `sVsNodeDefs` row](#pin-types-and-the-svsnodedefs-row)
6. [Color / category conventions](#color--category-conventions)
7. [Param widget types](#param-widget-types)
8. [The 16-point checklist for adding an action node](#the-16-point-checklist-for-adding-an-action-node)
9. [Gate nodes (conditions)](#gate-nodes-conditions)
10. [Event nodes](#event-nodes)
11. [Data nodes (pure math / getters)](#data-nodes-pure-math--getters)
12. [setActionFunc bodies — the runtime chain rule](#setactionfunc-bodies--the-runtime-chain-rule)
13. [Direct codegen vs blueprint codegen](#direct-codegen-vs-blueprint-codegen)
14. [Mode 0 / Mode 4 / Mode 7 runtime targets](#mode-0--mode-4--mode-7-runtime-targets)
15. [Runtime variable conventions (`afn_*`)](#runtime-variable-conventions-afn_)
16. [Where runtime globals are defined per platform](#where-runtime-globals-are-defined-per-platform)
17. [The sentinel macro pattern for backwards-compat](#the-sentinel-macro-pattern-for-backwards-compat)
18. [Enum ordering rules — NEVER insert in the middle](#enum-ordering-rules--never-insert-in-the-middle)
19. [Version migration when an enum has already shifted](#version-migration-when-an-enum-has-already-shifted)
20. [Worked example: `SetVelocityX` / `SetVelocityZ` / `VelocityFalloff`](#worked-example-setvelocityx--setvelocityz--velocityfalloff)
21. [Common pitfalls](#common-pitfalls)
22. [Glossary of identifiers](#glossary-of-identifiers)
23. [Quick reference card](#quick-reference-card)

---

## Philosophy

Three rules govern everything in the node system. Internalize them before touching anything.

**1. Behavior is purely node-driven.** The runtime (`gba_runtime/source/main.c`, `nds_runtime/source/*.c`) only acts on flags and variables that nodes have set. The runtime never hardcodes gameplay behavior independent of the script. If you find yourself adding `if (some_condition) do_something();` to `main.c` without a node controlling it, stop — add a node that sets a flag, and gate the runtime behavior on that flag.

Why: this is what makes the editor authoritative. The user designs in nodes; the runtime is "dumb." Two consequences:
- You can always trace runtime behavior back to a specific node in the editor.
- Disabling a node disables the behavior. No "magic" defaults sneaking in.

**2. `setActionFunc` bodies are the real C code, not previews.** The strings passed to `setActionFunc(infoNode, "_label", "...")` in `frame_loop.cpp` are displayed in the editor's "Mode 0 Runtime" code window. They must match the actual emitted code line-for-line, plus a `// --- Runtime (main.c) ---` separator and 2-4 lines showing how `main.c` consumes the variables.

Why: users learn what nodes do by reading the runtime preview. If it lies, they file bugs against working code or build expectations that don't match reality.

**3. When you change a node, you change all three places in the same edit.** The exporter (`gba_package.cpp` / `nds_package.cpp`), the `setActionFunc` body in `frame_loop.cpp`, and the runtime consumption (`main.c` / `fps3d.c`) form one unit. Touching one without the others is a bug. Always.

Why: drift between these three is invisible until you hit a specific code path, and then it's catastrophic — the preview says one thing, the export does another, the runtime does a third. The CLAUDE.md elevates this to "CRITICAL: ... do NOT wait to be asked." Treat it as a hard rule.

---

## Mental model

```
┌─────────────────────────────────────────┐
│ EDITOR (src/editor/frame_loop.cpp)      │
│                                         │
│ - VsNodeType enum                       │
│ - sVsNodeDefs[] table (name, pins)      │
│ - Description switch (tooltip)          │
│ - addNodeAt context menu                │
│ - setActionFunc body (preview text)     │
│ - Suffix returner (display chain naming)│
│ - Export suffix table (codegen naming)  │
└────────────────┬────────────────────────┘
                 │   user clicks Export
                 ▼
┌─────────────────────────────────────────┐
│ EXPORTER                                │
│  - GBA: src/platform/gba/gba_package.cpp│
│         (and .h for enum)               │
│  - NDS: src/platform/nds/nds_package.cpp│
│                                         │
│  Reads node graph, walks chains, emits  │
│  C source into runtime's mapdata.h.     │
└────────────────┬────────────────────────┘
                 │   make
                 ▼
┌─────────────────────────────────────────┐
│ RUNTIME                                 │
│  - GBA: gba_runtime/source/main.c       │
│  - NDS: nds_runtime/source/main.c       │
│         + fps3d.c (Mode 4 player)       │
│         + script_glue.c (script vars)   │
│                                         │
│  Includes mapdata.h, calls into emitted │
│  script functions, reads emitted vars.  │
└─────────────────────────────────────────┘
```

The same node graph drives two platforms with different code-gen and runtime conventions. A node is "complete" only when both targets render its behavior the same way (modulo platform limits — NDS has 16 channels, GBA has 6 voices, etc.).

---

## File map

If a section below says "edit X," these are the absolute paths:

| Purpose | Path |
|---|---|
| Editor / node UI | `src/editor/frame_loop.cpp` |
| Editor enum | `src/editor/frame_loop.cpp`, `enum class VsNodeType` |
| GBA exporter enum | `src/platform/gba/gba_package.h`, `enum class GBAScriptNodeType` |
| GBA exporter codegen | `src/platform/gba/gba_package.cpp` |
| NDS exporter codegen | `src/platform/nds/nds_package.cpp` |
| GBA runtime | `gba_runtime/source/main.c` |
| NDS runtime (player) | `nds_runtime/source/fps3d.c` |
| NDS runtime (script vars) | `nds_runtime/source/script_glue.c` |
| NDS runtime main loop | `nds_runtime/source/main.c` |
| Generated header (don't edit) | `gba_runtime/include/mapdata.h` / `nds_runtime/include/mapdata.h` |
| Mode 0 tile runtime | `nds_runtime/source/mode0.c` (also in main.c on GBA) |
| Top-level docs | `docs/AFN_API.md`, `docs/COOKBOOK.md`, `docs/REFERENCE.md`, this file |

`mapdata.h` files are **generated by the editor's export step**. Never edit them directly. If you need a value in the runtime, add it to the exporter and re-export.

---

## Anatomy of a node

Every node has six observable properties:

1. **Type** — `VsNodeType` enum value in `frame_loop.cpp`. Also has a mirror in `GBAScriptNodeType` in `gba_package.h` with identical ordering.
2. **Display name** — string in `sVsNodeDefs`. What appears on the node header in the editor canvas.
3. **Color** — RGBA hex in `sVsNodeDefs`. Conveys category at a glance (see below).
4. **Pins** — execution in/out + data in/out, defined in `sVsNodeDefs`.
5. **Behavior** — emitted C code from `setActionFunc` (preview), `gba_package.cpp` and `nds_package.cpp` (real export), and `main.c` / `fps3d.c` (consumption).
6. **Description** — tooltip string in a `case` of the description switch.

A node "works" only when all six are consistent.

---

## Pin types and the `sVsNodeDefs` row

`sVsNodeDefs` is a flat array indexed by `(int)VsNodeType`. Each row is:

```cpp
{ "Display Name", 0xFFcccccc /*color*/,
  execIn,      // 0 or 1 — does the node have an exec-input pin?
  execOut,     // 0 or 1 — does it have an exec-output pin?
  dataInCount, // number of data inputs
  dataOutCount,// number of data outputs
  {"Pin A","Pin B"}, // labels for the data inputs (size must equal dataInCount)
  {"Result"},        // labels for the data outputs (size must equal dataOutCount)
  {} },              // currently unused — reserved for future extension
```

Concrete examples from the codebase:

```cpp
// Action: 1 exec in, 1 exec out, takes a float velocity
{ "Set Velocity Y", 0xFF3355AA, 1, 1, 1, 0, {"Velocity (float)"}, {}, {} },

// Pure data node: no exec pins, 2 ins, 1 out
{ "Add",            0xFF666688, 0, 0, 2, 1, {"A", "B"}, {"Result"}, {} },

// Event: no exec in, 1 exec out, no data
{ "On Start",       0xFF338833, 0, 1, 0, 0, {}, {}, {} },

// Gate: takes exec in, passes through; condition decides
{ "Is Near",        0xFF885533, 1, 1, 3, 0, {"Obj A (int)", "Obj B (int)", "Radius (int)"}, {}, {} },
```

**Pin label conventions:** `"Name (int)"`, `"Name (float)"`, `"Name (string)"` clarifies the inline editor widget. The editor uses the suffix to choose what UI to show. If you omit the type hint, you get the default (treated as int).

---

## Color / category conventions

| Color | Hex | Category | Examples |
|---|---|---|---|
| Green | `0xFF338833` | Event | `OnStart`, `OnTimer`, `OnKeyPressed`, `OnDeath`, `OnHit`, `OnRise` |
| Blue | `0xFF3355AA` | Action | `SetVelocityY`, `Teleport`, `PlaySound`, `Jump`, `ShowHUD` |
| Orange | `0xFF885533` | Gate / Condition | `IsNear`, `IsMoving`, `IsFlagSet`, `IsHpZero`, `HasItem` |
| Purple | `0xFF666688` | Data / Math | `Add`, `Subtract`, `RandomInt`, `GetFlag`, `GetPlayerX`, `Min`, `Max` |

When you add a node, pick the color that matches its category. The user reads color before reading the name. A blue gate or a green action will confuse people for years.

---

## Param widget types

Inside a node, data input pins that aren't wired to other nodes get an inline editor widget. The type is inferred from the pin label suffix:

| Label suffix | Widget | Stored as |
|---|---|---|
| `(int)` | `ImGui::InputInt` | `paramInt[i]` |
| `(float)` | `ImGui::InputFloat` | `paramInt[i]` (bit-cast — see below) |
| `(string)` | `ImGui::InputText` | `paramStr[i]` |
| (none, default) | int | `paramInt[i]` |

**Float storage quirk:** floats are stored in the `int paramInt[]` slot via `memcpy` bit-cast, so the on-disk format stays integer. The exporter reads back with `memcpy(&value, &paramInt[0], sizeof(float))`. This is why `SetVelocityY`'s codegen does:

```cpp
auto* vData = findDataIn(action->id, 0);
float vel = vData ? resolveFloat(vData) : 0.0f;  // resolveFloat handles the memcpy
int velFixed = (int)(vel * 256.0f);
f << "    player_vy = " << velFixed << ";\n";
```

`resolveFloat` already does the right thing. Don't reinvent it.

---

## The 16-point checklist for adding an action node

This is the full list of edits. Skip a step and the node will fail in subtle ways (compile, but no-op; or compile, work in editor, fail on export; or work on GBA, fail on NDS).

Use this as a literal checklist. Tick each one as you do it.

### Editor side (`src/editor/frame_loop.cpp`)

- [ ] **(1)** Append to `enum class VsNodeType` **before `COUNT`**.
  ```cpp
  // Around line ~570
  UpdateRespawnPos,
  YourNewNode,    // <-- here
  COUNT
  ```

- [ ] **(2)** Append a row to `sVsNodeDefs` **in the same relative position as the enum entry**.
  ```cpp
  // Around line ~865
  { "Update Respawn Pos", ... },
  { "Your New Node", 0xFF3355AA, 1, 1, 1, 0, {"Param (int)"}, {}, {} }, // <-- here
  };
  ```

- [ ] **(3)** Add a `case` to the description switch for the tooltip.
  ```cpp
  // grep for "VsNodeType::UpdateRespawnPos: desc ="
  case VsNodeType::YourNewNode: desc = "What this node does. Brief but specific."; break;
  ```

- [ ] **(4)** Add a `case` to the **display-time** suffix returner (used by the in-editor chain preview).
  ```cpp
  // grep for `case VsNodeType::UpdateRespawnPos: return "_update_respawn_pos";`
  case VsNodeType::YourNewNode: return "_your_new_node";
  ```

- [ ] **(5)** Add a `case` to the `setActionFunc` body switch (the preview C code). **This is the rule-3 critical step.**
  ```cpp
  // grep for `case VsNodeType::SetVelocityY: {` — copy that pattern
  case VsNodeType::YourNewNode: {
      editorCode = "// Short description of what this does";
      char bodyBuf[512];
      snprintf(bodyBuf, sizeof(bodyBuf),
          "    afn_your_var = %s;\n"
          "    // --- Runtime (main.c) ---\n"
          "    // Mode 4: <what main.c does with afn_your_var>\n"
          "    //         <another line if needed>",
          fmtFloat(infoNode.id, 0, "<value>"));  // or fmtInt
      setActionFunc(infoNode, "_your_new_node", bodyBuf);
      break;
  }
  ```

- [ ] **(6)** Add a context-menu `ImGui::MenuItem` so the user can place it.
  ```cpp
  // grep for `if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::SetVelocityY].name))`
  if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::YourNewNode].name)) addNodeAt(VsNodeType::YourNewNode);
  ```

- [ ] **(7)** Add a `case` to the **export-time** suffix table (used at codegen time to name script functions).
  ```cpp
  // grep for `case VsNodeType::SetVelocityY:  suffix = "_set_vel_y"; break;`
  case VsNodeType::YourNewNode: suffix = "_your_new_node"; break;
  ```

### GBA exporter side (`src/platform/gba/`)

- [ ] **(8)** Append to `enum class GBAScriptNodeType` in `gba_package.h` **in the same order as VsNodeType**. ORDER MUST MATCH. Both enums are integers serialized into project files — divergence breaks every save.
  ```cpp
  // gba_package.h, around line ~382
  UpdateRespawnPos,
  YourNewNode,    // <-- here, mirror VsNodeType
  COUNT
  ```

- [ ] **(9)** Emit any required runtime globals to `mapdata.h`. In `gba_package.cpp`, find the block emitting `player_vy`, `afn_gravity`, etc., and add yours.
  ```cpp
  f << "static int   afn_your_var;\n";
  ```
  If your node introduces NEW globals, also emit a `#define AFN_HAS_YOUR_FEATURE 1` sentinel so runtime fallbacks work (see [sentinel macro pattern](#the-sentinel-macro-pattern-for-backwards-compat)).

- [ ] **(10)** Add the **direct exporter** codegen case (around `case GBAScriptNodeType::SetVelocityY:` ~line 2618).
  ```cpp
  case GBAScriptNodeType::YourNewNode: {
      auto* d = findDataIn(action->id, 0);
      float v = d ? resolveFloat(d) : 0.0f;
      int fixed = (int)(v * 256.0f);
      f << "    afn_your_var = " << fixed << ";\n";
      break;
  }
  ```

- [ ] **(11)** Add the **blueprint exporter** codegen case (around `case GBAScriptNodeType::SetVelocityY:` ~line 4151). Same logic, different resolver functions (`bpFindDataIn`, `bpResolveFloat`, `bpResolveInt`).
  ```cpp
  case GBAScriptNodeType::YourNewNode: {
      auto* d = bpFindDataIn(action->id, 0);
      std::string v = d ? bpResolveFloat(d) : "0";
      f << "    afn_your_var = " << v << ";\n";
      break;
  }
  ```

  Direct codegen resolves to a literal value at export time. Blueprint codegen emits an expression because blueprint params are baked at instance-time inside the blueprint's emitted function.

### NDS exporter side (`src/platform/nds/nds_package.cpp`)

- [ ] **(12)** Add the exporter codegen case (around `case GBAScriptNodeType::SetVelocityY:`). NDS uses the same `GBAScriptNodeType` enum.
  ```cpp
  case GBAScriptNodeType::YourNewNode: {
      auto* d = findDataIn(a->id, 0);
      float v = d ? resolveFloat(d) : 0.0f;
      f << "    afn_your_var = " << (int)(v * 256.0f) << ";\n";
      break;
  }
  ```

- [ ] **(13)** Add `extern` declarations to the NDS mapdata.h emit (around `extern int player_vy;`). NDS globals are defined in `script_glue.c`, not `mapdata.h`; mapdata.h only declares them as `extern`.
  ```cpp
  f << "extern int afn_your_var;\n";
  ```

### Runtime side

- [ ] **(14)** Add per-frame logic to GBA `gba_runtime/source/main.c` (in the Mode 4 player update block around line ~7720). Wrap in `#ifdef AFN_HAS_SCRIPT`.
  ```c
  #ifdef AFN_HAS_SCRIPT
  // Apply afn_your_var to player state
  player_x += afn_your_var;
  // decay or whatever
  #endif
  ```

- [ ] **(15)** Add per-frame logic to NDS `nds_runtime/source/fps3d.c` (in the script-driven movement block). Same gating.
  ```c
  #ifdef AFN_HAS_SCRIPT
  extern int afn_your_var;  // forward decl in case mapdata.h is stale
  player_x += afn_your_var;
  #endif
  ```

- [ ] **(16)** Add the global definition to NDS `nds_runtime/source/script_glue.c`. This is the actual variable storage; `mapdata.h` only externs.
  ```c
  int afn_your_var;
  ```

---

## Gate nodes (conditions)

A gate has 1 exec in, 1 exec out, and 1-3 data inputs that parameterize the condition. At runtime, the gate emits an `if (condition) {` block — the exec-out chain continues inside that block. The chain walker handles the closing brace automatically.

Example from the exporter (`nds_package.cpp`):
```cpp
case GBAScriptNodeType::IsNear:
    f << "    if (sprite_dist(spr_a, spr_b) < radius) {\n";
    break;
case GBAScriptNodeType::IsMoving:
    f << "    if (player_moving) {\n";
    break;
```

A gate does NOT emit a closing brace itself. The walker (search for `inJumpGate` or `closeBraceCount` in the exporters) closes them after all chained children emit. If you need a closing brace pattern that differs, add a flag and handle it in `emitChain`.

**Critical gate rule:** action nodes that WRITE state (e.g. `Jump` sets `player_vy`) must execute **before** gates that READ it (e.g. `IsJumping` checks `player_vy > 0`) in the same frame. The exporter's walk-order sort ensures this — search for "Walk action exec siblings before gates" in `frame_loop.cpp`. If you add a gate that reads state set by other nodes, verify that order still holds.

---

## Event nodes

Events have 0 exec in, 1 exec out, and supply the entry point for a chain. The runtime calls the entry function once per frame (`OnStart`, `OnTimer`) or on a triggering condition (`OnKeyPressed`, `OnHit`, `OnDeath`, `OnRise`).

`OnRise` is special: it edge-detects a condition wired in front of it. Its state is one `int` per OnRise node, stored in `mapdata.h` initialized to `-2` so the first true edge fires. Search for `riseIds` in `nds_package.cpp` to see how the state set is built.

When adding an event:
- Decide if it's frame-polling (like `OnTimer`) or trigger-driven (like `OnHit`).
- Frame-polling: the exporter emits a call into the event's chain every frame.
- Trigger-driven: identify the runtime code path that detects the trigger, and have it call into the emitted function for that node id.

---

## Data nodes (pure math / getters)

These have 0 exec pins. They're evaluated **at the call site** of whatever consumes them. The exporter recursively expands data nodes into expressions:

```cpp
// In the exporter, data resolution is recursive:
// SetVelocityX(value=Add(A=5, B=GetPlayerX))
// becomes:
//   afn_player_vx_world = (5 + player_x);
```

`resolveInt` / `resolveFloat` walk inputs of data pins recursively. The `setActionFunc` body for a pure data node should be:
```cpp
setActionFunc(infoNode, "_add", "// --- Runtime --- inline data node, evaluated at call site");
```

Don't try to give pure data nodes a "behavior" — they don't have one independent of their consumer.

---

## setActionFunc bodies — the runtime chain rule

This is the rule that ships the most invisibly-broken nodes. From CLAUDE.md:

> Every action node in `src/editor/frame_loop.cpp` has a `setActionFunc()` call that defines the actual C implementation shown in the editor's "Mode 0 Runtime" code window. This is NOT a preview — it is the real generated code.

Required structure of every action node's `setActionFunc` body:

```
    <the literal C code emitted into mapdata.h, e.g. `afn_var = 5;`>
    // --- Runtime (main.c) ---
    // Mode 0: <2-4 lines on how Mode 0 (tile runtime) consumes the var>
    // Mode 4: <2-4 lines on how Mode 4 (FPS runtime) consumes the var>
```

If only one mode is relevant, say so explicitly (`// Mode 4 only`). If the node is a pure data/gate, use the abbreviated form documented above.

**Worked example** from `Sprint`:

```cpp
setActionFunc(infoNode, "_sprint",
    "    afn_move_speed = 37;\n"
    "    // --- Runtime (main.c) ---\n"
    "    // Mode 0: tm_move_frames = 48 / afn_move_speed; // = 1 frames/tile\n"
    "    //         tm_move_timer = tm_move_frames;\n"
    "    //         px = lerp(fromX, toX, t / tm_move_frames);\n"
    "    // Mode 4: moveSpeed = afn_move_speed; // = 37\n"
    "    //         player_x += (viewSin * inputFwd * moveSpeed) >> 16;");
```

The "Runtime" block teaches users what the node actually does. It's the difference between "I see Sprint sets a number" and "I see Sprint makes me cross a tile in 1 frame on the tilemap and accelerate the player in 3D."

When you change ANY node behavior — the exporter emit, the runtime consumption, the gate condition, the variable name — update this string in the SAME edit pass. Do not defer. Search the codebase for `setActionFunc(infoNode, "_your_label"` and read the body. If it doesn't match the new behavior, the node is broken in a way nobody will notice for weeks.

---

## Direct codegen vs blueprint codegen

The GBA exporter has two parallel emission paths in `gba_package.cpp`:

- **Direct** (around line ~2620 in the file): emits when a node lives in the scene script. Values are resolved to literals at export time.
- **Blueprint** (around line ~4150): emits when a node lives inside a blueprint definition. Values are emitted as expressions because each blueprint *instance* substitutes its own params at runtime.

The two paths use different resolver functions:

| Direct | Blueprint |
|---|---|
| `findDataIn(id, slot)` | `bpFindDataIn(id, slot)` |
| `resolveInt(d)` | `bpResolveInt(d)` (returns std::string expression) |
| `resolveFloat(d)` | `bpResolveFloat(d)` (returns std::string expression) |

When adding a node, you write **both** cases. The body is functionally identical but the inputs come from different resolvers and outputs are either ints or stringified expressions.

NDS only has one path (no blueprint-instance distinction yet), so you write one case in `nds_package.cpp` and it covers both. Blueprints in NDS resolve params at export time.

---

## Mode 0 / Mode 4 / Mode 7 runtime targets

Affinity supports three rendering / movement modes. A node's runtime behavior may differ — or only apply to — a subset.

| Mode | Description | Player movement |
|---|---|---|
| **Mode 0** | Top-down tile RPG. Grid-locked. | `tm_move_frames`, `tm_move_timer`, tile lerp |
| **Mode 4** | First-person 3D ("FPS3D"). Free movement in world space. | `player_x/y/z`, `player_vy`, view-direction input |
| **Mode 7** | Pseudo-3D plane (Mario Kart style). | Currently editor-side preview only |

Nodes that touch movement need to handle both Mode 0 and Mode 4. Look at how `Walk` / `Sprint` do it:

- They set ONE variable (`afn_move_speed`).
- Mode 0 runtime: derives `tm_move_frames = 48 / afn_move_speed` (faster speed → fewer frames per tile).
- Mode 4 runtime: uses it as a direct multiplier on input.

If your node only makes sense in one mode (e.g. boost-pad velocity is meaningless on a grid), say so in the description AND in the `setActionFunc` body's Runtime comment. The user needs to know not to wire it on a tile scene.

---

## Runtime variable conventions (`afn_*`)

All script-driven globals share the `afn_` prefix. This identifies them as exporter-emitted, distinguishing from runtime-internal (`player_`, `s_`, `cam_`, etc.).

| Prefix | Meaning | Example |
|---|---|---|
| `afn_` | Editor / script global, emitted into mapdata.h or defined in `script_glue.c` (NDS) | `afn_move_speed`, `afn_gravity`, `afn_player_vx_world` |
| `player_` | Runtime player state, may or may not be exposed to scripts | `player_x`, `player_vy`, `player_on_ground` |
| `tm_` | Tilemap (Mode 0) state | `tm_move_timer`, `tm_player_facing` |
| `snd_`, `audio_` | Audio state | `snd_seq_active`, `audio.c` internals |
| `s_` | File-static, runtime-internal, never script-visible | `s_moveSpeed`, `s_playerY` |

When you add a global the script needs, prefix it `afn_`. When you add a global only the runtime uses for bookkeeping (cached values, smoothed state), prefix `s_` and keep it local to its TU.

---

## Where runtime globals are defined per platform

This trips up newcomers because the location is platform-specific.

### GBA

Script globals live in **`mapdata.h`** as `static` (because mapdata.h is included once, by main.c). The exporter (`gba_package.cpp`) emits them. Example:

```cpp
// In gba_package.cpp, around line ~1948:
f << "static FIXED player_vy;\n";
f << "static int   afn_player_vx_world;\n";
```

GBA runtime reads them directly because `main.c` includes `mapdata.h`.

### NDS

Script globals live in **`script_glue.c`** as plain `int` (no `static` — they need external linkage). They are declared `extern` in `mapdata.h` by the NDS exporter. Example:

```c
// nds_runtime/source/script_glue.c
int  player_vy;
int  afn_player_vx_world;
```

```cpp
// nds_package.cpp emits into mapdata.h:
f << "extern int player_vy;\n";
f << "extern int afn_player_vx_world;\n";
```

Why the difference: NDS splits code across multiple TUs (`fps3d.c`, `mode0.c`, `audio.c`, `script_glue.c`, `main.c`) and they each include `mapdata.h`. Having globals be `static` in mapdata.h would give each TU its own copy. So NDS uses `extern` + real definition in `script_glue.c`.

When you add a new global:
- GBA: emit `static T name;` in mapdata.h via `gba_package.cpp`. No further action.
- NDS: emit `extern T name;` in mapdata.h via `nds_package.cpp`, AND add `T name;` to `script_glue.c`.

---

## The sentinel macro pattern for backwards-compat

If you add a new global that runtime code references, projects exported with the **old** editor won't have that global declared in their `mapdata.h`. The runtime build will fail on the user's existing project until they re-export.

Avoid this with a sentinel define + fallback. The pattern:

### In the exporter (`gba_package.cpp` or `nds_package.cpp`)
```cpp
f << "#define AFN_HAS_VEL_XZ 1\n";
f << "static int afn_player_vx_world;\n";
// ... other related vars
```

### In the runtime (`gba_runtime/source/main.c`)
```c
#ifndef AFN_HAS_VEL_XZ
static int afn_player_vx_world;
static int afn_player_vz_world;
static int afn_velocity_falloff;
#endif
```

Now:
- Old projects exported before your change: `AFN_HAS_VEL_XZ` undefined, main.c provides fallback statics. Variables exist, value is 0, behavior is no-op. Build succeeds.
- New projects: `AFN_HAS_VEL_XZ` defined by mapdata.h, runtime skips fallback. Variables defined by the exporter.

Examples of this pattern already in use: `AFN_HAS_FINE_FACTOR`, `AFN_HAS_SCRIPT`, `AFN_MIDI_MASTER_VOL_FIX`, `AFN_HAS_SOUND`. Each one tracks a feature added later in the project's lifetime.

Use this every time you add globals that the runtime references. It saves you and the user from "I changed the editor and now I can't compile my older projects" surprises.

NDS doesn't strictly need this for the `extern` case if `script_glue.c` always defines the global (because the global always exists at link time). But you still need a forward declaration somewhere in the consuming TU when mapdata.h is stale. The pattern in `fps3d.c`:

```c
#ifdef AFN_HAS_SCRIPT
extern int afn_player_vx_world;  // forward decl in case mapdata.h is older
// ... use it
#endif
```

The inline `extern` inside the function block works even if `mapdata.h` doesn't have it.

---

## Enum ordering rules — NEVER insert in the middle

From CLAUDE.md, the rule is **absolute**:

> NEVER insert new entries in the middle of the `GBAScriptNodeType` enum in `gba_package.h`. Always append new node types immediately before `COUNT`. Inserting in the middle shifts all subsequent integer values, which breaks every saved project file that references those node types.

`VsNodeType` in `frame_loop.cpp` and `GBAScriptNodeType` in `gba_package.h` are **integer-serialized** into project save files. Their values are written to disk as the literal `(int)` cast. If you insert at position 36, every saved project's node typed 36 now means something different — and 37, 38, 39... all shift too.

Append. Always. Even if it makes the file look messy. Even if you "would just need to rearrange." Don't.

The two enums must stay in **identical order** because `VsNodeType` and `GBAScriptNodeType` are used interchangeably at codegen — `(int)VsNodeType::Foo == (int)GBAScriptNodeType::Foo` is assumed everywhere. If you add to one, add to the other in the same position.

---

## Version migration when an enum has already shifted

If somebody (or a past version of you) inserted an entry in the middle and shipped it — already merged, already in user projects — you cannot revert without breaking those projects. Instead, **bump the project save version** and migrate on load.

Recipe:

1. In `frame_loop.cpp`, find the save line and bump the version:
   ```cpp
   fprintf(f, "version=N\n");  // bump N
   ```
2. At the four load sites (search for `if (sscanf(line, "type=%d"` or similar; there are blueprint nodes, Mode 0 scene nodes, Mode 4 scene nodes, Mode 7 scene nodes), insert a fix-up:
   ```cpp
   if (projectVersion < N && typeInt >= POS) typeInt++;
   ```
   Where `POS` is the index at which the new entry was inserted. This shifts old-style ints up by one when loading older projects.
3. Verify by loading a saved project from a pre-N version of the editor — every node type should resolve to the right `VsNodeType`.

This is what `IsFalling` at position 36 required when it shipped mid-enum. It's a permanent stain on the codebase — every load now does this shift forever. The lesson: append. Don't insert.

---

## Worked example: `SetVelocityX` / `SetVelocityZ` / `VelocityFalloff`

These three nodes implement a "boost pad" — set a world-axis velocity that's applied each frame, then linearly decay it to zero over N frames. This walkthrough is the full set of edits performed to add them, as a checklist illustration.

### Concept

- `SetVelocityX(value)` writes `afn_player_vx_world = value * 256` (float scaled to fixed-point).
- `SetVelocityZ(value)` writes `afn_player_vz_world` similarly.
- `VelocityFalloff(frames)` writes `afn_velocity_falloff = frames`.
- Each frame, Mode 4 runtime:
  ```c
  player_x += afn_player_vx_world;
  player_z += afn_player_vz_world;
  if (afn_velocity_falloff > 0) {
      afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
      afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
      if (--afn_velocity_falloff == 0) {
          afn_player_vx_world = 0;
          afn_player_vz_world = 0;
      }
  }
  ```
- The `vx -= vx/N` with N decrementing is **mathematically a true linear lerp** to zero. At frame k of N, `vx = V * (N-k) / N`. Proof: each step multiplies vx by `(N-k-1)/(N-k)`, which telescopes.

### The 16 edits

1. **VsNodeType enum** (`frame_loop.cpp`): appended `SetVelocityX`, `SetVelocityZ`, `VelocityFalloff` before `COUNT`.
2. **sVsNodeDefs**: added three rows — blue color, 1/1 exec, 1 data input each.
3. **Description switch**: added three `desc =` lines explaining each node.
4. **Display suffix returner**: added `return "_set_vel_x";` etc.
5. **setActionFunc body**: added three `case` blocks with full Runtime chain comments.
6. **Context menu**: added three `MenuItem` entries next to `SetVelocityY`.
7. **Export suffix table**: added three `suffix =` lines.
8. **GBAScriptNodeType enum** (`gba_package.h`): same three appended.
9. **GBA exporter globals** (`gba_package.cpp`): emitted `static int afn_player_vx_world` etc. with `#define AFN_HAS_VEL_XZ 1`.
10. **GBA direct codegen**: `case GBAScriptNodeType::SetVelocityX: { ... }` emitting `afn_player_vx_world = ...`.
11. **GBA blueprint codegen**: same but using `bpResolveFloat`.
12. **NDS exporter codegen**: same as GBA direct, using `(int)(v * 256.0f)`.
13. **NDS extern declarations**: emitted `extern int afn_player_vx_world;` into mapdata.h.
14. **GBA runtime per-frame** (`gba_runtime/source/main.c`): added velocity application + decay block in Mode 4 player update, gated `#ifdef AFN_HAS_SCRIPT`.
15. **NDS runtime per-frame** (`nds_runtime/source/fps3d.c`): same logic, with inline `extern` decls for stale-mapdata robustness.
16. **NDS global definitions** (`nds_runtime/source/script_glue.c`): added `int afn_player_vx_world;` etc.

Plus a backwards-compat fallback in `gba_runtime/source/main.c`:
```c
#ifndef AFN_HAS_VEL_XZ
static int afn_player_vx_world;
static int afn_player_vz_world;
static int afn_velocity_falloff;
#endif
```

This lets older projects (without `AFN_HAS_VEL_XZ`) build successfully.

### Verification

After all edits:
1. Build editor — confirm no compile errors.
2. Re-launch editor, place a `SetVelocityX` node. Verify it appears with the right name, color, pins, tooltip.
3. Wire `OnStart → SetVelocityX(120) → SetVelocityZ(0) → VelocityFalloff(60)`. Export.
4. Inspect `mapdata.h` — confirm `afn_player_vx_world = 30720;` (120 * 256) is emitted, plus `#define AFN_HAS_VEL_XZ 1`.
5. Build runtime, run rom. Player should slide forward on scene start, decelerate over 1 second, stop.

If step 5 fails, the runtime per-frame logic is wrong. If step 4 fails, the exporter codegen is wrong. If step 2 fails, the editor-side wiring is wrong.

---

## Common pitfalls

### "Node appears in editor but does nothing"
Symptoms: node visible on the canvas, can wire to it, export succeeds, runtime ignores it.
Causes:
- Forgot the exporter codegen case (step 10 or 12). Exporter silently skipped your node type.
- Forgot the runtime per-frame application (step 14 or 15). Variable is set but never read.
- Runtime reads it but is gated on something else (e.g. wrapped in `if (afn_some_other_flag)` that's never true).

Debugging path: open `mapdata.h` and grep for your variable name. If it's not written, exporter is broken. If it's written but you don't see the behavior, runtime is broken.

### "Build fails after my edit, but only on user's project"
Symptoms: my hello-world test project builds fine, user's older project fails to compile in the runtime build.
Cause: missing sentinel macro. You added a global, runtime references it, but the user's old `mapdata.h` doesn't have it.
Fix: add `#define AFN_HAS_FEATURE 1` in exporter, `#ifndef AFN_HAS_FEATURE { fallback }` in runtime.

### "Saved projects from yesterday won't load"
Cause: inserted into the middle of `VsNodeType` or `GBAScriptNodeType`, shifting integer values.
Fix: see [version migration](#version-migration-when-an-enum-has-already-shifted). The right answer is to append-only and not get into this mess.

### "Editor shows wrong code for my node"
Cause: `setActionFunc` body is stale — you updated the exporter but forgot the preview.
Fix: rule 3. Update `setActionFunc` in the same edit pass. Always.

### "Works on GBA but not NDS (or vice versa)"
Cause: you only added the codegen case to one platform's exporter.
Fix: every node needs codegen in BOTH `gba_package.cpp` (direct + blueprint paths) AND `nds_package.cpp`. Verify both before reporting done.

### "Float param shows up as 0 even though I set 2.5"
Cause: using `paramInt[i]` instead of the float resolver.
Fix: in the exporter, `auto* d = findDataIn(id, 0); float v = resolveFloat(d);` (NOT `(float)resolveInt(d)`). The resolver knows about the memcpy bit-cast.

### "Two nodes execute in wrong order in same frame"
Cause: the chain walker walks exec links in declaration order. If a gate reads state that an action writes, the action must walk first.
Fix: search for "Walk action exec siblings before gates" in `frame_loop.cpp`. The walker already sorts action-before-gate. If your case isn't covered, extend the sort key.

### "Runtime variable has wrong initial value"
Cause: GBA `static` globals default to 0. NDS globals in `script_glue.c` default to 0. If you need a different default, initialize it:
- GBA: `f << "static int afn_var = 1;\n";` in the emit.
- NDS: `int afn_var = 1;` in script_glue.c. Note: when the NDS exporter emits the `extern`, the default is still controlled at the script_glue definition.

---

## Glossary of identifiers

| Identifier | Defined in | Meaning |
|---|---|---|
| `VsNodeType` | `frame_loop.cpp` | Editor enum of node types |
| `GBAScriptNodeType` | `gba_package.h` | Mirror enum for exporter; must match VsNodeType ordering |
| `sVsNodeDefs[]` | `frame_loop.cpp` | Table: name, color, pin counts, pin labels |
| `setActionFunc` | `frame_loop.cpp` | Sets the runtime-code preview shown in the editor |
| `findDataIn(id, slot)` | exporter | Finds the upstream data node feeding a pin |
| `resolveInt`, `resolveFloat` | exporter | Evaluates a data node's value at export time |
| `bpFindDataIn`, `bpResolveInt`, `bpResolveFloat` | exporter | Blueprint-mode equivalents (returns expression strings) |
| `emitChain` | exporter | Recursively walks an exec chain, emitting C code |
| `mapdata.h` | generated | Output of the exporter; included by the runtime |
| `afn_*` | mapdata.h / script_glue.c | Script-visible globals |
| `player_*` | runtime | Runtime player state |
| `tm_*` | runtime | Tilemap (Mode 0) state |
| `AFN_HAS_*` | mapdata.h | Sentinel macros for feature presence (gates runtime fallbacks) |
| Mode 0 | runtime | Top-down tile RPG mode |
| Mode 4 | runtime | First-person 3D mode |
| Mode 7 | runtime | Pseudo-3D plane mode |
| FIXED | gba_runtime | 16.16 fixed-point integer typedef |

---

## Quick reference card

For an action node with one float param, here's the minimum copy-paste skeleton. Replace `YourNode` and `_your_node` and `afn_your_var` everywhere.

### `frame_loop.cpp`
```cpp
// 1. Enum
YourNode,                                  // before COUNT in VsNodeType

// 2. sVsNodeDefs row (mirror position)
{ "Your Node", 0xFF3355AA, 1, 1, 1, 0, {"Param (float)"}, {}, {} },

// 3. Description
case VsNodeType::YourNode: desc = "What it does."; break;

// 4. Display suffix
case VsNodeType::YourNode: return "_your_node";

// 5. setActionFunc body
case VsNodeType::YourNode: {
    editorCode = "// Short note";
    char bodyBuf[512];
    snprintf(bodyBuf, sizeof(bodyBuf),
        "    afn_your_var = %s;\n"
        "    // --- Runtime (main.c) ---\n"
        "    // Mode 4: <what main.c does>",
        fmtFloat(infoNode.id, 0, "<value>"));
    setActionFunc(infoNode, "_your_node", bodyBuf);
    break;
}

// 6. Context menu
if (ImGui::MenuItem(sVsNodeDefs[(int)VsNodeType::YourNode].name)) addNodeAt(VsNodeType::YourNode);

// 7. Export suffix
case VsNodeType::YourNode: suffix = "_your_node"; break;
```

### `gba_package.h`
```cpp
YourNode,    // before COUNT in GBAScriptNodeType (mirror position)
```

### `gba_package.cpp`
```cpp
// 9. Global emit (in mapdata.h emit block)
f << "#define AFN_HAS_YOUR_FEATURE 1\n";
f << "static int afn_your_var;\n";

// 10. Direct codegen
case GBAScriptNodeType::YourNode: {
    auto* d = findDataIn(action->id, 0);
    float v = d ? resolveFloat(d) : 0.0f;
    f << "    afn_your_var = " << (int)(v * 256.0f) << ";\n";
    break;
}

// 11. Blueprint codegen
case GBAScriptNodeType::YourNode: {
    auto* d = bpFindDataIn(action->id, 0);
    std::string v = d ? bpResolveFloat(d) : "0";
    f << "    afn_your_var = " << v << ";\n";
    break;
}
```

### `nds_package.cpp`
```cpp
// 12. NDS codegen
case GBAScriptNodeType::YourNode: {
    auto* d = findDataIn(a->id, 0);
    float v = d ? resolveFloat(d) : 0.0f;
    f << "    afn_your_var = " << (int)(v * 256.0f) << ";\n";
    break;
}

// 13. NDS extern
f << "extern int afn_your_var;\n";
```

### `gba_runtime/source/main.c`
```c
// Fallback for stale mapdata.h
#ifndef AFN_HAS_YOUR_FEATURE
static int afn_your_var;
#endif

// 14. Per-frame logic
#ifdef AFN_HAS_SCRIPT
// use afn_your_var
#endif
```

### `nds_runtime/source/fps3d.c`
```c
// 15. Per-frame logic
#ifdef AFN_HAS_SCRIPT
extern int afn_your_var;
// use it
#endif
```

### `nds_runtime/source/script_glue.c`
```c
// 16. Real definition
int afn_your_var;
```

---

That's the system. If you read all of this, you can add a node correctly on the first try. If you skim and miss a section, you'll discover which one when the build fails — refer back to [common pitfalls](#common-pitfalls) for the symptom-to-cause map.

The author's recommendation: keep this doc open in another tab while you make edits. Tick off each of the 16 checklist items as you go. The cost of being thorough is a few minutes; the cost of being sloppy is hours of "why does this node compile but not work."

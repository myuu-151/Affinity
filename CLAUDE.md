# Affinity Editor — Development Conventions

## Visual Script Node Code Previews

Every action node in `src/editor/frame_loop.cpp` has a `setActionFunc()` call that defines the **actual C implementation** shown in the editor's "Mode 0 Runtime" code window. This is NOT a preview — it is the real generated code.

### Runtime Chain Convention

Every node's `setActionFunc` body MUST include:

1. **The generated C code** — the actual lines emitted into `mapdata.h` (e.g. `afn_player_frozen = 1;`)
2. **`// --- Runtime (main.c) ---`** — a separator comment
3. **How main.c consumes those variables** — showing the full chain of what happens at runtime

Example (Sprint node):
```c
setActionFunc(infoNode, "_sprint",
    "    afn_move_speed = 37;\n"
    "    // --- Runtime (main.c) ---\n"
    "    // Mode 0: tm_move_frames = 48 / afn_move_speed; // = 1 frames/tile\n"
    "    //         tm_move_timer = tm_move_frames;\n"
    "    //         px = lerp(fromX, toX, t / tm_move_frames);\n"
    "    // Mode 4: moveSpeed = afn_move_speed; // = 37\n"
    "    //         player_x += (viewSin * inputFwd * moveSpeed) >> 16;");
```

### Rules

- **Always show both Mode 0 and Mode 4 runtime paths** when the variable is used differently in each mode
- **Keep runtime comments to 2-4 lines** — concise, showing the key consumption points
- **Pure math/data nodes** (Add, Sub, Mul, etc.) that just `return` a value: add `// --- Runtime --- inline data node, evaluated at call site`
- **Gate/flow nodes** (IsMoving, DoOnce, etc.): add a brief comment about how the condition is evaluated
- **When adding a new node**, always include the runtime chain — search `main.c` for how the variable is consumed
- **When modifying node behavior** (e.g. FreezePlayer now also sets `afn_play_anim = -1`), update BOTH the exporter (`gba_package.cpp`) AND the `setActionFunc` body in `frame_loop.cpp`

### Key Files

- `src/editor/frame_loop.cpp` — Node code previews (`setActionFunc` calls, ~line 11500+)
- `src/platform/gba/gba_package.cpp` — GBA exporter (generates `mapdata.h` from nodes)
- `gba_runtime/source/main.c` — GBA runtime (consumes the generated variables)
- `gba_runtime/include/mapdata.h` — Generated header (output of exporter, read-only)

### Export Ordering in mapdata.h

HUD data arrays (`afn_hud_elems`, `afn_hud_stops`, etc.) are emitted BEFORE script and blueprint functions so that ShowHUD/CursorUp/CursorDown can reference them. Do not move the HUD section after the script/blueprint codegen sections.

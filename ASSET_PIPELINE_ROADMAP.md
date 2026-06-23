# Affinity — Asset Pipeline Roadmap

A focused plan for refining how project data gets from the editor onto the
target hardware. Scoped **only** to the asset/data pipeline — not gameplay,
nodes, or rendering features.

---

## Where it is today

- The editor exports a project as **C source headers, per platform**:
  `nds_runtime/include/mapdata.h`, `psp_runtime/include/psp_*.h`,
  `psv_runtime/include/psv_*.h`.
- Bulk data — HUD textures, PCM audio, mesh/rig geometry — is emitted as
  **string literals** (see `emitFrame` in `src/platform/psv/psv_package.cpp`
  and `GeneratePSPMapData` in `src/platform/psp/psp_package.cpp`). String
  literals were a deliberate win (commit `cea0866`): GCC parses one multi-MB
  string literal orders of magnitude faster than the equivalent
  `{0x..,0x..,...}` initializer list.
- Those headers are `#include`d and **compiled into the runtime binary**, then
  **committed to the repo** (e.g. `psv_hud.h` ~44 MB, `psv_sound.h` ~78 MB).

This works — every release ships from it. It's also the engine's weakest link.

## Why it's the weakest link

1. **Data changes force a full runtime recompile.** Tweak one sprite → re-export
   → recompile tens of MB of literals → relink. No hot-reload.
2. **Git bloat.** 44–78 MB generated files are committed per export; GitHub's
   large-file / LFS warnings already fire on every push, and history balloons.
3. **No incremental export.** One changed asset regenerates (and recompiles) the
   entire header.
4. **Data/code coupling.** Data lives inside the binary, so there's no patching,
   no loose-file dev iteration, and no streaming.
5. **Cross-platform duplication.** The same logical asset is re-encoded into each
   platform's headers from scratch.

## Guiding principles

- **Incremental, each phase shippable.** This is a working engine — no big-bang
  rewrite.
- **Separate DATA from CODE first.** Everything else (streaming, caching,
  patching, fast iteration) only becomes possible once that line exists.
- **Respect each target's reality.** GBA has no real filesystem (data must live
  in ROM via the linker); NDS has nitroFS; PSP/PSV have real filesystems.
- **The editor's struct layout stays the single source of truth.** The pack is
  just a serialization of it.

---

## Phase 1 — Kill the string-literal compile + git bloat *(highest leverage, lowest risk)*

Replace "emit data as C arrays in a header" with "emit a binary blob + reference
it." This solves the *same* problem string literals solved (slow compiles) but
goes the rest of the way, and it gets the data out of git.

- **GBA / NDS (ROM / nitroFS):** emit a `.bin` and pull it in with `.incbin`
  (or `objcopy` → `.o`). The compiler stops parsing megabytes of escapes; the
  linker just stitches the blob in. Identical memory layout, dramatically faster
  builds.
- **PSP / PSV (filesystem):** write the blob beside the app and `fread` /
  `sceIo*` it into a buffer at boot; structs index into the buffer.
- **Keep the small table headers** (`AfnHudPiece[]`, `AfnMesh[]`, the per-slot
  tables, …) but point their data pointers at the loaded/incbin'd blob instead
  of inline arrays.
- **Stop committing the blobs:** gitignore generated data (or move to LFS);
  commit a tiny sample export only if needed for CI/build sanity.

**Touchpoints:** `emitFrame` + the big-array emitters in `psv_package.cpp`;
`GeneratePSPMapData` in `psp_package.cpp`; split `include/*.h` into
"tables (small, committed)" vs "blob (large, ignored)".

**Done when:** a one-sprite change re-exports + builds in seconds, and the repo
stops growing on every export.

## Phase 2 — A real pack format (`.afnpak`)

- One versioned container: `magic + version + TOC + sections` (sprites,
  textures, meshes, rigs, audio, hud, scripts).
- Each record: `{ type, id, offset, size, encoding }`. The runtime loads the
  TOC, maps the sections, and resolves pointers.
- One format for all platforms (GBA links it; PSP/PSV load it; NDS via nitroFS).

**Done when:** there's one serializer instead of N per-platform header emitters,
and the runtime has one loader.

## Phase 3 — Incremental / content-addressed export

- Hash each source asset (sprite PNG, OBJ, glTF, audio sample). Cache the encoded
  blob keyed by `hash + encode-params`.
- Export only re-encodes changed assets and rewrites the TOC.

**Done when:** large-project exports go from seconds-to-minutes down to "only
what actually changed."

## Phase 4 — Scene streaming *(optional, unlocked by Phase 2)*

- With data out of the binary, load a scene's section on enter and free it on
  exit. Matters for multi-arena projects on 4–32 MB consoles.

## Phase 5 — Platform-shared intermediate

- Quantize / encode once into a platform-agnostic intermediate, with thin
  per-platform packers handling the format deltas (RGB15 vs RGBA8888, texture
  swizzle, alignment). Removes re-encoding the same asset per platform.

## Phase 6 — Loose-file dev mode

- Dev builds read loose encoded files for fast iteration; release builds bake the
  `.afnpak`. Easiest to land on PSP/PSV first (real filesystems).

---

## Suggested first PR

Do **Phase 1 on PSV only** (the current frontier): swap the `psv_hud.h` /
`psv_sound.h` string-literal arrays for a single `psv_data.bin` loaded at boot,
and gitignore the blob. That removes the worst of the compile + git pain with
no format design required. Generalize into the `.afnpak` container in Phase 2.

## Risks / watch-items

- **GBA has no heap or filesystem** to speak of — its "load" *is* the linker.
  Keep `.incbin` there; don't assume runtime file IO on that target.
- **Texture alignment:** the DS GE and Vita GU want 16-byte-aligned texture data
  (`__attribute__((aligned(16)))` today). Preserve alignment when moving to a
  blob — an unaligned texture base samples garbage on real hardware.
- **Endianness** is uniform (all targets little-endian), but record the
  assumption in the format header so it isn't a silent landmine later.
- **Don't drop the string-literal compile-speed win until `.incbin` / file-load
  replaces it** — they solve the same problem; run one or the other, never
  neither.

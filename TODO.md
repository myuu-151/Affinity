# Affinity NDS Port — Remaining Work

The NDS runtime now covers the bulk of what the GBA runtime does. What's
listed here are either rare BP nodes that haven't shown up in a test
project yet, or features that work but with caveats worth tracking.

## Script Nodes Not Yet Ported to NDS

Surface as `/* TODO: emit node type N */` in the generated `mapdata.h`
when a project uses them. Add a case in
`src/platform/nds/nds_package.cpp`'s BP-action / scene-action switch.

- Patrol / SetAI / SetTint / SetDrawDist
- EmitParticle / Shake
- SwapSprite / SetSpriteY / SetColor
- Print / SetTextColor (HUD text already works; this is the dynamic-text
  flavour)
- WaitUntil / RepeatWhile
- HasItem / AddItem / RemoveItem / SetItemCount / UseItem
- ShowDialogue / HideDialogue (rendering side too)
- SaveData / LoadData (no SRAM backend on NDS yet)
- DamageHP / SetHP / IsHPZero — half-wired (DamageHP/SetHP emit, gates may
  need work)

## Known Limitations

- **Audio:** GM instrument envelope is software-driven over libnds
  channels; no per-note vibrato yet. Pitch bend works but uses linear
  approximation (off by ~18% at full ±2-semitone bend).
- **Mode 4 → Mode 0 → Mode 4 swap:** needs `glResetTextures` +
  `glResetMatrixStack` on re-entry. Works but the GE state replay is
  brittle — adding new 3D state means adding it to both `init_video` and
  the swap-handler in `fps3d.c::afn_scene_tick`.
- **Polyphony:** capped at 16 voices (NDS hardware max). Editor's
  voiceCount setting is overridden unless it's < 8.
- **Sample data > 65535 Hz playback** clamps via halving (one octave
  drop) instead of skipping. Affects samples with extreme pitch shifts
  per multi-sample region.

## Editor / Project Concerns

- `paletteSrc` is honoured for explicit palette sharing. Content dedup
  also collapses assets with identical 16-color palettes. With > 13
  truly-distinct palettes, the wrap target picks the least-used asset
  bank (low-index slots, typically Pikachu / pikanpc) — usually fine but
  document the limit per project.
- 16x16 HUD pieces pixel-double from an 8x8 source asset via nearest-
  neighbour. Matches GBA but isn't an editor-side filter — re-export
  needed if source pixels change.

## Project File Compatibility

- Save version bumps on the editor side need matching migration in all 4
  node-load sites in `frame_loop.cpp` (blueprint, Mode 0, Mode 4, Mode 7
  scene nodes).

## Mode 7 (legacy Mode 1)

- Floor + rotation BG render path exists from the earlier port but
  hasn't been re-validated since the multi-mode swap landed. Likely
  needs the same VRAM-bank dance treatment when entered from a
  different mode.

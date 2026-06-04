# Affinity PSP Port — Status

NDS → PSP runtime port. Branch: `psp-port`. The PSP renders the Mode 4 (3D)
scene with the sceGu/sceGum hardware T&L pipeline (float geometry + RGBA8888
textures — no fixed-point/palette packing like the GBA/NDS).

## Done & verified in PPSSPP

| Feature | Notes |
|---|---|
| Textured 3D world | meshes via `sceGumDrawArray`, per-bucket frustum cull (4×4×4 grid, float port of the NDS bucket cull) |
| Atlas seams fixed | point sampling (`GU_NEAREST`) — DS has no bilinear |
| Player rig (Spyro) | CPU skinning: `skinned = animPose[bone]·baseVert` (DSMA rigid, 1 bone/vert), keyframe lerp/nlerp |
| Rig textures | rig UVs are **not** V-flipped (unlike OBJ meshes) |
| Collision | floor follow + walls + gravity + jump (Cross), built at load from mesh tris (`collision.c`) — no new export data |
| Idle/walk clips | switches on movement |
| Sky panorama | far quad in view space, U scrolled by yaw |
| Slope alignment | rig tilts to floor normal (basis matrix: up=normal, fwd=yaw on slope plane) |

## Critical PSP gotchas already solved (don't rediscover)
- **Black textures** → `sceKernelDcacheWritebackAll()` after init (GE reads physical
  RAM; const ELF data sits in CPU dcache). Dynamic verts: `DcacheWritebackRange`.
- **Float literals**: exporter `Flt()` forces `128.0f` not `128f` (invalid C).
- **Inverted mesh faces**: PSP front-face winding is opposite the data → back-cull
  uses `GU_CCW`.
- **Distant tris black**: only level-0 tex uploaded → `sceGuTexLevelMode(GU_TEXTURE_CONST,0)`.
- **Mesh V upside-down**: flip `1-v` (matches NDS). **Rig V: no flip.**
- **Textures 16-byte aligned** (real HW masks low addr bits).
- **2D `GU_TRANSFORM_2D` sprites rendered nothing** — use a 3D quad instead (sky).

## Build / test loop (autonomous-capable)
- Build:  `wsl -d Ubuntu -- bash /mnt/c/.../psp_runtime/build_psp.sh`
- Run:    `"C:\Program Files\PPSSPP\PPSSPPWindows64.exe" <EBOOT.PBP>`
- Screenshot (self-verify): `C:\Users\NoSig\AppData\Local\Temp\psp_shot.ps1`
  (PrintWindow capture → `psp_shot.png`)
- pspdev is in WSL `/opt/pspdev`; `~/.profile` patched so the editor's
  Export-PSP build finds `psp-config`. Editor reconfigure needs the VS-bundled cmake.
- **Generated headers** (`psp_mapdata.h`, `psp_rig.h`, `psp_sky.h`) are gitignored
  as placeholders; the editor's **PSP → build** regenerates them. **Close PPSSPP
  before exporting** (it locks EBOOT.PBP). Exporter does `make clean; make`.

## Nodes / visual scripts — runtime DONE, exporter codegen TODO
- **Input**: ABXY → Cross/Circle/Square/Triangle, L/R triggers, d-pad +
  Start/Select, analog → d-pad keys (`input.c`). `key_is_down/hit/released`.
- **Runtime** (`script_glue.c` + `script.h`): defines the node variables the
  emitted C reads/writes (core ones consumed, rest inert), runs OnStart /
  OnUpdate / OnKey* + blueprint dispatch each frame. Controller (`scene.c`) is
  now **node-driven**: movement = `afn_input_fwd/right`×`afn_move_speed`, camera
  = `orbit_angle`, clip = `afn_rig_clip`. No-script scenes fall back to analog.
- **`psp_script.h`**: currently **hand-ported** from the NDS codegen output for
  SpyroDemo3a (same C the NDS runs). Works, but is NOT regenerated on export.
- **TODO — exporter codegen**: port `nds_package.cpp`'s script section
  (`emitAction` switch ~100 node types + `walkExec`/`emitDispatcher`/
  `buildChains`, ~lines 2688–3530) so `PackagePSP` writes `psp_script.h`.
  Cleanest: extract it to a shared `EmitScriptCode(ostream&, script, blueprints,
  bpInstances, startMode)` called by both NDS and PSP (verify NDS still builds —
  a pure cut keeps output identical). Then add every referenced symbol to
  `script_glue.c` (most as inert ints; compiler tells you what's missing) and
  port the consumers (sound, HUD, sprite manipulation, grind, scene change).

## Implemented, awaiting visual verification (re-export a scene that uses them)
- **Sprites / billboards** (`billboard.c`, exporter `GeneratePSPSprites`).
  Camera-facing animated quads, alpha-blended. Frame pixels confirmed
  `pixels[py*64+px]` → `palette[idx]` (idx 0 transparent). Pending tuning:
  billboard **size** (`basesize*scale*0.25`, a guess), and **directional**
  (8-facing) sprites currently use the default anim only — facing-pick TODO.

## TODO — needs export data + re-export to iterate (do WITH live screenshots)
1. **HUD** — 2D overlay. The `GU_TRANSFORM_2D`/`GU_SPRITES` path rendered nothing
   for the sky; use the **3D-quad approach** (worked) or debug 2D. Needs HUD export.
2. **Audio** — `sceAudio` + software mixing of exported samples; port `audio.c`.
3. **Scripts** — `script_glue.c` visual-script runtime + node variables.
4. **Mode 0** — the separate 2D top-down adventure mode (`mode0.c`, big).
5. **Mode 7 floor** — affine floor (if used by any scene).

## Tuning knobs (in code, may need adjusting once seen)
- Rig: `AFN_RIG_YAW_OFFSET` (facing), `AFN_PLAYER_RIG_SCALE` (= playerScale×0.25).
- Collision: `PLAYER_RADIUS`, `PLAYER_HEIGHT`, `GRAVITY`, `JUMP_VEL` in scene.c/collision.c.
- Camera: orbit = `afn_orbit_dist`; framing in `scene_render`.
- Clip switching assumes default clip = idle, the other = walk.

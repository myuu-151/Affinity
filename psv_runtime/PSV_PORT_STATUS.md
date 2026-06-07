# Affinity PS Vita port — status

Native PS Vita (Mode 4 3D) runtime, rendered with **vitaGL** (fixed-function GL,
which maps cleanly from the PSP's sceGu). Work happens on the **`vita-port`**
branch. Everything below **compiles** but is **NOT yet verified on-device** —
the next step is your first run.

## Toolchain (done, one-time)
VitaSDK is installed at `C:\vitasdk` (via the existing devkitPro MSYS2's pacman +
`vdpm`), env vars persisted in `~/.bashrc`. Compiler: `arm-vita-eabi-gcc 15.2.0`.
Libs incl. **vitaGL**, vita2d, GXM, SDL3.

### Build
```bash
/c/devkitPro/msys2/usr/bin/bash.exe -lc '
  export VITASDK=/c/vitasdk; export PATH=$VITASDK/bin:$PATH
  cd /c/Users/NoSig/Documents/gbadev/Affinity/psv_runtime
  rm -rf build && mkdir build && cd build && cmake .. && make'
```
Output: `psv_runtime/build/affinity_psv.vpk` (also copied to
`C:/Users/NoSig/Documents/gbadev/affinity_psv.vpk` for FTP).

### Test loop (yours)
1. Run HENkaku (browser) so homebrew is active.
2. FTP `affinity_psv.vpk` to `ux0:/` (FileZilla, 1 connection).
3. VitaShell -> install -> launch the **Affinity** bubble.
4. START exits. Sticks/dpad orbit, triggers zoom.

## Done (compiles)
- **Build pipeline**: CMake -> velf -> self -> vpk. Exact vitaGL link set captured
  in `CMakeLists.txt` (vitaGL/vitashark/SceShaccCg(+Ext)/stdc++/SceKernelDmacMgr
  + Sce stubs).
- **Scene meshes**: level geometry rendered. Reuses the exported scene data
  (`psv_mapdata.h` = copy of `psp_mapdata.h`, with a Vita `affinity_psv.h` that
  drops the pspgu dependency). GL textures (RGBA8888 maps directly from the
  0xAABBGGRR data) + `glDrawElements`. **No PSP culling/bucketing or texture
  swizzling** — the Vita has the headroom.
- **Player rig**: CPU rigid skinning (copied verbatim from `psp_runtime/rig.c`) +
  vitaGL draw (matrix, per-material textures, eye-space directional headlamp via
  GL_LIGHT0). Reuses `psv_rig.h` (= `psp_rig.h`). Drawn at spawn, animating.

## Next (not started) — roughly in priority order
1. **Movement/physics + follow camera**: port `scene.c`'s update (input ->
   camera-relative move, gravity, floor snap) + `collision.c` (walls/floor, pure
   logic, ports directly) + `input.c`. Then the rig follows the player.
2. **Sprite billboards**: `billboard.c` -> vitaGL quads (camera-facing, animated).
3. **Sky panorama**: `sky.c` -> vitaGL.
4. **Audio**: `audio.c` is a software mixer on `sceAudio`; the Vita has `sceAudio`,
   so it ports with thread/API tweaks. Reuse `psp_sound.h` data.
5. **Scripts**: `script_glue.c` + the node-emitted code (reuse `psp_script.h`).
6. **Exporter**: add a `psv_package` target in the editor (mirrors `psp_package`)
   so `psv_mapdata.h`/`psv_rig.h`/`psv_sound.h` regenerate per project instead of
   being hand-copied from the PSP export. Until then, re-copy from `psp_runtime`
   after a PSP export:
   `sed 's/affinity_psp.h/affinity_psv.h/' psp_runtime/include/psp_mapdata.h > psv_runtime/include/psv_mapdata.h`

## Known caveats to check on first run
- vitaGL fixed-function **lighting** (rig headlamp) may need tuning vs the PSP look.
- Winding/cull: PSP used GU_CW front-face in places; GL default is CCW. The mesh
  `cullMode` mapping (`GL_CW` for front=1) may need a flip if faces look inverted.
- Depth: using `GL_LEQUAL` + glFrustum(near 1, far 5000); adjust if z-fighting.

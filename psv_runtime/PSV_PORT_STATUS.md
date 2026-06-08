# Affinity PS Vita port — status

Native PS Vita (Mode 4 3D) runtime, rendered with **vitaGL** (fixed-function GL,
which maps cleanly from the PSP's sceGu). Work happens on the **`vita-port`**
branch.

**VERIFIED ON-DEVICE (2026-06-08):** `affinity_psv.vpk` boots and renders on real
PS Vita hardware (mesh scene + player rig). One-time on-device requirement:
`libshacccg.suprx` (vitaGL's runtime shader compiler) must be present at
`ur0:/data/` — installed once via the "libshacccg.suprx Extractor" homebrew.

**Vita3K (PC emulator) ALSO works now** — needs a custom vitaGL build. The stock
VitaSDK `libvitaGL.a` crashes in Vita3K because:
  1. it inits GXM via `sceGxmVshInitialize` (Vita3K mishandles it), and
  2. vitaGL's **splash screen** creates a SECOND GXM context
     (`splashscreen.c` -> `init_gxm_context(VGL_CONTEXT_SPLASHSCREEN)`), but
     Vita3K only supports ONE immediate context, so the 2nd `sceGxmCreateContext`
     returns `SCE_GXM_ERROR_ALREADY_INITIALIZED` -> null deref at 0x78.

Fix = rebuild vitaGL with BOTH flags and relink:
```bash
git clone --depth 1 https://github.com/Rinnegatamante/vitaGL  # at C:\vitasdk-build\vitaGL
sed -i 's/shark_init_simple/shark_init/g' source/gxm.c   # master needs newer vitashark; revert to shark_init
make HAVE_VITA3K_SUPPORT=1 NO_SPLASHSCREEN=1 -j8 && make install
```
`HAVE_VITA3K_SUPPORT` -> `sceGxmInitialize` (the standard hw init; safe on device).
`NO_SPLASHSCREEN` -> no 2nd context. Stock lib backed up at
`$VITASDK/arm-vita-eabi/lib/libvitaGL.a.stock`. This custom lib works on BOTH
real hardware and Vita3K, so it's the one to keep installed. Vita3K still needs
`libshacccg.suprx` in `ur0/data/` (extracted from a real Vita).

Vita3K run: `Vita3K.exe -r AFNT00002` boots straight into the eboot (the vpk-path
form parks at the home GUI). Build 4047 is the current continuous release.

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
- **Editor exporter (`psv_package`)**: the **PSV** build target in the editor now
  regenerates the `psv_*.h` headers per project and builds the vpk — no more
  hand-copying from the PSP export. Implemented by sharing the PSP generator:
  `GenerateAffinityHeaders(runtimeDir, "psv_", "affinity_psv.h", ...)` in
  `psp_package.cpp` writes the headers; `psv_package.cpp` then builds via the
  VitaSDK CMake toolchain through the devkitPro MSYS2 bash (not WSL). Output:
  `psv_runtime/build/affinity_psv.vpk`. "Open in Vita3K" launches `Vita3K-new`.
- **Clocks**: `main()` maxes the Vita via `scePowerSetArmClockFrequency(444)` +
  bus/GPU maxima. 444 MHz is the API ceiling (default app clock is 333); a true
  500 MHz needs a kernel overclock plugin and is intentionally NOT done.

## Next (not started) — roughly in priority order
1. **Movement/physics + follow camera**: port `scene.c`'s update (input ->
   camera-relative move, gravity, floor snap) + `collision.c` (walls/floor, pure
   logic, ports directly) + `input.c`. Then the rig follows the player.
2. **Sprite billboards**: `billboard.c` -> vitaGL quads (camera-facing, animated).
3. **Sky panorama**: `sky.c` -> vitaGL.
4. **Audio**: `audio.c` is a software mixer on `sceAudio`; the Vita has `sceAudio`,
   so it ports with thread/API tweaks. Reuse `psp_sound.h` data.
5. **Scripts**: `script_glue.c` + the node-emitted code (reuse `psp_script.h`).
6. ~~**Exporter**: add a `psv_package` target in the editor~~ — **DONE** (see above).
   Note: the generator also emits `psv_sky.h`/`psv_sprites.h`/`psv_sound.h`/
   `psv_player.h`, but `main.c` doesn't `#include` them yet — they sit unused
   until those features (1-5 above) are ported. They're self-contained data
   headers, so they don't affect the build.

## Known caveats to check on first run
- vitaGL fixed-function **lighting** (rig headlamp) may need tuning vs the PSP look.
- Winding/cull: PSP used GU_CW front-face in places; GL default is CCW. The mesh
  `cullMode` mapping (`GL_CW` for front=1) may need a flip if faces look inverted.
- Depth: using `GL_LEQUAL` + glFrustum(near 1, far 5000); adjust if z-fighting.

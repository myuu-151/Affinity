<p align="center">
  <img src="assets/affinity_logo.png" alt="Affinity Logo" width="300">
</p>

<h1 align="center">Affinity Engine</h1>

<p align="center">
   NDS/PSP/PSV 3D engine with a Windows desktop editor.
</p>

<p align="center">
  <b>This project is in active development.</b> Features and APIs may change.
</p>

---

## Features

| | |
|---|---|
| **Software 3D Renderer** | Flat-shaded and textured polygon rasterizer with ARM ASM inner loops |
| **Multi-Target** | Build for GBA, NDS, PSP, or PS Vita from the same project |
| **OBJ Mesh Import** | Load .obj files with per-mesh culling, draw distance, LOD, and texture mapping |
| **Perspective Texturing** | Optional perspective-correct texturing with automatic mesh subdivision |
| **Live Viewport** | Real-time perspective preview matching GBA/NDS rendering |
| **Tilemap Editor** | Draggable grid with sprite tile painting, object placement, and save/load |
| **Visual Script Nodes** | Event-driven node graph for game logic — key input, movement, animation, branching |
| **Blueprint Scripts** | Reusable script assets with per-instance parameters, attachable to objects and scenes |
| **Collision System** | Pre-baked world-space collision with adaptive spatial grid, wall slide, barycentric floor height, and gravity |
| **OAM Sprites** | 8-directional animated sprites with LOD, running alongside 3D meshes |
| **MIDI Sound Engine** | DMA FIFO audio with SF2/DLS instruments, ARM ASM mixer, pitch bend, vibrato, and per-instance tuning |
| **SFX System** | Import WAV samples with waveform editor, trim, amplify, and one-shot playback via script nodes |
| **Delta Time** | Decouple game speed from framerate — consistent gameplay at any FPS |
| **Animated Skybox** | Panoramic sky with smooth scrolling and optional frame animation |
| **One-Click Build** | Package a `.gba` or `.nds` ROM directly from the editor |
| **mGBA Integration** | Launch ROMs directly in mGBA from the editor |
| **Blender-style Tools** | G to grab, S to scale, X/Y/Z axis constraints with visual guides |
| **Mode 7 Floor** | HBlank affine floor rendering for non-mesh projects |

---

## Getting Started

### Prerequisites

- **Windows 10/11**
- **Visual Studio 2022+** (MSVC C++17)
- **CMake 3.16+**
- **devkitPro** with devkitARM + libtonc *(for GBA ROM packaging; the devkitPro MSYS2 shell is also used to build NDS and PS Vita)*
- **mGBA** *(optional — for launching ROMs from the editor)*
- **WSL + pspdev** *(optional — for PSP `EBOOT.PBP` packaging)*
- **VitaSDK** *(optional — for PS Vita `.vpk` packaging)*
- **Visual C++ Redistributable** *(required to run pre-built releases — [download](https://aka.ms/vs/17/release/vc_redist.x64.exe))*

### 1. Build the Editor

```bash
git clone https://github.com/myuu-151/Affinity.git
cd Affinity
cmake -S . -B build
cmake --build build --config Release
```

Run the editor:
```
build\Release\AffinityEditor.exe
```

### 2. Install devkitPro (GBA Toolchain)

You need devkitPro to compile GBA ROMs. The editor handles the build process — you just need the toolchain installed.

1. Download the installer from **[devkitpro.org](https://devkitpro.org/wiki/Getting_Started)**
2. Run the installer
3. Open the devkitPro MSYS2 terminal and install the GBA packages:

```bash
pacman -S devkitARM libtonc
```

4. Verify the environment variables are set *(the installer usually handles this)*:

```
DEVKITPRO=/opt/devkitpro
DEVKITARM=/opt/devkitpro/devkitARM
```

### 3. Build a GBA ROM

1. Open or create a project in the editor
2. Add meshes, sprites, and set up your scene
3. Click the **GBA Build** button

The editor exports `mapdata.h` and runs `make` automatically. The output ROM is:
```
gba_runtime/affinity.gba
```

<details>
<summary><b>Manual ROM build</b></summary>

If you want to build the ROM from the command line (after exporting from the editor):

```bash
cd gba_runtime
make
```

> `mapdata.h` must be exported from the editor first — it contains all mesh, sprite, map, and script data.

</details>

### 4. Build a PSP EBOOT *(optional, Mode 4 / 3D)*

PSP packaging uses the **pspdev** toolchain. Because pspdev ships Linux binaries, the editor invokes it through **WSL**.

1. Install WSL (Ubuntu): run `wsl --install` in an elevated PowerShell, then reboot.
2. Inside WSL, install **pspdev** — follow **[pspdev.github.io](https://pspdev.github.io/)** (or use the `pspdev` installer). Make sure `PSPDEV` is set and `psp-gcc` is on the PATH.
3. In the editor, select the **PSP** build target and build. It exports the PSP data headers (`psp_mapdata.h`, `psp_rig.h`, `psp_sprites.h`, `psp_sound.h`, …) and runs `make` in `psp_runtime/`.

The output is:
```
psp_runtime/EBOOT.PBP
```

Run it in **PPSSPP**, or copy it to a real PSP under `PSP/GAME/<folder>/EBOOT.PBP`.

<details>
<summary><b>Manual EBOOT build</b></summary>

```bash
# inside WSL, after exporting the PSP headers from the editor
cd psp_runtime
make clean; make
```

> The `psp_*.h` headers must be exported from the editor first.

</details>

### 5. Build a PS Vita VPK *(optional, Mode 4 / 3D)*

PS Vita packaging uses **VitaSDK**, invoked through the **devkitPro MSYS2** shell (CMake + make).

1. Install **VitaSDK** to `C:\vitasdk` — follow **[vitasdk.org](https://vitasdk.org/)** (the `vdpm` bootstrap). The editor expects `VITASDK=/c/vitasdk`.
2. In the editor, select the **PS Vita** build target and build. It exports the PSV data headers (`psv_mapdata.h`, `psv_rig.h`, `psv_sprites.h`, `psv_hud.h`, …) and runs `cmake .. && make` in `psv_runtime/build`.

The output is:
```
psv_runtime/build/affinity_psv.vpk
```

Install it on a real Vita with **VitaShell**, or run it in **Vita3K**.

> **Vita3K:** install `libshacccg.suprx` into `ur0:/data/` (Vita3K can't compile shaders without it). The runtime already builds with the Vita3K-support flags enabled.

<details>
<summary><b>Manual VPK build</b></summary>

```bash
# devkitPro MSYS2 shell, after exporting the PSV headers from the editor
export VITASDK=/c/vitasdk
export PATH="$VITASDK/bin:$PATH"
cd psv_runtime && mkdir -p build && cd build
cmake .. && make
```

> The `psv_*.h` headers must be exported from the editor first.

</details>

---

## Controls

### Editor

| Key | Action |
|-----|--------|
| **W / S** | Move forward / back |
| **A / D** | Rotate left / right |
| **Q / E** | Camera height down / up |
| **I / K** | Pitch up / down |
| **G** | Grab (translate) selected object |
| **S** | Scale selected object |
| **X / Y / Z** | Constrain to axis (during grab) |
| **R + drag** | Resize selected object |
| **Delete** | Delete selected object |
| **Right-click** | Place new object in viewport |
| **Ctrl+A** | Select all nodes |
| **Ctrl+C / V** | Copy / paste nodes (works across projects) |
| **Ctrl+Z** | Undo delete |

### Nodes

| Key | Action |
|-----|--------|
| **Space** | Add node at cursor |
| **Right-click** | Add node / node properties |
| **Delete** | Delete selected nodes |
| **Ctrl+A** | Select all nodes |
| **Ctrl+C / V** | Copy / paste nodes (works across projects) |
| **Ctrl+Z** | Undo delete |
| **Ctrl+G** | Group selected nodes |
| **Ctrl+Shift+G** | Ungroup selected group |
| **Alt + click** | Create annotation |
| **Double-click** | Enter group node |
| **Escape** | Exit group |
| **Scroll wheel** | Zoom canvas |
| **Middle mouse + drag** | Pan canvas |

---

## Project Structure

```
src/
  editor/          — ImGui editor (main loop, frame tick)
  viewport/        — Software 3D rasterizer and Mode 7 preview
  map/             — Mesh, sprite, and tilemap data types
  math/            — Fixed-point types, camera struct
  platform/gba/    — GBA ROM packaging (invokes devkitARM)
  platform/nds/    — NDS ROM packaging
  platform/psp/    — PSP EBOOT packaging (invokes pspdev via WSL)
  platform/psv/    — PS Vita VPK packaging (invokes VitaSDK)
  platform/common/ — Shared node-graph -> C codegen (NDS + PSV)
gba_runtime/
  source/          — GBA runtime (software polygon renderer, OAM sprites, input)
  include/         — Generated mesh and map data header (mapdata.h)
nds_runtime/
  source/          — NDS runtime
psp_runtime/
  source/          — PSP runtime (Mode 4 / 3D, sceGu)
  include/         — Generated PSP data headers
psv_runtime/
  main.c, audio.c  — PS Vita runtime (Mode 4 / 3D, vitaGL)
  include/         — Generated PSV data headers
thirdparty/
  glfw/            — Windowing
  imgui/           — UI framework
```

---

## License

MIT

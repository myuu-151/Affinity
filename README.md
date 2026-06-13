<p align="center">
  <img src="assets/affinity_logo.png" alt="Affinity Logo" width="300">
</p>

<h1 align="center">Affinity Engine</h1>

<p align="center">
   An NDS/PSV 3D engine with a Windows desktop editor.
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
| **One-Click Build** | Package a `.gba`/`.nds` ROM, PSP `EBOOT.PBP`, or Vita `.vpk` directly from the editor |
| **mGBA Integration** | Launch ROMs directly in mGBA from the editor |
| **Blender-style Tools** | G to grab, S to scale, X/Y/Z axis constraints with visual guides |
| **Mode 7 Floor** | HBlank affine floor rendering for non-mesh projects |

---

## Getting Started

### Prerequisites

- **Windows 10/11**
- **Visual Studio 2022+** (MSVC C++17)
- **CMake 3.16+**
- **Visual C++ Redistributable** *(required to run pre-built releases — [download](https://aka.ms/vs/17/release/vc_redist.x64.exe))*

> Each build target needs its own toolchain — see **[Build Targets](#build-targets)** below. You can install only the ones you need.

### Build the Editor

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

---

## Build Targets

Author your project once, then package it for any target. In the editor, pick a target and build — it exports that target's data headers and invokes the matching toolchain automatically.

| Target | Output | Toolchain | Runs on |
|--------|--------|-----------|---------|
| [**GBA**](#gba) | `gba_runtime/affinity.gba` | devkitARM + libtonc | mGBA · real GBA |
| [**NDS**](#nds) | `nds_runtime/affinity.nds` | devkitARM + libnds | melonDS · real DS |
| [**PSP**](#psp) | `psp_runtime/EBOOT.PBP` | pspdev *(via WSL)* | PPSSPP · real PSP |
| [**PS Vita**](#ps-vita) | `psv_runtime/build/affinity_psv.vpk` | VitaSDK | Vita3K · real Vita |

GBA and NDS use the software/hardware 2D+3D path; PSP and PS Vita are **Mode 4 / 3D** targets.

---

### GBA

**Toolchain** — install **devkitPro** from **[devkitpro.org](https://devkitpro.org/wiki/Getting_Started)**, then in the devkitPro MSYS2 terminal:

```bash
pacman -S devkitARM libtonc
```

The installer usually sets `DEVKITPRO=/opt/devkitpro` and `DEVKITARM=/opt/devkitpro/devkitARM`.

**Build** — in the editor, click **GBA Build**. It exports `mapdata.h` and runs `make`. Output:

```
gba_runtime/affinity.gba
```

Open it in **mGBA** (the editor can launch it for you), or flash it to a real GBA.

<details>
<summary><b>Manual build</b></summary>

```bash
# after exporting mapdata.h from the editor
cd gba_runtime
make
```

</details>

---

### NDS

**Toolchain** — install **devkitPro** (same installer as GBA), then add the NDS packages in the devkitPro MSYS2 terminal:

```bash
pacman -S nds-dev      # pulls devkitARM, libnds, calico, maxmod, etc.
```

**Build** — in the editor, click **NDS Build**. It exports the NDS data headers and runs `make` in `nds_runtime/`. Output:

```
nds_runtime/affinity.nds
```

Run it in **melonDS**.

> **DeSmuME does not work** with modern libnds homebrew (the calico ARM7 core) — use **melonDS** for testing.

<details>
<summary><b>Manual build</b></summary>

```bash
# devkitPro MSYS2 shell, after exporting from the editor
cd nds_runtime
make
```

</details>

---

### PSP

PSP packaging uses the **pspdev** toolchain. Because pspdev ships Linux binaries, the editor invokes it through **WSL**.

**Toolchain**

1. Install WSL (Ubuntu): run `wsl --install` in an elevated PowerShell, then reboot.
2. Inside WSL, install **pspdev** — follow **[pspdev.github.io](https://pspdev.github.io/)** (or use the `pspdev` installer). Make sure `PSPDEV` is set and `psp-gcc` is on the PATH.

**Build** — in the editor, select the **PSP** build target and build. It exports the PSP data headers (`psp_mapdata.h`, `psp_rig.h`, `psp_sprites.h`, `psp_sound.h`, …) and runs `make` in `psp_runtime/`. Output:

```
psp_runtime/EBOOT.PBP
```

Run it in **PPSSPP**, or copy it to a real PSP under `PSP/GAME/<folder>/EBOOT.PBP`.

<details>
<summary><b>Manual build</b></summary>

```bash
# inside WSL, after exporting the PSP headers from the editor
cd psp_runtime
make clean; make
```

</details>

---

### PS Vita

PS Vita packaging uses **VitaSDK**, invoked through the **devkitPro MSYS2** shell (CMake + make).

**Toolchain** — install **VitaSDK** to `C:\vitasdk` by following **[vitasdk.org](https://vitasdk.org/)** (the `vdpm` bootstrap). The editor expects `VITASDK=/c/vitasdk`.

**Build** — in the editor, select the **PS Vita** build target and build. It exports the PSV data headers (`psv_mapdata.h`, `psv_rig.h`, `psv_sprites.h`, `psv_hud.h`, …) and runs `cmake .. && make` in `psv_runtime/build`. Output:

```
psv_runtime/build/affinity_psv.vpk
```

Install it on a real Vita with **VitaShell**, or run it in **Vita3K**.

> **Vita3K:** install `libshacccg.suprx` into `ur0:/data/` (Vita3K can't compile shaders without it). The runtime already builds with the Vita3K-support flags enabled.

<details>
<summary><b>Manual build</b></summary>

```bash
# devkitPro MSYS2 shell, after exporting the PSV headers from the editor
export VITASDK=/c/vitasdk
export PATH="$VITASDK/bin:$PATH"
cd psv_runtime && mkdir -p build && cd build
cmake .. && make
```

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

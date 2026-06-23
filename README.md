<p align="center">
  <img src="assets/affinity_logo.png" alt="Affinity Logo" width="300">
</p>

<h1 align="center">Affinity Engine</h1>

<p align="center">
   PS Vita 3D engine with a Windows desktop editor.
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
| **Local AI Assistant** | Offline LLM (llama.cpp) that knows your node catalog — ask how to set things up, or have it generate node graphs and insert them into a blueprint |
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

## Local AI Assistant

A built-in chat assistant (**View ▸ Assistant**) powered by a **local** LLM via embedded [llama.cpp](https://github.com/ggml-org/llama.cpp) — it runs entirely on your machine, **no internet, no API keys, nothing leaves your PC**.

It's grounded in the editor's actual node catalog (every node, its type, and its pins), so it can:

- **Answer how to set things up** — *"which nodes make the player jump?"*, *"how do I do a lock-on camera?"*
- **Generate node graphs** — *"build a graph: on Circle held, freeze the player and play a skel anim"* — then **one-click insert** the result straight into the open blueprint (placed and pre-selected so you can drag it into place).

### Setup

The model isn't bundled (it's large and separately licensed) — download any **GGUF** chat model and drop it in a `models/` folder at the repo root. A small **coder** model works best for node generation:

1. Get a GGUF from **[bartowski on Hugging Face](https://huggingface.co/bartowski)** — e.g. [Qwen2.5-Coder-3B-Instruct-GGUF](https://huggingface.co/bartowski/Qwen2.5-Coder-3B-Instruct-GGUF) (grab a `*Q4_K_M.gguf`; the 3B is light on CPU, the 7B is sharper if you have the RAM/GPU).
2. Put it in `models/` (e.g. `models/Qwen2.5-Coder-3B-Instruct-Q4_K_M.gguf`). The editor auto-detects the first `.gguf` there.
3. Open **View ▸ Assistant**, click **Load** (first load digests the node catalog once — give it a moment on CPU), then chat once it says **Loaded … (ready)**.

> CPU-only by default. It caches the node catalog after the first load so replies stay fast. The panel's **Settings** button picks compute: **CPU 50% / 75%** (share of cores) or **GPU 50% / 75% / 100%** (share of layers offloaded).

### Instruction vs. reasoning models

Two kinds of GGUF work here, and they suit different jobs — you load **one at a time**, and switching is just a **Load** of the other file:

- **Instruction models** (e.g. **Qwen2.5-Coder-Instruct**, 3B / 7B / 14B) answer *directly*. Fast, and the right default for everyday building and edits — bigger = sharper on complex graphs.
- **Reasoning models** (e.g. **[DeepSeek-R1-Distill-Qwen-14B](https://huggingface.co/bartowski/DeepSeek-R1-Distill-Qwen-14B-GGUF)**) *think* through the problem first — a long internal chain-of-thought — before answering. Slower, but they hold large, interdependent graphs together much better. Reach for one when an instruction model keeps fumbling a big (100+ node) graph.

The **Constrain output (grammar)** toggle works with both: it's *lazy*, so a reasoning model can do its full thinking pass and **only the final graph** is locked to valid syntax (real node types, params, clip names) — plain Q&A and prose stay free, so you can leave it on. **Auto-repair** (also in Settings) then lints the result and re-prompts the model to fix any broken links / pins.

Reasoning models generate a lot of think-tokens, so give them room: bump **Settings ▸ Context** to **32K** if your VRAM allows. (A 14B at 32K won't fully fit a 16 GB GPU — keep 16K, or use a smaller quant, if it spills to CPU and slows down.)

#### GPU acceleration (optional)

GPU offload only does something if llama.cpp is built with a GPU backend (the default build is CPU-only). On a machine with the toolkit installed, enable one at configure time — then the Settings ▸ GPU options run on the GPU (a full 7B fits in ~5 GB VRAM, a 14B in ~11 GB, running ~20–50× faster than CPU):

```bash
cmake -B build -S . -DAFFINITY_LLM_CUDA=ON      # NVIDIA — needs the CUDA Toolkit
# or
cmake -B build -S . -DAFFINITY_LLM_VULKAN=ON    # any GPU — needs the Vulkan SDK
cmake --build build --config Release
```

Once built, open **View ▸ Assistant ▸ Settings**, choose **GPU 100% (full offload)**, and pick your card in the **GPU device** dropdown. The choice is saved to `assistant_prefs.ini` and restored on the next launch.

##### Vulkan build, step by step (Windows)

Vulkan works on any modern GPU (NVIDIA/AMD/Intel) and needs no CUDA Toolkit — it's the quickest path on an NVIDIA card if you don't already have CUDA installed.

1. **Install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home)** (LunarG). Your GPU driver already ships the runtime; the SDK provides the build-time headers + `glslc`.
2. **Configure** with the Visual Studio generator and the Vulkan flag:
   ```bash
   cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DAFFINITY_LLM_VULKAN=ON
   ```
3. **If configure fails with `Could not find ... SPIRV-Headers`** (older SDKs, e.g. 1.3.275, don't ship its CMake package), install the header-only package and point CMake at it — no SDK re-download needed:
   ```bash
   git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git
   cmake -S SPIRV-Headers -B SPIRV-Headers/build -DCMAKE_INSTALL_PREFIX=SPIRV-Headers/out
   cmake --build SPIRV-Headers/build --target install
   # then re-run configure, adding:
   #   -DSPIRV-Headers_DIR=<abs path>/SPIRV-Headers/out/share/cmake/SPIRV-Headers
   ```
   *(Or just update to a newer Vulkan SDK, which bundles the SPIRV-Headers CMake package.)*
4. **Build:**
   ```bash
   cmake --build build --config Release --target AffinityEditor
   ```

To confirm offload is working, load a model with **GPU 100%** set and watch dedicated VRAM rise in Task Manager (or `nvidia-smi`) — a 14B should put ~10–11 GB on the card.

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

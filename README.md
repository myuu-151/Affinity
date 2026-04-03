<p align="center">
  <img src="assets/affinity_logo.png" alt="Affinity Logo" width="300">
</p>

# Affinity GBA Engine
A Game Boy Advance 3D engine with a Windows desktop editor. Import OBJ meshes, place objects in a live perspective viewport, and package directly to a `.gba` ROM with software polygon rasterization.

> **This project is in active development.** Features and APIs may change.

## Features

- **Software 3D Renderer** — Flat-shaded polygon rasterizer running on GBA hardware in Mode 4 bitmap
- **OBJ Mesh Import** — Load .obj files as mesh assets with per-mesh backface culling options
- **Live Viewport** — Real-time perspective preview matching the GBA's rendering
- **Blender-style Transform Tools** — G to grab, S to scale, X/Y/Z axis constraints with visual guides
- **One-Click GBA Packaging** — Builds a `.gba` ROM from the editor via devkitARM
- **mGBA Integration** — Launch the ROM directly in mGBA from the editor
- **OAM Sprite Support** — 8-directional animated sprites with LOD, running alongside 3D meshes
- **Mode 7 Fallback** — HBlank affine floor rendering still available for non-mesh projects

## Requirements

- **Windows 10/11**
- **Visual Studio 2022+** (MSVC C++17)
- **CMake 3.16+**
- **devkitPro** with devkitARM + libtonc (for GBA ROM packaging)

## Build

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
```

Run: `build\Debug\AffinityEditor.exe`

## Editor Controls

| Key | Action |
|-----|--------|
| W/S | Move forward/back |
| A/D | Rotate left/right |
| Q/E | Camera height down/up |
| I/K | Pitch up/down |
| G | Grab (translate) selected object |
| S | Scale selected object |
| X/Y/Z | Constrain to axis (during grab) |
| R + drag | Resize selected object |
| Delete | Delete selected object |
| Right-click viewport | Place new object |

## GBA Controls

| Button | Action |
|--------|--------|
| D-pad Up/Down | Move forward/back |
| D-pad Left/Right | Rotate |
| L/R | Camera height |
| Start | Reset camera |

## Architecture

```
src/
  editor/       — ImGui editor (main loop, frame tick)
  viewport/     — Software 3D rasterizer and Mode 7 preview
  map/          — Mesh, sprite, and tilemap data types
  math/         — Fixed-point types, camera struct
  platform/gba/ — GBA ROM packaging (invokes devkitARM)
gba_runtime/
  source/       — GBA runtime (software polygon renderer, OAM sprites, input)
  include/      — Generated mesh and map data header
thirdparty/
  glfw/         — Windowing
  imgui/        — UI framework
```

## License

MIT

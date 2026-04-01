# Affinity — GBA Mode 7 Engine

A Game Boy Advance Mode 7 engine with a Windows desktop editor. Build affine-scrolling worlds with a live perspective preview, then package directly to a `.gba` ROM.

> **This project is in active development.** Features and APIs may change.

<p align="center">
  <img src="assets/affinity_burst.png" alt="Affinity Logo" width="200">
</p>

## Features

- **Live Mode 7 Preview** — Software rasterizer matching GBA hardware per-scanline affine math
- **NEXXT-style Editor** — Tileset, tilemap, and palette panels with a large viewport
- **One-Click GBA Packaging** — Builds a `.gba` ROM from the editor via devkitARM
- **mGBA Integration** — Launch the ROM directly in mGBA from the editor

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
  viewport/     — Mode 7 software rasterizer
  map/          — Tile/tilemap data types
  math/         — Fixed-point types, camera struct
  platform/gba/ — GBA ROM packaging (invokes devkitARM)
gba_runtime/
  source/       — GBA Mode 7 runtime (HBlank ISR, input, tiles)
  include/      — Generated map data header
thirdparty/
  glfw/         — Windowing
  imgui/        — UI framework
```

## License

MIT

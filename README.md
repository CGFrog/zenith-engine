# Zenith Engine

The Zenith Engine is first and foremost and educational learning project. The goal is make an engine implementation that I can use and build off of for all future game projects.

## Build

Minimal CMake foundation for a C++20 Vulkan project. It provides:

- Official Vulkan headers
- Volk
- GLM
- SDL 3
- cgltf
- Vulkan Memory Allocator (VMA)
- Khronos glslang shader compiler

### Requirements

- CMake 3.24 or newer
- A C++20 compiler
- Git and an internet connection for the first configure
- Python 3 (used while building glslang)

### Configure and build

```sh
cmake -S . -B build
cmake --build build --config Debug
```

GLSL shaders placed under `shaders/` are compiled automatically during the
build. Use a stage-specific extension such as `.vert`, `.frag`, or `.comp`;
the generated SPIR-V files preserve the source directory layout under
`build/shaders/` and append `.spv` (for example,
`shaders/basic.vert` becomes `build/shaders/basic.vert.spv`).

Dependencies are pinned to release tags and downloaded into the build tree by
CMake's `FetchContent` module. The project links Volk instead of the Vulkan
loader, so include `volk.h` before other Vulkan-related headers in future source
files and initialize Volk when you begin writing the renderer.

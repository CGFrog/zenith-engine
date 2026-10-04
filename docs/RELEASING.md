# Release packaging

Zenith produces relocatable archives through CPack. A release contains the
game executable, compiled SPIR-V shaders, the selected project (the
`ZENITH_PACKAGE_PROJECT` cache variable, `projects/demo.zenith-project` by
default) with its assets, every scene, and game module, required non-system
runtime libraries, third-party license texts, and `content-manifest.json`. The manifest records the product and format
versions plus a SHA-256 digest for every packaged file.

## The packaged runtime

The `zenith` executable is project-agnostic. Started without arguments in an
extracted package (a folder holding `content-manifest.json`), it plays the one
`.zenith-project` at the package root; `--project <file>` names another and
`--scene <file>` plays that scene instead of the startup scene (the editor's
Standalone play passes an absolute path), with the project's asset root and
module; outside the project's scene root its folder becomes the scene root,
and without a project it plays with the package's own `assets/`. Arguments
may be any Unicode path. A project's game module is loaded directly from the package (in a
development tree, from a shadow copy so it can be rebuilt while the game
runs), its component types load the scenes, and its Play systems run every
Update and FixedUpdate with the player's input. Which application presents
it follows the module:

- a module other than the demo's: `ProjectApplication`, which adds nothing to
  the scene (no menus, player, or audio), so the module decides everything;
- the demo's module (`ZENITH_GAME_MODULE("demo", ...)`, the demo project's
  `modules/zenith_game_demo.zmodule`): the demo's title, pause, and settings
  menus, item HUD, first-person player, and music around the module's systems;
- no module (the built-in demo room in a build tree, or a project such as the
  showcase that names none): the demo gameplay linked into the executable,
  presented by the demo, so the scene can be walked.

The runtime needs nothing from `assets/` itself (its UI font is built in), so a
package's `assets/` holds only the project's assets. The engine's development
content (the demo room, its audio, the showcase) ships only when it is the
project's asset root, as for the projects under `projects/`.

## Create an archive

From a clean checkout with a C++20 toolchain, CMake 3.24 or newer, and the
platform development packages needed by SDL and Vulkan headers:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure -LE vulkan
cmake --build build --config Release --target package
```

To package another project, configure a release tree for it:

```powershell
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF `
  -DZENITH_PACKAGE_PROJECT="$PWD/projects/showcase.zenith-project" `
  -DZENITH_COPY_PROJECT_MODULE=OFF
cmake --build build-release --config Release --target package
```

A C++ project made by the Project Hub builds its module outside this tree, so
build it for the release first, against the release tree, then package it:

```powershell
cmake -S C:/games/my_game -B C:/games/my_game/build-release -DZENITH_ROOT="$PWD/build-release"
cmake --build C:/games/my_game/build-release --config Release   # copies modules/my_game.zmodule
cmake -S . -B build-release -DZENITH_PACKAGE_PROJECT=C:/games/my_game/my_game.zenith-project
cmake --build build-release --config Release --target package
```

`cmake/PackageProject.cmake` reads the project: its file goes to the archive
root, every `.zenith-scene` under its `sceneRoot` to the same place under the
package's `sceneRoot` (adding or removing a scene reconfigures), its game
module to the path it names (built from the matching `MODULE` target when this
tree builds it; a prebuilt module, such as a C++ template project's, must be
built for the package's configuration, since the build hash refuses another),
and its asset root to `assets/`. Engine assets the runtime itself needs are
listed in `ZENITH_ENGINE_RUNTIME_ASSETS` (paths under `assets/`, none by
default) and ship in the same folder; a project asset at one of those paths
(compared without case) fails the configure with the colliding paths, unless
the path is listed in `ZENITH_PACKAGE_ASSET_OVERRIDES`, which ships the
project's file instead with a warning. The project's `sceneRoot`, `startupScene`, and
`gameModule` must stay inside its folder; the packaged copy of the project file
names `assets` as its `assetRoot` (only that value is rewritten), so the
packaged project resolves inside the archive. `ZENITH_COPY_PROJECT_MODULE=OFF`
keeps a release tree from replacing the module beside the source project that
the editor loads. The editor's **File > Build and Package** runs exactly these
steps (configure when needed, build, `zenith_cooked_textures`, `package`) in
`build-release` for the open project.

Windows produces a relocatable `.zip` and, with
`-DZENITH_WINDOWS_INSTALLER=ON`, a WiX `.msi`; Linux produces `.tar.gz` and
installer-ready `.deb` artifacts with desktop metadata; macOS produces an
application bundle in a `.dmg` after its MoltenVK CI lane passes. Packaging
fails before creating the artifact if required content is missing, if an entry
escapes the staging root through a symlink, or if paths conflict under
case-insensitive comparison.

Release staging also writes `release-provenance.json` and generated release
notes from the exact Git revision, pinned dependency file, compiler, target,
and content-manifest digest. Windows release policy supplies
`ZENITH_SIGNING_CERT_SHA1`; setting `ZENITH_REQUIRE_SIGNING=1` makes packaging
fail unless Authenticode signing succeeds. Credentials are never stored in the
repository.

## Verify the artifact

The same check used by CI extracts into a clean directory whose name contains
spaces, then launches the packaged executable in CPU-only verification mode:

```powershell
cmake -DZENITH_ARCHIVE="build/Zenith-0.1.0-Windows-AMD64.zip" `
  -DZENITH_VERIFY_ROOT="build/clean extraction with spaces" `
  -P cmake/VerifyExtractedPackage.cmake
```

`VerifyExtractedPackage.cmake` verifies the single `.zenith-project` at the
archive root, which `zenith --verify-package` finds the way the packaged game
does; pass `-DZENITH_PACKAGE_PROJECT_NAME=<file>` to choose one with
`--project`. Verification checks the content manifest and shaders, loads the
project's game module, and loads every packaged scene with the module's
component types, printing `Verified scene <file>` for each.

For a normal smoke run from an extracted archive (`ZENITH_SMOKE_FRAMES=<n>`
bounds it), run the executable; it prints `Loaded game module ...` when the
project has one:

```powershell
./zenith
./zenith --project ./demo.zenith-project --scene "$PWD/scenes/showcase.zenith-scene"
```

The host must provide a Vulkan 1.4-capable driver and normal window/audio
services. Windows builds use the dynamic Vulkan loader supplied by the driver.
Linux builds require the standard C/C++ runtime, X11 or Wayland client
libraries selected by SDL, audio services, and a Vulkan loader/driver. Other
non-system shared libraries discovered from the executable are copied beside
it during installation.

The packaged project manifest keeps `assetRoot` and `sceneRoot` relative to the
archive root. Runtime lookup starts beside the executable and does not require
the source tree, build tree, or current working directory. Developers may set
`ZENITH_RESOURCE_ROOT` to test an alternate resource tree explicitly.

User settings and saves are never installed into the artifact. Runtime writes
go to the platform user-data directory through atomic replace plus a `.bak`
recovery copy, so package upgrade or rollback does not erase the active profile.
Structured diagnostics are written locally and are never transmitted unless a
shipping application adds an explicit, disclosed upload policy.

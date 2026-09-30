# Release packaging

Zenith produces relocatable archives through CPack. A release contains the
game executable, compiled SPIR-V shaders, the selected demo project, its assets
and startup scene, required non-system runtime libraries, third-party license
texts, and `content-manifest.json`. The manifest records the product and format
versions plus a SHA-256 digest for every packaged file.

## Create an archive

From a clean checkout with a C++20 toolchain, CMake 3.24 or newer, and the
platform development packages needed by SDL and Vulkan headers:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure -LE vulkan
cmake --build build --config Release --target package
```

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

For a normal smoke run from an extracted archive:

```powershell
./zenith --project ./demo.zenith-project
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

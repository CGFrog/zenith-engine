# Application and engine orchestration

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## `src/main.cpp`

The composition root. It parses the command line (`GameLaunch`), loads the
project (`--project`, else the one a release package holds at its root) and
its gameplay (`loadRuntimeGameplay`), answers `--verify-package` without a GPU
(`verifyPackage`: content manifest, shaders, the module, and every project
scene), or constructs `Engine` and the presenting application
(`DemoApplication` or `ProjectApplication`), initializes the engine, runs the
application, and shuts down in reverse dependency order. The
`RuntimeGameplay` is declared first, so the game module outlives the engine's
scene.

## `src/engine/Engine.hpp` and `Engine.cpp`

`Engine` owns the reusable window, input, renderer, assets, scene, and physics
instances, plus audio output and voice state. `run(Application&)` creates a
testable `ApplicationSession`, drives
variable and fixed scene callbacks, renders the active scene, and applies
requested transitions after presentation.

The engine loop polls input once per rendered frame, applies view input, runs
zero or more fixed physics steps, interpolates visible transforms, renders, and
presents. It also implements bounded smoke runs and rendering benchmarks via
environment variables. After each presented frame it reports to a
`DeferredAssetCollection`, which runs the asset collections that scene
changes and `ApplicationControl::requestAssetCollection` asked for once the
frames in flight retired (see [Assets](assets.md)).

## `src/engine/diagnostics/Logger.*`

`Engine` owns one `Logger` (declared first, so it outlives every subsystem)
and passes it explicitly; there is no process-wide logger. Each message has a
level, category, wall and steady timestamps, and a sequence number; the logger
keeps a bounded ring of recent entries, echoes each message unchanged to
stderr by default (so command-line output and validation tooling see the same
text), and calls subscribed sinks. It is thread-safe: renderer, asset worker,
and Vulkan debug-callback threads log through it. Subsystems hold a
`LogChannel` (logger plus category: `engine`, `graphics`, `assets`, `audio`,
`module`), which writes to stderr when it has no logger, so standalone tools
and tests that build a subsystem directly keep working. The renderer takes the
logger through `RendererOptions::logger` and `VulkanContext::initInstance`;
passes log through `VulkanContext::log()`. `AssetManager` and `AudioSystem`
take it at construction and `GameModule` through `GameModuleOptions`.
Applications reach it as `ApplicationContext::logger`. Still on plain stderr:
`checkVulkan` (`vulkan/VulkanCheck.hpp`), `ShaderModule`, ImGui's Vulkan error
callback (which carries no user data), the validation summary in
`GraphicsValidation.cpp` (a tooling contract), and the benchmark and capture
lines on stdout.

## `src/engine/application/Application.*`

Defines `Application`, `ApplicationScene`, contexts, deferred scene/quit
and asset-collection requests, and `ApplicationSession`. The session guarantees load/update/fixed
update/unload ordering and clears ECS/physics state between scenes while assets
remain cached.

## Folder contents

`src/engine/application/`: Application/scene lifecycle contracts, deferred
control requests, and the simulation clock (time scale and pause).

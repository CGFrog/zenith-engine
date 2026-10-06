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

The game executable accepts `[--verify-package] [--project <project-file>]
[--scene <scene-file>]` (`game/application/GameLaunch`). Without `--project`, a
release package (a resource root holding `content-manifest.json`) plays the one
project at its root, and a build tree plays the built-in demo room. `--scene`
plays that scene file (which must exist) with the project's asset root and
module; a file outside the scene root (an editor's standalone play) brings its
folder as the scene root, and without any project it plays with the runtime's
own `assets/`. Arguments are read as UTF-8 (on Windows from the wide command
line), so paths outside the ANSI code page work. A project with a game module
plays that module alone, presented by `DemoApplication` when it is the demo's
module (named `demo`) and otherwise by the project-agnostic
`ProjectApplication`, which only loads the scene and runs the module's Play
systems. Runs without a module (the built-in demo room, or a project such as
the showcase that names none) play the demo gameplay linked into the
executable, presented by `DemoApplication` with its first-person player (see
[Demo game](../game/demo.md)). `GameplayScene` loads the authored scene before
adding demo-specific entities and behavior. Other game scenes can load a
scene-root-relative reference with `loadProjectScene`, or any scene file with
`loadProjectSceneFile`; the engine does not assign gameplay meaning to those
references.

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

## `src/engine/application/SimulationClock.hpp` and frame timings

`ApplicationScene::simulationClock()` gives a time scale (clamped to [0, 10]),
a paused flag, and a frame step, read each frame after `update` (where editor
operators change it). `Engine::run` applies it to fixed-step accumulation (so
physics and FixedUpdate systems), skeletal animation, and audio
(`AudioSystem::setPlaybackRate`, which scales Scene-scope voices and holds
them at 0 while paused; Persistent voices such as music and menu sounds keep
playing in real time, see [Audio](audio.md)). `ApplicationScene::update` still
receives wall-clock time for tools and cameras; a scene scales its own
simulation work (the editor scales its Update systems). A paused clock
advances nothing except for a frame step (`frameStepSeconds`): that one frame
advances animation and the fixed-step accumulator by the step, whatever the
time scale, and plays audio at real-time rate, so a step of one fixed time
step runs exactly one fixed update (the scene enables fixed updates for that
frame). The engine loop needs no special case for it.

The engine measures its CPU phases (input, update, animation, fixed updates
and physics, transforms, UI, renderer, audio) every frame into
`EngineFrameTimings` and shares the last frame through
`ApplicationContext::frameTimings`; benchmark mode samples the same values.

## Folder contents

`src/engine/application/`: Application/scene lifecycle contracts, deferred
control requests, and the simulation clock (time scale and pause).

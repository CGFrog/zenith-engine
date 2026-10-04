# Demo game (`src/game/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The demo is split into the gameplay a game module registers and the
application composition around it. `gameplay/DemoGameplay.*` defines
`registerDemoGameplay(ModuleRegistrar&)`: the item collection component types
and the `demo.item_collection` system. The player is the engine's
`CharacterControllerComponent` with a `FirstPersonCameraComponent` camera,
which the engine's built-in systems drive (see
[Gameplay](../engine/gameplay.md)); the demo's action names (`Move`, `Look`,
`Jump`, `Sprint`) are the components' defaults. The registration declares the
demo's actions (`createDemoInputMap`) with `addInputActions`.
`GameplayScene`'s menus read the same map through its `GameplayRunner`. Look
preferences reach the engine's camera system as a `FirstPersonViewPreferences`
value in the scene registry's context, which `GameplayScene` refreshes every
frame from `GameSettings`.
`module/DemoGameModule.cpp` is the only module-specific file: it exports the
entry points and calls `registerDemoGameplay`. For runs without a game module
the `zenith` executable calls `linkDemoGameplay` (`application/LinkedGameplay.*`),
which runs the same registration through `linkGameplay`, so the game and the
editor run identical registration.

The executable's composition lives in `application/`: `GameLaunch` parses the
command line and finds a release package's project; `RuntimeGameplay` chooses
what plays (`loadRuntimeGameplay`: a project's game module, loaded in place when
it ships inside the resource root and from a shadow copy otherwise, or the
linked demo gameplay when there is no module) and which application presents
it (the demo's for the demo module, named `demo`, and for runs without a module;
`ProjectApplication` for any other module); `PackageVerification` checks a
package for `--verify-package`. `ProjectApplication` is project-agnostic: it
applies the project's render settings, loads the startup scene (or the
`--scene` file) with the module's component types, and runs the module's Play
systems every frame with the player's input; a capture camera
(`ZENITH_CAPTURE_CAMERA`) poses the view instead.

`DemoApplication` maps its typed scene enum to engine `SceneKey` values.
`GameplayScene` loads the room, creates props and the player, and requests a
fresh gameplay scene through the Reload action. It loads project scenes with
the gameplay's component types (the demo module's, or the linked ones), spawns
a player (`spawnFirstPersonPlayer`) only when the scene has no authored
character controller (otherwise it gives an authored one's camera the audio
listener when the scene has none), and runs the gameplay's systems while the
game is not paused. It also composes the title screen and HUD with the engine game-UI API,
and its pause and settings screens with the engine's `drawPauseMenu` and
`drawSettingsMenu`: the settings entries apply anti-aliasing, bloom, master
volume and mute, look sensitivity and inversion, and accessibility, and are
persisted in `GameSettings` (version 3 added `masterVolume`). `MenuLeft` and
`MenuRight` step the focused slider.

## Folder contents

`src/game/application/`: The executable's composition: command line
(`GameLaunch`), what plays (`RuntimeGameplay`: a project's game module, or the
demo gameplay linked in by `LinkedGameplay` for runs without one), package
verification, the project-agnostic `ProjectApplication`, and the demo's
application, which selects its scenes and owns demo-wide lifecycle state.

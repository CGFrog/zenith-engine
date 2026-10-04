# Demo game (`src/game/`)

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The demo is split into the gameplay a game module registers and the
application composition around it. `gameplay/DemoGameplay.*` defines
`registerDemoGameplay(ModuleRegistrar&)`: the `character_controller` component
type (`CharacterControllerComponent` with reflected tuning fields; `grounded`
and `camera` stay runtime-only, and a controller without a camera adopts its
first camera child) and two play-mode systems, `demo.first_person_view`
(Update) and `demo.first_person_movement` (FixedUpdate; it needs
`SystemContext::application` for physics). The systems keep the demo's
immutable `InputActionMap` in their closures and evaluate it against
`SystemContext::input`; without input they do nothing. Look preferences reach
them as a `FirstPersonViewPreferences` value in the scene registry's context,
which `GameplayScene` refreshes every frame from `GameSettings`.
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
a player only when the scene has no
authored character controller (otherwise it adds the player's collider, body,
and audio listener an authored one lacks), and runs the gameplay's systems while the game is
not paused. It also composes the title screen, HUD, pause menu, and persisted
audio, rendering, and input settings with the engine game-UI API.
`FirstPersonController` interprets semantic movement and view input and
remains entirely outside `zenith_engine`.

## Folder contents

`src/game/application/`: The executable's composition: command line
(`GameLaunch`), what plays (`RuntimeGameplay`: a project's game module, or the
demo gameplay linked in by `LinkedGameplay` for runs without one), package
verification, the project-agnostic `ProjectApplication`, and the demo's
application, which selects its scenes and owns demo-wide lifecycle state.

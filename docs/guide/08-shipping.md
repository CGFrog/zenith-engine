# 8. Shipping a game

[Guide contents](README.md) · Previous: [How the engine works](07-engine-concepts.md)

## Play outside the editor

The `zenith` game runner plays any project with its module:

```powershell
build/Debug/zenith.exe --project "C:/Users/you/Documents/Zenith Projects/my_game/my_game.zenith-project"
build/Debug/zenith.exe --project <project file> --scene <scene file>   # a specific scene
```

It loads the same `.zmodule` the editor uses, applies the project's input
actions and physics layers, and runs your systems and behaviours with the
player's input. For a project with its own module, the runner adds nothing
of its own (no menus, player, or audio), so the module decides everything.
The editor's **Standalone Game** play target runs exactly this.

> 📷 **Screenshot needed — `GUIDE-08-STANDALONE`:** *The game running in its own
> window via `zenith --project`, beside the editor's Info log showing the
> streamed `game` output.*

## Build and Package from the editor

**File > Build and Package** (for the platform you are on) builds the open
project in Release in a `build-release` folder, cooks its textures, and
packages it. The Build panel streams the output; when the package is ready the
editor opens its folder with the archive selected. The first package
configures the release folder and takes a while; later ones only rebuild what
changed. The release folder is set in **Preferences > Build**.

> 📷 **Screenshot needed — `GUIDE-08-PACKAGE`:** *The Build panel at the end of a
> successful Build and Package, and the file manager window it opens with the
> .zip selected.*

## What a package contains

| Content | Notes |
| --- | --- |
| `zenith` executable and the engine and runtime libraries | Project-agnostic; it plays the one `.zenith-project` at the package root. |
| Compiled shaders | SPIR-V. |
| Your project file, every scene, your module | The module must be built for the package's configuration (the build hash refuses another). |
| `assets/` | Only your project's assets. |
| `content-manifest.json` | A SHA-256 digest of every file, checked by `zenith --verify-package`. |
| Third-party licence texts, provenance, release notes | Generated from the exact Git revision and pinned dependencies. |

Windows produces a `.zip` (and an `.msi` with
`-DZENITH_WINDOWS_INSTALLER=ON`); Linux a `.tar.gz` and `.deb`; macOS a
`.dmg`. Packaging fails early if content is missing, a path escapes the
staging folder, or two paths collide case-insensitively.

To package from the command line, or for the full rules (asset overrides,
signing, verification), see [Releasing](../RELEASING.md).

## Testing your game

Because gameplay is deterministic and runs without a window, you can test it
headless:

- `GameplayRunner` plays a `Gameplay` (your module's components and systems)
  in a scene: load the scene, step it, then assert on components, events, or
  the UI commands in `GameUiContext::commands()`.
- The Zenith repository does exactly this for the First-Person Shooter
  template: `zenith_fps_template_module_build` builds the template's module,
  plays its scene, fires at a target, and checks the HUD counts it.
- `DefinitionTable` loading is strict, so a test that loads every data file
  catches typos before players do.

See [Debug, test, and ship](../tutorials/fps-game/06-debug-test-and-ship.md)
in the FPS tutorial, `tests/game/`, and `zenith_project_hub_tests` (`tests/editor/`) for examples.

## A checklist before you ship

- [ ] The game starts from the project's startup scene with `zenith --project`.
- [ ] Every scene loads (the Info log shows no migration or missing-asset warnings).
- [ ] Input works on keyboard/mouse **and** gamepad (actions, not keys).
- [ ] Pause stops the game (`setPaused`) and menus use `unscaledDeltaSeconds`.
- [ ] No state lives in module statics (play twice in a row in the editor without restarting).
- [ ] Data tables load without errors.
- [ ] The package passes `zenith --verify-package` after extraction to a fresh folder.

[Guide contents](README.md) · Previous: [How the engine works](07-engine-concepts.md)

# Game workspace guide

This subtree is the current demo client, not reusable engine code. It may depend
on `src/engine/`; the engine must never depend on it.

## Directory map

| Path | Responsibility |
| --- | --- |
| `application/` | The executable's composition: command line (`GameLaunch`), what plays (`RuntimeGameplay`: a project's game module, or the demo gameplay linked in by `LinkedGameplay` for runs without one), package verification, the project-agnostic `ProjectApplication`, and the demo's application, which selects its scenes and owns demo-wide lifecycle state. |
| `scene/` | Composes current assets, entities, lighting, physics, and gameplay scenes. |
| `gameplay/` | `registerDemoGameplay`: the component types and systems the demo game module registers. |
| `input/` | The demo's input action bindings. |
| `player/` | Interprets input as first-person player behavior. |
| `module/` | The `zenith_game_demo` entry points; nothing but a call to `registerDemoGameplay`. |

Gameplay that the editor must see (serialized components, simulation systems)
is registered in `gameplay/` and reaches the game only through the registered
systems, so the editor's play mode and the game behave the same. Menus,
settings, audio, and scene switching stay in `application/` and `scene/`, which
the module does not link.

`ProjectApplication`, `GameLaunch`, `RuntimeGameplay`, and
`PackageVerification` play any project; keep demo knowledge out of them (the
demo module's name in `RuntimeGameplay` is the one place the demo is chosen).

Keep asset names, key mappings, demo constants, and level composition here. If
behavior is genuinely project-agnostic, first define a narrow engine contract
and cover it under `tests/engine/`. Game lifecycle and control integration tests
belong under `tests/game/`.

# Game workspace guide

This subtree is the current demo client, not reusable engine code. It may depend
on `src/engine/`; the engine must never depend on it.

## Directory map

| Path | Responsibility |
| --- | --- |
| `application/` | Selects game scenes and owns demo-wide lifecycle state. |
| `scene/` | Composes current assets, entities, lighting, physics, and gameplay scenes. |
| `player/` | Interprets input as first-person player behavior. |

Keep asset names, key mappings, demo constants, and level composition here. If
behavior is genuinely project-agnostic, first define a narrow engine contract
and cover it under `tests/engine/`. Game lifecycle and control integration tests
belong under `tests/game/`.

# Editor workspace guide

The editor is a client of `zenith_engine`. Engine and game code must never
depend on this subtree.

## Directory map

| Path | Responsibility |
| --- | --- |
| `main.cpp` | Editor executable composition root. |
| `application/` | Editor application lifecycle and the UI/session coordinator. |
| `document/` | Scene document state, persistence workflow, and undo/redo history. |
| `interaction/` | Editor camera, picking, gizmos, and component editing widgets. |
| `settings/` | Persistent editor preferences and panel/layout state. |

## Local rules

- Reusable runtime behavior belongs in `src/engine/`; authoring policy and ImGui
  workflows stay here.
- Keep document operations usable without a window or GPU so they remain easy
  to test.
- Add editor tests under `tests/editor/` and update the explicit target source
  lists in the root `CMakeLists.txt`.
- `EditorSession` is integration code. Move reusable document or interaction
  behavior into its focused folder rather than growing the coordinator.

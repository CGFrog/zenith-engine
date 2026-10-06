# CLAUDE.md

@AGENTS.md

## Find code fast (read this before searching)

Do not explore the tree blind. In order:

1. **[`docs/CODE_MAP.md`](docs/CODE_MAP.md)** maps features and questions to
   files, key symbols, and tests, and has a one-line index of every source
   file per directory plus the main call chains (startup, a frame, Play, module
   hot reload). Most lookups end here.
2. **[`docs/CONTRIBUTOR_PLAYBOOK.md`](docs/CONTRIBUTOR_PLAYBOOK.md)** lists every
   file to touch for common changes (new component, operator, panel, render
   pass, service, test, template, file move).
3. The nested `AGENTS.md` (imported by each folder's `CLAUDE.md`) for the
   folder you edit, then the **one** subsystem doc it links
   (`docs/engine/*.md`, `docs/editor/*.md`).
4. Only then search, scoped to the folder: `git grep -n '<symbol>' -- src/engine/<subsystem>`.

Search hygiene:

- Never search or read `build/` (generated; contains fetched third-party
  sources and is huge). `git grep` and `git ls-files` ignore it.
- Long documents (`ROGUELIKE_READINESS_REVIEW.md`, `PERFORMANCE_ROADMAP.md`,
  `RENDERER_VALIDATION.md`, `CODE_MAP.md`, and the large subsystem pages such
  as `engine/graphics-passes.md`) are best grepped for a heading instead of
  read whole. `docs/history/` holds finished plans and dated measurement logs;
  read it only for background, never as current state.
- Headers carry the contract comments (ownership, units, threading). Read the
  `.hpp` before the `.cpp`.
- Tests are named after what they cover (`tests/<area>/<Thing>Tests.cpp`) and
  registered in that folder's `CMakeLists.txt`; `git grep -n '<Thing>' -- tests/*/CMakeLists.txt`
  gives the CTest name.

## Documentation map

| Audience | Start at |
| --- | --- |
| Game makers (editor, game logic) | `docs/guide/README.md` (user guide), `docs/tutorials/fps-game/` |
| Engine/editor contributors | `docs/CODE_MAP.md`, `docs/CONTRIBUTOR_PLAYBOOK.md`, `docs/REPOSITORY_GUIDE.md`, `docs/ARCHITECTURE.md` (overview) |
| Everything | `docs/README.md` |

When a change alters behaviour a user sees, update the matching
`docs/guide/` page too; when it adds, moves, or removes a source file, run
`python tools/docs/code_index.py` (and add a *Find it fast* row to
`docs/CODE_MAP.md` for a new capability). UI changes that need a new image get a
`📷 Screenshot needed` callout; regenerate the checklist with
`python tools/docs/screenshot_checklist.py`. CI's `docs` job fails on a broken
link (`python tools/docs/check_links.py`) or a stale generated file.

## Environment notes

- Primary development is Windows (MSVC, PowerShell 7, `tools/check.ps1`).
  On Linux use the same steps directly:
  `cmake --preset default && cmake --build --preset debug && ctest --preset debug-cpu`.
- The first configure downloads dependencies and needs network access; a
  build of everything is slow, so build the specific target you changed
  (`cmake --build build --target <target>`) and run the matching CTest (`-R`).
- GPU tests (label `vulkan`) need a Vulkan 1.4 device; if none is available,
  say so instead of claiming they pass.

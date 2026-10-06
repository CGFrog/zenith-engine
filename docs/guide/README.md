# Zenith user guide

This guide teaches you to make games with Zenith: first in the editor, then in
C++ game code, and finally how the engine underneath works. It assumes you
know some C++ and have used a 3D tool before (Blender, Unity, Unreal, Godot),
but not Zenith.

Read it in order the first time. Each chapter ends with a link to the next.

1. [Getting started](01-getting-started.md)
2. [The editor at a glance](02-editor-tour.md)
3. [Building a level](03-building-levels.md)
4. [Playing and debugging](04-play-and-debug.md)
5. [Writing game logic](05-game-logic.md)
6. [Gameplay services](06-gameplay-services.md)
7. [How the engine works](07-engine-concepts.md)
8. [Shipping a game](08-shipping.md)

Reference pages:

- [Cookbook](cookbook.md): short task-focused recipes.
- [Glossary](glossary.md): every Zenith term, with where it lives.
- [Screenshot checklist](SCREENSHOTS.md): the images this guide still needs.

## How this guide is written

- **Menu paths** are bold with `>` between levels: **Edit > Project
  Settings...**.
- **Keys** use code style: `F5`, `Shift+A`, `Ctrl+Z`. Every key runs a named
  *operator* (for example `play.start`); the name is given when you may want
  to search for it with `F3` or rebind it in **Edit > Keymap...**.
- **Code** is real Zenith API taken from the engine and its project
  templates. Paths such as `engine/scene/Scene.hpp` are include paths rooted
  at the engine's `src/` directory.
- **Deep dives** link to the subsystem reference pages under `docs/engine/`
  and `docs/editor/` when you want every detail.
- **Screenshots** that still need to be taken appear as callouts in this
  form (the ID names the image file):

```markdown
> 📷 **Screenshot needed — `GUIDE-03-INSPECTOR`:** *What to capture.*
```

[SCREENSHOTS.md](SCREENSHOTS.md) lists every callout with its target file
name; `tools/docs/screenshot_checklist.py` generates it. When a screenshot is
added, replace its callout with `![Alt text](../images/guide/<ID>.png)` and
re-run the script.

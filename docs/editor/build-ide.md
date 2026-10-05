# Build and IDE workflow

Part of the [repository guide](../REPOSITORY_GUIDE.md).

The session owns one build, `project/ProjectBuild`. It runs the open
project's own build (`projectBuildRequest`: configure and build its preset in
the project directory) or, once `build.command` or `build.directory` is edited,
that command through the platform shell in that directory, without blocking the
UI, using `project/BuildRunner`. The session polls it every frame, so a build
progresses with no Build area open; when it ends, its diagnostics replace the
previous build's entries (source `Build`) in `EditorDiagnostics`.

- `build.run` (**Ctrl B**, `operators/BuildOperators`) starts a build; pressed
  while one runs, it queues one more, which starts when that one finishes.
  `build.cancel` stops the build and drops a queued one. Both run headless.
- **Build on save** (`build.on_save`, off by default; Preferences > Build or
  the Build panel's settings) watches the project's `src` folder with
  `FileWatcher` while the project's own build is used. A settled save of a
  C or C++ source, header, or `CMakeLists.txt` runs `build.run`
  (`buildOnSaveWatches`).
- `panels/BuildPanel` shows the build's output with error and warning lines
  highlighted. Its Build and Cancel buttons run the operators.
- A successful build reloads the module once the editor is back in Edit mode
  (see [Game module hosting](modules.md)); while Play or Simulate runs, the
  status bar shows **Reload pending - stop to apply**.
- When a project opens and its module cannot load, the session starts the
  project's own build at once (`EditorSession::loadGameModule`), so a new
  project, or one whose module was built against another engine
  (`ZENITH_MODULE_API_VERSION`), needs no manual first build before Play.
  The module may be missing or refused; either way the Info panel says the
  module is building, and it loads when the build finishes. A
  user-configured `build.command` is not run automatically.
`project/CompilerOutputParser` turns MSVC/MSBuild, GCC/Clang, CMake, and linker
lines into located, deduplicated diagnostics. Clicking a located line in the
Build or Diagnostics panel runs `ide.command` through `project/ExternalEditor`,
which tokenizes the template and substitutes `{file}`, `{line}`, and `{column}`
into arguments (never a shell); the default is
`code -g "{file}:{line}:{column}"`. These settings live in the editor's
`settings.ini` and are editable in the Build panel and **Edit > Preferences**.

## Code generators

**Module > New Behaviour**, **New Component**, and **New System**
(`module.new_behaviour`, `module.new_component`, `module.new_system`, in the
`code` family of `operators/CodeOperators`) open `application/NewCodeDialog`,
which asks for a PascalCase name, a category (or a system's phase), and
reflected fields (`bool`, `int`, `float`, `vec3`, `string`, `entity`), and
checks them as they are typed. Create runs `module.generate_code {kind, name,
category, phase, fields}`, which runs headless:

- `project/CodeGenerator` (`generateModuleCode`, pure and CPU-tested) fills the
  snippets in `templates/cpp_game/snippets/<kind>.{hpp,cpp}.in` with
  `TemplateText`'s `{{TOKEN}}` substitution and writes `src/<Type>.hpp` and
  `.cpp`. A component's type gets a `Component` and a system's a `System`
  suffix; scene keys are the snake_case name, and a system is named
  `<module>.<snake_case>`.
- It adds the source above `# zenith:sources` in `src/CMakeLists.txt`, and the
  include and register call above `// zenith:includes` and
  `// zenith:register` in `src/GameModule.cpp`, indented as the marker is.
  A missing marker, an existing type, or a bad field fails before anything is
  written, with the file and marker named.
- It opens the new source at its register function with `ide.command` and
  requests a build, which hot reloads the module when it finishes.

The editor reads the snippets from the C++ game template it found at startup
(`codeSnippetDirectory`); the Project Hub never copies `snippets/` (or a
template's `template.json`) into a new project.

## Project templates

The Project Hub lists every folder of the template roots
(`listProjectTemplates`: the build's `templates/`, then the source tree's),
C++ Game first. Each template's `template.json` (`ProjectTemplateInfo`) gives
its name and description and may name a startup scene, an input action file,
and tags, which the generated project's manifest records; a template without
a startup scene gets the generated starter scene. `templates/fps_game/` ships
its scene and impact prefab, which `tests/editor/FpsTemplateTests.cpp`
writes with the engine's serializers (`--write <folder>`) and checks.
`zenith_fps_template_module_build` builds a generated FPS project, loads its
scene with the module's types, and plays it headless: a shot destroys a
target, the HUD counts it, and Escape pauses. `zenith_project_template_module_build` generates one type of each
kind into a new project and builds it with the template's warnings.

## Packaging

**File > Build and Package** runs `file.build_package {platform,
configuration}` (`operators/PackageOperators`): it plans the release
packaging of `docs/RELEASING.md` for the open project with
`project/PackageRunner` (`planPackage` is a pure function returning the
commands) — configure (only when `packageCacheNeedsConfigure` says the tree
was made for another project or configuration), build, the
`zenith_cooked_textures` target, and the `package` target — in its own release
tree (`package.directory`, default `build-release` inside `build.directory`),
never the editor's Debug tree. The configure step passes
`ZENITH_PACKAGE_PROJECT=<project>` and `ZENITH_COPY_PROJECT_MODULE=OFF`, so the
release module never replaces the module the editor has loaded and packaging
never triggers a hot reload. `PackageRunner` runs the steps one at a time on a
`PackageStepRunner` (a `BuildRunner` in the editor, a scripted fake in tests),
stops at the first failure, and can be cancelled. The session owns the runner
and polls it every frame; the Build panel shows its output (Build Output /
Package Output), the outcome is published as `Package` diagnostics, and a
successful package opens the folder with the archive selected. Only the host
platform can be packaged; the others are listed disabled with the reason.

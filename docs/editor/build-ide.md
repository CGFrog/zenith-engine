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
`project/CompilerOutputParser` turns MSVC/MSBuild, GCC/Clang, CMake, and linker
lines into located, deduplicated diagnostics. Clicking a located line in the
Build or Diagnostics panel runs `ide.command` through `project/ExternalEditor`,
which tokenizes the template and substitutes `{file}`, `{line}`, and `{column}`
into arguments (never a shell); the default is
`code -g "{file}:{line}:{column}"`. These settings live in the editor's
`settings.ini` and are editable in the Build panel and **Edit > Preferences**.

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

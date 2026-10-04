# Build and IDE workflow

Part of the [repository guide](../REPOSITORY_GUIDE.md).

`panels/BuildPanel` runs the `build.command` setting through the platform shell
in `build.directory` (default: the editor's working directory) without blocking
the UI, using `project/BuildRunner`. Output streams into the panel with error
and warning lines highlighted; when the build ends its diagnostics replace the
previous build's entries (source `Build`) in `EditorDiagnostics`.
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

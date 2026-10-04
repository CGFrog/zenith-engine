# Theme, preferences, and render settings

Part of the [repository guide](../REPOSITORY_GUIDE.md).

## Preferences (`panels/PreferencesWindow`)

**Edit > Preferences** (`wm.preferences`, `panels/PreferencesWindow`) edits
the theme tuning, UI scale, keymap link, IDE command (with a Test button),
autosave interval, and build settings. Edits are validated as they are made
(`validatePreferences`); every section without a problem is stored at once and
`settings.ini` is saved, while a section with a problem keeps its last valid
values. Each section has Reset to Defaults. The look is applied between
frames through the application's `application/EditorAppearance` (the one
owner of the font cache): art-pixel metrics, the pixel fonts for a new art
pixel (added to the atlas once, which the UI pass uploads again when it
grows), and the palette with the chosen accent; the session gives the glass
the theme tuning (`EditorSession::applyAppearance`). `EditorApplication`
creates the appearance at startup from `settings.ini`, so the Project Hub
draws in the preferred look, and since sessions apply their changes through
it, returning to the hub keeps the last session's look. Hard-coded accent
uses read `editorAccent()` (the outliner's selected-row cursor, the box
select fill).

## `src/editor/theme/`

The "Pixel Aero Night" theme: Frutiger Aero's glass-over-nature look drawn as
palette-limited pixel art. `EditorPalette` holds every colour the editor
draws (panels never write colour literals). `EditorTheme` puts ImGui on a
whole-number art-pixel grid (no rounding, no anti-aliasing), applies the
palette to ImGui and ImGuizmo, and loads the pixel fonts (VT323 for body
text, Press Start 2P for titles, Inter merged in for missing glyphs).
`PixelWallpaper` generates the deterministic, dithered night wallpaper and a
frosted copy on the CPU; `GlassBackdrop` uploads both as UI images and draws
glass by sampling the frosted copy where it lies on screen, so no blur pass
is needed. `ThemeChrome` paints the wallpaper, the glass frames behind docked
areas and the editor bars, and popup bevels. `EditorWidgets` wraps ImGui
buttons and headers with the same ids, painting the pixel look over them;
`PixelIcons` draws the pixel-art icon set and `EntityIcon` picks an entity's
icon from its components. `ThemeParameters` is the theme's only tuning (there
is one look): wallpaper on or off, frost radius, glass opacity, pane opacity
(never below the 4.5:1 text contrast over the brightest wallpaper colour), and
an accent from a curated set of palette hues. It is passed explicitly to
`applyEditorColors`, `ThemeChrome`, and `GlassBackdrop` (which regenerates the
frosted wallpaper when the frost changes); the defaults are the designed look.
The accent lives in the ImGui style, where widgets read it (`editorAccent`).

## `src/editor/settings/EditorRenderSettings.*`

Owns the render settings the editor requests and their persistent home: the
loaded project file, or `render-settings.json` in the editor's settings
directory when no project file was given. `apply` hands the renderer the
capability-validated settings and returns the fallbacks; the request itself
keeps what the user chose. `settings/RenderSettingsController` is the
session's single owner of that request and the fallbacks of its latest apply;
the Graphics panel and the Properties panel's Render tab both edit it through
`panels/RenderSettingsControls` (Save, Revert, storage location, fallbacks, and
the settings widgets), so they always show and change the same settings.

## `src/editor/interaction/GraphicsSettingsEditor.*`

ImGui controls for every `RenderSettings` field (options a device cannot run
stay visible but disabled), clamping into the ranges the render-settings
document accepts, and the live per-pass GPU timing table built from
`RenderStats`. `panels/GraphicsPanel` hosts them in the dockable Graphics
panel with Save and Revert, and the Properties panel's Render tab hosts the
settings controls over the same `RenderSettingsController`.

## Folder contents

`src/editor/settings/`: Persistent editor preferences (`settings.ini`: tools,
theme tuning, UI scale, pie menus, autosave), including the component
inspector's section order and collapse state (legacy panel flags are only read,
for migration) and the systems chosen for Simulate.

`src/editor/theme/`: The "Pixel Aero Night" look: palette, its tuning
(`ThemeParameters`, passed explicitly; one look only), art-pixel metrics, pixel
fonts, the procedural wallpaper and frosted glass, window chrome, themed widget
wrappers, and pixel-art icons.

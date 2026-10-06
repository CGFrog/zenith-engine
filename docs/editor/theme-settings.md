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
grows), and the palette with the chosen accent; the session gives the
chrome the theme tuning (`EditorSession::applyAppearance`). `EditorApplication`
creates the appearance at startup from `settings.ini`, so the Project Hub
draws in the preferred look, and since sessions apply their changes through
it, returning to the hub keeps the last session's look. Hard-coded accent
uses read `editorAccent()` (the outliner's selected-row cursor, the box
select fill).

## `src/editor/theme/`

The "8-bit Blender" theme: Blender's dark interface drawn as palette-limited
pixel art. Areas are square grey panes separated by a near-black gap, each
under a flat header strip; raised controls carry a hard one-art-pixel bevel
(light along the top and left, black along the bottom and right) and a black
outline; the current tool, tab, or running mode is lit in Blender's selection
blue, and selected objects in the viewport take its selection orange.
`EditorPalette` holds every colour the editor draws (panels never write colour
literals); its surfaces step up in lightness from the gap to the panes,
headers, and buttons. `EditorTheme` puts ImGui on a whole-number art-pixel
grid (no rounding, no anti-aliasing), applies the palette to ImGui and
ImGuizmo, and loads the pixel font (VT323, with Inter merged in for missing
glyphs); section titles share it and stand out by colour
(`palette::sectionInk`). `PixelPainter` draws the hard-edged rectangles,
outlines, bevels, and focus rings. `ThemeChrome` paints the gap, the header
strips and panes behind docked areas, the editor bars, translucent panels over
the 3D viewport (`paintOverlayPanel`), and popup bevels. `EditorWidgets` wraps
ImGui buttons and headers with the same ids, painting the pixel look over them
(`PaintButtonFrame` lets custom-drawn controls such as the pie menu match);
`PixelIcons` draws the pixel-art icon set and `EntityIcon` picks an entity's
icon from its components. `ThemeParameters` is the theme's only tuning (there
is one look): an accent from a curated set that each stand out from every
surface they are drawn on. It is passed explicitly to `applyEditorColors` and
`ThemeChrome`; the default is the designed blue. The accent lives in the ImGui
style, where widgets read it (`editorAccent`). Settings files from the
earlier "Pixel Aero Night" theme still load: its wallpaper and glass keys are
skipped and its accents map to the nearest current one.

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
theme tuning, UI scale, pie menus, the Prefab workspace preference `ui.prefab_workspace`, autosave), including the component
inspector's section order and collapse state (legacy panel flags are only read,
for migration) and the systems chosen for Simulate.

`src/editor/theme/`: The "8-bit Blender" look: palette, its tuning
(`ThemeParameters`, passed explicitly; one look only), art-pixel metrics, the
pixel font, hard-edged painting, window chrome, themed widget wrappers, and
pixel-art icons.

`widgets::CopyableText` draws wrapped text the user can copy with Ctrl+C while
hovering or from a right-click Copy menu; use it for errors and other messages
meant to be pasted elsewhere.

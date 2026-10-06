#pragma once

// One dockable editor window.

#include <string>
#include <string_view>

struct EditorContext;
struct InputState;

// One dockable editor window. The session owns each panel, decides whether it
// is visible, and opens its ImGui window; the panel draws only its contents.
class EditorPanel {
  public:
    virtual ~EditorPanel() = default;

    // Window title. It also names the panel's dock slot in the saved layout,
    // so changing it resets where users docked the panel.
    virtual const char* title() const = 0;

    // ImGuiWindowFlags for the panel's window.
    virtual int windowFlags() const {
        return 0;
    }

    // Whether the panel is drawn disabled outside Edit mode. Scene editing
    // panels need not be: in Play and Simulate their SceneEditing edits the
    // running scene live without recording history (SceneEditing::runtime).
    // Only a panel that edits nothing but the document should return true.
    virtual bool editModeOnly() const {
        return false;
    }

    // Runs every frame before drawing, whether or not the panel is visible.
    virtual void update(EditorContext& context, const InputState& input, float deltaSeconds);
    virtual void draw(EditorContext& context) = 0;
    // Runs on frames where the user has closed the panel.
    virtual void hidden(EditorContext& context);

    // Per-area view state saved with the area in workspaces.json, for
    // example which Properties tab it shows. Only panels created per area
    // (AreaInstancing::Multiple) should override these; a shared panel is
    // shown by every workspace and has no per-area state. The text is the
    // panel's own format; restoreAreaState ignores text it does not
    // understand, and an empty state means the panel's defaults.
    virtual std::string areaState() const;
    virtual void restoreAreaState(std::string_view state);
};

inline void EditorPanel::update(EditorContext&, const InputState&, float) {}

inline void EditorPanel::hidden(EditorContext&) {}

inline std::string EditorPanel::areaState() const {
    return {};
}

inline void EditorPanel::restoreAreaState(std::string_view) {}

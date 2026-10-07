#pragma once

// The prefab windows: opening, drawing, saving, closing, and recovering prefabs edited in floating windows.

#include "editor/application/EditorSessionCommands.hpp"
#include "editor/application/PrefabWindowSet.hpp"
#include "editor/document/PrefabWindowRecovery.hpp"
#include "editor/document/PrefabWriteCommand.hpp"
#include "editor/operators/KeymapRegion.hpp"
#include "editor/panels/EditorContext.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class AreaRegistry;
class EditorDiagnostics;
class EditorDocument;
class EditorPanel;
class EntityEyedropper;
struct InputState;
class Keymap;
class ThemeChrome;

// What the prefab windows need of the session that hosts them: running
// operators and keys on a window's document, the context a window's panels
// get, and the session's modal tools, prefab file hooks, and pending action.
class PrefabWindowHost {
  public:
    // The session's own actions, for what a window's panels ask that is not
    // about the window's document.
    virtual EditorSessionActions& levelActions() = 0;
    // Runs or polls an operator, and the region's keys and mouse bindings,
    // on window's document.
    virtual OperatorResult invokeOperatorOn(ApplicationContext& context, PrefabWindowDocument* window,
                                            std::string_view operatorId, std::string_view properties,
                                            KeymapRegion region) = 0;
    virtual bool operatorAvailableOn(ApplicationContext& context, PrefabWindowDocument* window,
                                     std::string_view operatorId) = 0;
    virtual void handleObjectKeysOn(ApplicationContext& context, PrefabWindowDocument* window, KeymapRegion region) = 0;
    virtual uint32_t dispatchMouseChordsOn(ApplicationContext& context, PrefabWindowDocument* window,
                                           KeymapRegion region, uint32_t allowedButtons) = 0;
    // What window's panels get: windowContext is the session's with the
    // window's scene; context is the session's own.
    virtual EditorContext prefabWindowEditorContext(ApplicationContext& windowContext, ApplicationContext& context,
                                                    PrefabWindowDocument& window, EditorSessionActions& actions) = 0;
    // A file dialog is pending or a modal transform runs: windows neither
    // open, close, nor save.
    virtual bool prefabWindowToolsBusy() const = 0;
    // Before window's document goes: ends a modal transform running in it.
    virtual void cancelModalTransformIn(ApplicationContext& context, uint64_t window) = 0;
    // Every prefab file a window reads or writes goes through these.
    virtual PrefabWriteHooks prefabWriteHooks() = 0;
    // Refreshes change.reference's instances in every open document but
    // originWindow (and the level unless refreshLevel); returns how many.
    virtual size_t refreshPrefabInstancesElsewhere(ApplicationContext& context, const PrefabFileChange& change,
                                                   const PrefabWindowDocument* originWindow, bool refreshLevel) = 0;
    // The unsaved-changes prompt of a window closing as part of quitting or
    // leaving for the Project Hub was answered.
    virtual void continuePendingAction(ApplicationContext& context) = 0;
    virtual void cancelPendingAction() = 0;

  protected:
    ~PrefabWindowHost() = default;
};

// What a PrefabWindowController is built with. References outlive it.
struct PrefabWindowServices {
    PrefabWindowHost& host;
    // The level's document: the options windows open prefabs with.
    EditorDocument& document;
    EditorDiagnostics& diagnostics;
    // Makes each window's Hierarchy, Viewport, and Inspector panels.
    const AreaRegistry& areaTypes;
    ThemeChrome& themeChrome;
    const Keymap& keymap;
    EntityEyedropper& eyedropper;
    // Where windows with unsaved changes are autosaved.
    std::filesystem::path recoveryDirectory;
};

// Prefabs open in floating windows of their own (PrefabWindowSet) while the
// level stays open, editable, and playable underneath. Each window shows its
// own Hierarchy, Viewport, and Inspector on the window's document, runs
// operators on it through the host, and saves to the prefab's file,
// refreshing the prefab's instances in every other open document. Windows
// open and close between frames' draws or by request, never while their
// panels draw.
class PrefabWindowController final : public EditorPrefabWindowCommands {
  public:
    explicit PrefabWindowController(PrefabWindowServices services);
    PrefabWindowController(const PrefabWindowController&) = delete;
    PrefabWindowController& operator=(const PrefabWindowController&) = delete;

    // EditorPrefabWindowCommands
    bool prefabCloseAvailable() const override;
    bool prefabEditAvailable() const override;
    OperatorResult editPrefab(ApplicationContext& context, const std::filesystem::path& reference) override;
    void requestCloseFocusedPrefab(ApplicationContext& context) override;

    PrefabWindowSet& windows();
    const PrefabWindowSet& windows() const;
    // The window that last had keyboard focus, or null when a level area
    // had it since (clearFocus); the main menu, global keys, and F3 act on it.
    PrefabWindowDocument* focused() const;
    void clearFocus();
    // window's viewport panel, or null.
    ViewportPanel* viewport(uint64_t window) const;
    // Whether window's panels are docked in the window's own dockspace (the
    // editor smoke checks it); false before the window was first drawn.
    bool panelsDocked(uint64_t window) const;
    // Runs draw with the EditorContext window's panels get.
    void withEditorContext(ApplicationContext& context, PrefabWindowDocument& window,
                           const std::function<void(EditorContext&)>& draw);

    // Closes the window at the next update, first asking about unsaved
    // changes (Save, Discard, or Cancel).
    void requestClose(uint64_t id);
    // Writes window's prefab to its file and refreshes its instances in every
    // other open document; false after reporting why not.
    bool save(ApplicationContext& context, PrefabWindowDocument& window);
    // For quitting and the Project Hub: asks about the first window with
    // unsaved changes; false when none has any.
    bool askAboutUnsaved();
    // A prefab file changed outside the editor: refreshes its instances in
    // the windows, and warns when a window edits that prefab itself.
    void reconcile(ApplicationContext& context, const PrefabFileChange& change);

    // Writes the windows with unsaved changes to the recovery directory,
    // replacing its previous set; reportErrors is false in a crashed process.
    void autosave(ApplicationContext& context, bool reportErrors);
    bool anyUnsaved() const;
    bool recoveryAvailable() const;
    // Recover: reopens the autosaved windows.
    void recover(ApplicationContext& context);
    bool discardRecovery(std::string& error);

    // Frame hooks, in the order the session calls them.
    // Between frames: closes the windows whose close was requested.
    void applyCloses(ApplicationContext& context);
    // The windows' panels' updates, and their scenes' asset leases.
    void update(ApplicationContext& context, const InputState& input, float deltaSeconds);
    // The windows and the unsaved-changes prompt.
    void draw(ApplicationContext& context);
    // Once per frame before the renderer draws: each window's world
    // transforms, and its scene bound as its render scene's content.
    void bindScenes(ApplicationContext& context);
    // Drops every window with its unsaved changes (the session ending).
    void closeAll(ApplicationContext& context);

  private:
    class Actions;

    // The ImGui side of one window: its own panels and layout state.
    struct View {
        uint64_t id{0};
        std::shared_ptr<EditorPanel> hierarchy;
        std::shared_ptr<EditorPanel> viewport;
        std::shared_ptr<EditorPanel> inspector;
        bool layoutBuilt{false};
        // The window's dockspace, once it was first drawn, and the nodes its
        // layout gives each panel.
        unsigned int dockspaceId{0};
        unsigned int hierarchyDock{0};
        unsigned int viewportDock{0};
        unsigned int inspectorDock{0};
        // Bring the window to the front at the next draw.
        bool focusRequest{true};
    };

    // A window waiting for Save, Discard, or Cancel before it closes;
    // continuesPendingAction when closing it is part of quitting or leaving
    // for the Project Hub.
    struct Prompt {
        uint64_t id{0};
        bool continuesPendingAction{false};
    };

    std::shared_ptr<EditorPanel> createPanel(std::string_view typeId) const;
    View* findView(uint64_t id) const;
    // The open windows with their views, copied so that an operator a
    // window's panel runs may open another window meanwhile.
    std::vector<std::pair<PrefabWindowDocument*, View*>> snapshot() const;
    // Creates the panels of a window just opened or recovered, its render
    // scene, and frames its viewport on the prefab.
    void addView(ApplicationContext& context, PrefabWindowDocument& window);
    // Before window closes, after its panels: drops its render scene and an
    // eyedropper it armed.
    void releaseWindow(ApplicationContext& context, const PrefabWindowDocument& window);
    void refreshAfterSave(ApplicationContext& context, PrefabWindowDocument& window, const SavedPrefabDocument& saved);
    void drawWindow(ApplicationContext& context, PrefabWindowDocument& window, View& view);
    // Splits the window's dockspace into the panels' nodes, kept in view.
    void buildLayout(View& view, unsigned int dockspaceId);
    void drawToolbar(ApplicationContext& context, PrefabWindowDocument& window);
    void drawPrompt(ApplicationContext& context);

    PrefabWindowServices m_services;
    PrefabWindowRecovery m_recovery;
    // The windows' scenes may hold module component storage: the session
    // declares the controller after its module host, so they go first.
    PrefabWindowSet m_windows;
    // Declared after the windows: a viewport panel shows its window's
    // document, so the views go first.
    std::vector<std::unique_ptr<View>> m_views;
    uint64_t m_focused{0};
    std::vector<uint64_t> m_closes;
    std::optional<Prompt> m_prompt;
    bool m_openPrompt{false};
};

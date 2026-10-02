#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ThumbnailCache.hpp"
#include "editor/settings/AssetBrowserSettings.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class EditorDiagnostics;

// What the browser asks of the editor when the user acts on an asset or the
// project changes on disk. Unset actions are simply not offered or skipped.
struct AssetBrowserActions {
    std::function<void(const std::filesystem::path&)> openScene;
    // Re-imports a model; called by the Reload Model button and automatically
    // when a model file changes on disk.
    std::function<void(const std::filesystem::path&)> reloadModel;
    // Project files moved or were renamed, in the browser or outside it.
    std::function<void(std::span<const ProjectFileMove>)> filesMoved;
    // Project files were moved to the trash or deleted.
    std::function<void(std::span<const std::filesystem::path>)> filesRemoved;
};

// Browses the project's asset and scene roots: a folder tree with favourites,
// breadcrumbs, a grid or list of the current folder, search and type filters,
// and file operations (create, rename, move, duplicate, move to trash, show in
// the OS file browser). Models drag into the viewport or inspector as
// "ZENITH_MODEL_ASSET" payloads, and any item drags onto a folder to move it;
// scenes open on double-click.
//
// The catalog identifies files through the project's asset databases, so
// renames and moves keep `.meta` sidecars and GUIDs, and file watchers report
// external changes as they happen: changed models are reloaded, changed
// textures get fresh thumbnails, and moves and removals are passed to the
// editor so open documents follow them. View mode, favourites, and the last
// folder persist in the settings file.
class AssetBrowserPanel final : public EditorPanel {
  public:
    // settingsFile is the panel's preferences file (asset-browser.json in the
    // editor's settings directory); it is read here and written on change.
    AssetBrowserPanel(std::filesystem::path assetRoot, std::filesystem::path sceneRoot,
                      std::filesystem::path settingsFile, AssetCatalogOptions catalogOptions = {});

    const char* title() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;

    // Draws the panel contents into the current ImGui window. draw() calls it
    // with actions bound to the editor session; it needs no session itself.
    void drawBrowser(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions);

    // Rescans the project. Changes after the first scan are queued for the
    // next dispatchChanges, and changed textures get fresh thumbnails.
    void refresh(EditorDiagnostics& diagnostics);
    // Picks up what the file watchers saw (or rescans once a second when the
    // project cannot be watched), then dispatches every queued change.
    void pollChanges(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions, float deltaSeconds = 0.0f);
    // Acts on queued changes: modified models are reloaded, modified textures
    // are reported, and moves and removals are passed on.
    void dispatchChanges(EditorDiagnostics& diagnostics, const AssetBrowserActions& actions);

    const AssetCatalog& catalog() const;
    // The asset root's database, for scene documents to resolve GUIDs.
    std::shared_ptr<const AssetDatabase> assetDatabase() const;
    const std::filesystem::path& currentFolder() const;
    // Shows a catalog folder; unknown folders are refused with a diagnostic.
    void navigateTo(const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    AssetBrowserViewMode viewMode() const;
    void setViewMode(AssetBrowserViewMode mode, EditorDiagnostics& diagnostics);
    bool isFavourite(const std::filesystem::path& folder) const;
    void toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    const std::filesystem::path& selectedPath() const;
    // Search text matches file names in the current folder and below.
    void setSearchText(std::string text);
    void setKindShown(EditorAssetKind kind, bool shown);

    // File commands. Each reports its outcome through diagnostics, rescans the
    // catalog, and selects what it produced.
    void createFolder(EditorDiagnostics& diagnostics);
    void createScene(EditorDiagnostics& diagnostics);
    void renameItem(const std::filesystem::path& item, const std::string& utf8NewName, EditorDiagnostics& diagnostics);
    // Moves an item into another folder of the same root.
    void moveItem(const std::filesystem::path& item, const std::filesystem::path& folder,
                  EditorDiagnostics& diagnostics);
    void duplicateItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    void moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    void revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics);

  private:
    struct BrowserEntry {
        std::filesystem::path path;
        std::string label;
        const EditorAssetRecord* asset{nullptr};
    };

    enum class CommandKind {
        Navigate,
        CreateFolder,
        CreateScene,
        BeginRename,
        Move,
        Duplicate,
        BeginDelete,
        Reveal,
        ToggleFavourite,
    };

    // Commands chosen while drawing run after the catalog is no longer being
    // iterated, since they may rescan it.
    struct DeferredCommand {
        CommandKind kind{};
        std::filesystem::path path;
        // Move only: the folder to move path into.
        std::filesystem::path destination;
    };

    enum class OpenModal {
        None,
        Rename,
        Delete,
    };

    void rescan(EditorDiagnostics& diagnostics);
    void absorb(AssetCatalogResult result, EditorDiagnostics& diagnostics);
    void followMovedItem(const std::filesystem::path& source, const std::filesystem::path& destination,
                         EditorDiagnostics& diagnostics);
    AssetBrowserActions sessionActions(EditorContext& context);
    void acceptItemDrop(const std::filesystem::path& folder);
    void reportOutcome(EditorDiagnostics& diagnostics, bool success, const std::string& message);
    void saveSettings(EditorDiagnostics& diagnostics);
    void reportPendingSettingsError(EditorDiagnostics& diagnostics);
    void keepStateValid();

    std::string folderKey(const std::filesystem::path& folder) const;
    std::filesystem::path resolveFolderKey(const std::string& key) const;
    std::string rootLabel(const std::filesystem::path& root) const;
    bool isRoot(const std::filesystem::path& path) const;

    void drawToolbar(EditorDiagnostics& diagnostics);
    void drawBreadcrumbs();
    void drawFolderPane();
    void drawFolderNode(const EditorAssetFolder& folder, const std::string& label);
    void drawContents(const AssetBrowserActions& actions);
    std::vector<BrowserEntry> visibleEntries() const;
    void drawList(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions);
    void drawGrid(const std::vector<BrowserEntry>& entries, const AssetBrowserActions& actions);
    void handleEntryInteraction(const BrowserEntry& entry, bool pressed, const AssetBrowserActions& actions);
    void drawItemMenu(const std::filesystem::path& item, bool folder);
    void drawFolderMenu();
    void drawSelectedAsset(const AssetBrowserActions& actions);
    void runDeferredCommand(EditorDiagnostics& diagnostics);
    void beginRename(const std::filesystem::path& item);
    void drawModals(EditorDiagnostics& diagnostics);
    std::string textureDetail(const EditorAssetRecord& asset);

    AssetCatalog m_catalog;
    ThumbnailCache m_thumbnails;
    std::filesystem::path m_settingsPath;
    AssetBrowserSettings m_settings;
    std::string m_pendingSettingsError;
    std::filesystem::path m_currentFolder;
    std::filesystem::path m_selected;
    std::string m_search;
    std::array<bool, 4> m_shownKinds{true, true, true, true};
    std::optional<DeferredCommand> m_deferred;
    // Catalog changes and moved or removed files not yet dispatched.
    AssetCatalogResult m_pending{.success = true};
    OpenModal m_openModal{OpenModal::None};
    bool m_modalNeedsOpening{false};
    std::filesystem::path m_modalTarget;
    std::string m_renameText;
    bool m_revealCurrentInTree{true};
    float m_refreshElapsed{0.0f};
    bool m_scanAttempted{false};
    bool m_initialized{false};
};

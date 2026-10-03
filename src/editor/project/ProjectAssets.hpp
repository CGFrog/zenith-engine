#pragma once

#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/project/ThumbnailCache.hpp"
#include "editor/settings/AssetBrowserSettings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class AssetManager;
class EditorDiagnostics;

// What the editor does when the project changes on disk. Unset actions are
// skipped.
struct ProjectAssetActions {
    // Re-imports a model that changed on disk.
    std::function<void(const std::filesystem::path&)> reloadModel;
    // Project files moved or were renamed, in the editor or outside it.
    std::function<void(std::span<const ProjectFileMove>)> filesMoved;
    // Project files were moved to the trash or deleted.
    std::function<void(std::span<const std::filesystem::path>)> filesRemoved;
};

// A file or folder renamed or moved through ProjectAssets, so every view can
// carry its own state (current folder, selection) along. Sequence numbers
// start at 1 and increase by one per move.
struct ProjectItemMove {
    uint64_t sequence{0};
    std::filesystem::path source;
    std::filesystem::path destination;
};

// The project's asset service, shared by every asset browser: the catalog
// with its asset databases and file watchers, texture thumbnails, change
// polling and dispatch, the file operations, and the shared browser
// preferences in asset-browser.json.
//
// Exactly one owner (the editor session) calls pollChanges once per frame, so
// a change on disk is scanned once and dispatched once however many browsers
// are open. Browsers only read the catalog and call the file operations;
// changes those operations cause are queued and go out with the next
// dispatch. The settings file has one writer, this object: favourites are
// shared by every browser, while the last folder and view mode are whatever
// the primary browser last chose (see AssetBrowserPanel).
//
// Threading: everything here runs on the UI thread. The catalog's file
// watchers report from their own threads and ThumbnailCache decodes on its
// worker thread; both hand results over under their own locks, so callers
// never synchronize.
class ProjectAssets {
  public:
    // settingsFile is the browser preferences file (asset-browser.json in the
    // editor's settings directory); it is read here and written on change.
    ProjectAssets(std::filesystem::path assetRoot, std::filesystem::path sceneRoot, std::filesystem::path settingsFile,
                  AssetCatalogOptions catalogOptions = {});
    ProjectAssets(const ProjectAssets&) = delete;
    ProjectAssets& operator=(const ProjectAssets&) = delete;

    // Rescans the project. Changes after the first scan are queued for the
    // next dispatchChanges, and changed textures get fresh thumbnails.
    void refresh(EditorDiagnostics& diagnostics);
    // Picks up what the file watchers saw (or rescans once a second when the
    // project cannot be watched; the first call scans at once), then
    // dispatches every queued change.
    void pollChanges(EditorDiagnostics& diagnostics, const ProjectAssetActions& actions, float deltaSeconds = 0.0f);
    // Acts on queued changes once: modified models are reloaded, modified
    // textures are reported, and moves and removals are passed on.
    void dispatchChanges(EditorDiagnostics& diagnostics, const ProjectAssetActions& actions);

    // Whether a scan has listed the project yet.
    bool scanned() const;
    const AssetCatalog& catalog() const;
    // The asset root's database, for scene documents to resolve GUIDs. It
    // stays the same object for this service's lifetime.
    std::shared_ptr<const AssetDatabase> assetDatabase() const;
    // CPU texture thumbnails, keyed by absolute path and invalidated here when
    // textures change, move, or are trashed.
    ThumbnailCache& thumbnails();

    bool isRoot(const std::filesystem::path& path) const;
    // Settings-file keys for folders: relative to the asset root, UTF-8, '/'.
    std::string folderKey(const std::filesystem::path& folder) const;
    std::filesystem::path resolveFolderKey(const std::string& key) const;

    // The preferences as last loaded or saved.
    const AssetBrowserSettings& settings() const;
    bool isFavourite(const std::filesystem::path& folder) const;
    void toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    // Persists the primary browser's folder and view mode when they changed.
    void rememberView(const std::filesystem::path& folder, AssetBrowserViewMode mode, EditorDiagnostics& diagnostics);
    // Reports, once, why the settings file could not be loaded.
    void reportPendingSettingsError(EditorDiagnostics& diagnostics);

    // Asset browser views. At most one view is the primary: the one whose
    // folder and view mode are remembered, and the one the editor addresses
    // when it needs "the" asset browser. A view gets an id from registerView,
    // claims the role each frame it is shown, and releases it when hidden or
    // destroyed. A claim succeeds while nobody holds the role, or when the
    // primary has not been shown this frame or the last (frames are counted
    // by pollChanges), so a primary left in another workspace or a
    // background tab is replaced by a browser the user can see.
    uint64_t registerView();
    // Returns whether view is the primary afterwards.
    bool claimPrimaryView(uint64_t view);
    // Does nothing unless view is the primary.
    void releasePrimaryView(uint64_t view);
    bool isPrimaryView(uint64_t view) const;
    bool hasPrimaryView() const;
    // Asks the primary browser to show item (an absolute path): its folder
    // opened and the item selected. A later request replaces an untaken one.
    void requestReveal(std::filesystem::path item);
    // The pending reveal when view is the primary; it is handed out once.
    std::optional<std::filesystem::path> takeRevealRequest(uint64_t view);

    // File operations. Each reports its outcome through diagnostics and
    // rescans the catalog. The returned path is what the operation produced,
    // or nullopt when it failed.
    std::optional<std::filesystem::path> createFolder(const std::filesystem::path& parent,
                                                      EditorDiagnostics& diagnostics);
    std::optional<std::filesystem::path> createScene(const std::filesystem::path& folder,
                                                     EditorDiagnostics& diagnostics);
    // Returns the renamed path, which is item itself when the name was
    // unchanged.
    std::optional<std::filesystem::path> renameItem(const std::filesystem::path& item, const std::string& utf8NewName,
                                                    EditorDiagnostics& diagnostics);
    // Moves an item into another folder of the same root.
    std::optional<std::filesystem::path> moveItem(const std::filesystem::path& item,
                                                  const std::filesystem::path& folder, EditorDiagnostics& diagnostics);
    std::optional<std::filesystem::path> duplicateItem(const std::filesystem::path& item,
                                                       EditorDiagnostics& diagnostics);
    bool moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics);
    void revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics);

    // Recent renames and moves made through this service, oldest first. Only
    // the most recent ones are kept; a view that falls further behind simply
    // falls back to the nearest surviving folder.
    std::span<const ProjectItemMove> recentItemMoves() const;
    // The sequence number of the latest move, or 0 before the first.
    uint64_t latestItemMoveSequence() const;

  private:
    void absorb(AssetCatalogResult result, EditorDiagnostics& diagnostics);
    void recordItemMove(const std::filesystem::path& source, const std::filesystem::path& destination,
                        EditorDiagnostics& diagnostics);
    std::optional<std::filesystem::path> finishRelocation(const std::filesystem::path& source, AssetDatabase* database,
                                                          ProjectFileResult result, const std::string& message,
                                                          EditorDiagnostics& diagnostics);
    void saveSettings(EditorDiagnostics& diagnostics);

    AssetCatalog m_catalog;
    ThumbnailCache m_thumbnails;
    std::filesystem::path m_settingsPath;
    AssetBrowserSettings m_settings;
    std::string m_pendingSettingsError;
    // Catalog changes and moved or removed files not yet dispatched.
    AssetCatalogResult m_pending{.success = true};
    std::vector<ProjectItemMove> m_itemMoves;
    uint64_t m_latestItemMove{0};
    float m_refreshElapsed{0.0f};
    uint64_t m_lastViewId{0};
    std::optional<uint64_t> m_primaryView;
    std::optional<std::filesystem::path> m_revealRequest;
    // Frames are counted by pollChanges; the primary records the last frame
    // it claimed (was shown) in.
    uint64_t m_frame{0};
    uint64_t m_primaryShownFrame{0};
    bool m_scanAttempted{false};
    bool m_initialized{false};
};

// Reloads a model through the asset manager and reports the outcome; the
// editor's ProjectAssetActions::reloadModel and the browser's Reload Model
// button both use it.
void reloadProjectModel(AssetManager& assets, EditorDiagnostics& diagnostics, const std::filesystem::path& model);

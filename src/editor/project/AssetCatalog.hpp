#pragma once

#include "editor/project/AssetRelocation.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/platform/FileWatcher.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class EditorAssetKind {
    Model,
    Texture,
    Audio,
    Scene,
    Prefab,
    // C and C++ source and headers: the game module's code.
    Script,
};

// Paths in records and folders are absolute and lexically normal, so they can
// be compared directly. relativePath is relative to the root that holds it.
struct EditorAssetRecord {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
    EditorAssetKind kind{};
    uintmax_t byteSize{0};
    std::filesystem::file_time_type lastWriteTime{};
    // The asset's identity in the project's asset database.
    AssetGuid guid;
};

// A directory under one of the catalog roots, including empty directories and
// the roots themselves (whose relativePath is empty).
struct EditorAssetFolder {
    std::filesystem::path relativePath;
    std::filesystem::path absolutePath;
};

enum class AssetCatalogChangeKind {
    Added,
    Modified,
    Removed,
    // The asset kept its GUID under a new path (a rename or move, made by the
    // editor or outside it).
    Moved,
};

struct AssetCatalogChange {
    AssetCatalogChangeKind kind{};
    // The current record; for Removed, the last known one.
    EditorAssetRecord asset;
    // Moved only: the absolute, normalized path before the move.
    std::filesystem::path previousPath;
};

struct AssetCatalogResult {
    bool success{false};
    std::string error;
    // Changes to supported assets only; files of other types are tracked by
    // the asset database but not listed.
    std::vector<AssetCatalogChange> changes;
    // Every tracked file that moved or disappeared, whatever its type, so
    // references to any asset can follow.
    std::vector<ProjectFileMove> movedFiles;
    std::vector<std::filesystem::path> removedFiles;
    // Recoverable problems from the asset databases and file watchers.
    std::vector<std::string> warnings;

    explicit operator bool() const {
        return success;
    }
};

struct AssetCatalogOptions {
    // Watches the roots so poll() reports changes as they happen. Without a
    // watcher, callers rescan with refresh() periodically.
    bool watchFiles{true};
    FileWatcherOptions watcher;
    // Seeds the asset databases' GUID generation for reproducible tests.
    std::optional<uint64_t> guidSeed;
    // The game's source folder, listed after the other roots for its scripts
    // and folders. It is watched but has no asset database, so no `.meta`
    // sidecars are written beside code and its records have no GUID; its
    // changes are not reported, only relisted. Empty for none; ignored when
    // it overlaps the asset or scene root (scripts inside those are listed
    // like any other asset).
    std::filesystem::path sourceRoot;
};

// The project's authored files: supported assets and every folder under the
// asset and scene roots. Each root's files are identified by an AssetDatabase
// (`<file>.meta` sidecars hold their GUIDs), and a FileWatcher per root tells
// poll() when to reconcile. A root inside the other root (either way round)
// shares the outer root's database and watcher.
class AssetCatalog {
  public:
    // Relative roots resolve against the current directory.
    AssetCatalog(std::filesystem::path assetRoot, std::filesystem::path sceneRoot, AssetCatalogOptions options = {});
    ~AssetCatalog();
    AssetCatalog(const AssetCatalog&) = delete;
    AssetCatalog& operator=(const AssetCatalog&) = delete;

    // Reconciles every root's database with the disk and relists folders.
    // Missing roots list as empty.
    AssetCatalogResult refresh();
    // Starts watchers for roots that exist, then rescans only the roots whose
    // watchers reported changes since the previous call. Returns no changes
    // when nothing happened.
    AssetCatalogResult poll();
    // Whether every existing root is watched. When false, callers should
    // refresh() periodically instead of relying on poll().
    bool watching() const;

    // Sorted by absolute path.
    const std::vector<EditorAssetRecord>& assets() const;
    // Every existing folder, sorted by absolute path; roots included.
    const std::vector<EditorAssetFolder>& folders() const;
    // The asset root first, then the scene root when it differs, then the
    // source root when there is one.
    const std::vector<std::filesystem::path>& roots() const;

    // Whether absolutePath (normalized) is the source root or lies inside it.
    // Only scripts are listed there.
    bool inSourceRoot(const std::filesystem::path& absolutePath) const;

    const EditorAssetFolder* findFolder(const std::filesystem::path& absolutePath) const;
    const EditorAssetRecord* findAsset(const std::filesystem::path& absolutePath) const;
    // Direct children of a folder, in path order.
    std::vector<const EditorAssetFolder*> subfolders(const std::filesystem::path& folder) const;
    std::vector<const EditorAssetRecord*> assetsInFolder(const std::filesystem::path& folder) const;

    // The asset root's database, shared so scene documents can resolve GUIDs.
    // It stays the same object for the catalog's lifetime.
    std::shared_ptr<const AssetDatabase> assetDatabase() const;
    // The database that tracks absolutePath, or null when no root with a
    // database contains it (outside the project, or in the source root).
    AssetDatabase* databaseFor(const std::filesystem::path& absolutePath);

    // Folds the outcome of an operation on a database returned by databaseFor
    // (a move, rename, or removal) into the listing and reports its changes.
    AssetCatalogResult applyDatabaseChanges(const AssetDatabase& database, const std::vector<AssetChange>& changes,
                                            std::vector<std::string> warnings);

  private:
    struct Root;

    AssetCatalogResult rescanRoot(Root& root);
    void appendChanges(const Root& root, const std::vector<AssetChange>& changes,
                       const std::vector<AssetRecord>& previousRecords, AssetCatalogResult& result) const;
    AssetCatalogResult relist();
    void startWatcher(Root& root, std::vector<std::string>& warnings);
    Root* rootOwning(const AssetDatabase& database);

    AssetCatalogOptions m_options;
    std::vector<std::filesystem::path> m_roots;
    // Roots that are scanned and watched: one per root not nested in another.
    // The source root has no database.
    std::vector<std::unique_ptr<Root>> m_trackedRoots;
    // Empty when there is no source root or it was ignored.
    std::filesystem::path m_sourceRoot;
    std::vector<EditorAssetRecord> m_assets;
    std::vector<EditorAssetFolder> m_folders;
};

// Absolute, lexically normal, and without a trailing separator, matching the
// paths the catalog reports. Falls back to a lexical result if the current
// directory cannot be read.
std::filesystem::path normalizedCatalogPath(const std::filesystem::path& path);

// The kind of asset a file is, from its extension (case-insensitive), or
// nullopt for unsupported files.
std::optional<EditorAssetKind> editorAssetKind(const std::filesystem::path& path);

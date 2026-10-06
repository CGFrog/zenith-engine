#pragma once

// Tracks every project file by a GUID in a .meta sidecar: scans, moves, external changes, dependencies.

#include "engine/assets/AssetGuid.hpp"
#include "engine/assets/AssetMeta.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Paths in this API are project-relative, UTF-8, and use forward slashes
// (`textures/stone.png`). Lookups are byte-exact, even on case-insensitive
// file systems.

enum class AssetChangeKind {
    Added,
    Removed,
    Modified,
    Moved,
};

struct AssetChange {
    AssetChangeKind kind{AssetChangeKind::Added};
    AssetGuid guid;
    // The current path, or the last known path for `Removed`.
    std::string path;
    // The path before the change; set only for `Moved`.
    std::string previousPath;
};

struct AssetDatabaseResult {
    bool success{false};
    std::string error;
    // Sorted by path, then kind. A moved asset whose content or import
    // settings also changed reports both `Moved` and `Modified`.
    std::vector<AssetChange> changes;
    // Recoverable problems: unreadable sidecars, duplicate GUIDs that were
    // regenerated, sidecars that could not be written.
    std::vector<std::string> warnings;
    // Sidecars whose asset no longer exists, as `<asset path>.meta`.
    std::vector<std::string> orphanMetaFiles;

    explicit operator bool() const {
        return success;
    }
};

enum class OrphanMetaPolicy {
    // Leave the sidecar in place. If its asset reappears at that path (an
    // undo, a version-control restore) it keeps its GUID.
    Keep,
    // Delete sidecars whose asset is gone.
    Delete,
};

struct AssetDatabaseOptions {
    // Exact file or directory names skipped anywhere under the root, for
    // example `build` or `Thumbs.db`. Names beginning with `.` and files
    // ending in `.tmp` are always skipped.
    std::vector<std::string> ignoredNames;
    // Extra file extensions (with the dot, case-insensitive) that are skipped.
    std::vector<std::string> ignoredExtensions;
    OrphanMetaPolicy orphanMetaPolicy{OrphanMetaPolicy::Keep};
    // Seeds GUID generation for reproducible tests; unset uses random_device.
    std::optional<uint64_t> guidSeed;
};

struct AssetRecord {
    AssetGuid guid;
    std::string path;
    nlohmann::json importSettings = nlohmann::json::object();
    uint64_t sizeBytes{0};
    std::filesystem::file_time_type lastWriteTime{};
};

// Tracks every regular file under a project root by a GUID stored in a
// `<file>.meta` sidecar beside it. Directories are not assets and have no
// sidecar; a directory's identity is the set of assets inside it.
//
// The database never touches the disk on its own: `rescan()` reconciles the
// in-memory state with the file system, creating missing sidecars, and the
// mutation methods change both together. Identity rules on rescan:
//
// - A file whose sidecar travelled with it keeps the sidecar's GUID, so moves
//   made outside the editor are reported as `Moved`.
// - When several files claim one GUID (a file copied with its sidecar), the
//   path that held the GUID before keeps it; with no previous holder the
//   lexicographically first path keeps it. Every other claimant gets a fresh
//   GUID, its sidecar is rewritten with the same import settings, and a
//   warning names it.
// - A tracked file whose sidecar was deleted gets its previous GUID back.
// - A new file without a sidecar adopts the GUID of a vanished asset only
//   when exactly one vanished asset and exactly one new file share the same
//   size and modification time (a move that left the sidecar behind).
// - A sidecar that cannot be parsed is never overwritten. If the path was
//   already tracked it keeps its previous identity; otherwise the file stays
//   untracked until the sidecar is fixed. Both cases add a warning.
//
// Iteration order is by path. The dependency graph is in-memory, supplied by
// importers, and keyed by GUID, so it survives moves; edges into an asset that
// is later removed are kept so callers can find broken references.
class AssetDatabase {
  public:
    explicit AssetDatabase(std::filesystem::path root, AssetDatabaseOptions options = {});

    const std::filesystem::path& root() const {
        return m_root;
    }

    // Reconciles with the file system and reports what changed since the
    // previous scan. The first scan reports every asset as `Added`.
    AssetDatabaseResult rescan();

    std::optional<AssetGuid> guidForPath(std::string_view path) const;
    std::optional<std::string> pathForGuid(AssetGuid guid) const;
    const AssetRecord* find(AssetGuid guid) const;
    std::vector<AssetRecord> records() const;

    size_t assetCount() const {
        return m_records.size();
    }

    // Rewrites the asset's sidecar; `settings` must be a JSON object.
    AssetDatabaseResult setImportSettings(AssetGuid guid, nlohmann::json settings);

    // Moves a tracked file and its sidecar, or a directory with everything in
    // it. The destination must not exist; missing parent directories are
    // created. `rename` keeps the parent directory and takes a bare file name.
    AssetDatabaseResult move(std::string_view fromPath, std::string_view toPath);
    AssetDatabaseResult rename(std::string_view path, std::string_view newName);

    // Permanently deletes a tracked file and its sidecar, or a directory and
    // everything in it.
    AssetDatabaseResult remove(std::string_view path);

    // Replaces the asset's outgoing edges. Null and self edges are ignored.
    void setDependencies(AssetGuid asset, std::span<const AssetGuid> dependencies);
    std::vector<AssetGuid> dependencies(AssetGuid asset) const;
    // Assets that list `asset` directly, sorted by GUID.
    std::vector<AssetGuid> dependents(AssetGuid asset) const;
    // Everything that depends on `asset` directly or indirectly, sorted by
    // GUID and excluding `asset` itself. Cycles are tolerated.
    std::vector<AssetGuid> transitiveDependents(AssetGuid asset) const;

  private:
    std::filesystem::path absolutePath(std::string_view relativePath) const;
    bool isTrackablePath(std::string_view relativePath) const;
    void forgetAsset(AssetGuid guid);
    void forgetStalePath(const std::string& path, AssetDatabaseResult& result);
    void clearDependencies(AssetGuid asset);
    AssetDatabaseResult moveFile(const std::string& fromPath, const std::string& toPath);
    AssetDatabaseResult moveDirectory(const std::string& fromPath, const std::string& toPath);

    std::filesystem::path m_root;
    AssetDatabaseOptions m_options;
    AssetGuidGenerator m_generator;
    std::unordered_map<AssetGuid, AssetRecord> m_records;
    std::map<std::string, AssetGuid> m_guidByPath;
    std::unordered_map<AssetGuid, std::set<AssetGuid>> m_dependencies;
    std::unordered_map<AssetGuid, std::set<AssetGuid>> m_dependents;
};

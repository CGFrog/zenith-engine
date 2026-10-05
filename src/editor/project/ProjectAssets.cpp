#include "ProjectAssets.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/scene/Scene.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace {
constexpr float refreshIntervalSeconds = 1.0f;
// Enough for every browser to catch up within a frame or two.
constexpr size_t retainedItemMoves = 64;
constexpr const char* diagnosticSource = "Asset Browser";

std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// True when path is base or lies inside it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    const std::filesystem::path relative = path.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

// Moves path from under oldBase to the same place under newBase.
std::filesystem::path rebase(const std::filesystem::path& path, const std::filesystem::path& oldBase,
                             const std::filesystem::path& newBase) {
    return normalizedCatalogPath(newBase / path.lexically_relative(oldBase));
}

void reportOutcome(EditorDiagnostics& diagnostics, bool success, const std::string& message) {
    if (success) {
        diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource, message);
    } else {
        diagnostics.reportError(message);
    }
}
} // namespace

ProjectAssets::ProjectAssets(std::filesystem::path assetRoot, std::filesystem::path sceneRoot,
                             std::filesystem::path settingsFile, AssetCatalogOptions catalogOptions)
    : m_catalog(std::move(assetRoot), std::move(sceneRoot), std::move(catalogOptions))
    , m_settingsPath(std::move(settingsFile)) {
    if (const AssetBrowserSettingsResult loaded = loadAssetBrowserSettings(m_settingsPath, m_settings); !loaded) {
        m_pendingSettingsError = loaded.error;
    }
}

void ProjectAssets::refresh(EditorDiagnostics& diagnostics) {
    absorb(m_catalog.refresh(), diagnostics);
}

void ProjectAssets::pollChanges(EditorDiagnostics& diagnostics, const ProjectAssetActions& actions,
                                float deltaSeconds) {
    ++m_frame;
    reportPendingSettingsError(diagnostics);
    m_refreshElapsed += deltaSeconds;
    if (!m_scanAttempted) {
        // The first call scans at once and starts the watchers on the next.
        m_scanAttempted = true;
        m_refreshElapsed = 0.0f;
        refresh(diagnostics);
    } else if (m_catalog.watching() || m_refreshElapsed < refreshIntervalSeconds) {
        // poll() also starts watchers for roots that have appeared.
        if (AssetCatalogResult result = m_catalog.poll(); !result.changes.empty() || !result.movedFiles.empty() ||
                                                          !result.removedFiles.empty() || !result.warnings.empty() ||
                                                          !result) {
            absorb(std::move(result), diagnostics);
        }
    } else {
        // Without a watcher, rescans wait for the interval, including after
        // a failed scan.
        m_refreshElapsed = 0.0f;
        refresh(diagnostics);
    }
    dispatchChanges(diagnostics, actions);
}

void ProjectAssets::absorb(AssetCatalogResult result, EditorDiagnostics& diagnostics) {
    for (std::string& warning : result.warnings) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, diagnosticSource, std::move(warning));
    }
    if (!result) {
        diagnostics.reportError(result.error);
        return;
    }
    for (const AssetCatalogChange& change : result.changes) {
        // Thumbnails are keyed by path.
        if (change.asset.kind == EditorAssetKind::Texture) {
            m_thumbnails.invalidate(change.asset.absolutePath);
            if (!change.previousPath.empty()) {
                m_thumbnails.invalidate(change.previousPath);
            }
        }
    }
    // The first scan lists the project; only later changes are acted on.
    if (m_initialized) {
        std::ranges::move(result.changes, std::back_inserter(m_pending.changes));
        std::ranges::move(result.movedFiles, std::back_inserter(m_pending.movedFiles));
        std::ranges::move(result.removedFiles, std::back_inserter(m_pending.removedFiles));
    }
    m_initialized = true;
}

void ProjectAssets::dispatchChanges(EditorDiagnostics& diagnostics, const ProjectAssetActions& actions) {
    AssetCatalogResult pending = std::exchange(m_pending, AssetCatalogResult{.success = true});
    if (!pending.movedFiles.empty() && actions.filesMoved) {
        actions.filesMoved(pending.movedFiles);
    }
    if (!pending.removedFiles.empty() && actions.filesRemoved) {
        actions.filesRemoved(pending.removedFiles);
    }
    for (const AssetCatalogChange& change : pending.changes) {
        if (change.kind != AssetCatalogChangeKind::Modified) {
            continue;
        }
        const std::string name = utf8PathText(change.asset.absolutePath.filename());
        if (change.asset.kind == EditorAssetKind::Model && actions.reloadModel) {
            actions.reloadModel(change.asset.absolutePath);
        } else if (change.asset.kind == EditorAssetKind::Material && actions.reloadMaterial) {
            actions.reloadMaterial(change.asset.absolutePath);
        } else if (change.asset.kind == EditorAssetKind::Prefab && actions.prefabChanged) {
            actions.prefabChanged(change.asset.absolutePath);
        } else if (change.asset.kind == EditorAssetKind::Texture) {
            // AssetManager has no texture reload; textures load fresh when a
            // scene using them is opened again.
            diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource,
                            "Texture '" + name + "' changed on disk. Reopen scenes that use it to see the change.");
        }
    }
}

bool ProjectAssets::scanned() const {
    return m_initialized;
}

const AssetCatalog& ProjectAssets::catalog() const {
    return m_catalog;
}

std::shared_ptr<const AssetDatabase> ProjectAssets::assetDatabase() const {
    return m_catalog.assetDatabase();
}

ThumbnailCache& ProjectAssets::thumbnails() {
    return m_thumbnails;
}

UiTextureHandle ProjectAssets::thumbnailTexture(RenderRuntime& rendering, const std::filesystem::path& image) {
    return m_thumbnailTextures.texture(rendering, image);
}

void ProjectAssets::releaseThumbnailTextures(RenderRuntime& rendering) {
    m_thumbnailTextures.release(rendering);
}

bool ProjectAssets::isRoot(const std::filesystem::path& path) const {
    return std::ranges::find(m_catalog.roots(), path) != m_catalog.roots().end();
}

std::string ProjectAssets::folderKey(const std::filesystem::path& folder) const {
    const std::filesystem::path relative = folder.lexically_relative(m_catalog.roots().front());
    return genericUtf8(relative.empty() ? folder : relative);
}

std::filesystem::path ProjectAssets::resolveFolderKey(const std::string& key) const {
    return normalizedCatalogPath(m_catalog.roots().front() / pathFromUtf8Text(key));
}

const AssetBrowserSettings& ProjectAssets::settings() const {
    return m_settings;
}

bool ProjectAssets::isFavourite(const std::filesystem::path& folder) const {
    return std::ranges::find(m_settings.favourites, folderKey(normalizedCatalogPath(folder))) !=
           m_settings.favourites.end();
}

void ProjectAssets::toggleFavourite(const std::filesystem::path& folder, EditorDiagnostics& diagnostics) {
    const std::string key = folderKey(normalizedCatalogPath(folder));
    if (const auto found = std::ranges::find(m_settings.favourites, key); found != m_settings.favourites.end()) {
        m_settings.favourites.erase(found);
    } else {
        m_settings.favourites.push_back(key);
    }
    saveSettings(diagnostics);
}

void ProjectAssets::rememberView(const std::filesystem::path& folder, AssetBrowserViewMode mode,
                                 EditorDiagnostics& diagnostics) {
    const std::string key = folderKey(normalizedCatalogPath(folder));
    if (key == m_settings.lastFolder && mode == m_settings.viewMode) {
        return;
    }
    m_settings.lastFolder = key;
    m_settings.viewMode = mode;
    saveSettings(diagnostics);
}

void ProjectAssets::reportPendingSettingsError(EditorDiagnostics& diagnostics) {
    if (!m_pendingSettingsError.empty()) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, diagnosticSource, std::move(m_pendingSettingsError));
        m_pendingSettingsError.clear();
    }
}

uint64_t ProjectAssets::registerView() {
    return ++m_lastViewId;
}

bool ProjectAssets::claimPrimaryView(uint64_t view) {
    // A primary not shown this frame or the last one (in another workspace
    // or a background tab) is replaced; the grace frame keeps the role from
    // moving to a view that merely draws before the primary.
    const bool primaryAbsent = m_primaryShownFrame + 1 < m_frame;
    if (!m_primaryView || (*m_primaryView != view && primaryAbsent)) {
        m_primaryView = view;
    }
    if (*m_primaryView != view) {
        return false;
    }
    m_primaryShownFrame = m_frame;
    return true;
}

void ProjectAssets::releasePrimaryView(uint64_t view) {
    if (m_primaryView == view) {
        m_primaryView.reset();
    }
}

bool ProjectAssets::isPrimaryView(uint64_t view) const {
    return m_primaryView == view;
}

bool ProjectAssets::hasPrimaryView() const {
    return m_primaryView.has_value();
}

void ProjectAssets::requestReveal(std::filesystem::path item) {
    m_revealRequest = std::move(item);
}

std::optional<std::filesystem::path> ProjectAssets::takeRevealRequest(uint64_t view) {
    if (!isPrimaryView(view) || !m_revealRequest) {
        return std::nullopt;
    }
    return std::exchange(m_revealRequest, std::nullopt);
}

void ProjectAssets::saveSettings(EditorDiagnostics& diagnostics) {
    if (const AssetBrowserSettingsResult saved = saveAssetBrowserSettings(m_settingsPath, m_settings); !saved) {
        diagnostics.reportError(saved.error);
    }
}

std::optional<std::filesystem::path> ProjectAssets::createFolder(const std::filesystem::path& parent,
                                                                 EditorDiagnostics& diagnostics) {
    const ProjectFileResult result = createProjectFolder(parent);
    reportOutcome(diagnostics, result.success,
                  result ? "Created folder '" + utf8PathText(result.path) + "'." : result.error);
    if (!result) {
        return std::nullopt;
    }
    refresh(diagnostics);
    return normalizedCatalogPath(result.path);
}

std::optional<std::filesystem::path> ProjectAssets::createScene(const std::filesystem::path& folder,
                                                                EditorDiagnostics& diagnostics) {
    if (m_catalog.inSourceRoot(normalizedCatalogPath(folder))) {
        diagnostics.reportError("Scenes cannot be created in the source folder; use the asset or scene folders.");
        return std::nullopt;
    }
    const ProjectFileResult result = createProjectScene(folder, m_catalog.roots().front());
    reportOutcome(diagnostics, result.success,
                  result ? "Created scene '" + utf8PathText(result.path) + "'." : result.error);
    if (!result) {
        return std::nullopt;
    }
    refresh(diagnostics);
    return normalizedCatalogPath(result.path);
}

std::optional<std::filesystem::path> ProjectAssets::createMaterial(const std::filesystem::path& folder,
                                                                   EditorDiagnostics& diagnostics) {
    if (m_catalog.inSourceRoot(normalizedCatalogPath(folder))) {
        diagnostics.reportError("Materials cannot be created in the source folder; use the asset folders.");
        return std::nullopt;
    }
    const ProjectFileResult result = createProjectMaterial(folder);
    reportOutcome(diagnostics, result.success,
                  result ? "Created material '" + utf8PathText(result.path) + "'." : result.error);
    if (!result) {
        return std::nullopt;
    }
    refresh(diagnostics);
    return normalizedCatalogPath(result.path);
}

std::optional<std::filesystem::path> ProjectAssets::renameItem(const std::filesystem::path& item,
                                                               const std::string& utf8NewName,
                                                               EditorDiagnostics& diagnostics) {
    const std::filesystem::path source = normalizedCatalogPath(item);
    if (isRoot(source)) {
        diagnostics.reportError("Project roots cannot be renamed from the asset browser.");
        return std::nullopt;
    }
    AssetDatabase* database = m_catalog.databaseFor(source);
    ProjectFileResult result = renameProjectItem(source, utf8NewName, database);
    if (!result) {
        diagnostics.reportError(result.error);
        return std::nullopt;
    }
    const std::filesystem::path destination = normalizedCatalogPath(result.path);
    return finishRelocation(source, database, std::move(result),
                            "Renamed '" + utf8PathText(source) + "' to '" + utf8PathText(destination.filename()) + "'.",
                            diagnostics);
}

std::optional<std::filesystem::path> ProjectAssets::moveItem(const std::filesystem::path& item,
                                                             const std::filesystem::path& folder,
                                                             EditorDiagnostics& diagnostics) {
    const std::filesystem::path source = normalizedCatalogPath(item);
    const std::filesystem::path target = normalizedCatalogPath(folder);
    if (isRoot(source)) {
        diagnostics.reportError("Project roots cannot be moved from the asset browser.");
        return std::nullopt;
    }
    AssetDatabase* database = m_catalog.databaseFor(source);
    if (database != m_catalog.databaseFor(target)) {
        diagnostics.reportError("Cannot move '" + utf8PathText(source) +
                                "': items cannot move between the asset, scene, and source roots.");
        return std::nullopt;
    }
    ProjectFileResult result = moveProjectItem(source, target, database);
    if (!result) {
        diagnostics.reportError(result.error);
        return std::nullopt;
    }
    return finishRelocation(source, database, std::move(result),
                            "Moved '" + utf8PathText(source) + "' to '" + utf8PathText(target) + "'.", diagnostics);
}

// Shared tail of a successful rename or move: reports it, folds the database's
// changes into the catalog (queuing them for dispatch), records the move for
// views to follow, and rescans.
std::optional<std::filesystem::path> ProjectAssets::finishRelocation(const std::filesystem::path& source,
                                                                     AssetDatabase* database, ProjectFileResult result,
                                                                     const std::string& message,
                                                                     EditorDiagnostics& diagnostics) {
    const std::filesystem::path destination = normalizedCatalogPath(result.path);
    if (destination == source) {
        return destination;
    }
    reportOutcome(diagnostics, true, message);
    AssetCatalogResult applied =
        database != nullptr ? m_catalog.applyDatabaseChanges(*database, result.assetChanges, std::move(result.warnings))
                            : AssetCatalogResult{.success = true};
    recordItemMove(source, destination, diagnostics);
    absorb(std::move(applied), diagnostics);
    refresh(diagnostics);
    return destination;
}

// Carries the service's own path-keyed state (thumbnails, favourites, and the
// remembered folder) from a renamed or moved item to its new path, and logs
// the move for views.
void ProjectAssets::recordItemMove(const std::filesystem::path& source, const std::filesystem::path& destination,
                                   EditorDiagnostics& diagnostics) {
    m_thumbnails.invalidate(source);
    bool settingsChanged = false;
    for (std::string& favourite : m_settings.favourites) {
        if (const std::filesystem::path folder = resolveFolderKey(favourite); isWithin(folder, source)) {
            favourite = folderKey(rebase(folder, source, destination));
            settingsChanged = true;
        }
    }
    if (!m_settings.lastFolder.empty()) {
        if (const std::filesystem::path folder = resolveFolderKey(m_settings.lastFolder); isWithin(folder, source)) {
            m_settings.lastFolder = folderKey(rebase(folder, source, destination));
            settingsChanged = true;
        }
    }
    if (settingsChanged) {
        saveSettings(diagnostics);
    }
    m_itemMoves.push_back({++m_latestItemMove, source, destination});
    if (m_itemMoves.size() > retainedItemMoves) {
        m_itemMoves.erase(m_itemMoves.begin());
    }
}

std::optional<std::filesystem::path> ProjectAssets::duplicateItem(const std::filesystem::path& item,
                                                                  EditorDiagnostics& diagnostics) {
    const std::filesystem::path source = normalizedCatalogPath(item);
    if (isRoot(source)) {
        diagnostics.reportError("Project roots cannot be duplicated from the asset browser.");
        return std::nullopt;
    }
    const ProjectFileResult result = duplicateProjectItem(source);
    reportOutcome(diagnostics, result.success,
                  result
                      ? "Duplicated '" + utf8PathText(source) + "' as '" + utf8PathText(result.path.filename()) + "'."
                      : result.error);
    for (const std::string& warning : result.warnings) {
        diagnostics.add(EditorDiagnosticSeverity::Warning, diagnosticSource, warning);
    }
    if (!result) {
        return std::nullopt;
    }
    refresh(diagnostics);
    return normalizedCatalogPath(result.path);
}

bool ProjectAssets::moveItemToTrash(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    const std::filesystem::path target = normalizedCatalogPath(item);
    if (isRoot(target)) {
        diagnostics.reportError("Project roots cannot be deleted from the asset browser.");
        return false;
    }
    AssetDatabase* database = m_catalog.databaseFor(target);
    ProjectFileResult result = moveProjectItemToTrash(target, database);
    reportOutcome(diagnostics, result.success,
                  result ? "Moved '" + utf8PathText(target) + "' to the trash." : result.error);
    if (!result) {
        return false;
    }
    if (database != nullptr) {
        absorb(m_catalog.applyDatabaseChanges(*database, result.assetChanges, std::move(result.warnings)), diagnostics);
    }
    m_thumbnails.invalidate(target);
    const auto removed = std::erase_if(m_settings.favourites, [this, &target](const std::string& favourite) {
        return isWithin(resolveFolderKey(favourite), target);
    });
    if (removed > 0) {
        saveSettings(diagnostics);
    }
    refresh(diagnostics);
    return true;
}

void ProjectAssets::revealItem(const std::filesystem::path& item, EditorDiagnostics& diagnostics) {
    if (const ProjectFileResult result = revealInFileBrowser(item); !result) {
        diagnostics.reportError(result.error);
    }
}

std::span<const ProjectItemMove> ProjectAssets::recentItemMoves() const {
    return m_itemMoves;
}

uint64_t ProjectAssets::latestItemMoveSequence() const {
    return m_latestItemMove;
}

void reloadProjectModel(AssetManager& assets, EditorDiagnostics& diagnostics, const std::filesystem::path& model) {
    const std::string name = utf8PathText(model.filename());
    const AssetLoadResult<ModelHandle> result = assets.reloadModel(model);
    if (!result) {
        diagnostics.reportError("Model reload of '" + name + "' failed: " + result.error);
        return;
    }
    diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource,
                    "Reloaded model '" + name +
                        "'. Existing instances keep their previous resources; new placements and reopened scenes "
                        "use the refreshed model.");
}

size_t reloadProjectMaterial(AssetManager& assets, std::span<Scene* const> scenes, EditorDiagnostics& diagnostics,
                             const std::filesystem::path& material) {
    const std::string name = utf8PathText(material.filename());
    const AssetLoadResult<MaterialHandle> result = assets.reloadMaterial(material);
    if (!result) {
        diagnostics.reportError("Material reload of '" + name + "' failed: " + result.error);
        return 0;
    }
    // The reloaded material's source names the file as the asset manager
    // normalized it, which identifies the users of earlier loads.
    const std::filesystem::path source = assets.materialSource(result.handle)->path;
    size_t repointed = 0;
    for (Scene* scene : scenes) {
        if (scene == nullptr) {
            continue;
        }
        auto users = scene->registry().view<MaterialOverrideComponent>();
        for (const Entity entity : users) {
            MaterialOverrideComponent& user = users.get<MaterialOverrideComponent>(entity);
            const std::optional<MaterialAssetSource> used = assets.materialSource(user.material);
            if (user.material == result.handle || !used || used->path != source) {
                continue;
            }
            user.material = result.handle;
            scene->markMaterialDirty(entity);
            ++repointed;
        }
    }
    diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource, "Reloaded material '" + name + "'.");
    return repointed;
}
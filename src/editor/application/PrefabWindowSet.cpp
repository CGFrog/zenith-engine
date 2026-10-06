#include "PrefabWindowSet.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PrefabEditTargets.hpp"
#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/application/SceneEditing.hpp"
#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace {
std::filesystem::path normalReference(const std::filesystem::path& reference) {
    return std::filesystem::path(reference.lexically_normal().generic_string());
}

SceneEditing windowEditing(PrefabWindowDocument& window, AssetManager& assets, EditorDiagnostics& diagnostics) {
    return {window.scene, assets, window.document, window.history, diagnostics};
}

void addDiagnostics(PrefabResult& into, const PrefabResult& from) {
    into.diagnostics.insert(into.diagnostics.end(), from.diagnostics.begin(), from.diagnostics.end());
}
} // namespace

PrefabWindowDocument* PrefabWindowSet::find(const std::filesystem::path& reference) const {
    const std::filesystem::path wanted = normalReference(reference);
    const auto found = std::ranges::find_if(m_windows, [&wanted](const std::unique_ptr<PrefabWindowDocument>& window) {
        return window->target.reference == wanted;
    });
    return found == m_windows.end() ? nullptr : found->get();
}

PrefabWindowDocument* PrefabWindowSet::find(uint64_t id) const {
    const auto found = std::ranges::find_if(
        m_windows, [id](const std::unique_ptr<PrefabWindowDocument>& window) { return window->id == id; });
    return found == m_windows.end() ? nullptr : found->get();
}

PrefabWindowDocument* PrefabWindowSet::owner(const EditorDocument& document) const {
    const auto found =
        std::ranges::find_if(m_windows, [&document](const std::unique_ptr<PrefabWindowDocument>& window) {
            return &window->document == &document;
        });
    return found == m_windows.end() ? nullptr : found->get();
}

PrefabWindowDocument* PrefabWindowSet::owner(const Scene& scene) const {
    const auto found = std::ranges::find_if(
        m_windows, [&scene](const std::unique_ptr<PrefabWindowDocument>& window) { return &window->scene == &scene; });
    return found == m_windows.end() ? nullptr : found->get();
}

PrefabResult PrefabWindowSet::open(const std::filesystem::path& reference, const EditorDocument& level,
                                   AssetManager& assets, PrefabWindowDocument*& window) {
    const std::filesystem::path wanted = normalReference(reference);
    if (PrefabWindowDocument* existing = find(wanted)) {
        window = existing;
        return {.success = true};
    }
    PrefabSystem prefabs(assets, level.options());
    std::filesystem::path file;
    if (PrefabResult resolved = prefabs.resolve(wanted, file); !resolved) {
        return resolved;
    }
    Scene opened;
    OpenedPrefabDocument document;
    PrefabResult result = openPrefabDocument(wanted, opened, prefabs, document);
    if (!result) {
        return result;
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    window = &add(opened,
                  {.reference = wanted,
                   .file = (error ? file : absolute).lexically_normal(),
                   .displayName = std::move(document.displayName),
                   .root = document.root},
                  level);
    return result;
}

PrefabWindowDocument& PrefabWindowSet::adopt(Scene& prefabScene, PrefabEditTarget target, const EditorDocument& level) {
    PrefabWindowDocument& window = add(prefabScene, std::move(target), level);
    window.document.markDirty();
    return window;
}

PrefabWindowDocument& PrefabWindowSet::add(Scene& prefabScene, PrefabEditTarget target, const EditorDocument& level) {
    // The level's options (asset root, database, component types), as an
    // untitled, clean document holding the prefab.
    auto window = std::make_unique<PrefabWindowDocument>(m_nextId++, level);
    window->document.createNew(window->scene);
    window->scene.swap(prefabScene);
    if (window->scene.findEntity(target.root) != entt::null) {
        window->selection.selectOnly(target.root);
    }
    target.reference = normalReference(target.reference);
    window->target = std::move(target);
    m_windows.push_back(std::move(window));
    return *m_windows.back();
}

bool PrefabWindowSet::close(uint64_t id) {
    const auto found = std::ranges::find_if(
        m_windows, [id](const std::unique_ptr<PrefabWindowDocument>& window) { return window->id == id; });
    if (found == m_windows.end()) {
        return false;
    }
    m_windows.erase(found);
    return true;
}

void PrefabWindowSet::closeAll() {
    m_windows.clear();
}

PrefabResult PrefabWindowSet::save(PrefabWindowDocument& window, AssetManager& assets, EditorDiagnostics& diagnostics,
                                   const PrefabWriteHooks& hooks, SavedPrefabDocument& saved) {
    return saveOpenPrefab(window.binding(), window.target, assets, diagnostics, hooks, saved);
}

PrefabResult PrefabWindowSet::refreshInstances(AssetManager& assets, EditorDiagnostics& diagnostics,
                                               const std::filesystem::path& reference, const PrefabDocument* previous,
                                               const std::string& stepName, const PrefabWindowDocument* except,
                                               size_t& refreshed) {
    refreshed = 0;
    const std::filesystem::path changed = normalReference(reference);
    PrefabResult combined{.success = true};
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows) {
        // The window editing this prefab holds the prefab itself; its
        // content is the user's to save or discard.
        if (window->target.reference == changed || window.get() == except) {
            continue;
        }
        size_t count = 0;
        const PrefabResult result =
            refreshPrefabInstances(windowEditing(*window, assets, diagnostics), changed, previous, stepName, &count);
        addDiagnostics(combined, result);
        if (!result) {
            combined.success = false;
            combined.error = result.error;
        }
        window->selection.removeMissing(window->scene);
        refreshed += count;
    }
    return combined;
}

void PrefabWindowSet::filesMoved(std::span<const ProjectFileMove> moves, const std::filesystem::path& assetRoot,
                                 AssetManager& assets, EditorDiagnostics& diagnostics) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows) {
        followMovedPrefab(window->target, assetRoot, moves, diagnostics);
        size_t relocated = 0;
        if (const SceneDocumentResult result =
                relocateSceneAssets(window->scene, assets, window->document.options(), moves, relocated);
            !result) {
            diagnostics.reportError("Could not follow moved assets in the prefab " + window->target.displayName + ": " +
                                    result.error);
        } else if (relocated > 0) {
            window->document.markDirty();
        }
    }
}

void PrefabWindowSet::filesRemoved(std::span<const std::filesystem::path> removed, EditorDiagnostics& diagnostics) {
    for (const std::unique_ptr<PrefabWindowDocument>& window : m_windows) {
        warnRemovedPrefab(window->target, removed, diagnostics);
    }
}

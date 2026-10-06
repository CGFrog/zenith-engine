#include "DocumentStack.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/PrefabEditTargets.hpp"
#include "editor/application/PrefabInstanceRefresh.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/document/PrefabEditDocument.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <utility>

namespace {
std::filesystem::path normalReference(const std::filesystem::path& reference) {
    return std::filesystem::path(reference.lexically_normal().generic_string());
}

// Exchanges the active document's contents with parked's.
void exchange(DocumentBinding active, ParkedDocument& parked) {
    active.scene.swap(parked.scene);
    std::swap(active.document, parked.document);
    active.history.exchangeTimeline(parked.history);
    std::swap(active.selection, parked.selection);
}

SceneEditing parkedEditing(ParkedDocument& parked, AssetManager& assets, EditorDiagnostics& diagnostics) {
    return {parked.scene, assets, parked.document, parked.history, diagnostics};
}

void addDiagnostics(PrefabResult& into, const PrefabResult& from) {
    into.diagnostics.insert(into.diagnostics.end(), from.diagnostics.begin(), from.diagnostics.end());
}

std::string refreshStepName(const std::string& displayName) {
    return "Refresh " + displayName + " instances";
}
} // namespace

const PrefabEditTarget* DocumentStack::activePrefab() const {
    return m_frames.empty() ? nullptr : &m_frames.back()->prefab;
}

const ParkedDocument* DocumentStack::parkedScene() const {
    return m_frames.empty() ? nullptr : &m_frames.front()->parent;
}

ParkedDocument* DocumentStack::parkedScene() {
    return m_frames.empty() ? nullptr : &m_frames.front()->parent;
}

ParkedDocument* DocumentStack::top() {
    return m_frames.empty() ? nullptr : &m_frames.back()->parent;
}

bool DocumentStack::contains(const std::filesystem::path& reference) const {
    const std::filesystem::path wanted = normalReference(reference);
    return std::ranges::any_of(
        m_frames, [&wanted](const std::unique_ptr<Frame>& frame) { return frame->prefab.reference == wanted; });
}

PrefabResult DocumentStack::push(DocumentBinding active, AssetManager& assets, const std::filesystem::path& reference) {
    const std::filesystem::path wanted = normalReference(reference);
    if (contains(wanted)) {
        return {.error = wanted.generic_string() + " is already open; go back to it with the breadcrumbs"};
    }
    PrefabSystem prefabs(assets, active.document.options());
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
    std::filesystem::path absolute = std::filesystem::absolute(file, error);
    pushRecovered(active, opened,
                  {.reference = wanted,
                   .file = (error ? file : absolute).lexically_normal(),
                   .displayName = std::move(document.displayName),
                   .root = document.root});
    return result;
}

void DocumentStack::pushRecovered(DocumentBinding active, Scene& prefabScene, PrefabEditTarget target) {
    auto frame = std::make_unique<Frame>(active.document);
    exchange(active, frame->parent);
    // The active document is now a copy of the parked one's state (same
    // options); it becomes an untitled, clean document holding the prefab.
    active.document.createNew(active.scene);
    active.scene.swap(prefabScene);
    if (active.scene.findEntity(target.root) != entt::null) {
        active.selection.selectOnly(target.root);
    }
    target.reference = normalReference(target.reference);
    frame->prefab = std::move(target);
    m_frames.push_back(std::move(frame));
}

PrefabResult DocumentStack::save(DocumentBinding active, AssetManager& assets, EditorDiagnostics& diagnostics,
                                 const PrefabWriteHooks& hooks) {
    if (m_frames.empty()) {
        return {.error = "No prefab is open"};
    }
    SavedPrefabDocument saved;
    return saveOpenPrefab(active, m_frames.back()->prefab, assets, diagnostics, hooks, saved);
}

DocumentStackPopResult DocumentStack::pop(DocumentBinding active, AssetManager& assets, EditorDiagnostics& diagnostics,
                                          const PrefabWriteHooks& hooks) {
    DocumentStackPopResult popped;
    if (m_frames.empty()) {
        return popped;
    }
    std::unique_ptr<Frame> frame = std::move(m_frames.back());
    m_frames.pop_back();
    exchange(active, frame->parent);
    popped.views = std::move(frame->parent.views);
    const PrefabEditTarget& prefab = frame->prefab;
    if (!prefab.savedThisVisit || prefab.before == prefab.saved) {
        return popped;
    }

    PrefabDocument previousDocument;
    const PrefabDocument* previous =
        prefab.before && deserializePrefab(*prefab.before, previousDocument).success ? &previousDocument : nullptr;
    std::optional<std::string> current;
    std::string readError;
    const bool unchangedSinceSave = readPrefabFileBytes(prefab.file, current, readError) && current == prefab.saved;
    const SceneEditing editing(active.scene, assets, active.document, active.history, diagnostics);
    if (unchangedSinceSave) {
        std::string before;
        if (!editing.capture(before)) {
            popped.refresh = {.error = "Could not record the edit of " + prefab.displayName};
            return popped;
        }
        PrefabAuthoring authoring(assets, active.document.options());
        addDiagnostics(popped.refresh, authoring.refreshInstances(active.scene, prefab.reference, previous));
        std::string after;
        if (!editing.capture(after)) {
            popped.refresh = {.error = "Could not record the edit of " + prefab.displayName};
            return popped;
        }
        const PrefabFileWrite write{.file = prefab.file, .before = prefab.before, .after = prefab.saved};
        editing.commit(
            "Edit " + prefab.displayName, before, after, [write, hooks](std::string name, SceneChanges changes) {
                return std::make_unique<PrefabWriteCommand>(std::move(name), write, std::move(changes), hooks);
            });
    } else {
        // Someone else wrote the file after the last save: undo must not put
        // back the old content over theirs, so only the instances follow.
        addDiagnostics(popped.refresh, refreshPrefabInstances(editing, prefab.reference, previous,
                                                              refreshStepName(prefab.displayName)));
    }
    active.selection.removeMissing(active.scene);
    for (const std::unique_ptr<Frame>& below : m_frames) {
        addDiagnostics(popped.refresh,
                       refreshPrefabInstances(parkedEditing(below->parent, assets, diagnostics), prefab.reference,
                                              previous, refreshStepName(prefab.displayName)));
        below->parent.selection.removeMissing(below->parent.scene);
    }
    return popped;
}

std::vector<std::pair<ViewportId, ViewportPanel::DocumentView>> DocumentStack::unwind(DocumentBinding active) {
    if (m_frames.empty()) {
        return {};
    }
    std::unique_ptr<Frame> scene = std::move(m_frames.front());
    // Prefab scenes may hold module storage: drop them before the scene's.
    m_frames.clear();
    exchange(active, scene->parent);
    return std::move(scene->parent.views);
}

PrefabResult DocumentStack::refreshParked(AssetManager& assets, EditorDiagnostics& diagnostics,
                                          const std::filesystem::path& reference, const PrefabDocument* previous,
                                          const std::string& stepName, size_t& refreshed) {
    refreshed = 0;
    const std::filesystem::path changed = normalReference(reference);
    PrefabResult combined{.success = true};
    for (size_t index = 0; index < m_frames.size(); ++index) {
        // A parked prefab document's own root is the prefab itself, not an
        // instance to refresh; the user decides by saving or discarding it.
        if (index > 0 && m_frames[index - 1]->prefab.reference == changed) {
            continue;
        }
        ParkedDocument& parked = m_frames[index]->parent;
        size_t count = 0;
        const PrefabResult result =
            refreshPrefabInstances(parkedEditing(parked, assets, diagnostics), changed, previous, stepName, &count);
        addDiagnostics(combined, result);
        if (!result) {
            combined.success = false;
            combined.error = result.error;
        }
        parked.selection.removeMissing(parked.scene);
        refreshed += count;
    }
    return combined;
}

void DocumentStack::filesMoved(std::span<const ProjectFileMove> moves, AssetManager& assets,
                               EditorDiagnostics& diagnostics) {
    for (const std::unique_ptr<Frame>& frame : m_frames) {
        followMovedPrefab(frame->prefab, frame->parent.document.options().assetRoot, moves, diagnostics);
        ParkedDocument& parked = frame->parent;
        if (parked.document.path()) {
            if (std::optional<std::filesystem::path> moved =
                    relocatedProjectPath(normalizedCatalogPath(*parked.document.path()), moves)) {
                parked.document.relocate(std::move(*moved));
            }
        }
        size_t relocated = 0;
        if (const SceneDocumentResult result =
                relocateSceneAssets(parked.scene, assets, parked.document.options(), moves, relocated);
            !result) {
            diagnostics.reportError("Could not follow moved assets in a parked document: " + result.error);
        } else if (relocated > 0) {
            parked.document.markDirty();
        }
    }
}

void DocumentStack::filesRemoved(std::span<const std::filesystem::path> removed, EditorDiagnostics& diagnostics) {
    for (const std::unique_ptr<Frame>& frame : m_frames) {
        warnRemovedPrefab(frame->prefab, removed, diagnostics);
        ParkedDocument& parked = frame->parent;
        if (parked.document.path() && isRemovedProjectPath(normalizedCatalogPath(*parked.document.path()), removed)) {
            diagnostics.add(EditorDiagnosticSeverity::Warning, "Editor",
                            "The parked scene's file was removed; save it to keep the scene.");
            parked.document.detach();
        }
    }
}

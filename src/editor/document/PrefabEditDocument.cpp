#include "PrefabEditDocument.hpp"

#include "editor/document/PrefabAuthoring.hpp"

#include <utility>

PrefabResult openPrefabDocument(const std::filesystem::path& reference, Scene& scene, PrefabSystem& prefabs,
                                OpenedPrefabDocument& output) {
    if (!scene.registry().view<const SceneEntityIdComponent>().empty()) {
        return {.error = "A prefab opens into an empty scene"};
    }
    std::filesystem::path file;
    if (PrefabResult resolved = prefabs.resolve(reference, file); !resolved) {
        return resolved;
    }
    std::optional<std::string> bytes;
    std::string error;
    if (!readPrefabFileBytes(file, bytes, error)) {
        return {.error = error};
    }
    if (!bytes) {
        return {.error = "Could not open prefab: " + reference.generic_string() + " does not exist"};
    }
    PrefabDocument document;
    if (PrefabResult parsed = deserializePrefab(*bytes, document); !parsed) {
        parsed.error = file.filename().generic_string() + ": " + parsed.error;
        return parsed;
    }
    // The scene must show the bytes returned, whatever the cache last read.
    prefabs.invalidate(reference);
    PrefabInstanceResult instance = prefabs.instantiate(reference, scene);
    if (!instance) {
        return {.error = std::move(instance.error), .diagnostics = std::move(instance.diagnostics)};
    }
    output = {.root = scene.id(instance.root),
              .fileBytes = std::move(*bytes),
              .displayName =
                  document.displayName.empty() ? file.stem().generic_string() : std::move(document.displayName)};
    return {.success = true, .diagnostics = std::move(instance.diagnostics)};
}

PrefabResult savePrefabDocument(Scene& scene, SceneEntityId root, const std::filesystem::path& reference,
                                PrefabSystem& prefabs, SavedPrefabDocument& output, const PrefabWriteHooks& hooks) {
    const Entity rootEntity = scene.findEntity(root);
    if (rootEntity == entt::null) {
        return {.error = "The prefab's root is no longer in the scene"};
    }
    std::filesystem::path file;
    if (PrefabResult resolved = prefabs.resolve(reference, file); !resolved) {
        return resolved;
    }
    SavedPrefabDocument saved;
    std::string error;
    if (!readPrefabFileBytes(file, saved.before, error)) {
        return {.error = error};
    }
    PrefabCapture capture;
    PrefabResult result =
        prefabs.capture(scene, rootEntity, reference, capture, PrefabCaptureOptions{.keepRootTransform = true});
    if (!result) {
        return result;
    }
    if (PrefabResult written = savePrefab(file, capture.document); !written) {
        return written;
    }
    prefabs.invalidate(reference);
    std::optional<std::string> after;
    if (!readPrefabFileBytes(file, after, error) || !after) {
        return {.error = error.empty() ? "The saved prefab disappeared: " + reference.generic_string() : error};
    }
    saved.after = std::move(*after);
    hooks.notify(file, saved.after);
    linkPrefabCapture(scene, rootEntity, reference, capture);
    output = std::move(saved);
    return result;
}

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EditorShortcuts.hpp"
#include "editor/document/EditorRecovery.hpp"
#include "editor/document/SceneFragment.hpp"
#include "engine/assets/Assets.hpp"

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_hardening_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;

    Scene scene;
    const Entity root = scene.createEntity("Root");
    const Entity child = scene.createEntity("Child");
    scene.setParent(child, root);
    std::string fragment;
    const SceneEntityId rootId = scene.id(root);
    expect(SceneFragment::copy(scene, assets, options, std::span(&rootId, 1), fragment).success,
           "a hierarchy should serialize to a fragment");
    std::vector<SceneEntityId> pasted;
    expect(SceneFragment::paste(fragment, scene, assets, options, entt::null, pasted).success,
           "a fragment should paste atomically");
    expect(pasted.size() == 1 && pasted.front() != rootId, "pasted roots should receive fresh stable IDs");
    const Entity pastedRoot = scene.findEntity(pasted.front());
    expect(scene.registry().get<RelationshipComponent>(pastedRoot).children.size() == 1,
           "pasting should retain fragment hierarchy");

    EditorRecovery recovery(directory / "recovery.zenith-scene");
    expect(recovery.autosave(scene, assets, options).success && recovery.available(),
           "autosave should create a separate recovery document");
    scene.clear();
    expect(recovery.recover(scene, assets, options).success && scene.findEntity(pasted.front()) != entt::null,
           "recovery should restore autosaved work");
    std::string discardError;
    expect(recovery.discard(discardError) && !recovery.available(), "discard should remove only the recovery file");

    EditorShortcuts shortcuts;
    expect(shortcuts.bind("Save", "Ctrl+S").success, "a unique shortcut should bind");
    expect(!shortcuts.bind("Stop", "Ctrl+S").success, "shortcut conflicts should be rejected");
    expect(shortcuts.chord("Save") == "Ctrl+S", "shortcut labels should be discoverable from the registry");

    EditorDiagnostics diagnostics;
    const uint64_t id = diagnostics.add(EditorDiagnosticSeverity::Error, "Document", "Save failed", "Save As");
    expect(diagnostics.entries().size() == 1 && diagnostics.entries().front().action == "Save As",
           "diagnostics should retain severity, source, and action");
    diagnostics.dismiss(id);
    expect(diagnostics.entries().empty(), "diagnostics should be individually dismissible");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}

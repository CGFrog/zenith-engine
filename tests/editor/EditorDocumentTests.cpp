#include "editor/document/EditorDocument.hpp"

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
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_editor_document_tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    AssetManager assets;
    EditorDocument document(directory);
    Scene scene;
    const Entity entity = scene.createEntity("Original");
    const SceneEntityId originalId = scene.id(entity);
    document.markDirty();
    expect(document.dirty() && document.displayName() == "Untitled*", "edits should mark an untitled document dirty");
    expect(!document.save(scene, assets).success, "untitled document should require Save As");

    const std::filesystem::path path = directory / "test.zenith-scene";
    expect(document.saveAs(path, scene, assets).success, "Save As should write the document");
    expect(!document.dirty() && document.path() == path, "successful Save As should establish a clean path");

    scene.registry().get<NameComponent>(entity).name = "Changed";
    document.markDirty();
    expect(document.save(scene, assets).success, "Save should use the established path");
    expect(!document.dirty(), "successful Save should clear dirty state");

    const std::filesystem::path missing = directory / "missing.zenith-scene";
    document.markDirty();
    const SceneDocumentResult failedOpen = document.open(missing, scene, assets);
    expect(!failedOpen.success, "opening a missing document should fail");
    expect(document.path() == path && document.dirty(), "failed Open should preserve path and dirty state");
    expect(scene.findEntity(originalId) != entt::null, "failed Open should preserve the scene");

    document.createNew(scene);
    expect(scene.registry().view<SceneEntityIdComponent>().empty(), "New should clear the runtime scene");
    expect(!document.path() && !document.dirty(), "New should reset path and dirty state");
    expect(document.open(path, scene, assets).success, "Open should load a saved document");
    expect(scene.registry().get<NameComponent>(scene.findEntity(originalId)).name == "Changed",
           "Open should restore saved component data");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}

#include "editor/document/CommandHistory.hpp"

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

std::string snapshot(const Scene& scene, const AssetManager& assets, const SceneDocumentOptions& options) {
    std::string output;
    const SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, output);
    expect(result.success, "scene snapshot should serialize");
    return output;
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_command_history_tests";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    SceneDocumentOptions options{.assetRoot = directory};
    AssetManager assets;
    Scene scene;
    const Entity entity = scene.createEntity("Before");
    const std::string before = snapshot(scene, assets, options);
    scene.registry().get<NameComponent>(entity).name = "After";
    const std::string after = snapshot(scene, assets, options);
    SceneDocument::deserialize(before, assets, options, scene);
    EditorCommandContext context{scene, assets, options};

    CommandHistory history(2);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", before, after), context).success,
           "command should execute");
    expect(scene.registry().get<NameComponent>(scene.findEntity({1})).name == "After",
           "execute should apply after state");
    expect(history.dirty() && history.canUndo() && !history.canRedo(), "execute should advance dirty history");
    expect(history.undo(context).success, "undo should succeed");
    expect(snapshot(scene, assets, options) == before, "undo should restore byte-identical serialized content");
    expect(history.redo(context).success, "redo should succeed");
    expect(snapshot(scene, assets, options) == after, "redo should restore byte-identical serialized content");
    history.crossBoundary(CommandHistoryBoundary::SaveDocument);
    expect(!history.dirty(), "markSaved should establish the clean history position");

    const size_t savedHistorySize = history.size();
    history.crossBoundary(CommandHistoryBoundary::EnterPlayMode);
    history.crossBoundary(CommandHistoryBoundary::LeavePlayMode);
    expect(history.size() == savedHistorySize && history.canUndo() && !history.dirty(),
           "play-mode transitions should preserve editable document history and its save point");

    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "After 2";
    const std::string after2 = snapshot(scene, assets, options);
    SceneDocument::deserialize(after, assets, options, scene);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", after, after2, "name:1"), context).success,
           "first coalesced command should execute");
    scene.registry().get<NameComponent>(scene.findEntity({1})).name = "After 3";
    const std::string after3 = snapshot(scene, assets, options);
    SceneDocument::deserialize(after2, assets, options, scene);
    expect(history.execute(std::make_unique<SceneStateCommand>("Rename", after2, after3, "name:1"), context).success,
           "second coalesced command should execute");
    expect(history.size() == 2, "continuous edits with one key should coalesce into one command");
    expect(history.undo(context).success, "coalesced edit should undo");
    expect(snapshot(scene, assets, options) == after, "coalesced undo should restore state before the full drag");

    SceneDocument::deserialize(after2, assets, options, scene);
    history.execute(std::make_unique<SceneStateCommand>("Edit 2", after, after2), context);
    history.execute(std::make_unique<SceneStateCommand>("Edit 3", after2, after3), context);
    expect(history.size() == 2, "history should enforce its configured command limit");

    history.crossBoundary(CommandHistoryBoundary::OpenDocument);
    expect(history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty(),
           "Open should start a clean history timeline");
    history.execute(std::make_unique<SceneStateCommand>("Edit after open", after3, after2), context);
    history.crossBoundary(CommandHistoryBoundary::NewDocument);
    expect(history.size() == 0 && !history.canUndo() && !history.canRedo() && !history.dirty(),
           "New should start a clean history timeline");
    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}

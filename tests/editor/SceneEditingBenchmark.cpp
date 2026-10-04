// What recording one editor edit costs in a large scene: an inspector field
// edit and one frame of a coalesced gizmo drag (each captured as the whole
// scene, and as only the edited entity, which the inspector and viewport
// use), an undoable operator (which the OperatorRunner captures before and
// after, then commits), and undo and redo. The scene holds 5,000 entities. Run by hand (Release) for numbers; CTest
// only checks that it completes.
#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/diagnostics/Benchmark.hpp"

#include <chrono>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t entityCount = 5'000;
constexpr uint32_t groupSize = 10;
constexpr uint32_t edits = 10;
constexpr uint32_t dragFrames = 10;

double millisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

std::vector<Entity> populate(Scene& scene) {
    std::vector<Entity> entities;
    entities.reserve(entityCount);
    for (uint32_t index = 0; index < entityCount; ++index) {
        const Entity entity = scene.createEntity("Entity " + std::to_string(index));
        scene.registry().get<TransformComponent>(entity).local =
            glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(index % 100), 0.0f, 0.0f));
        if (index % groupSize != 0) {
            scene.setParent(entity, entities[index - index % groupSize]);
        }
        if (index % 4 == 0) {
            scene.registry().emplace<PointLightComponent>(entity);
        }
        entities.push_back(entity);
    }
    scene.updateWorldTransforms();
    return entities;
}
} // namespace

int main() {
    try {
        AssetManager assets;
        Scene scene;
        EditorDocument document(std::filesystem::temp_directory_path());
        CommandHistory history;
        EditorDiagnostics diagnostics;
        const SceneEditing editing(scene, assets, document, history, diagnostics);
        const std::vector<Entity> entities = populate(scene);

        BenchmarkReport report;
        addBuildMetadata(report);
        report.metadata("scenario", "scene-editing-record");
        report.metadata("entities", std::to_string(entityCount));

        // An inspector edit of one field on one entity.
        for (uint32_t edit = 0; edit < edits; ++edit) {
            const Entity target = entities[(edit * 397) % entities.size()];
            const auto begin = std::chrono::steady_clock::now();
            std::string before;
            editing.capture(before);
            scene.registry().get<NameComponent>(target).name = "Edited " + std::to_string(edit);
            editing.commit("Rename", {}, std::move(before));
            report.sample("cpu.field_edit", millisecondsSince(begin));
        }

        // The same edit, capturing only the edited entity.
        for (uint32_t edit = 0; edit < edits; ++edit) {
            const Entity target = entities[(edit * 397 + 1) % entities.size()];
            const auto begin = std::chrono::steady_clock::now();
            SceneEditing::EntitySnapshot before;
            editing.captureEntities(std::span(&target, 1), before);
            scene.registry().get<NameComponent>(target).name = "Edited again " + std::to_string(edit);
            editing.commit("Rename", {}, std::move(before));
            report.sample("cpu.field_edit_entities", millisecondsSince(begin));
        }

        // A gizmo drag: one coalesced edit per frame.
        const Entity dragged = entities[1234];
        for (uint32_t frame = 0; frame < dragFrames; ++frame) {
            const auto begin = std::chrono::steady_clock::now();
            std::string before;
            editing.capture(before);
            scene.registry().get<TransformComponent>(dragged).local[3].x = static_cast<float>(frame) * 0.1f;
            scene.markTransformDirty(dragged);
            editing.commit("Move", "drag", std::move(before));
            report.sample("cpu.drag_frame", millisecondsSince(begin));
        }
        for (uint32_t frame = 0; frame < dragFrames; ++frame) {
            const auto begin = std::chrono::steady_clock::now();
            SceneEditing::EntitySnapshot before;
            editing.captureEntities(std::span(&dragged, 1), before);
            scene.registry().get<TransformComponent>(dragged).local[3].y = static_cast<float>(frame) * 0.1f;
            scene.markTransformDirty(dragged);
            editing.commit("Move", "drag entities", std::move(before));
            report.sample("cpu.drag_frame_entities", millisecondsSince(begin));
        }

        // What OperatorRunner does for an Undo operator that creates an
        // entity: capture, execute, capture, compare, commit.
        for (uint32_t edit = 0; edit < edits; ++edit) {
            const auto begin = std::chrono::steady_clock::now();
            std::string before;
            editing.capture(before);
            scene.setParent(scene.createEntity("Added " + std::to_string(edit)), entities[edit * groupSize]);
            std::string after;
            editing.capture(after);
            if (after != before) {
                editing.commit("Add", {}, before, after);
            }
            report.sample("cpu.operator_edit", millisecondsSince(begin));
        }

        const size_t recorded = history.size();
        for (uint32_t step = 0; step < edits; ++step) {
            const auto begin = std::chrono::steady_clock::now();
            editing.undo();
            report.sample("cpu.undo", millisecondsSince(begin));
        }
        for (uint32_t step = 0; step < edits; ++step) {
            const auto begin = std::chrono::steady_clock::now();
            editing.redo();
            report.sample("cpu.redo", millisecondsSince(begin));
        }
        if (recorded != edits * 3 + 2 || diagnostics.hasUnacknowledgedError()) {
            throw std::runtime_error("the benchmark edits were not all recorded");
        }
        report.metadata("history_bytes", std::to_string(history.storedBytes()));
        std::cout << report.json(false) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Scene editing benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

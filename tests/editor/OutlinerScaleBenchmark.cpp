// What the outliner costs per frame in a large scene: 10,000 entities in
// groups of ten (a root and nine children, so 1,000 root rows, every
// fourth group with a nested grandchild), a third of the roots in one of
// three collections, point lights and tags sprinkled in. It times the
// panel's first frame (building the tree), frames with nothing changed
// (which must rebuild nothing), a frame after a recorded edit (one
// rebuild), searched frames, and, for comparison, what rebuilding the
// tree and its rows costs on its own, which the panel used to pay every
// frame. Run by hand (Release) for numbers; CTest checks that it completes
// and that unchanged frames rebuild nothing.
#include "EditorTestFixture.hpp"
#include "HeadlessImGui.hpp"

#include "editor/operators/ObjectOperators.hpp"
#include "editor/panels/HierarchyPanel.hpp"
#include "editor/panels/outliner/OutlinerRows.hpp"
#include "engine/diagnostics/Benchmark.hpp"
#include "engine/scene/Tags.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t entityCount = 10'000;
constexpr uint32_t groupSize = 10;
constexpr uint32_t steadyFrames = 300;
constexpr uint32_t rebuildSamples = 10;

double millisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void populate(Scene& scene) {
    SceneCollections collections;
    std::vector<SceneCollectionId> ids(3);
    collections.create("Props", {}, ids[0]);
    collections.create("Lights", {}, ids[1]);
    collections.create("Enemies", ids[0], ids[2]);
    scene.setCollections(std::move(collections));
    Entity root = entt::null;
    for (uint32_t index = 0; index < entityCount; ++index) {
        const Entity entity = scene.createEntity("Entity " + std::to_string(index));
        if (index % groupSize == 0) {
            root = entity;
            if (index % 3 == 0) {
                linkToCollection(scene, entity, ids[(index / groupSize) % ids.size()]);
            }
        } else if (index % groupSize == groupSize - 1 && (index / groupSize) % 4 == 0) {
            // A grandchild under the group's first child.
            scene.setParent(entity, scene.registry().get<RelationshipComponent>(root).children.front());
        } else {
            scene.setParent(entity, root);
        }
        if (index % 7 == 0) {
            scene.registry().emplace<PointLightComponent>(entity);
        }
        if (index % 11 == 0) {
            scene.registry().emplace<TagsComponent>(entity, TagsComponent{{"enemy"}});
        }
    }
    scene.updateWorldTransforms();
}

struct Bench : EditorTestFixture {
    HierarchyPanel panel;

    Bench() {
        if (!registerObjectOperators(operators).success) {
            throw std::runtime_error("the object operators did not register");
        }
    }

    double frame() {
        const auto begin = std::chrono::steady_clock::now();
        {
            HeadlessImGuiFrame imguiFrame(panel.title());
            EditorContext editor = context();
            panel.draw(editor);
        }
        return millisecondsSince(begin);
    }
};

// The rows the panel would draw if it rebuilt everything this frame.
double rebuildOnce(const Scene& scene, std::string_view filter) {
    const auto begin = std::chrono::steady_clock::now();
    const std::vector<OutlinerNode> tree =
        buildOutlinerTree(scene, {.mode = OutlinerDisplayMode::ViewLayer, .filter = filter});
    const std::vector<OutlinerRowEntry> entries = indexOutlinerRows(tree, 1);
    const std::vector<uint32_t> visible = visibleOutlinerRows(
        entries, [](const OutlinerRowEntry& entry) { return entry.node->kind != OutlinerNodeKind::Entity; });
    if (visible.empty()) {
        throw std::runtime_error("the rebuilt tree has no rows");
    }
    return millisecondsSince(begin);
}
} // namespace

int main() {
    try {
        HeadlessImGuiContext imgui;
        Bench bench;
        populate(bench.scene);

        BenchmarkReport report;
        addBuildMetadata(report);
        report.metadata("scenario", "outliner-scale");
        report.metadata("entities", std::to_string(entityCount));

        report.sample("cpu.first_frame", bench.frame());
        bench.frame();
        const OutlinerRowCache::Stats before = bench.panel.rowCacheStats();
        for (uint32_t frame = 0; frame < steadyFrames; ++frame) {
            report.sample("cpu.unchanged_frame", bench.frame());
        }
        const OutlinerRowCache::Stats steady = bench.panel.rowCacheStats();
        if (steady.treeBuilds != before.treeBuilds || steady.visibleBuilds != before.visibleBuilds) {
            throw std::runtime_error("frames without changes rebuilt the outliner rows");
        }

        for (uint32_t edit = 0; edit < rebuildSamples; ++edit) {
            const SceneCollectionId collection{1};
            bench.invoke(
                "collection.rename",
                nlohmann::json{{"collection", collection.value}, {"name", "Props " + std::to_string(edit)}}.dump());
            report.sample("cpu.frame_after_edit", bench.frame());
        }
        if (bench.panel.rowCacheStats().treeBuilds != steady.treeBuilds + rebuildSamples) {
            throw std::runtime_error("each recorded edit should rebuild the rows once");
        }

        // A search for tagged objects with a point light.
        const ImGuiWindow* window = HeadlessImGuiContext::findWindow(bench.panel.title());
        if (window == nullptr) {
            throw std::runtime_error("the panel window was not drawn");
        }
        HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "##HierarchyFilter"));
        bench.frame();
        HeadlessImGuiContext::typeNextFrame("t:enemy c:point");
        report.sample("cpu.frame_searching", bench.frame());
        const uint64_t searched = bench.panel.rowCacheStats().treeBuilds;
        for (uint32_t frame = 0; frame < steadyFrames; ++frame) {
            report.sample("cpu.unchanged_search_frame", bench.frame());
        }
        if (bench.panel.rowCacheStats().treeBuilds != searched) {
            throw std::runtime_error("an unchanged search rebuilt the outliner rows");
        }

        for (uint32_t sample = 0; sample < rebuildSamples; ++sample) {
            report.sample("cpu.full_rebuild", rebuildOnce(bench.scene, {}));
            report.sample("cpu.full_rebuild_searching", rebuildOnce(bench.scene, "t:enemy c:point"));
        }
        report.metadata("displayed_rows", std::to_string(bench.panel.displayedEntities().size()));
        std::cout << report.json(false) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Outliner scale benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

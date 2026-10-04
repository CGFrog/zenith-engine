// Prefab costs in a large scene: comparing an instance with its prefab (what
// the editor's inspector does while an instance is selected), refreshing
// instances (what Apply does to the other instances), and spawning a small
// prefab at runtime (what EntityCommandBuffer and effects do). The scene holds
// 5,000 background entities and ten instances of a 200-entity prefab with a
// nested prefab. Run by hand (Release) for numbers; CTest only checks that it
// completes.
#include "engine/assets/Assets.hpp"
#include "engine/diagnostics/Benchmark.hpp"
#include "engine/scene/Prefab.hpp"
#include "engine/scene/Scene.hpp"

#include <chrono>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr uint32_t backgroundEntities = 5'000;
constexpr uint32_t prefabEntities = 200;
constexpr uint32_t nestedEntities = 5;
constexpr uint32_t instanceCount = 10;
constexpr uint32_t repeats = 5;
constexpr uint32_t spawns = 20;

template <typename Result> void require(const Result& result, const std::string& what) {
    if (!result) {
        throw std::runtime_error(what + ": " + result.error);
    }
}

double millisecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

// An entity with the components a level object typically carries.
Entity addContent(Scene& scene, const std::string& name, Entity parent, float offset) {
    const Entity entity = scene.createEntity(name);
    scene.registry().get<TransformComponent>(entity).local =
        glm::translate(glm::mat4(1.0f), glm::vec3(offset, 0.0f, offset * 0.5f));
    scene.registry().emplace<ColliderComponent>(entity, BoxCollider{glm::vec3(0.5f)});
    scene.registry().emplace<PointLightComponent>(entity);
    if (parent != entt::null) {
        scene.setParent(entity, parent);
    }
    return entity;
}

// Builds count entities under one root (in groups of ten, so the hierarchy
// has depth) in its own scene, and saves it as reference.
void writePrefab(PrefabSystem& prefabs, const fs::path& root, const std::string& reference, uint32_t count,
                 const std::string& nested = {}) {
    Scene authoring;
    const Entity prefabRoot = addContent(authoring, reference + " root", entt::null, 0.0f);
    Entity group = prefabRoot;
    for (uint32_t index = 1; index < count; ++index) {
        const Entity parent = index % 10 == 1 ? prefabRoot : group;
        const Entity entity = addContent(authoring, "Part " + std::to_string(index), parent, static_cast<float>(index));
        if (index % 10 == 1) {
            group = entity;
        }
    }
    if (!nested.empty()) {
        require(prefabs.instantiate(nested, authoring, prefabRoot), "instantiate the nested prefab");
    }
    PrefabCapture capture;
    require(prefabs.capture(authoring, prefabRoot, reference, capture), "capture " + reference);
    require(savePrefab(root / reference, capture.document), "save " + reference);
}

// Runs body repeats times and records each run's milliseconds as name.
template <typename Body> void measure(BenchmarkReport& report, const std::string& name, uint32_t count, Body&& body) {
    for (uint32_t run = 0; run < count; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        body();
        report.sample(name, millisecondsSince(begin));
    }
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_prefab_benchmark";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);
    try {
        AssetManager assets;
        PrefabSystem prefabs(assets, {.assetRoot = root});
        writePrefab(prefabs, root, "nested.zprefab", nestedEntities);
        writePrefab(prefabs, root, "big.zprefab", prefabEntities, "nested.zprefab");
        writePrefab(prefabs, root, "effect.zprefab", 3);

        Scene scene;
        for (uint32_t index = 0; index < backgroundEntities; ++index) {
            addContent(scene, "Background " + std::to_string(index), entt::null, static_cast<float>(index % 100));
        }
        std::vector<Entity> instances;
        for (uint32_t index = 0; index < instanceCount; ++index) {
            const PrefabInstanceResult instance = prefabs.instantiate("big.zprefab", scene);
            require(instance, "instantiate big.zprefab");
            instances.push_back(instance.root);
        }
        // One override on the measured instance: a renamed part.
        const Entity inspected = instances.front();
        const Entity part = scene.registry().get<RelationshipComponent>(inspected).children.front();
        scene.registry().get<NameComponent>(part).name = "Renamed part";

        BenchmarkReport report;
        addBuildMetadata(report);
        report.metadata("scenario", "prefab-compare-refresh-spawn");
        report.metadata("background_entities", std::to_string(backgroundEntities));
        report.metadata("prefab_entities", std::to_string(prefabEntities));
        report.metadata("instances", std::to_string(instanceCount));
        report.metadata("scene_entities", std::to_string(scene.registry().view<NameComponent>().size()));

        PrefabDocument document;
        require(prefabs.load("big.zprefab", document), "load big.zprefab");
        PrefabComparisonSource prepared;
        measure(report, "cpu.prepare_comparison", repeats,
                [&] { require(prefabs.prepareComparison(document, prepared), "prepare"); });

        std::vector<PrefabOverride> found;
        measure(report, "cpu.overrides_prepared", repeats,
                [&] { require(prefabs.overrides(scene, inspected, prepared, found), "overrides (prepared)"); });
        if (found.empty()) {
            throw std::runtime_error("the renamed part is not an override");
        }
        measure(report, "cpu.entity_overrides", repeats * 4,
                [&] { require(prefabs.entityOverrides(scene, inspected, part, prepared, found), "entity overrides"); });
        measure(report, "cpu.overrides_file", repeats,
                [&] { require(prefabs.overrides(scene, inspected, found), "overrides (file)"); });
        measure(report, "cpu.refresh_keep_added", repeats, [&] {
            require(prefabs.refresh(scene, instances.back(), PrefabRefreshMode::KeepAddedObjects), "refresh");
        });

        // What PrefabAuthoring::apply does: compare every other instance with
        // the previous prefab, capture the edited instance, then refresh the
        // others (it also saves the file, which is left out here).
        measure(report, "cpu.apply_pattern", 1, [&] {
            PrefabComparisonSource previous;
            require(prefabs.prepareComparison(document, previous), "apply: prepare");
            std::vector<PrefabOverride> otherOverrides;
            for (size_t index = 1; index < instances.size(); ++index) {
                require(prefabs.overrides(scene, instances[index], previous, otherOverrides), "apply: overrides");
            }
            PrefabCapture capture;
            require(prefabs.capture(scene, inspected, "big.zprefab", capture), "apply: capture");
            for (size_t index = 1; index < instances.size(); ++index) {
                require(prefabs.refresh(scene, instances[index], PrefabRefreshMode::KeepAddedObjects),
                        "apply: refresh");
            }
        });

        measure(report, "cpu.spawn_large_scene", spawns,
                [&] { require(prefabs.instantiate("effect.zprefab", scene), "spawn into the large scene"); });
        Scene empty;
        measure(report, "cpu.spawn_empty_scene", spawns,
                [&] { require(prefabs.instantiate("effect.zprefab", empty), "spawn into an empty scene"); });

        std::cout << report.json(false) << '\n';
    } catch (const std::exception& failure) {
        std::cerr << "Prefab benchmark failed: " << failure.what() << '\n';
        fs::remove_all(root, error);
        return 1;
    }
    fs::remove_all(root, error);
    return 0;
}

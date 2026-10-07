#include "EditorTestFixture.hpp"

#include "editor/operators/ModelOperators.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("ModelReplace");

// object.model_replace: replacing a model instance's node entities as one
// undo step, moving user children off the old nodes, giving an empty entity a
// model, and the refusals.

namespace {

const std::filesystem::path assetRoot{ZENITH_ASSET_DIR};

struct Fixture : EditorTestFixture {
    SceneEntityId prop;
    ModelHandle cube;

    Fixture()
        : EditorTestFixture(EditorTestFixtureOptions{.documentRoot = assetRoot}) {
        CHECK_MESSAGE(registerModelOperators(operators).success, "the model operators should register");
        cube = assets.loadModel(assetRoot / "primitives" / "cube.gltf").handle;
        const Entity entity = scene.createEntity("Prop");
        CHECK_MESSAGE(scene.instantiateModel(entity, cube, assets).success, "the cube should instantiate");
        prop = scene.id(entity);
    }

    OperatorResult replace(SceneEntityId entity, const std::string& asset) {
        return invoke("object.model_replace", nlohmann::json{{"entity", entity.value}, {"asset", asset}}.dump());
    }

    // The node entities of the prop's model instance.
    std::vector<Entity> nodes() {
        std::vector<Entity> found;
        for (const auto [entity, node] : scene.registry().view<const ModelNodeComponent>().each()) {
            if (node.instanceRoot == prop) {
                found.push_back(entity);
            }
        }
        return found;
    }

    ModelHandle model() {
        const auto* component = scene.registry().try_get<ModelComponent>(scene.findEntity(prop));
        return component == nullptr ? ModelHandle{} : component->model;
    }

    Entity parentOf(SceneEntityId id) {
        return scene.registry().get<RelationshipComponent>(scene.findEntity(id)).parent;
    }
};

TEST_CASE("ModelReplace: replace with undo") {
    Fixture fixture;
    const std::vector<Entity> cubeNodes = fixture.nodes();
    CHECK_MESSAGE(!cubeNodes.empty(), "the cube should have node entities");
    if (cubeNodes.empty()) {
        return;
    }
    // A child the user added under one of the cube's nodes.
    const Entity attached = fixture.scene.createEntity("Attached");
    fixture.scene.setParent(attached, cubeNodes.front());
    const SceneEntityId attachedId = fixture.scene.id(attached);
    const SceneEntityId firstNode = fixture.scene.id(cubeNodes.front());

    const OperatorResult result = fixture.replace(fixture.prop, "sample.gltf");
    CHECK_MESSAGE(result.status == OperatorStatus::Finished, ("replacing the model should finish: " + result.message));
    const ModelHandle sample = fixture.assets.loadModel(assetRoot / "sample.gltf").handle;
    CHECK_MESSAGE(fixture.model() == sample, "the prop should use the new model");
    const std::vector<Entity> sampleNodes = fixture.nodes();
    CHECK_MESSAGE((!sampleNodes.empty() &&
                   std::ranges::all_of(sampleNodes,
                                       [&](Entity node) {
                                           return fixture.scene.registry().get<ModelNodeComponent>(node).model ==
                                                  sample;
                                       })),
                  "every node of the instance should come from the new model");
    CHECK_MESSAGE((fixture.scene.findEntity(firstNode) == entt::null), "the old nodes should be destroyed");
    CHECK_MESSAGE(fixture.parentOf(attachedId) == fixture.scene.findEntity(fixture.prop),
                  "a user child of an old node should move to the prop");
    CHECK_MESSAGE(std::ranges::any_of(fixture.diagnostics.entries(),
                                      [](const EditorDiagnostic& entry) {
                                          return entry.severity == EditorDiagnosticSeverity::Warning;
                                      }),
                  "moving a user child should warn");
    CHECK_MESSAGE(fixture.history.size() == 1, "the replacement should be one undo step");

    fixture.editing().undo();
    CHECK_MESSAGE((fixture.model() == fixture.cube && fixture.nodes().size() == cubeNodes.size()),
                  "undo should bring the cube instance back");
    CHECK_MESSAGE((fixture.scene.findEntity(firstNode) != entt::null &&
                   fixture.parentOf(attachedId) == fixture.scene.findEntity(firstNode)),
                  "undo should put the user child back under its node");
    fixture.editing().redo();
    CHECK_MESSAGE(fixture.model() == sample, "redo should replace the model again");
}

TEST_CASE("ModelReplace: empty entity and refusals") {
    Fixture fixture;
    const SceneEntityId empty = fixture.scene.id(fixture.scene.createEntity("Empty"));
    CHECK_MESSAGE(fixture.replace(empty, (assetRoot / "primitives" / "cube.gltf").string()).status ==
                      OperatorStatus::Finished,
                  "an empty entity should get a model from an absolute path");
    CHECK_MESSAGE((fixture.scene.registry().all_of<ModelComponent>(fixture.scene.findEntity(empty))),
                  "the empty entity should now hold the model");

    CHECK_MESSAGE(fixture.replace(fixture.prop, "primitives/cube.gltf").status == OperatorStatus::Cancelled,
                  "giving an object the model it has should change nothing");
    CHECK_MESSAGE(fixture.replace(fixture.prop, "missing.gltf").status != OperatorStatus::Finished,
                  "a missing model should fail");
    CHECK_MESSAGE(fixture.replace(SceneEntityId{999999}, "sample.gltf").status != OperatorStatus::Finished,
                  "a missing object should fail");
    const SceneEntityId node = fixture.scene.id(fixture.nodes().front());
    CHECK_MESSAGE(fixture.replace(node, "sample.gltf").status != OperatorStatus::Finished,
                  "a model's node entity should not take a model");
    fixture.mode = EditorMode::Play;
    CHECK_MESSAGE(fixture.replace(fixture.prop, "sample.gltf").status != OperatorStatus::Finished,
                  "models should be replaced in Edit mode only");
}
} // namespace

TEST_SUITE_END();

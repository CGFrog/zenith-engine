#include "EditorTestFixture.hpp"

#include "editor/operators/ModelOperators.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// object.model_replace: replacing a model instance's node entities as one
// undo step, moving user children off the old nodes, giving an empty entity a
// model, and the refusals.

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

const std::filesystem::path assetRoot{ZENITH_ASSET_DIR};

struct Fixture : EditorTestFixture {
    SceneEntityId prop;
    ModelHandle cube;

    Fixture()
        : EditorTestFixture(EditorTestFixtureOptions{.documentRoot = assetRoot}) {
        expect(registerModelOperators(operators).success, "the model operators should register");
        cube = assets.loadModel(assetRoot / "primitives" / "cube.gltf").handle;
        const Entity entity = scene.createEntity("Prop");
        expect(scene.instantiateModel(entity, cube, assets).success, "the cube should instantiate");
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

void testReplaceWithUndo() {
    Fixture fixture;
    const std::vector<Entity> cubeNodes = fixture.nodes();
    expect(!cubeNodes.empty(), "the cube should have node entities");
    if (cubeNodes.empty()) {
        return;
    }
    // A child the user added under one of the cube's nodes.
    const Entity attached = fixture.scene.createEntity("Attached");
    fixture.scene.setParent(attached, cubeNodes.front());
    const SceneEntityId attachedId = fixture.scene.id(attached);
    const SceneEntityId firstNode = fixture.scene.id(cubeNodes.front());

    const OperatorResult result = fixture.replace(fixture.prop, "sample.gltf");
    expect(result.status == OperatorStatus::Finished, "replacing the model should finish: " + result.message);
    const ModelHandle sample = fixture.assets.loadModel(assetRoot / "sample.gltf").handle;
    expect(fixture.model() == sample, "the prop should use the new model");
    const std::vector<Entity> sampleNodes = fixture.nodes();
    expect(!sampleNodes.empty() &&
               std::ranges::all_of(
                   sampleNodes,
                   [&](Entity node) { return fixture.scene.registry().get<ModelNodeComponent>(node).model == sample; }),
           "every node of the instance should come from the new model");
    expect(fixture.scene.findEntity(firstNode) == entt::null, "the old nodes should be destroyed");
    expect(fixture.parentOf(attachedId) == fixture.scene.findEntity(fixture.prop),
           "a user child of an old node should move to the prop");
    expect(std::ranges::any_of(
               fixture.diagnostics.entries(),
               [](const EditorDiagnostic& entry) { return entry.severity == EditorDiagnosticSeverity::Warning; }),
           "moving a user child should warn");
    expect(fixture.history.size() == 1, "the replacement should be one undo step");

    fixture.editing().undo();
    expect(fixture.model() == fixture.cube && fixture.nodes().size() == cubeNodes.size(),
           "undo should bring the cube instance back");
    expect(fixture.scene.findEntity(firstNode) != entt::null &&
               fixture.parentOf(attachedId) == fixture.scene.findEntity(firstNode),
           "undo should put the user child back under its node");
    fixture.editing().redo();
    expect(fixture.model() == sample, "redo should replace the model again");
}

void testEmptyEntityAndRefusals() {
    Fixture fixture;
    const SceneEntityId empty = fixture.scene.id(fixture.scene.createEntity("Empty"));
    expect(fixture.replace(empty, (assetRoot / "primitives" / "cube.gltf").string()).status == OperatorStatus::Finished,
           "an empty entity should get a model from an absolute path");
    expect(fixture.scene.registry().all_of<ModelComponent>(fixture.scene.findEntity(empty)),
           "the empty entity should now hold the model");

    expect(fixture.replace(fixture.prop, "primitives/cube.gltf").status == OperatorStatus::Cancelled,
           "giving an object the model it has should change nothing");
    expect(fixture.replace(fixture.prop, "missing.gltf").status != OperatorStatus::Finished,
           "a missing model should fail");
    expect(fixture.replace(SceneEntityId{999999}, "sample.gltf").status != OperatorStatus::Finished,
           "a missing object should fail");
    const SceneEntityId node = fixture.scene.id(fixture.nodes().front());
    expect(fixture.replace(node, "sample.gltf").status != OperatorStatus::Finished,
           "a model's node entity should not take a model");
    fixture.mode = EditorMode::Play;
    expect(fixture.replace(fixture.prop, "sample.gltf").status != OperatorStatus::Finished,
           "models should be replaced in Edit mode only");
}
} // namespace

int main() {
    testReplaceWithUndo();
    testEmptyEntityAndRefusals();
    if (failures != 0) {
        std::cerr << failures << " model replace test(s) failed\n";
        return 1;
    }
    std::cout << "Model replace tests passed\n";
    return 0;
}

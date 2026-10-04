// Tags: the component's helpers, finding entities by tag, and its scene
// document form.

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void testHelpers() {
    TagsComponent tags;
    expect(tags.add("enemy") && !tags.add("enemy") && tags.has("enemy") && tags.tags.size() == 1,
           "a tag is added once");
    expect(tags.remove("enemy") && !tags.remove("enemy") && tags.tags.empty(), "a tag is removed once");
}

void testFind() {
    Scene scene;
    const Entity first = scene.createEntity("First");
    const Entity untagged = scene.createEntity("Untagged");
    const Entity second = scene.createEntity("Second");
    scene.registry().emplace<TagsComponent>(second).tags = {"enemy"};
    scene.registry().emplace<TagsComponent>(first).tags = {"enemy", "boss"};
    expect(hasTag(scene, first, "boss") && !hasTag(scene, second, "boss") && !hasTag(scene, untagged, "enemy") &&
               !hasTag(scene, entt::null, "enemy"),
           "hasTag reads the entity's tags");
    const std::vector<Entity> enemies = findEntitiesWithTag(scene, "enemy");
    expect(enemies == std::vector<Entity>{first, second}, "findEntitiesWithTag lists entities in scene id order");
    expect(findEntitiesWithTag(scene, "pickup").empty(), "an unused tag finds nothing");
}

void testDocument() {
    const ComponentType* type = builtInComponentTypes()->find("tags");
    expect(type != nullptr && type->displayName() == "Tags" && type->category() == "Gameplay",
           "tags are a built-in Gameplay component");
    AssetManager assets;
    const SceneDocumentOptions options{.assetRoot = std::filesystem::temp_directory_path()};
    Scene scene;
    scene.registry().emplace<TagsComponent>(scene.createEntity("Crate")).tags = {"pickup", "breakable"};
    std::string text;
    expect(SceneDocument::serialize(scene, assets, options, text).success, "tags save");
    nlohmann::json document = nlohmann::json::parse(text);
    nlohmann::json& saved = document.at("entities").at(0).at("components").at("tags");
    expect(saved == nlohmann::json{{"tags", {"pickup", "breakable"}}}, "tags save as a list of names");
    Scene restored;
    expect(SceneDocument::deserialize(text, assets, options, restored).success, "tags load");
    const auto view = restored.registry().view<const TagsComponent>();
    expect(view.size() == 1 &&
               view.get<const TagsComponent>(view.front()).tags == std::vector<std::string>{"pickup", "breakable"},
           "tags round-trip in order");
    saved["tags"] = {"pickup", ""};
    Scene rejected;
    expect(!SceneDocument::deserialize(document.dump(), assets, options, rejected), "an empty tag is rejected");
}
} // namespace

int main() {
    testHelpers();
    testFind();
    testDocument();
    if (failures != 0) {
        std::cerr << failures << " tags test(s) failed\n";
        return 1;
    }
    std::cout << "Tags tests passed\n";
    return 0;
}

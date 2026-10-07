// Tags: the component's helpers, finding entities by tag, and its scene
// document form.

#include "engine/assets/Assets.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/scene/Tags.hpp"

#include "support/Doctest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

TEST_SUITE_BEGIN("Tags");

namespace {

TEST_CASE("Tags: helpers") {
    TagsComponent tags;
    CHECK_MESSAGE((tags.add("enemy") && !tags.add("enemy") && tags.has("enemy") && tags.tags.size() == 1),
                  "a tag is added once");
    CHECK_MESSAGE((tags.remove("enemy") && !tags.remove("enemy") && tags.tags.empty()), "a tag is removed once");
}

TEST_CASE("Tags: find") {
    Scene scene;
    const Entity first = scene.createEntity("First");
    const Entity untagged = scene.createEntity("Untagged");
    const Entity second = scene.createEntity("Second");
    scene.registry().emplace<TagsComponent>(second).tags = {"enemy"};
    scene.registry().emplace<TagsComponent>(first).tags = {"enemy", "boss"};
    CHECK_MESSAGE((hasTag(scene, first, "boss") && !hasTag(scene, second, "boss") &&
                   !hasTag(scene, untagged, "enemy") && !hasTag(scene, entt::null, "enemy")),
                  "hasTag reads the entity's tags");
    const std::vector<Entity> enemies = findEntitiesWithTag(scene, "enemy");
    CHECK_MESSAGE((enemies == std::vector<Entity>{first, second}),
                  "findEntitiesWithTag lists entities in scene id order");
    CHECK_MESSAGE(findEntitiesWithTag(scene, "pickup").empty(), "an unused tag finds nothing");
}

TEST_CASE("Tags: document") {
    const ComponentType* type = builtInComponentTypes()->find("tags");
    CHECK_MESSAGE((type != nullptr && type->displayName() == "Tags" && type->category() == "Gameplay"),
                  "tags are a built-in Gameplay component");
    AssetManager assets;
    const testsupport::TempDirectory root("tags");
    const SceneDocumentOptions options{.assetRoot = root.path()};
    Scene scene;
    scene.registry().emplace<TagsComponent>(scene.createEntity("Crate")).tags = {"pickup", "breakable"};
    std::string text;
    CHECK_MESSAGE(SceneDocument::serialize(scene, assets, options, text).success, "tags save");
    nlohmann::json document = nlohmann::json::parse(text);
    nlohmann::json& saved = document.at("entities").at(0).at("components").at("tags");
    CHECK_MESSAGE((saved == nlohmann::json{{"tags", {"pickup", "breakable"}}}), "tags save as a list of names");
    Scene restored;
    CHECK_MESSAGE(SceneDocument::deserialize(text, assets, options, restored).success, "tags load");
    const auto view = restored.registry().view<const TagsComponent>();
    CHECK_MESSAGE((view.size() == 1 &&
                   view.get<const TagsComponent>(view.front()).tags == std::vector<std::string>{"pickup", "breakable"}),
                  "tags round-trip in order");
    saved["tags"] = {"pickup", ""};
    Scene rejected;
    CHECK_MESSAGE(!SceneDocument::deserialize(document.dump(), assets, options, rejected), "an empty tag is rejected");
}
} // namespace

TEST_SUITE_END();

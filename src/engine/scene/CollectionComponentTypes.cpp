// Document form of the collection and visibility components. Field keys are
// the scene document format; renaming one needs a schema migration.

#include "CollectionComponentTypes.hpp"

#include "ComponentTypes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace {
using Json = nlohmann::json;

ComponentType membershipType() {
    return ComponentTypeBuilder<CollectionMembershipComponent>("collection_membership", "Collections")
        .category("Collections")
        .internal()
        .codec(
            [](const CollectionMembershipComponent& membership, const ComponentSaveContext&, Json& output) {
                Json ids = Json::array();
                for (const SceneCollectionId id : membership.collections) {
                    ids.push_back(id.value);
                }
                output = {{"collections", std::move(ids)}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, CollectionMembershipComponent& membership, const ComponentLoadContext&) {
                membership.collections.clear();
                for (const Json& id : input.at("collections")) {
                    membership.insert(SceneCollectionId{id.get<uint64_t>()});
                }
                return SceneDocumentResult{.success = true};
            })
        .validate([](const CollectionMembershipComponent& membership) {
            return std::ranges::any_of(membership.collections, [](SceneCollectionId id) { return !id; })
                       ? std::string("Collection membership references invalid collection id 0")
                       : std::string{};
        })
        .build();
}

ComponentType instanceType() {
    return ComponentTypeBuilder<CollectionInstanceComponent>("collection_instance", "Collection Instance")
        .category("Collections")
        .internal()
        .codec(
            [](const CollectionInstanceComponent& instance, const ComponentSaveContext&, Json& output) {
                output = {{"collection", instance.collection.value}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, CollectionInstanceComponent& instance, const ComponentLoadContext&) {
                instance.collection = SceneCollectionId{input.at("collection").get<uint64_t>()};
                return SceneDocumentResult{.success = true};
            })
        .validate([](const CollectionInstanceComponent& instance) {
            return instance.collection ? std::string{}
                                       : std::string("Collection instance references invalid collection id 0");
        })
        .build();
}

ComponentType visibilityType() {
    using C = VisibilityComponent;
    return ComponentTypeBuilder<C>("visibility", "Visibility")
        .category("Rendering")
        .fields(Fields<C>()
                    .add("hidden", &C::hidden)
                    .label("Hide in Viewport")
                    .tooltip("Editor-only: hidden in editor viewports, still drawn by the game and in Play mode")
                    .add("selectable", &C::selectable)
                    .tooltip("Editor-only: whether clicking in a viewport can select it")
                    .add("renderable", &C::renderable)
                    .tooltip("Drawn at all; when off it is excluded from the game too"))
        .build();
}
} // namespace

SceneDocumentResult registerCollectionComponentTypes(ComponentTypeRegistry& registry) {
    for (ComponentType type : {membershipType(), instanceType(), visibilityType()}) {
        if (SceneDocumentResult result = registry.add(std::move(type)); !result) {
            return result;
        }
    }
    return {.success = true};
}

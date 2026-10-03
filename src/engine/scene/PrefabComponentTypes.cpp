#include "ComponentTypes.hpp"
#include "Prefab.hpp"
#include "SceneAssetReferences.hpp"

#include <nlohmann/json.hpp>

// The prefab link components are registered here rather than with the other
// built-in types so the prefab system owns its persisted form.

namespace {
using Json = nlohmann::json;

ComponentType prefabInstanceType() {
    return ComponentTypeBuilder<PrefabInstanceComponent>("prefab_instance", "Prefab Instance")
        .category("Prefabs")
        .internal()
        .codec(
            [](const PrefabInstanceComponent& instance, const ComponentSaveContext& context, Json& output) {
                Json reference = Json::object();
                writeAssetReference(instance.prefab, context.options, reference);
                Json overrides = Json::array();
                for (const PrefabOverride& overrideValue : instance.overrides) {
                    overrides.push_back(prefabOverrideToJson(overrideValue));
                }
                output = {{"prefab", std::move(reference)}, {"overrides", std::move(overrides)}};
                if (instance.nestedId != 0) {
                    output["nested_id"] = instance.nestedId;
                }
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, PrefabInstanceComponent& instance, const ComponentLoadContext& context) {
                if (SceneDocumentResult result =
                        readAssetReference(input.at("prefab"), context.options, instance.prefab);
                    !result) {
                    return result;
                }
                if (instance.prefab.empty()) {
                    return SceneDocumentResult{.error = "A prefab instance must reference a prefab"};
                }
                instance.overrides.clear();
                for (const Json& overrideJson : input.value("overrides", Json::array())) {
                    PrefabOverride overrideValue;
                    if (PrefabResult result = prefabOverrideFromJson(overrideJson, overrideValue); !result) {
                        return SceneDocumentResult{.error = result.error};
                    }
                    instance.overrides.push_back(std::move(overrideValue));
                }
                instance.nestedId = input.value("nested_id", uint64_t{0});
                return SceneDocumentResult{.success = true};
            })
        .build();
}

ComponentType prefabEntityType() {
    return ComponentTypeBuilder<PrefabEntityComponent>("prefab_entity", "Prefab Entity")
        .category("Prefabs")
        .internal()
        .codec(
            [](const PrefabEntityComponent& entity, const ComponentSaveContext&, Json& output) {
                output = {{"entity", entity.entity.value}};
                return SceneDocumentResult{.success = true};
            },
            [](const Json& input, PrefabEntityComponent& entity, const ComponentLoadContext&) {
                entity.entity.value = input.at("entity").get<uint64_t>();
                return entity.entity ? SceneDocumentResult{.success = true}
                                     : SceneDocumentResult{.error = "Prefab entity IDs must be non-zero"};
            })
        .build();
}
} // namespace

SceneDocumentResult registerPrefabComponentTypes(ComponentTypeRegistry& registry) {
    if (SceneDocumentResult result = registry.add(prefabInstanceType()); !result) {
        return result;
    }
    return registry.add(prefabEntityType());
}

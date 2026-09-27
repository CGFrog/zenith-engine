#include "SceneFragment.hpp"

#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

using Json = nlohmann::json;

SceneDocumentResult SceneFragment::copy(const Scene& scene, const AssetManager& assets,
                                        const SceneDocumentOptions& options, std::span<const SceneEntityId> roots,
                                        std::string& fragment) {
    std::string serialized;
    if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, serialized); !result) {
        return result;
    }
    try {
        Json document = Json::parse(serialized);
        std::unordered_map<uint64_t, Json> entities;
        for (const Json& entity : document.at("entities")) {
            entities.emplace(entity.at("id").get<uint64_t>(), entity);
        }
        std::unordered_set<uint64_t> included;
        std::unordered_set<uint64_t> selected;
        for (SceneEntityId root : roots) {
            selected.emplace(root.value);
        }
        std::vector<uint64_t> effectiveRoots;
        for (SceneEntityId root : roots) {
            auto entity = entities.find(root.value);
            if (entity == entities.end()) {
                continue;
            }
            bool nested = false;
            Json parent = entity->second.at("parent");
            while (!parent.is_null()) {
                const uint64_t parentId = parent.get<uint64_t>();
                if (selected.contains(parentId)) {
                    nested = true;
                    break;
                }
                const auto ancestor = entities.find(parentId);
                parent = ancestor == entities.end() ? Json(nullptr) : ancestor->second.at("parent");
            }
            if (!nested) {
                effectiveRoots.push_back(root.value);
            }
        }
        const auto includeChildren = [&](const auto& self, uint64_t parent) -> void {
            if (!included.emplace(parent).second) {
                return;
            }
            for (const auto& [id, entity] : entities) {
                if (!entity.at("parent").is_null() && entity.at("parent").get<uint64_t>() == parent) {
                    self(self, id);
                }
            }
        };
        for (uint64_t root : effectiveRoots) {
            if (entities.contains(root)) {
                includeChildren(includeChildren, root);
            }
        }
        if (included.empty()) {
            return {.error = "Copy requires at least one valid entity"};
        }
        Json output{{"version", SceneDocument::currentVersion}, {"entities", Json::array()}, {"roots", Json::array()}};
        for (uint64_t root : effectiveRoots) {
            if (included.contains(root)) {
                output["roots"].push_back(root);
            }
        }
        for (const Json& entity : document.at("entities")) {
            if (included.contains(entity.at("id").get<uint64_t>())) {
                Json fragmentEntity = entity;
                if (std::ranges::find(effectiveRoots, fragmentEntity.at("id").get<uint64_t>()) !=
                    effectiveRoots.end()) {
                    fragmentEntity["parent"] = nullptr;
                }
                output["entities"].push_back(std::move(fragmentEntity));
            }
        }
        fragment = output.dump(2) + '\n';
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Could not serialize scene fragment: ") + exception.what()};
    }
}

SceneDocumentResult SceneFragment::paste(std::string_view fragment, Scene& scene, AssetManager& assets,
                                         const SceneDocumentOptions& options, Entity parent,
                                         std::vector<SceneEntityId>& pastedRoots) {
    try {
        Json source = Json::parse(fragment);
        if (!source.contains("entities") || !source.contains("roots")) {
            return {.error = "Clipboard does not contain a Zenith scene fragment"};
        }
        std::string destinationText;
        if (SceneDocumentResult result = SceneDocument::serialize(scene, assets, options, destinationText); !result) {
            return result;
        }
        Json destination = Json::parse(destinationText);
        uint64_t nextId = 1;
        for (const Json& entity : destination.at("entities")) {
            nextId = std::max(nextId, entity.at("id").get<uint64_t>() + 1);
        }
        std::unordered_map<uint64_t, uint64_t> remap;
        for (const Json& entity : source.at("entities")) {
            remap.emplace(entity.at("id").get<uint64_t>(), nextId++);
        }
        const SceneEntityId parentId = parent == entt::null ? SceneEntityId{} : scene.id(parent);
        for (Json entity : source.at("entities")) {
            const uint64_t oldId = entity.at("id").get<uint64_t>();
            entity["id"] = remap.at(oldId);
            if (entity.at("parent").is_null()) {
                entity["parent"] = parentId ? Json(parentId.value) : Json(nullptr);
            } else {
                const uint64_t oldParent = entity.at("parent").get<uint64_t>();
                entity["parent"] = remap.contains(oldParent) ? Json(remap.at(oldParent)) : Json(nullptr);
            }
            destination["entities"].push_back(std::move(entity));
        }
        Scene staged;
        const std::string merged = destination.dump();
        if (SceneDocumentResult result = SceneDocument::deserialize(merged, assets, options, staged); !result) {
            return result;
        }
        pastedRoots.clear();
        for (const Json& root : source.at("roots")) {
            pastedRoots.push_back({remap.at(root.get<uint64_t>())});
        }
        scene.replaceWith(std::move(staged));
        return {.success = true};
    } catch (const std::exception& exception) {
        return {.error = std::string("Could not paste scene fragment: ") + exception.what()};
    }
}

// Prefab authoring on a staging scene, with prefabs written under a scratch
// asset root so the project is never touched: create, instantiate twice,
// delete an entity from one instance (a structural override), apply the
// other, and check both; then nest one in a second prefab and check a nested
// instance deleted from it stays deleted.

#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Prefab.hpp"

#include <system_error>
#include <utility>

namespace {

class PrefabSmokeStep final : public SmokeStep {
  public:
    explicit PrefabSmokeStep(std::filesystem::path assetRoot)
        : m_assetRoot(std::move(assetRoot)) {}

    std::string_view name() const override {
        return "prefabs";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        SceneDocumentOptions options = context.editor.document().options();
        options.assetRoot = m_assetRoot;
        std::error_code error;
        std::filesystem::remove_all(options.assetRoot, error);
        SmokeStepResult result = run(context.app.assets, options);
        std::filesystem::remove_all(options.assetRoot, error);
        return result;
    }

  private:
    static SmokeStepResult run(AssetManager& assets, const SceneDocumentOptions& options) {
        PrefabAuthoring prefabs(assets, options);
        Scene staging;
        const Entity lamp = staging.createEntity("Lamp");
        staging.registry().emplace<PointLightComponent>(lamp);
        staging.setParent(staging.createEntity("Shade"), lamp);
        Entity first = entt::null;
        Entity second = entt::null;
        if (PrefabResult created = prefabs.create(staging, lamp, "smoke/lamp.prefab"); !created) {
            return SmokeStepResult::failed(created.error);
        }
        if (!prefabs.instantiate(staging, "smoke/lamp.prefab", entt::null, first) ||
            !prefabs.instantiate(staging, "smoke/lamp.prefab", entt::null, second)) {
            return SmokeStepResult::failed("the prefab did not instantiate");
        }
        const SceneEntityId firstId = staging.id(first);
        const Entity shade = staging.registry().get<RelationshipComponent>(first).children.front();
        recordPrefabEntityRemoval(staging, shade);
        staging.destroyEntity(shade);
        staging.registry().get<PointLightComponent>(second).intensity = 9.0f;
        if (PrefabResult applied = prefabs.apply(staging, second); !applied) {
            return SmokeStepResult::failed(applied.error);
        }
        if (staging.registry().get<PointLightComponent>(staging.findEntity(firstId)).intensity != 9.0f ||
            !staging.registry().get<RelationshipComponent>(staging.findEntity(firstId)).children.empty()) {
            return SmokeStepResult::failed("apply did not update the other instance while keeping its deletion");
        }
        return checkNestedRemoval(assets, options, prefabs, staging, firstId);
    }

    // Nests the first lamp in a cart prefab, deletes it from a cart instance,
    // and checks a refresh keeps it deleted.
    static SmokeStepResult checkNestedRemoval(AssetManager& assets, const SceneDocumentOptions& options,
                                              PrefabAuthoring& prefabs, Scene& staging, SceneEntityId lampId) {
        const Entity cart = staging.createEntity("Cart");
        staging.setParent(staging.findEntity(lampId), cart);
        Entity cartInstance = entt::null;
        if (!prefabs.create(staging, cart, "smoke/cart.prefab") ||
            !prefabs.instantiate(staging, "smoke/cart.prefab", entt::null, cartInstance)) {
            return SmokeStepResult::failed("the prefab holding a nested prefab was not created");
        }
        const Entity nestedLamp = staging.registry().get<RelationshipComponent>(cartInstance).children.front();
        recordPrefabEntityRemoval(staging, nestedLamp);
        staging.destroyEntity(nestedLamp);
        PrefabSystem system(assets, options);
        if (!system.refresh(staging, cartInstance, PrefabRefreshMode::KeepAddedObjects) ||
            !staging.registry().get<RelationshipComponent>(cartInstance).children.empty()) {
            return SmokeStepResult::failed("a deleted nested instance came back after a refresh");
        }
        return SmokeStepResult::done();
    }

    std::filesystem::path m_assetRoot;
};

} // namespace

std::unique_ptr<SmokeStep> makePrefabSmokeStep(std::filesystem::path assetRoot) {
    return std::make_unique<PrefabSmokeStep>(std::move(assetRoot));
}

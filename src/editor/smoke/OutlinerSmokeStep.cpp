// The outliner in Edit mode: show a Hierarchy in Collections mode, add an
// empty, create a collection and link the empty to it (the Hierarchy draws
// the result in between), then undo every step and restore the mode.

#include "editor/areas/Workspace.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/CollectionOperators.hpp"
#include "editor/operators/OperatorRunner.hpp"
#include "editor/panels/EditorPanel.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <nlohmann/json.hpp>

namespace {

class OutlinerSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "outliner collections";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        return stepFrame == 0 ? addLinkedEmpty(context) : undoAndRestore(context);
    }

  private:
    SmokeStepResult addLinkedEmpty(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        m_marker = editor.history().marker();
        for (const AreaInstance& area : editor.activeWorkspace().areas()) {
            if (area.typeId == "hierarchy" && area.visible && area.panel) {
                m_area = area.id();
                m_areaState = area.panel->areaState();
                area.panel->restoreAreaState("collections");
                if (area.panel->areaState() != "collections") {
                    return SmokeStepResult::failed("the Hierarchy did not switch to the Collections display mode");
                }
                break;
            }
        }
        if (const OperatorResult result = editor.invokeOperator("object.add_empty", {}, KeymapRegion::Global);
            !result) {
            return SmokeStepResult::failed("object.add_empty did not run: " + result.message);
        }
        const std::optional<SceneEntityId> added = editor.selection().active();
        editor.invokeOperator("collection.new", R"({"name":"Smoke Collection","link_selection":false})",
                              KeymapRegion::Global);
        const LastOperator* created = editor.lastOperator();
        const NewCollectionProperties* properties =
            created == nullptr ? nullptr : created->properties.get<NewCollectionProperties>();
        if (!added || properties == nullptr || properties->created == 0) {
            return SmokeStepResult::failed("collection.new did not create a collection");
        }
        m_entity = *added;
        const SceneCollectionId collection{properties->created};
        const OperatorResult linked = editor.invokeOperator(
            "collection.link", nlohmann::json{{"collection", collection.value}}.dump(), KeymapRegion::Outliner);
        const Scene& scene = context.app.scene;
        const Entity entity = scene.findEntity(*added);
        const auto* membership =
            entity == entt::null ? nullptr : scene.registry().try_get<CollectionMembershipComponent>(entity);
        if (membership == nullptr || !membership->contains(collection)) {
            return SmokeStepResult::failed("collection.link did not link the new empty: " + linked.message);
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult undoAndRestore(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        const Scene& scene = context.app.scene;
        const size_t collectionsBefore = scene.collections().all().size();
        editor.undoSince(m_marker);
        if (scene.collections().all().size() + 1 != collectionsBefore || scene.findEntity(m_entity) != entt::null) {
            return SmokeStepResult::failed("undo did not remove the smoke collection and empty");
        }
        if (const AreaInstance* area = editor.activeWorkspace().find(m_area); area != nullptr && area->panel) {
            area->panel->restoreAreaState(m_areaState);
        }
        return SmokeStepResult::done();
    }

    // The Hierarchy area whose display mode the step switched (and its
    // previous state), the history before the step's edits, and its empty.
    std::string m_area;
    std::string m_areaState;
    CommandHistoryMarker m_marker;
    SceneEntityId m_entity;
};

} // namespace

std::unique_ptr<SmokeStep> makeOutlinerSmokeStep() {
    return std::make_unique<OutlinerSmokeStep>();
}

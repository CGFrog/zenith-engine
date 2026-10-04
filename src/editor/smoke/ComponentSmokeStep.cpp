// Component operators on several entities: add two empties, select both, give
// the active one a point light, copy it to the other and reset both through
// the component operators (the Inspector draws the shared component in
// between), then undo every step.

#include "editor/interaction/Selection.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <algorithm>
#include <vector>

namespace {

class ComponentSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "component operators";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        const Scene& scene = context.app.scene;
        if (stepFrame == 0) {
            m_marker = editor.history().marker();
            for (int index = 0; index < 2; ++index) {
                editor.invokeOperator("object.add_empty", {}, KeymapRegion::Global);
                if (const std::optional<SceneEntityId> added = editor.selection().active()) {
                    m_entities.push_back(*added);
                }
            }
            editor.selection().assign(std::vector<SceneEntityId>(m_entities.rbegin(), m_entities.rend()));
            constexpr std::string_view light = R"({"component":"point_light"})";
            for (const char* operatorId :
                 {"object.component_add", "object.component_copy_to_selected", "object.component_reset"}) {
                editor.invokeOperator(operatorId, light, KeymapRegion::Global);
            }
            const auto pointLights = std::ranges::count_if(m_entities, [&](SceneEntityId id) {
                return scene.registry().all_of<PointLightComponent>(scene.findEntity(id));
            });
            if (m_entities.size() != 2 || pointLights != 2) {
                return SmokeStepResult::failed("the point light was not copied to both selected entities");
            }
            return SmokeStepResult::running();
        }
        editor.undoSince(m_marker);
        if (std::ranges::any_of(m_entities, [&](SceneEntityId id) { return scene.findEntity(id) != entt::null; })) {
            return SmokeStepResult::failed("undo did not remove the smoke entities");
        }
        editor.selection().clear();
        return SmokeStepResult::done();
    }

  private:
    // The entities the step added, and the history before it.
    std::vector<SceneEntityId> m_entities;
    CommandHistoryMarker m_marker;
};

} // namespace

std::unique_ptr<SmokeStep> makeComponentSmokeStep() {
    return std::make_unique<ComponentSmokeStep>();
}

// The 3D cursor: place it through view3d.cursor3d_place at a point of the
// active viewport, add an Empty, check it spawned at the cursor, undo it, and
// restore the cursor. Skipped without a viewport.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <nlohmann/json.hpp>

namespace {

class CursorSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "3D cursor";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        EditorSmokeHooks& editor = context.editor;
        Scene& scene = context.app.scene;
        const ViewportPanel* viewport = editor.viewports().active();
        if (viewport == nullptr || editor.playMode().mode() != EditorMode::Edit) {
            return SmokeStepResult::done();
        }
        glm::vec3& cursorLocation = editor.document().editorData().cursorLocation;
        const glm::vec3 previousCursor = cursorLocation;
        const glm::vec2 point = viewport->size() * glm::vec2(0.4f, 0.6f);
        if (!editor.invokeOperator("view3d.cursor3d_place", nlohmann::json{{"x", point.x}, {"y", point.y}}.dump(),
                                   KeymapRegion::Viewport)) {
            return SmokeStepResult::failed("view3d.cursor3d_place did not run");
        }
        const glm::vec3 cursor = cursorLocation;
        const size_t entityCount = scene.registry().view<const SceneEntityIdComponent>().size();
        if (!editor.invokeOperator("object.add_empty", {}, KeymapRegion::Viewport) || editor.selection().size() != 1) {
            return SmokeStepResult::failed("object.add_empty did not add and select an Empty");
        }
        scene.updateWorldTransforms();
        const Entity added = scene.findEntity(*editor.selection().active());
        const glm::vec3 location =
            added != entt::null ? glm::vec3(scene.registry().get<TransformComponent>(added).world[3]) : glm::vec3(0.0f);
        if (added == entt::null || glm::length(location - cursor) > 1e-3f) {
            return SmokeStepResult::failed("the Empty did not spawn at the 3D cursor");
        }
        editor.undo();
        if (scene.registry().view<const SceneEntityIdComponent>().size() != entityCount) {
            return SmokeStepResult::failed("undo did not remove the Empty");
        }
        cursorLocation = previousCursor;
        return SmokeStepResult::done();
    }
};

} // namespace

std::unique_ptr<SmokeStep> makeCursorSmokeStep() {
    return std::make_unique<CursorSmokeStep>();
}

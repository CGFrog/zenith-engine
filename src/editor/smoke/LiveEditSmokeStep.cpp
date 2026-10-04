// Live editing: an edit in Play changes the running scene without touching
// the edit history, Apply to Edit Scene records it as one undoable edit, and
// Stop restores the edit scene with it. The first frame adds an empty and
// enters Play; the second moves it and gives it a point light live, then
// applies it to the edit scene; the third stops, checks the edit scene, and
// undoes the apply and the empty.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <string>

namespace {

constexpr glm::vec3 liveEditLocation{1.0f, 2.0f, 3.0f};

glm::vec3 localLocation(const Scene& scene, Entity entity) {
    return glm::vec3(scene.registry().get<TransformComponent>(entity).local[3]);
}

class LiveEditSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "live edit in Play";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        switch (stepFrame) {
        case 0:
            return addEmptyAndPlay(context);
        case 1:
            return editLiveAndApply(context);
        default:
            return stopAndUndo(context);
        }
    }

  private:
    SmokeStepResult addEmptyAndPlay(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        if (editor.playMode().mode() != EditorMode::Edit ||
            !editor.invokeOperator("object.add_empty", {}, KeymapRegion::Global) || !editor.selection().active()) {
            return SmokeStepResult::failed("could not add an empty in Edit mode");
        }
        m_entity = *editor.selection().active();
        m_historySize = editor.history().size();
        m_startLocation = localLocation(context.app.scene, context.app.scene.findEntity(m_entity));
        // An explicit target: without one play.start reuses the last target,
        // which the play target step left as Standalone.
        if (!editor.invokeOperator("play.start", R"({"target":"viewport"})", KeymapRegion::Global) ||
            editor.playMode().mode() != EditorMode::Play) {
            return SmokeStepResult::failed("play.start did not enter Play");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult editLiveAndApply(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        Scene& scene = context.app.scene;
        const Entity entity = scene.findEntity(m_entity);
        if (entity == entt::null) {
            return SmokeStepResult::failed("the empty is missing from the running scene");
        }
        editor.selection().selectOnly(m_entity);
        // As the Inspector edits: capture, change, commit.
        const SceneEditing live = editor.editing();
        const uint64_t revision = editor.history().revision();
        std::string before;
        if (live.capture(before)) {
            scene.registry().get<TransformComponent>(entity).local[3] = glm::vec4(liveEditLocation, 1.0f);
            scene.markTransformDirty(entity);
            live.commit("Edit Transform", "transform:" + std::to_string(m_entity.value), std::move(before));
        }
        editor.invokeOperator("object.component_add", R"({"component":"point_light"})", KeymapRegion::Global);
        if (editor.history().revision() != revision || editor.history().size() != m_historySize ||
            !scene.registry().all_of<PointLightComponent>(entity)) {
            return SmokeStepResult::failed(
                "a runtime edit should change the running scene without recording edit history");
        }
        if (!editor.invokeOperator("play.apply_to_edit", {}, KeymapRegion::Global) ||
            editor.history().size() != m_historySize + 1) {
            return SmokeStepResult::failed("play.apply_to_edit did not record one edit");
        }
        const Scene* editScene = editor.playMode().editScene();
        const Entity edited = editScene != nullptr ? editScene->findEntity(m_entity) : Entity{entt::null};
        if (edited == entt::null || localLocation(*editScene, edited) != liveEditLocation ||
            !editScene->registry().all_of<PointLightComponent>(edited) ||
            localLocation(scene, entity) != liveEditLocation) {
            return SmokeStepResult::failed(
                "the edit scene did not receive the running entity, or the running scene changed");
        }
        return SmokeStepResult::running();
    }

    SmokeStepResult stopAndUndo(SmokeStepContext& context) {
        EditorSmokeHooks& editor = context.editor;
        const Scene& scene = context.app.scene;
        editor.invokeOperator("play.stop", {}, KeymapRegion::Global);
        const Entity entity = scene.findEntity(m_entity);
        if (editor.playMode().mode() != EditorMode::Edit || entity == entt::null ||
            localLocation(scene, entity) != liveEditLocation || !scene.registry().all_of<PointLightComponent>(entity)) {
            return SmokeStepResult::failed("Stop did not restore the edit scene with the applied entity");
        }
        editor.undo();
        const Entity undone = scene.findEntity(m_entity);
        if (undone == entt::null || localLocation(scene, undone) != m_startLocation ||
            scene.registry().all_of<PointLightComponent>(undone)) {
            return SmokeStepResult::failed("undo after Stop did not revert the applied entity");
        }
        // Removes the empty again.
        editor.undo();
        if (scene.findEntity(m_entity) != entt::null) {
            return SmokeStepResult::failed("undo did not remove the smoke empty");
        }
        editor.selection().clear();
        return SmokeStepResult::done();
    }

    // The step's empty, its location before the step moved it, and the edit
    // history's size once it was added.
    SceneEntityId m_entity;
    glm::vec3 m_startLocation{0.0f};
    size_t m_historySize{0};
};

} // namespace

std::unique_ptr<SmokeStep> makeLiveEditSmokeStep() {
    return std::make_unique<LiveEditSmokeStep>();
}

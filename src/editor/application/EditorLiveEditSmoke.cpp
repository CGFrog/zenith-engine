// EditorSession's live editing smoke: an edit in Play changes the running scene
// without touching the edit history, Apply to Edit Scene records it as one
// undoable edit, and Stop restores the edit scene with it.

#include "EditorSession.hpp"

#include "engine/scene/ComponentTypes.hpp"

#include <limits>
#include <string>

namespace {
constexpr uint32_t liveEditFirstFrame = 425;
constexpr glm::vec3 liveEditLocation{1.0f, 2.0f, 3.0f};

glm::vec3 localLocation(const Scene& scene, Entity entity) {
    return glm::vec3(scene.registry().get<TransformComponent>(entity).local[3]);
}
} // namespace

bool EditorSession::updateSmokeLiveEdit(ApplicationContext& context) {
    // Frame 425 adds an empty and enters Play; 426 moves it and gives it a
    // point light live, then applies it to the edit scene; 427 stops, checks
    // the edit scene, and undoes the apply and the empty.
    const auto fail = [&](const std::string& message) {
        context.logger.log(LogLevel::Error, "editor", "Live edit smoke: " + message);
        m_diagnostics.reportError("Live edit smoke: " + message);
        context.control.requestSceneChange({std::numeric_limits<uint32_t>::max()});
    };
    if (m_smokeFrame < liveEditFirstFrame) {
        return false;
    }
    if (m_smokeFrame == liveEditFirstFrame) {
        if (m_playMode.mode() != EditorMode::Edit ||
            !invokeOperator(context, "object.add_empty", {}, KeymapRegion::Global) || !m_selection.active()) {
            fail("could not add an empty in Edit mode");
            return true;
        }
        m_smokeLiveEntity = *m_selection.active();
        m_smokeLiveHistorySize = m_history.size();
        m_smokeLiveStartLocation = localLocation(context.scene, context.scene.findEntity(m_smokeLiveEntity));
        if (!invokeOperator(context, "play.start", {}, KeymapRegion::Global) || m_playMode.mode() != EditorMode::Play) {
            fail("play.start did not enter Play");
            return true;
        }
        return false;
    }
    if (m_smokeFrame == liveEditFirstFrame + 1) {
        const Entity entity = context.scene.findEntity(m_smokeLiveEntity);
        if (entity == entt::null) {
            fail("the empty is missing from the running scene");
            return true;
        }
        m_selection.selectOnly(m_smokeLiveEntity);
        // As the Inspector edits: capture, change, commit.
        const SceneEditing live = editing(context);
        const uint64_t revision = m_history.revision();
        std::string before;
        if (live.capture(before)) {
            context.scene.registry().get<TransformComponent>(entity).local[3] = glm::vec4(liveEditLocation, 1.0f);
            context.scene.markTransformDirty(entity);
            live.commit("Edit Transform", "transform:" + std::to_string(m_smokeLiveEntity.value), std::move(before));
        }
        invokeOperator(context, "object.component_add", R"({"component":"point_light"})", KeymapRegion::Global);
        if (m_history.revision() != revision || m_history.size() != m_smokeLiveHistorySize ||
            !context.scene.registry().all_of<PointLightComponent>(entity)) {
            fail("a runtime edit should change the running scene without recording edit history");
        }
        if (!invokeOperator(context, "play.apply_to_edit", {}, KeymapRegion::Global) ||
            m_history.size() != m_smokeLiveHistorySize + 1) {
            fail("play.apply_to_edit did not record one edit");
        }
        const Scene* editScene = m_playMode.editScene();
        const Entity edited = editScene != nullptr ? editScene->findEntity(m_smokeLiveEntity) : Entity{entt::null};
        if (edited == entt::null || localLocation(*editScene, edited) != liveEditLocation ||
            !editScene->registry().all_of<PointLightComponent>(edited) ||
            localLocation(context.scene, entity) != liveEditLocation) {
            fail("the edit scene did not receive the running entity, or the running scene changed");
        }
        return false;
    }
    if (m_smokeFrame == liveEditFirstFrame + 2) {
        invokeOperator(context, "play.stop", {}, KeymapRegion::Global);
        const Entity entity = context.scene.findEntity(m_smokeLiveEntity);
        if (m_playMode.mode() != EditorMode::Edit || entity == entt::null ||
            localLocation(context.scene, entity) != liveEditLocation ||
            !context.scene.registry().all_of<PointLightComponent>(entity)) {
            fail("Stop did not restore the edit scene with the applied entity");
            return true;
        }
        undo(context);
        const Entity undone = context.scene.findEntity(m_smokeLiveEntity);
        if (undone == entt::null || localLocation(context.scene, undone) != m_smokeLiveStartLocation ||
            context.scene.registry().all_of<PointLightComponent>(undone)) {
            fail("undo after Stop did not revert the applied entity");
        }
        // Removes the empty again.
        undo(context);
        if (context.scene.findEntity(m_smokeLiveEntity) != entt::null) {
            fail("undo did not remove the smoke empty");
        }
        m_selection.clear();
    }
    return m_smokeFrame > liveEditFirstFrame + 2;
}

// A generator's preview in Edit mode: a transient entity, which saves leave
// out, and a persistent debug draw channel whose label the active viewport
// keeps projecting frame after frame until the channel is hidden. Restores
// the scene and the debug draw. Skipped without a viewport or outside Edit
// mode.

#include "editor/application/EditorPlayMode.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"
#include "engine/application/Application.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace {
constexpr std::string_view previewName = "Smoke Preview";
constexpr std::string_view channelName = "smoke.preview";
constexpr std::string_view labelText = "smoke preview";

bool showsLabel(const RenderRuntime& rendering, RenderViewId view) {
    return std::ranges::any_of(rendering.viewDebugLabels(view),
                               [](const ProjectedDebugLabel& label) { return label.text == labelText; });
}

class GeneratedPreviewSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "generated preview";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        RenderRuntime& rendering = context.app.rendering;
        if (stepFrame == 0) {
            const ViewportPanel* viewport = editor.viewports().active();
            if (viewport == nullptr || editor.playMode().mode() != EditorMode::Edit) {
                return SmokeStepResult::done();
            }
            return build(context, *viewport);
        }
        if (!m_hiddenFrame) {
            // A persistent label outlives the frame it was recorded in.
            if (stepFrame >= 3 && showsLabel(rendering, m_view)) {
                rendering.debugDraw().setChannelVisible(channelName, false);
                m_hiddenFrame = stepFrame;
            } else if (stepFrame > 40) {
                restore(context);
                return SmokeStepResult::failed("the active viewport did not project the persistent label");
            }
            return SmokeStepResult::running();
        }
        if (stepFrame < *m_hiddenFrame + 2) {
            return SmokeStepResult::running();
        }
        const bool hidden = !showsLabel(rendering, m_view);
        restore(context);
        if (!hidden) {
            return SmokeStepResult::failed("a hidden debug draw channel's label was still projected");
        }
        context.app.logger.log(LogLevel::Info, "editor",
                               "Generated preview smoke: the transient preview stayed unsaved and its persistent "
                               "label showed until its channel was hidden");
        return SmokeStepResult::done();
    }

  private:
    static void restore(SmokeStepContext& context) {
        DebugDraw& debugDraw = context.app.rendering.debugDraw();
        debugDraw.setChannelVisible(channelName, true);
        debugDraw.clearChannel(channelName);
        context.app.scene.destroyTransient();
    }

    SmokeStepResult build(SmokeStepContext& context, const ViewportPanel& viewport) {
        Scene& scene = context.app.scene;
        const RenderView lens = viewport.lens(scene);
        const glm::vec3 point = lens.position + glm::normalize(lens.forward) * 4.0f;
        const Entity preview = scene.createEntity(std::string(previewName));
        scene.markTransient(preview);
        scene.setPosition(preview, point);
        const DebugDrawRecorder channel = context.app.rendering.debugDraw().channel(channelName).persistent();
        channel.aabb(point - glm::vec3(0.5f), point + glm::vec3(0.5f), {.color = {0.3f, 0.9f, 0.4f, 1.0f}});
        channel.label(point, std::string(labelText));
        m_view = viewport.renderView();

        std::string document;
        const SceneDocumentResult saved =
            SceneDocument::serialize(scene, context.app.assets, context.editor.document().options(), document);
        if (!saved || document.find(previewName) != std::string::npos) {
            scene.destroyTransient();
            return SmokeStepResult::failed("the transient preview was written to the scene document");
        }
        return SmokeStepResult::running();
    }

    RenderViewId m_view{};
    std::optional<uint32_t> m_hiddenFrame;
};

} // namespace

std::unique_ptr<SmokeStep> makeGeneratedPreviewSmokeStep() {
    return std::make_unique<GeneratedPreviewSmokeStep>();
}

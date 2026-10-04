// H hides everything in the editor views (the frame rendered in between draws
// fewer items), then Alt+H reveals it again.

#include "editor/interaction/Selection.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

namespace {

class HideSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "hide";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        if (stepFrame == 0) {
            m_renderablesBeforeHide = context.app.rendering.stats().renderableCount;
            editor.invokeOperator("object.select_all", R"({"action":"select"})", KeymapRegion::Viewport);
            if (editor.selection().empty()) {
                // An empty startup scene has nothing to hide.
                context.app.logger.log(
                    LogLevel::Info, "editor",
                    "Hide smoke skipped: " +
                        std::to_string(context.app.scene.registry().view<const SceneEntityIdComponent>().size()) +
                        " entities, none selectable");
                return SmokeStepResult::done();
            }
            if (!editor.invokeOperator("object.hide", {}, KeymapRegion::Viewport)) {
                return SmokeStepResult::failed("object.hide did not run");
            }
            return SmokeStepResult::running();
        }
        const uint32_t renderablesWhileHidden = context.app.rendering.stats().renderableCount;
        if (m_renderablesBeforeHide > 0 && renderablesWhileHidden >= m_renderablesBeforeHide) {
            return SmokeStepResult::failed("hidden objects were still rendered");
        }
        if (!editor.invokeOperator("object.unhide", {}, KeymapRegion::Viewport)) {
            return SmokeStepResult::failed("object.unhide did not run");
        }
        return SmokeStepResult::done();
    }

  private:
    // Items the renderer drew before the step hid everything.
    uint32_t m_renderablesBeforeHide{0};
};

} // namespace

std::unique_ptr<SmokeStep> makeHideSmokeStep() {
    return std::make_unique<HideSmokeStep>();
}

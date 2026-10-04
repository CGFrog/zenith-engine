// The viewport chrome: open the sidebar, hide and restore the tool shelf, and
// render through a narrower lens with tighter clipping; and the Info area
// has received the engine's log.

#include "editor/panels/InfoPanel.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <glm/trigonometric.hpp>

namespace {

class ViewportChromeSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "viewport chrome";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t) override {
        EditorSmokeHooks& editor = context.editor;
        ViewportPanel* viewport = editor.viewports().active();
        for (const char* operatorId : {"view3d.toggle_sidebar", "view3d.toggle_toolbar", "view3d.toggle_toolbar"}) {
            editor.invokeOperator(operatorId, {}, KeymapRegion::Viewport);
        }
        if (viewport == nullptr) {
            return SmokeStepResult::failed("no viewport is open");
        }
        viewport->camera().setVerticalFovRadians(glm::radians(45.0f));
        viewport->camera().setClipPlanes(0.05f, 500.0f);
        if (!viewport->chrome().sidebarVisible || !viewport->chrome().toolbarVisible) {
            return SmokeStepResult::failed("the sidebar and toolbar should be visible");
        }
        // The renderer logs at least its validation-layer state at startup.
        if (editor.infoLog().count(InfoSource::Engine) == 0) {
            return SmokeStepResult::failed("the Info panel received no engine log entries");
        }
        return SmokeStepResult::done();
    }
};

} // namespace

std::unique_ptr<SmokeStep> makeViewportChromeSmokeStep() {
    return std::make_unique<ViewportChromeSmokeStep>();
}

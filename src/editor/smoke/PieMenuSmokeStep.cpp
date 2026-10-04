// Pie menus: open the Shading pie with wm.call_pie (pies turned on for the
// step), choose Wireframe from it as a release over that item would, check
// the active viewport's shading, then restore it. Skipped without a viewport.

#include "editor/menus/EditorMenus.hpp"
#include "editor/menus/PieMenu.hpp"
#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

class PieMenuSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "Shading pie";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        EditorMenus& menus = editor.menus();
        ViewportPanel* viewport = editor.viewports().active();
        switch (stepFrame) {
        case 0:
            if (viewport == nullptr) {
                return SmokeStepResult::done();
            }
            m_shading = viewport->chrome().shading;
            // The session applies the preference again on the next update.
            menus.setPieMenusEnabled(true);
            editor.invokeOperator("wm.call_pie", nlohmann::json{{"menu", shadingPieMenuId}}.dump(),
                                  KeymapRegion::Viewport);
            if (!menus.pieOpen()) {
                return SmokeStepResult::failed("wm.call_pie did not open the Shading pie");
            }
            return SmokeStepResult::running();
        case 1: {
            const std::optional<ResolvedMenu> pie =
                menus.resolve(menus.pieRequest(), editor.operatorContext(KeymapRegion::Viewport));
            const std::vector<MenuItem> items = pie ? pieItems(*pie) : std::vector<MenuItem>{};
            const auto wireframe =
                std::ranges::find_if(items, [](const MenuItem& item) { return item.label == "Wireframe"; });
            if (!menus.pieOpen() || wireframe == items.end()) {
                return SmokeStepResult::failed("the Shading pie did not offer Wireframe");
            }
            editor.invokeOperator(wireframe->operatorId, wireframe->properties, KeymapRegion::Viewport);
            menus.closePie();
            if (viewport == nullptr || viewport->chrome().shading != ViewportShading::Wireframe) {
                return SmokeStepResult::failed("choosing Wireframe did not change the viewport's shading");
            }
            return SmokeStepResult::running();
        }
        default:
            if (menus.pieOpen()) {
                return SmokeStepResult::failed("the Shading pie did not close");
            }
            if (viewport != nullptr) {
                viewport->chrome().shading = m_shading;
            }
            return SmokeStepResult::done();
        }
    }

  private:
    // The active viewport's shading before the step chose Wireframe.
    ViewportShading m_shading{};
};

} // namespace

std::unique_ptr<SmokeStep> makePieMenuSmokeStep() {
    return std::make_unique<PieMenuSmokeStep>();
}

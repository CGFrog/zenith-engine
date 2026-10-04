// Menus: open the Add menu with wm.call_menu, choose Empty from its search as
// typing "empty" would, check the entity was added and selected, then undo it.

#include "editor/interaction/Selection.hpp"
#include "editor/menus/EditorMenus.hpp"
#include "editor/menus/MenuRegistry.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace {

class MenuSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "Add menu";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        EditorMenus& menus = editor.menus();
        const auto entityCount = [&context] {
            return context.app.scene.registry().view<const SceneEntityIdComponent>().size();
        };
        switch (stepFrame) {
        case 0:
            m_entityCount = entityCount();
            m_marker = editor.history().marker();
            editor.invokeOperator("wm.call_menu", nlohmann::json{{"menu", addMenuId}}.dump(), KeymapRegion::Viewport);
            return SmokeStepResult::running();
        case 1: {
            const std::optional<ResolvedMenu> menu =
                menus.resolve(menus.request(), editor.operatorContext(KeymapRegion::Global));
            const std::vector<MenuSearchResult> results =
                menu ? searchMenu(*menu, "empty") : std::vector<MenuSearchResult>{};
            if (!menus.isOpen() || results.empty()) {
                return SmokeStepResult::failed("wm.call_menu did not open an Add menu offering Empty");
            }
            editor.invokeOperator(results.front().item->operatorId, results.front().item->properties,
                                  KeymapRegion::Global);
            menus.close();
            if (entityCount() != m_entityCount + 1 || editor.selection().size() != 1) {
                return SmokeStepResult::failed("choosing Empty did not add and select one entity");
            }
            return SmokeStepResult::running();
        }
        default:
            if (menus.isOpen()) {
                return SmokeStepResult::failed("the Add menu did not close");
            }
            editor.undoSince(m_marker);
            if (entityCount() != m_entityCount) {
                return SmokeStepResult::failed("undo did not remove the added entity");
            }
            return SmokeStepResult::done();
        }
    }

  private:
    // Entities in the scene before the step added one, and the history
    // before it.
    size_t m_entityCount{0};
    CommandHistoryMarker m_marker;
};

} // namespace

std::unique_ptr<SmokeStep> makeMenuSmokeStep() {
    return std::make_unique<MenuSmokeStep>();
}

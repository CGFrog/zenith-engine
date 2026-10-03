#include "ViewportSelectOperators.hpp"

#include "editor/panels/ViewportPanel.hpp"
#include "editor/panels/ViewportRegistry.hpp"

#include <utility>

namespace {
constexpr const char* noViewportError = "No viewport is open";

struct ClickSelectProperties {
    // View pixels from the image's top-left corner; negative uses the mouse.
    float x{-1.0f};
    float y{-1.0f};
    bool extend{false};
};

struct RegionSelectProperties {
    SelectionSetMode mode{SelectionSetMode::Auto};
    bool rightDrag{false};
};

Fields<ClickSelectProperties> clickSelectFields() {
    return std::move(Fields<ClickSelectProperties>()
                         .add("x", &ClickSelectProperties::x)
                         .add("y", &ClickSelectProperties::y)
                         .add("extend", &ClickSelectProperties::extend));
}

Fields<RegionSelectProperties> regionSelectFields(bool withRightDrag) {
    Fields<RegionSelectProperties> fields;
    fields.add("mode", &RegionSelectProperties::mode)
        .options({{"auto", "Auto", static_cast<int64_t>(SelectionSetMode::Auto)},
                  {"set", "Set", static_cast<int64_t>(SelectionSetMode::Set)},
                  {"extend", "Extend", static_cast<int64_t>(SelectionSetMode::Extend)},
                  {"subtract", "Subtract", static_cast<int64_t>(SelectionSetMode::Subtract)}});
    if (withRightDrag) {
        fields.add("right_drag", &RegionSelectProperties::rightDrag);
    }
    return fields;
}

// An operator acting on the active viewport.
template <typename Run>
Operator viewportOperator(ViewportRegistry& viewports, Operator op, Run run,
                          std::function<bool(const ViewportPanel&, const OperatorContext&)> extraPoll = {}) {
    op.poll = [&viewports, extraPoll = std::move(extraPoll)](const OperatorContext& context) {
        const ViewportPanel* viewport = viewports.active();
        return viewport != nullptr && (!extraPoll || extraPoll(*viewport, context));
    };
    op.execute = [&viewports, run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        ViewportPanel* viewport = viewports.active();
        if (viewport == nullptr) {
            return OperatorResult::failed(noViewportError);
        }
        return run(*viewport, context, properties);
    };
    return op;
}

Operator clickSelectOperator(ViewportRegistry& viewports, std::string id, std::string label, std::string description,
                             bool cycle) {
    return viewportOperator(viewports,
                            {.id = std::move(id),
                             .label = std::move(label),
                             .description = std::move(description),
                             .flags = OperatorFlags::Internal,
                             .properties = OperatorProperties::make(ClickSelectProperties{}, clickSelectFields())},
                            [cycle](ViewportPanel& viewport, OperatorContext&, OperatorProperties& properties) {
                                const ClickSelectProperties values = properties.get<ClickSelectProperties>() != nullptr
                                                                         ? *properties.get<ClickSelectProperties>()
                                                                         : ClickSelectProperties{};
                                const glm::vec2 point = values.x < 0.0f || values.y < 0.0f
                                                            ? viewport.mouseInView()
                                                            : glm::vec2(values.x, values.y);
                                viewport.requestClickSelect(point, values.extend, cycle);
                                return OperatorResult::finished();
                            });
}

bool noSelectTool(const ViewportPanel& viewport, const OperatorContext&) {
    return !viewport.selectToolActive();
}
} // namespace

std::vector<Operator> viewportSelectOperators(ViewportRegistry& viewports) {
    std::vector<Operator> operators;
    operators.push_back(clickSelectOperator(viewports, "view3d.select", "Select",
                                            "Select the object under a point of the viewport", false));
    operators.push_back(clickSelectOperator(viewports, "view3d.select_cycle", "Select Cycle",
                                            "Select the next of the objects under a point, front to back", true));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.select_box",
         .label = "Box Select",
         .description = "Drag a rectangle to select the objects inside it",
         .properties = OperatorProperties::make(RegionSelectProperties{}, regionSelectFields(false))},
        [](ViewportPanel& viewport, OperatorContext&, OperatorProperties& properties) {
            const RegionSelectProperties* values = properties.get<RegionSelectProperties>();
            viewport.beginBoxSelect(values != nullptr ? values->mode : SelectionSetMode::Auto);
            return OperatorResult::finished();
        },
        noSelectTool));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.select_lasso",
         .label = "Lasso Select",
         .description = "Draw an outline to select the objects inside it",
         .properties = OperatorProperties::make(RegionSelectProperties{}, regionSelectFields(true))},
        [](ViewportPanel& viewport, OperatorContext&, OperatorProperties& properties) {
            const RegionSelectProperties* values = properties.get<RegionSelectProperties>();
            viewport.beginLassoSelect(values != nullptr ? values->mode : SelectionSetMode::Auto,
                                      values != nullptr && values->rightDrag);
            return OperatorResult::finished();
        },
        noSelectTool));
    operators.push_back(viewportOperator(
        viewports,
        {.id = "view3d.localview",
         .label = "Local View",
         .description = "Show only the selected objects in this viewport, or show everything again"},
        [](ViewportPanel& viewport, OperatorContext& context, OperatorProperties&) {
            if (viewport.localViewActive()) {
                viewport.exitLocalView();
                return OperatorResult::finished();
            }
            if (context.selection.empty()) {
                return OperatorResult::cancelled("Select objects to show in local view");
            }
            if (!viewport.enterLocalView(context.editing.scene(), context.editing.assets(), context.selection.ids())) {
                return OperatorResult::cancelled("The selected objects no longer exist");
            }
            return OperatorResult::finished();
        },
        [](const ViewportPanel& viewport, const OperatorContext& context) {
            return viewport.localViewActive() || !context.selection.empty();
        }));
    return operators;
}

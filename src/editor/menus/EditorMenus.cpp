#include "EditorMenus.hpp"

#include "editor/panels/EditorContext.hpp"

#include <algorithm>
#include <utility>

namespace {
constexpr const char* popupId = "EditorMenuPopup";

OperatorContext operatorContextFor(EditorContext& context) {
    return {.editing = context.editing,
            .selection = context.selection,
            .mode = context.mode,
            .region = KeymapRegion::Global,
            .app = &context.app};
}

OperatorResult openMenu(EditorMenus& menus, MenuRequest request) {
    if (const MenuResult result = menus.open(std::move(request)); !result) {
        return OperatorResult::failed(result.error);
    }
    return OperatorResult::finished();
}
} // namespace

std::optional<ResolvedMenu> EditorMenus::resolve(const MenuRequest& request, const OperatorContext& context) const {
    std::optional<ResolvedMenu> menu = m_registry.resolve(request.menu, context);
    if (!menu || request.category.empty()) {
        return menu;
    }
    const auto category = std::ranges::find_if(
        menu->items, [&request](const MenuItem& item) { return item.isGroup() && item.label == request.category; });
    std::vector<MenuItem> items;
    if (category != menu->items.end()) {
        items = std::move(category->children);
    }
    menu->items = std::move(items);
    return menu;
}

MenuResult EditorMenus::open(MenuRequest request) {
    if (m_registry.find(request.menu) == nullptr) {
        return {.error = "No menu with id '" + request.menu + "' is registered"};
    }
    m_request = std::move(request);
    m_popup.open();
    return {.success = true};
}

void EditorMenus::close() {
    m_popup.close();
}

bool EditorMenus::isOpen() const {
    return m_popup.isOpen();
}

void EditorMenus::draw(EditorContext& context) {
    if (!m_popup.isOpen()) {
        return;
    }
    const std::optional<ResolvedMenu> menu = resolve(m_request, operatorContextFor(context));
    if (!menu) {
        m_popup.close();
        m_popup.draw(popupId, context, ResolvedMenu{});
        return;
    }
    if (const std::optional<MenuChoice> chosen = m_popup.draw(popupId, context, *menu)) {
        context.invoke(chosen->operatorId, chosen->properties);
    }
}

OperatorRegistrationResult registerMenuOperators(OperatorRegistry& registry, EditorMenus& menus) {
    Operator callMenu{.id = "wm.call_menu",
                      .label = "Call Menu",
                      .description = "Open a menu at the mouse",
                      .flags = OperatorFlags::Internal,
                      .properties = OperatorProperties::make(
                          CallMenuProperties{}, std::move(Fields<CallMenuProperties>()
                                                              .add("menu", &CallMenuProperties::menu)
                                                              .add("category", &CallMenuProperties::category))),
                      .poll = {},
                      .execute = [&menus](OperatorContext&, OperatorProperties& properties) {
                          const CallMenuProperties* values = properties.get<CallMenuProperties>();
                          if (values == nullptr) {
                              return OperatorResult::failed("Operator properties have the wrong type");
                          }
                          return openMenu(menus, {.menu = values->menu, .category = values->category});
                      }};
    Operator addMenu{
        .id = "object.add_menu",
        .label = "Add...",
        .description = "Open the Add menu at the mouse",
        .flags = OperatorFlags::None,
        .properties = {},
        .poll = [](const OperatorContext& context) { return context.editMode(); },
        .execute = [&menus](OperatorContext&,
                            OperatorProperties&) { return openMenu(menus, {.menu = addMenuId, .category = {}}); }};
    for (Operator* op : {&callMenu, &addMenu}) {
        if (OperatorRegistrationResult result = registry.add(std::move(*op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

#include "MenuPopup.hpp"

#include "editor/operators/OperatorSearch.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace {
constexpr int visibleResults = 12;

// Where a menu item's label starts, measured from its item rectangle.
float menuLabelOffset() {
    return std::floor(ImGui::GetStyle().ItemSpacing.x * 0.5f);
}

void drawItems(EditorContext& context, const std::vector<MenuItem>& items, bool sectionHeadings,
               std::optional<MenuChoice>& chosen) {
    const bool anyIcon = std::ranges::any_of(items, [](const MenuItem& item) { return item.icon.has_value(); });
    for (size_t index = 0; index < items.size(); ++index) {
        const MenuItem& item = items[index];
        ImGui::PushID(static_cast<int>(index));
        if (item.isGroup() && sectionHeadings) {
            ImGui::SeparatorText(item.label.c_str());
            drawItems(context, item.children, false, chosen);
        } else if (item.isGroup()) {
            // Group icons are not drawn: an open submenu leaves ImGui drawing
            // into its own window.
            if (ImGui::BeginMenu(item.label.c_str())) {
                drawItems(context, item.children, false, chosen);
                ImGui::EndMenu();
            }
        } else {
            const std::string label = (anyIcon ? std::string(widgets::IconLabelPadding()) : std::string()) + item.label;
            const std::string shortcut = context.shortcut(item.operatorId, item.properties);
            const bool enabled = context.canInvoke(item.operatorId);
            if (ImGui::MenuItem(label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled)) {
                chosen = MenuChoice{.operatorId = item.operatorId, .properties = item.properties};
            }
            if (item.icon) {
                widgets::ItemIcon(*item.icon, menuLabelOffset(), {.disabled = !enabled});
            }
        }
        ImGui::PopID();
    }
}

// Search results the user can run now.
std::vector<MenuSearchResult> runnableMatches(EditorContext& context, const ResolvedMenu& menu,
                                              std::string_view query) {
    std::vector<MenuSearchResult> results = searchMenu(menu, query);
    std::erase_if(results,
                  [&context](const MenuSearchResult& result) { return !context.canInvoke(result.item->operatorId); });
    return results;
}
} // namespace

std::optional<MenuChoice> drawMenuItems(EditorContext& context, const ResolvedMenu& menu) {
    std::optional<MenuChoice> chosen;
    drawItems(context, menu.items, menu.layout == MenuLayout::Sections, chosen);
    if (menu.items.empty()) {
        ImGui::TextDisabled("Nothing to show");
    }
    return chosen;
}

void MenuPopup::open() {
    m_openRequested = true;
    m_closeRequested = false;
}

void MenuPopup::close() {
    m_openRequested = false;
    m_closeRequested = true;
}

bool MenuPopup::isOpen() const {
    return m_open || m_openRequested;
}

std::optional<MenuChoice> MenuPopup::draw(const char* popupId, EditorContext& context, const ResolvedMenu& menu) {
    if (m_openRequested) {
        ImGui::OpenPopup(popupId);
        m_openRequested = false;
        m_focusQuery = true;
        m_query.clear();
        m_previousQuery.clear();
        m_highlighted = 0;
    }
    ImGui::SetNextWindowSizeConstraints({ImGui::GetFontSize() * 14.0f, 0.0f}, {FLT_MAX, FLT_MAX});
    m_open = ImGui::BeginPopup(popupId);
    if (!m_open) {
        m_closeRequested = false;
        return std::nullopt;
    }
    if (m_closeRequested) {
        m_closeRequested = false;
        m_open = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return std::nullopt;
    }

    if (!menu.title.empty()) {
        ImGui::TextDisabled("%s", menu.title.c_str());
    }
    if (m_focusQuery) {
        ImGui::SetKeyboardFocusHere();
        m_focusQuery = false;
    }
    const int highlightedBefore = m_highlighted;
    const int resultCount = m_query.empty() ? 0 : static_cast<int>(runnableMatches(context, menu, m_query).size());
    const bool entered = searchField("##MenuSearch", "Search...", m_query, m_highlighted, resultCount);
    if (m_query != m_previousQuery) {
        m_previousQuery = m_query;
        m_highlighted = 0;
    }

    std::optional<MenuChoice> chosen;
    if (m_query.empty()) {
        ImGui::Separator();
        chosen = drawMenuItems(context, menu);
    } else {
        chosen = drawSearchResults(context, menu, entered, m_highlighted != highlightedBefore);
    }
    if (chosen || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        m_open = false;
    }
    ImGui::EndPopup();
    return chosen;
}

std::optional<MenuChoice> MenuPopup::drawSearchResults(EditorContext& context, const ResolvedMenu& menu, bool entered,
                                                       bool highlightMoved) {
    const std::vector<MenuSearchResult> results = runnableMatches(context, menu, m_query);
    m_highlighted = std::clamp(m_highlighted, 0, std::max(static_cast<int>(results.size()) - 1, 0));
    std::optional<MenuChoice> chosen;
    if (entered && !results.empty()) {
        const MenuItem& item = *results[static_cast<size_t>(m_highlighted)].item;
        chosen = MenuChoice{.operatorId = item.operatorId, .properties = item.properties};
    }
    const float listHeight = ImGui::GetTextLineHeightWithSpacing() *
                             static_cast<float>(std::clamp(static_cast<int>(results.size()), 1, visibleResults));
    if (ImGui::BeginChild("##MenuSearchResults", {0.0f, listHeight})) {
        if (results.empty()) {
            ImGui::TextDisabled("Nothing matches");
        }
        for (size_t index = 0; index < results.size(); ++index) {
            const MenuSearchResult& result = results[index];
            const bool highlighted = static_cast<int>(index) == m_highlighted;
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(result.path.c_str(), highlighted)) {
                chosen = MenuChoice{.operatorId = result.item->operatorId, .properties = result.item->properties};
            }
            if (highlighted && highlightMoved) {
                ImGui::SetScrollHereY();
            }
            if (const std::string shortcut = context.shortcut(result.item->operatorId, result.item->properties);
                !shortcut.empty()) {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(shortcut.c_str()).x);
                ImGui::TextDisabled("%s", shortcut.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    return chosen;
}

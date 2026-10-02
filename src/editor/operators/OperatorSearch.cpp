#include "OperatorSearch.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>

namespace {
constexpr const char* searchPopup = "Operator Search";
constexpr int visibleResults = 12;

char lower(char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

bool wordStart(std::string_view text, size_t index) {
    if (index == 0) {
        return true;
    }
    const char previous = text[index - 1];
    return previous == ' ' || previous == '.' || previous == '_' || previous == '-' || previous == '/';
}

struct SearchNavigation {
    int* highlighted;
    int count;
};

// Up/Down arrive as history events, so the query field keeps focus.
int moveHighlight(ImGuiInputTextCallbackData* data) {
    auto* navigation = static_cast<SearchNavigation*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || navigation->count == 0) {
        return 0;
    }
    int& highlighted = *navigation->highlighted;
    if (data->EventKey == ImGuiKey_UpArrow) {
        highlighted = (highlighted + navigation->count - 1) % navigation->count;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        highlighted = (highlighted + 1) % navigation->count;
    }
    return 0;
}
} // namespace

std::optional<int> fuzzyMatchScore(std::string_view query, std::string_view text) {
    int score = 0;
    size_t position = 0;
    std::optional<size_t> previous;
    for (const char wanted : query) {
        if (wanted == ' ') {
            continue;
        }
        while (position < text.size() && lower(text[position]) != lower(wanted)) {
            ++position;
        }
        if (position == text.size()) {
            return std::nullopt;
        }
        score += 1;
        if (previous && *previous + 1 == position) {
            score += 5;
        }
        if (wordStart(text, position)) {
            score += 8;
        }
        previous = position;
        ++position;
    }
    return score;
}

std::vector<OperatorSearchEntry> searchOperators(const OperatorRegistry& registry, const Keymap& keymap,
                                                 const OperatorContext& context, std::string_view query) {
    std::vector<OperatorSearchEntry> entries;
    for (const Operator* op : registry.operators()) {
        const std::optional<int> labelScore = fuzzyMatchScore(query, op->label);
        const std::optional<int> idScore = fuzzyMatchScore(query, op->id);
        if ((!labelScore && !idScore) || hasFlag(op->flags, OperatorFlags::Internal) || !op->canRun(context)) {
            continue;
        }
        entries.push_back({.op = op,
                           .shortcut = keymap.shortcutLabel(op->id),
                           .score = std::max(labelScore.value_or(0), idScore.value_or(0))});
    }
    std::ranges::stable_sort(entries, [](const OperatorSearchEntry& left, const OperatorSearchEntry& right) {
        return left.score > right.score;
    });
    return entries;
}

void OperatorSearch::open() {
    m_openRequested = true;
}

bool OperatorSearch::isOpen() const {
    return m_open || m_openRequested;
}

std::optional<std::string> OperatorSearch::draw(const OperatorRegistry& registry, const Keymap& keymap,
                                                const OperatorContext& context) {
    if (m_openRequested) {
        ImGui::OpenPopup(searchPopup);
        m_openRequested = false;
        m_focusQuery = true;
        m_query.clear();
        m_previousQuery.clear();
        m_highlighted = 0;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        {viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.15f},
        ImGuiCond_Appearing, {0.5f, 0.0f});
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 32.0f, 0.0f}, ImGuiCond_Appearing);
    m_open = ImGui::BeginPopup(searchPopup);
    if (!m_open) {
        return std::nullopt;
    }

    std::vector<OperatorSearchEntry> entries = searchOperators(registry, keymap, context, m_query);
    if (m_focusQuery) {
        ImGui::SetKeyboardFocusHere();
        m_focusQuery = false;
    }
    const int highlightedBefore = m_highlighted;
    SearchNavigation navigation{.highlighted = &m_highlighted, .count = static_cast<int>(entries.size())};
    ImGui::SetNextItemWidth(-1.0f);
    const bool entered = ImGui::InputTextWithHint(
        "##OperatorSearchQuery", "Search operators...", &m_query,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, moveHighlight, &navigation);
    if (m_query != m_previousQuery) {
        entries = searchOperators(registry, keymap, context, m_query);
        m_previousQuery = m_query;
        m_highlighted = 0;
    }
    m_highlighted = std::clamp(m_highlighted, 0, std::max(static_cast<int>(entries.size()) - 1, 0));
    const bool highlightMoved = m_highlighted != highlightedBefore;

    std::optional<std::string> chosen;
    if (entered && !entries.empty()) {
        chosen = entries[static_cast<size_t>(m_highlighted)].op->id;
    }
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float listHeight =
        rowHeight * static_cast<float>(std::clamp(static_cast<int>(entries.size()), 1, visibleResults));
    if (ImGui::BeginChild("##OperatorSearchResults", {0.0f, listHeight})) {
        if (entries.empty()) {
            ImGui::TextDisabled("No operators match");
        }
        for (size_t index = 0; index < entries.size(); ++index) {
            const OperatorSearchEntry& entry = entries[index];
            const bool highlighted = static_cast<int>(index) == m_highlighted;
            ImGui::PushID(entry.op->id.c_str());
            if (ImGui::Selectable(entry.op->label.c_str(), highlighted)) {
                chosen = entry.op->id;
            }
            if (highlighted && highlightMoved) {
                ImGui::SetScrollHereY();
            }
            ImGui::SetItemTooltip("%s\n%s", entry.op->description.c_str(), entry.op->id.c_str());
            if (!entry.shortcut.empty()) {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(entry.shortcut.c_str()).x);
                ImGui::TextDisabled("%s", entry.shortcut.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (chosen || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        m_open = false;
    }
    ImGui::EndPopup();
    return chosen;
}

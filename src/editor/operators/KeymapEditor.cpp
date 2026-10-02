#include "KeymapEditor.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace {
constexpr ImVec4 warningColor{1.0f, 0.7f, 0.25f, 1.0f};

bool modifierKey(ImGuiKey key) {
    return key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper;
}

bool containsIgnoringCase(std::string_view text, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    return std::ranges::search(text, filter, [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
           }).begin() != text.end();
}

std::vector<KeymapBinding> bindingsOf(const std::vector<KeymapBinding>& bindings, std::string_view operatorId) {
    std::vector<KeymapBinding> result;
    for (const KeymapBinding& binding : bindings) {
        if (binding.operatorId == operatorId) {
            result.push_back(binding);
        }
    }
    return result;
}

// Whether the operator's bindings differ from its defaults, ignoring order.
bool customized(const Keymap& keymap, std::string_view operatorId) {
    std::vector<KeymapBinding> current = bindingsOf(keymap.bindings(), operatorId);
    const std::vector<KeymapBinding> defaults = bindingsOf(keymap.defaults(), operatorId);
    if (current.size() != defaults.size()) {
        return true;
    }
    for (const KeymapBinding& binding : defaults) {
        const auto found = std::ranges::find(current, binding);
        if (found == current.end()) {
            return true;
        }
        current.erase(found);
    }
    return false;
}

std::string bindingLabel(const KeymapBinding& binding) {
    std::string label = formatKeyChord(binding.chord);
    if (binding.trigger == KeyTrigger::DoubleTap) {
        label += " twice";
    }
    if (binding.region != KeymapRegion::Global) {
        label += std::string(" (") + keymapRegionName(binding.region) + ")";
    }
    return label;
}
} // namespace

bool KeymapEditor::capturing() const {
    return m_capture.has_value();
}

void KeymapEditor::captureRebind(size_t index) {
    m_capture = Capture{.bindingIndex = index};
    m_message.clear();
}

void KeymapEditor::captureNewBinding(std::string operatorId, KeymapRegion region) {
    m_capture = Capture{.operatorId = std::move(operatorId), .region = region};
    m_message.clear();
}

const std::string& KeymapEditor::message() const {
    return m_message;
}

bool KeymapEditor::updateCapture(Keymap& keymap) {
    const ImGuiIO& io = ImGui::GetIO();
    for (int value = ImGuiKey_NamedKey_BEGIN; value < ImGuiKey_GamepadStart; ++value) {
        const ImGuiKey key = static_cast<ImGuiKey>(value);
        if (modifierKey(key) || !ImGui::IsKeyPressed(key, false)) {
            continue;
        }
        const Capture capture = std::move(*m_capture);
        m_capture.reset();
        if (key == ImGuiKey_Escape && io.KeyMods == ImGuiMod_None) {
            m_message.clear();
            return false;
        }
        const ImGuiKeyChord chord = io.KeyMods | key;
        const KeymapResult result =
            capture.bindingIndex
                ? keymap.rebind(*capture.bindingIndex, chord)
                : keymap.add({.operatorId = capture.operatorId, .region = capture.region, .chord = chord});
        m_message = result ? std::string{} : result.error;
        return result.success;
    }
    return false;
}

bool KeymapEditor::drawOperator(Keymap& keymap, const std::string& operatorId, const std::string& label,
                                const std::string& description) {
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::PushID(operatorId.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SetItemTooltip("%s\n%s", description.c_str(), operatorId.c_str());

    ImGui::TableNextColumn();
    std::optional<size_t> removed;
    const std::vector<KeymapBinding>& bindings = keymap.bindings();
    for (size_t index = 0; index < bindings.size(); ++index) {
        const KeymapBinding& binding = bindings[index];
        if (binding.operatorId != operatorId) {
            continue;
        }
        ImGui::PushID(static_cast<int>(index));
        const bool capturingThis = m_capture && m_capture->bindingIndex == index;
        if (ImGui::Button(capturingThis ? "Press a key..." : bindingLabel(binding).c_str())) {
            captureRebind(index);
        }
        if (!binding.properties.empty()) {
            ImGui::SetItemTooltip("Properties: %s", binding.properties.c_str());
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::SmallButton("x")) {
            removed = index;
        }
        ImGui::SetItemTooltip("Remove this shortcut");
        ImGui::SameLine();
        ImGui::PopID();
    }
    const bool capturingNew = m_capture && !m_capture->bindingIndex && m_capture->operatorId == operatorId;
    if (ImGui::SmallButton(capturingNew ? "Press a key..." : "+")) {
        captureNewBinding(operatorId, m_newBindingRegion);
    }
    ImGui::SetItemTooltip("Add a %s shortcut", keymapRegionName(m_newBindingRegion));
    if (removed) {
        m_capture.reset();
        keymap.remove(*removed);
        changed = true;
    }

    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!customized(keymap, operatorId));
    if (ImGui::SmallButton("Reset")) {
        m_capture.reset();
        const KeymapResult result = keymap.resetOperator(operatorId);
        m_message = result.warnings.empty() ? std::string{} : result.warnings.front();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

bool KeymapEditor::draw(bool& open, Keymap& keymap, const OperatorRegistry& registry) {
    bool changed = m_capture ? updateCapture(keymap) : false;
    if (!open) {
        m_capture.reset();
        return changed;
    }
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 44.0f, ImGui::GetFontSize() * 34.0f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Keymap", &open)) {
        for (const std::string& problem : keymap.defaultProblems()) {
            ImGui::TextColored(warningColor, "%s", problem.c_str());
        }
        if (!m_message.empty()) {
            ImGui::TextColored(warningColor, "%s", m_message.c_str());
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        ImGui::InputTextWithHint("##KeymapFilter", "Filter operators...", &m_filter);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::BeginCombo("New shortcuts", keymapRegionName(m_newBindingRegion))) {
            for (const KeymapRegion region : {KeymapRegion::Global, KeymapRegion::Viewport, KeymapRegion::Outliner}) {
                if (ImGui::Selectable(keymapRegionName(region), region == m_newBindingRegion)) {
                    m_newBindingRegion = region;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset All")) {
            m_capture.reset();
            m_message.clear();
            keymap.resetToDefaults();
            changed = true;
        }

        constexpr ImGuiTableFlags tableFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##Keymap", 3, tableFlags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Operator", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Shortcuts", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (const Operator* op : registry.operators()) {
                if (containsIgnoringCase(op->label, m_filter) || containsIgnoringCase(op->id, m_filter)) {
                    changed |= drawOperator(keymap, op->id, op->label, op->description);
                }
            }
            // Bindings saved for operators a module has not registered (yet).
            std::vector<std::string> unknown;
            for (const KeymapBinding& binding : keymap.bindings()) {
                if (registry.find(binding.operatorId) == nullptr &&
                    std::ranges::find(unknown, binding.operatorId) == unknown.end() &&
                    containsIgnoringCase(binding.operatorId, m_filter)) {
                    unknown.push_back(binding.operatorId);
                }
            }
            for (const std::string& operatorId : unknown) {
                changed |= drawOperator(keymap, operatorId, operatorId + " (not registered)",
                                        "No loaded module registers this operator");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    return changed;
}

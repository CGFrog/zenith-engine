#include "SystemClipboard.hpp"

#include <imgui.h>

#include <utility>

std::string SystemClipboard::text() const {
    if (ImGui::GetCurrentContext() != nullptr) {
        if (const char* system = ImGui::GetClipboardText(); system != nullptr && *system != '\0') {
            return system;
        }
    }
    return m_lastWritten;
}

void SystemClipboard::setText(std::string text) {
    m_lastWritten = std::move(text);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::SetClipboardText(m_lastWritten.c_str());
    }
}

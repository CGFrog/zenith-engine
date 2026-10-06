#include "FileDropQueue.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <iterator>
#include <utility>

void FileDropQueue::add(std::span<const DroppedFile> drops) {
    m_drops.insert(m_drops.end(), drops.begin(), drops.end());
}

bool FileDropQueue::empty() const {
    return m_drops.empty();
}

std::span<const DroppedFile> FileDropQueue::pending() const {
    return m_drops;
}

std::vector<std::filesystem::path> FileDropQueue::claimWithin(glm::vec2 minimum, glm::vec2 maximum,
                                                              const std::function<bool(glm::vec2)>& lands) {
    std::vector<std::filesystem::path> claimed;
    std::erase_if(m_drops, [&](const DroppedFile& drop) {
        const bool inside = drop.position.x >= minimum.x && drop.position.y >= minimum.y &&
                            drop.position.x < maximum.x && drop.position.y < maximum.y &&
                            (!lands || lands(drop.position));
        if (inside) {
            claimed.push_back(drop.path);
        }
        return inside;
    });
    return claimed;
}

std::vector<DroppedFile> FileDropQueue::takeAll() {
    return std::exchange(m_drops, {});
}

bool dropLandsInCurrentWindow(glm::vec2 position) {
    ImGuiWindow* hovered = nullptr;
    ImGui::FindHoveredWindowEx({position.x, position.y}, false, &hovered, nullptr);
    const ImGuiWindow* current = ImGui::GetCurrentWindowRead();
    // No window there (the first frame): the rectangle alone decides.
    return hovered == nullptr || current == nullptr || hovered->RootWindowDockTree == current->RootWindowDockTree;
}

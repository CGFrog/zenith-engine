#pragma once

#include "engine/input/InputManager.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

// Files and folders dropped onto the editor window from the operating system
// in one frame (InputState::droppedFiles), handed to the window they landed
// in. The session fills it in update; while drawing, the Asset Browser and the
// viewports claim the drops inside their rectangles that landed on their own
// window (dropLandsInCurrentWindow: a prefab window floating over a level
// viewport takes the drops on it), and the session reports whatever no window
// took after the frame.
//
// Positions are in window coordinates, which is the space ImGui draws in.
class FileDropQueue {
  public:
    void add(std::span<const DroppedFile> drops);
    bool empty() const;
    // The pending drops, for a window that needs to know where they landed
    // before it claims them.
    std::span<const DroppedFile> pending() const;
    // Removes and returns the paths of the drops whose position lies in the
    // rectangle [minimum, maximum) and, when lands is given, for which it
    // holds.
    std::vector<std::filesystem::path> claimWithin(glm::vec2 minimum, glm::vec2 maximum,
                                                   const std::function<bool(glm::vec2)>& lands = {});
    // Removes and returns every pending drop.
    std::vector<DroppedFile> takeAll();

  private:
    std::vector<DroppedFile> m_drops;
};

// Whether position (window coordinates) lies on the ImGui window being drawn:
// the topmost window there, as last frame stacked them, belongs to the same
// dock tree (the window itself, a child, or a panel docked with it).
bool dropLandsInCurrentWindow(glm::vec2 position);

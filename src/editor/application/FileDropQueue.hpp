#pragma once

#include "engine/input/InputManager.hpp"

#include <filesystem>
#include <span>
#include <vector>

// Files and folders dropped onto the editor window from the operating system
// in one frame (InputState::droppedFiles), handed to the window they landed
// in. The session fills it in update; while drawing, the Asset Browser and the
// viewports claim the drops inside their rectangles, and the session reports
// whatever no window took after the frame.
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
    // rectangle [minimum, maximum).
    std::vector<std::filesystem::path> claimWithin(glm::vec2 minimum, glm::vec2 maximum);
    // Removes and returns every pending drop.
    std::vector<DroppedFile> takeAll();

  private:
    std::vector<DroppedFile> m_drops;
};

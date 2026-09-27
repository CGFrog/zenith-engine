#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

struct EditorShortcutResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class EditorShortcuts {
  public:
    EditorShortcutResult bind(std::string action, std::string chord);
    const std::string& chord(std::string_view action) const;

  private:
    std::unordered_map<std::string, std::string> m_actions;
    std::unordered_map<std::string, std::string> m_chords;
};

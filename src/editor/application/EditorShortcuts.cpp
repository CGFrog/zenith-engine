#include "EditorShortcuts.hpp"

EditorShortcutResult EditorShortcuts::bind(std::string action, std::string chord) {
    if (action.empty() || chord.empty()) {
        return {.error = "Shortcut action and chord must not be empty"};
    }
    if (const auto found = m_chords.find(chord); found != m_chords.end() && found->second != action) {
        return {.error = "Shortcut '" + chord + "' is already assigned to " + found->second};
    }
    if (const auto previous = m_actions.find(action); previous != m_actions.end()) {
        m_chords.erase(previous->second);
    }
    m_chords[chord] = action;
    m_actions[std::move(action)] = std::move(chord);
    return {.success = true};
}

const std::string& EditorShortcuts::chord(std::string_view action) const {
    static const std::string empty;
    const auto found = m_actions.find(std::string(action));
    return found == m_actions.end() ? empty : found->second;
}

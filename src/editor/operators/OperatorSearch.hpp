#pragma once

#include "editor/operators/Keymap.hpp"
#include "editor/operators/OperatorRegistry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Scores text against a fuzzy query: every query character must appear in
// order, ignoring case. Higher is better; consecutive runs and matches at word
// starts score more. nullopt when text does not match. An empty query
// matches everything with score 0.
std::optional<int> fuzzyMatchScore(std::string_view query, std::string_view text);

// A full-width search field for a popup listing resultCount results: Up and
// Down move highlighted through them (wrapping) while the field keeps
// keyboard focus. Returns whether Enter was pressed.
bool searchField(const char* id, const char* hint, std::string& query, int& highlighted, int resultCount);

struct OperatorSearchEntry {
    const Operator* op{nullptr};
    std::string shortcut;
    int score{0};
};

// Operators that are not Internal, whose poll passes in context and whose label or id matches query,
// best match first (registration order breaks ties).
std::vector<OperatorSearchEntry> searchOperators(const OperatorRegistry& registry, const Keymap& keymap,
                                                 const OperatorContext& context, std::string_view query);

// The F3 popup: a fuzzy search over runnable operators showing their
// shortcuts. Up/Down move the highlight, Enter or a click runs it, Escape
// closes. Draw it at the top level of the frame, outside other windows.
class OperatorSearch {
  public:
    void open();
    bool isOpen() const;
    // Returns the id of the operator the user chose this frame.
    std::optional<std::string> draw(const OperatorRegistry& registry, const Keymap& keymap,
                                    const OperatorContext& context);

  private:
    std::string m_query;
    std::string m_previousQuery;
    int m_highlighted{0};
    bool m_openRequested{false};
    bool m_focusQuery{false};
    bool m_open{false};
};

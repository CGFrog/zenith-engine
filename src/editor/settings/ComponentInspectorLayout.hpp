#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class ComponentMoveDirection : uint8_t {
    Up,
    Down,
    Top,
    Bottom,
};

struct ComponentInspectorLayoutResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// How the component inspector presents component types, per type key rather
// than per entity: which sections are collapsed and the order sections are
// listed in. Kept in component-inspector.json in the editor's settings
// directory; it is user data, never scene data, and survives game module
// reloads because it names types by key.
//
// The display order is a list of keys. Types it names come first, in that
// order; the rest follow in the order the caller gave them (registry order).
class ComponentInspectorLayout {
  public:
    // Sections are expanded until collapsed.
    bool collapsed(std::string_view componentKey) const;
    void setCollapsed(std::string_view componentKey, bool collapsed);

    // keys (in registry order) sorted into display order.
    std::vector<std::string> sorted(std::vector<std::string> keys) const;
    // Moves componentKey within displayed (keys in their current display
    // order, as sorted() returns them) and records the new order, keeping
    // the relative positions of stored keys that are not displayed. Returns
    // false, changing nothing, when the key is not displayed or is already at
    // that end.
    bool move(std::string_view componentKey, ComponentMoveDirection direction, std::span<const std::string> displayed);

    const std::vector<std::string>& order() const;
    const std::vector<std::string>& collapsedKeys() const;

    // Whether anything changed since the last call, which clears the flag,
    // so the owner knows when to save.
    bool takeChanged();

    // A missing file leaves the defaults and succeeds. An unreadable or
    // invalid file also leaves the defaults, and reports why.
    ComponentInspectorLayoutResult load(const std::filesystem::path& path);
    ComponentInspectorLayoutResult save(const std::filesystem::path& path) const;

    bool operator==(const ComponentInspectorLayout& other) const {
        return m_order == other.m_order && m_collapsed == other.m_collapsed;
    }

  private:
    std::vector<std::string> m_order;
    std::vector<std::string> m_collapsed;
    bool m_changed{false};
};

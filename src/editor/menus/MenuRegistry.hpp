#pragma once

// Menus as data.

#include "editor/operators/Operator.hpp"
#include "editor/theme/PixelIconBitmaps.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Menus as data. A menu is a named list of entries, each running an operator
// with JSON properties (so the operator's poll enables it and the keymap
// labels it), optionally nested in categories, plus dynamic sections whose
// entries are rebuilt each time the menu is shown. Menu widgets
// (menus/MenuPopup) draw a resolved snapshot, so nothing a widget holds points
// into the registry or into a module's code.

// Who added a menu, entry, or section. The editor registers its own with
// editorMenuOwner; any other client (a game module) takes a fresh token from
// MenuRegistry::createOwner and calls removeOwner with it before its code
// unloads.
struct MenuOwner {
    uint32_t value{0};

    friend bool operator==(MenuOwner, MenuOwner) = default;
};

inline constexpr MenuOwner editorMenuOwner{0};

enum class MenuLayout : uint8_t {
    // Every category is a submenu (the Add menu).
    Submenus,
    // Top-level categories are headed sections listed inline and deeper
    // categories are submenus (Add Component).
    Sections,
};

struct MenuDefinition {
    // Stable id that wm.call_menu and keymap bindings name, for example
    // "VIEW3D_MT_add". Changing it breaks saved user keymaps.
    std::string id;
    // Shown at the top of the menu.
    std::string title;
    MenuLayout layout{MenuLayout::Submenus};
};

struct MenuEntry {
    std::string label;
    // The operator the entry runs, with a JSON object overriding its default
    // properties (empty for none). Its poll decides whether the entry is
    // enabled.
    std::string operatorId;
    std::string properties;
    // Another registered menu shown as this entry's submenu, instead of an
    // operator.
    std::string submenu;
    std::optional<EditorIcon> icon;
    // The categories the entry is nested in, outermost first; empty places it
    // at the top level. Entries naming the same category share it.
    std::vector<std::string> category;
};

// Appends the entries a dynamic section shows now, for content that changes
// while the editor runs (for example component types from a game module).
using MenuSectionFill = std::function<void(const OperatorContext& context, std::vector<MenuEntry>& entries)>;

struct MenuResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// One item of a menu as it is shown now: an operator leaf, or a category or
// submenu with children.
struct MenuItem {
    std::string label;
    std::optional<EditorIcon> icon;
    // Empty for a category or submenu.
    std::string operatorId;
    std::string properties;
    std::vector<MenuItem> children;

    bool isGroup() const {
        return operatorId.empty();
    }
};

// A snapshot of a menu built by MenuRegistry::resolve. It owns copies only,
// so it stays valid after the registry changes.
struct ResolvedMenu {
    std::string id;
    std::string title;
    MenuLayout layout{MenuLayout::Submenus};
    std::vector<MenuItem> items;
};

class MenuRegistry {
  public:
    // A token no other client has, for removeOwner.
    MenuOwner createOwner();
    // Rejects an empty or duplicate id.
    MenuResult addMenu(MenuOwner owner, MenuDefinition menu);
    // Appends an entry to a registered menu. Rejects an entry without a
    // label, with both or neither of an operator and a submenu, naming a
    // submenu that is not registered, or whose properties are not a JSON
    // object.
    MenuResult addEntry(MenuOwner owner, std::string_view menu, MenuEntry entry);
    // Appends a dynamic section to a registered menu. Entries and sections
    // are shown in the order they were added.
    MenuResult addSection(MenuOwner owner, std::string_view menu, MenuSectionFill fill);
    // Removes every menu, entry, and section owner added and returns how many
    // there were. Entries and sections other owners added to a removed menu
    // are kept and show again if the menu is registered again (a module
    // reloading).
    size_t removeOwner(MenuOwner owner);

    const MenuDefinition* find(std::string_view menu) const;
    // The menu as it is shown now, with its sections filled for context and
    // its submenus expanded; nullopt when no menu has the id. Dynamic entries
    // that are malformed, empty categories, and submenus that are missing or
    // nest too deeply (a cycle) are left out.
    std::optional<ResolvedMenu> resolve(std::string_view menu, const OperatorContext& context) const;

  private:
    struct RegisteredMenu {
        MenuOwner owner;
        MenuDefinition definition;
    };

    // An entry or, when fill is set, a dynamic section.
    struct Contribution {
        MenuOwner owner;
        std::string menu;
        MenuEntry entry;
        MenuSectionFill fill;
    };

    void resolveInto(std::string_view menu, const OperatorContext& context, int depth,
                     std::vector<MenuItem>& items) const;
    void insert(const MenuEntry& entry, const OperatorContext& context, int depth, std::vector<MenuItem>& items) const;

    std::vector<RegisteredMenu> m_menus;
    std::vector<Contribution> m_contributions;
    uint32_t m_nextOwner{editorMenuOwner.value + 1};
};

struct MenuSearchResult {
    // Points into the searched menu.
    const MenuItem* item{nullptr};
    // The categories and label, for example "Light > Sun".
    std::string path;
    int score{0};
};

// The operator leaves of menu whose label or path matches query (see
// fuzzyMatchScore), best first; menu order breaks ties. An empty query
// matches every leaf.
std::vector<MenuSearchResult> searchMenu(const ResolvedMenu& menu, std::string_view query);

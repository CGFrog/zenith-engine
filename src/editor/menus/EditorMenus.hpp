#pragma once

#include "editor/menus/MenuPopup.hpp"
#include "editor/menus/MenuRegistry.hpp"
#include "editor/operators/OperatorRegistry.hpp"

#include <optional>
#include <string>

struct EditorContext;

// Which menu to show, and optionally only one of its top-level categories
// (shown without its heading), as the Properties editor's component tabs do.
struct MenuRequest {
    std::string menu;
    std::string category;
};

// The session's menus: the registry every menu is defined in and the one
// popup menu (wm.call_menu) open at a time. The session owns it; panels open
// menus by running wm.call_menu and may draw a menu inline through
// EditorContext::menus.
class EditorMenus {
  public:
    MenuRegistry& registry() {
        return m_registry;
    }

    const MenuRegistry& registry() const {
        return m_registry;
    }

    // The menu as it is shown now (restricted to request.category when one is
    // given); nullopt for an unknown menu.
    std::optional<ResolvedMenu> resolve(const MenuRequest& request, const OperatorContext& context) const;
    // Opens the menu as a popup at the mouse on the next draw; fails for an
    // unknown menu.
    MenuResult open(MenuRequest request);
    void close();
    bool isOpen() const;

    // The open popup's request.
    const MenuRequest& request() const {
        return m_request;
    }

    // Draws the open popup at the top level of the frame and runs the chosen
    // entry's operator through context. The popup closes if its menu is
    // removed (a module unloading).
    void draw(EditorContext& context);

  private:
    MenuRegistry m_registry;
    MenuPopup m_popup;
    MenuRequest m_request;
};

// Properties of wm.call_menu.
struct CallMenuProperties {
    // A registered menu id, for example "VIEW3D_MT_add".
    std::string menu;
    // Optional top-level category of the menu to show alone.
    std::string category;
};

// The id of the Add menu (Shift A) and of the Add Component menu.
inline constexpr const char* addMenuId = "VIEW3D_MT_add";
inline constexpr const char* addComponentMenuId = "OBJECT_MT_add_component";

// wm.call_menu {menu, category}, which opens a registered menu at the mouse,
// and object.add_menu, which opens the Add menu (kept so keymaps naming it
// keep working). Both run headless; menus holds the request until the
// session draws it.
OperatorRegistrationResult registerMenuOperators(OperatorRegistry& registry, EditorMenus& menus);

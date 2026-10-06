#pragma once

// The session's menus: the registry every menu is defined in and the one popup menu (wm.call_menu) open at a time.

#include "editor/menus/MenuPopup.hpp"
#include "editor/menus/MenuRegistry.hpp"
#include "editor/menus/PieMenu.hpp"
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

    // Opens the menu as a pie at the mouse on the next draw (closing the
    // popup menu), or, with pie menus turned off, as the popup menu; fails
    // for an unknown menu.
    MenuResult openPie(MenuRequest request);
    void closePie();
    bool pieOpen() const;

    // The open pie's request.
    const MenuRequest& pieRequest() const {
        return m_pieRequest;
    }

    const PieMenu& pie() const {
        return m_pie;
    }

    // The Interface preference: whether wm.call_pie shows pies (off by
    // default) or the ordinary searchable menus.
    void setPieMenusEnabled(bool enabled);
    bool pieMenusEnabled() const;

    // Draws the open popup and pie at the top level of the frame and runs
    // the chosen entry's operator through context. Either closes if its menu
    // is removed (a module unloading).
    void draw(EditorContext& context);

  private:
    MenuRegistry m_registry;
    MenuPopup m_popup;
    MenuRequest m_request;
    PieMenu m_pie;
    MenuRequest m_pieRequest;
    bool m_pieMenusEnabled{false};
};

// Properties of wm.call_menu.
struct CallMenuProperties {
    // A registered menu id, for example "VIEW3D_MT_add".
    std::string menu;
    // Optional top-level category of the menu to show alone.
    std::string category;
};

// Properties of wm.call_pie.
struct CallPieProperties {
    // A registered menu id, for example "VIEW3D_MT_shading_pie".
    std::string menu;
};

// The id of the Add menu (Shift A) and of the Add Component menu.
inline constexpr const char* addMenuId = "VIEW3D_MT_add";
inline constexpr const char* addComponentMenuId = "OBJECT_MT_add_component";
// The viewport pies: shading (Z), view (`), and pivot point (.).
inline constexpr const char* shadingPieMenuId = "VIEW3D_MT_shading_pie";
inline constexpr const char* viewPieMenuId = "VIEW3D_MT_view_pie";
inline constexpr const char* pivotPieMenuId = "VIEW3D_MT_pivot_pie";
// Select Grouped (Shift G): one object.select_grouped entry per type.
inline constexpr const char* selectGroupedMenuId = "VIEW3D_MT_select_grouped";

// wm.call_menu {menu, category}, which opens a registered menu at the mouse,
// wm.call_pie {menu}, which opens one as a pie (or as the popup menu with
// pies turned off), and object.add_menu, which opens the Add menu (kept so
// keymaps naming it keep working). All run headless; menus holds the request
// until the session draws it.
OperatorRegistrationResult registerMenuOperators(OperatorRegistry& registry, EditorMenus& menus);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerMenuOperatorFamily(OperatorRegistry& registry,
                                                      const EditorOperatorServices& services);

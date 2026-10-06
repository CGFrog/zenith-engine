#pragma once

// Shows a game module's panel in an area.

#include "editor/panels/EditorPanel.hpp"

#include <string>
#include <string_view>

class GameModuleHost;

// The area type id of a game module panel: "module:" followed by the panel's
// title, with the window-name delimiters '.' and '#' replaced by '_'.
std::string gameModuleAreaTypeId(std::string_view panelTitle);

// Shows a game module's panel in an area. It finds the panel by title on
// every draw instead of keeping it, because the panel's code goes away when
// the module reloads; while the module has no such panel it shows a notice.
class GameModulePanel final : public EditorPanel {
  public:
    // host must outlive the panel.
    GameModulePanel(GameModuleHost& host, std::string panelTitle);

    const char* title() const override;
    void draw(EditorContext& context) override;

  private:
    GameModuleHost& m_host;
    std::string m_panelTitle;
};

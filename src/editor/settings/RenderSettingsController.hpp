#pragma once

#include "editor/settings/EditorRenderSettings.hpp"

#include <string>
#include <vector>

// The session's single owner of the render settings the editor requests. It
// keeps the persisted EditorRenderSettings together with the capability
// fallbacks of the latest apply, so every panel that edits render settings
// (Graphics, Properties) sees and changes the same request.
class RenderSettingsController {
  public:
    explicit RenderSettingsController(EditorRenderSettings settings);

    // Reads the saved settings and applies them to the renderer. The current
    // request is applied even when loading fails, which leaves it unchanged.
    EditorRenderSettingsResult load(RenderRuntime& rendering);
    // Writes the request to its persistent home.
    EditorRenderSettingsResult save();
    // Replaces the request and applies it to the renderer live.
    void request(const RenderSettings& settings, RenderRuntime& rendering);

    const EditorRenderSettings& settings() const {
        return m_settings;
    }

    // Capability fallbacks that changed the request in the latest apply.
    const std::vector<std::string>& fallbacks() const {
        return m_fallbacks;
    }

  private:
    void apply(RenderRuntime& rendering);

    EditorRenderSettings m_settings;
    std::vector<std::string> m_fallbacks;
};

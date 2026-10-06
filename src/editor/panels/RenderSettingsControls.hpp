#pragma once

// Render-settings widgets shared by the Graphics and Properties panels.

struct EditorContext;
class RenderSettingsController;

// Render-settings widgets shared by the panels that edit the session's
// RenderSettingsController (Graphics and Properties), so both present and
// change the one request the same way.
namespace RenderSettingsControls {
// Save and Revert buttons, where the settings are stored, and the capability
// fallbacks of the latest apply. Failures are reported to diagnostics.
void drawStorage(EditorContext& context, RenderSettingsController& controller);
// Every RenderSettings field; a change is requested and applied live.
// Returns whether the request changed.
bool drawSettings(EditorContext& context, RenderSettingsController& controller);
} // namespace RenderSettingsControls

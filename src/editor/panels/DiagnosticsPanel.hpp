#pragma once

// Lists editor diagnostics and shows asset ownership and memory, with a manual collection of unused assets.

#include "editor/panels/EditorPanel.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"

#include <optional>

class EditorDiagnostics;
struct EditorToolSettings;

// Lists editor diagnostics and shows asset ownership and memory, with a
// manual collection of unused assets. Diagnostics with a source location open
// in the configured IDE when clicked.
class DiagnosticsPanel final : public EditorPanel {
  public:
    // tools supplies the IDE command and must outlive the panel.
    explicit DiagnosticsPanel(const EditorToolSettings& tools);

    const char* title() const override;
    void draw(EditorContext& context) override;
    // The diagnostic list alone, which needs no running session.
    void drawDiagnostics(EditorDiagnostics& diagnostics);

  private:
    void drawAssetLifetime(EditorContext& context);
    void drawAudioLifetime(EditorContext& context);

    const EditorToolSettings& m_tools;
    std::optional<AssetCollectionResult> m_lastAssetCollection;
    std::optional<AudioAssetCollectionResult> m_lastAudioCollection;
};

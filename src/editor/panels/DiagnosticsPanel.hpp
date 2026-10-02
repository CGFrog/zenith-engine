#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/audio/AudioSystem.hpp"

#include <optional>

// Lists editor diagnostics and shows asset ownership and memory, with a
// manual collection of unused assets.
class DiagnosticsPanel final : public EditorPanel {
  public:
    const char* title() const override;
    void draw(EditorContext& context) override;

  private:
    void drawDiagnostics(EditorContext& context);
    void drawAssetLifetime(EditorContext& context);
    void drawAudioLifetime(EditorContext& context);

    std::optional<AssetCollectionResult> m_lastAssetCollection;
    std::optional<AudioAssetCollectionResult> m_lastAudioCollection;
};

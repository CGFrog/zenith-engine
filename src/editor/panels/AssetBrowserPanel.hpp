#pragma once

#include "editor/panels/EditorPanel.hpp"
#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ThumbnailCache.hpp"

#include <filesystem>
#include <string>

class EditorDiagnostics;

// Lists project assets and scenes. Models drag into the viewport or inspector;
// scenes open on double-click. The catalog rescans once a second so external
// file changes are reported without a file watcher.
class AssetBrowserPanel final : public EditorPanel {
  public:
    AssetBrowserPanel(std::filesystem::path assetRoot, std::filesystem::path sceneRoot);

    const char* title() const override;
    void update(EditorContext& context, const InputState& input, float deltaSeconds) override;
    void draw(EditorContext& context) override;

    // Rescans the project. Changes after the first scan are reported, and
    // changed textures get fresh thumbnails.
    void refresh(EditorDiagnostics& diagnostics);

  private:
    void drawSelectedAsset(EditorContext& context);

    AssetCatalog m_catalog;
    ThumbnailCache m_thumbnails;
    std::string m_filter;
    std::filesystem::path m_selectedAsset;
    float m_refreshElapsed{0.0f};
    bool m_initialized{false};
};

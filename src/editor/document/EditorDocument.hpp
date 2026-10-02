#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class AssetManager;

class EditorDocument {
  public:
    explicit EditorDocument(std::filesystem::path assetRoot);

    void createNew(Scene& scene);
    SceneDocumentResult open(const std::filesystem::path& path, Scene& scene, AssetManager& assets);
    SceneDocumentResult save(const Scene& scene, const AssetManager& assets);
    SceneDocumentResult saveAs(const std::filesystem::path& path, const Scene& scene, const AssetManager& assets);

    // The open scene's file was renamed or moved outside the document.
    void relocate(std::filesystem::path path);
    // The open scene's file is gone; the document becomes untitled and dirty.
    void detach();
    // Lets references resolve by GUID and records GUIDs on save.
    void setAssetDatabase(std::shared_ptr<const AssetDatabase> database);

    void markDirty();
    void setDirty(bool dirty);
    bool dirty() const;
    const std::optional<std::filesystem::path>& path() const;
    std::string displayName() const;
    const SceneDocumentOptions& options() const;

  private:
    SceneDocumentOptions m_options;
    std::optional<std::filesystem::path> m_path;
    bool m_dirty{false};
};

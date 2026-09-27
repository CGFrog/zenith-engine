#pragma once

#include "engine/scene/SceneDocument.hpp"

#include <filesystem>

class AssetManager;

class EditorRecovery {
  public:
    explicit EditorRecovery(std::filesystem::path path);

    SceneDocumentResult autosave(const Scene& scene, const AssetManager& assets,
                                 const SceneDocumentOptions& options) const;
    SceneDocumentResult recover(Scene& scene, AssetManager& assets, const SceneDocumentOptions& options) const;
    bool available() const;
    bool discard(std::string& error) const;
    const std::filesystem::path& path() const;

  private:
    std::filesystem::path m_path;
};

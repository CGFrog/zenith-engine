#pragma once

// One open scene document: path, dirty state, save/load options, asset root and database.

#include "editor/document/SceneEditorData.hpp"
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
    // The document's content was saved somewhere other than a scene file (a
    // prefab edited as its own document): it becomes clean, and the editor
    // data as it is now counts as saved.
    void markSaved();
    bool dirty() const;
    const std::optional<std::filesystem::path>& path() const;
    std::string displayName() const;
    const SceneDocumentOptions& options() const;
    // The component types documents are read and written with; null means the
    // engine's built-in types. A game module host sets its merged registry
    // here and clears it before unloading the module, since the options share
    // ownership of the registry.
    void setComponentTypes(std::shared_ptr<const ComponentTypeRegistry> componentTypes);

    // The open scene's editor-only state (the 3D cursor), kept in a sidecar
    // beside the scene file (SceneEditorData.hpp): open reads it, save and
    // saveAs write it, createNew resets it. It is not part of undo, but a
    // change from the saved value makes the document dirty.
    const SceneEditorData& editorData() const;
    SceneEditorData& editorData();
    // Why the last open could not read the sidecar (its defaults were used
    // instead), or empty. Taking it clears it.
    std::string takeEditorDataWarning();

  private:
    SceneDocumentOptions m_options;
    std::optional<std::filesystem::path> m_path;
    bool m_dirty{false};
    SceneEditorData m_editorData;
    // The editor data as last opened or saved; a difference makes the
    // document dirty.
    SceneEditorData m_savedEditorData;
    std::string m_editorDataWarning;
};

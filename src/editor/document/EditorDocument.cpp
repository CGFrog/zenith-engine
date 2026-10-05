#include "EditorDocument.hpp"

#include "engine/assets/Assets.hpp"

#include <utility>

EditorDocument::EditorDocument(std::filesystem::path assetRoot)
    : m_options{.assetRoot = std::move(assetRoot)} {}

void EditorDocument::createNew(Scene& scene) {
    scene.clear();
    m_path.reset();
    m_dirty = false;
    m_editorData = {};
    m_savedEditorData = {};
    m_editorDataWarning.clear();
}

SceneDocumentResult EditorDocument::open(const std::filesystem::path& path, Scene& scene, AssetManager& assets) {
    if (SceneDocumentResult result = SceneDocument::load(path, assets, m_options, scene); !result) {
        return result;
    }
    m_path = path;
    m_dirty = false;
    // A sidecar that cannot be read must not keep the scene from opening.
    const SceneEditorDataResult editorData = readSceneEditorData(path, m_editorData);
    m_editorDataWarning = editorData ? std::string() : editorData.error + "; the 3D cursor was reset";
    m_savedEditorData = m_editorData;
    return {.success = true};
}

SceneDocumentResult EditorDocument::save(const Scene& scene, const AssetManager& assets) {
    if (!m_path) {
        return {.error = "The untitled scene requires Save As"};
    }
    SceneDocumentResult saved = SceneDocument::save(*m_path, scene, assets, m_options);
    if (!saved) {
        return saved;
    }
    if (const SceneEditorDataResult result = writeSceneEditorData(*m_path, m_editorData); !result) {
        // The scene itself was saved; the document stays dirty so saving
        // again retries the sidecar.
        return {.error = "The scene was saved, but not its editor data: " + result.error};
    }
    m_dirty = false;
    m_savedEditorData = m_editorData;
    return saved;
}

SceneDocumentResult EditorDocument::saveAs(const std::filesystem::path& path, const Scene& scene,
                                           const AssetManager& assets) {
    if (path.empty()) {
        return {.error = "A scene path is required"};
    }
    SceneDocumentResult saved = SceneDocument::save(path, scene, assets, m_options);
    if (!saved) {
        return saved;
    }
    // The document moves to the new path only once both files are written.
    if (const SceneEditorDataResult result = writeSceneEditorData(path, m_editorData); !result) {
        return {.error = "The scene was saved, but not its editor data: " + result.error};
    }
    m_path = path;
    m_dirty = false;
    m_savedEditorData = m_editorData;
    return saved;
}

void EditorDocument::relocate(std::filesystem::path path) {
    m_path = std::move(path);
}

void EditorDocument::detach() {
    m_path.reset();
    m_dirty = true;
}

void EditorDocument::setAssetDatabase(std::shared_ptr<const AssetDatabase> database) {
    m_options.assetDatabase = std::move(database);
}

void EditorDocument::markDirty() {
    m_dirty = true;
}

void EditorDocument::setDirty(bool dirty) {
    m_dirty = dirty;
}

void EditorDocument::markSaved() {
    m_dirty = false;
    m_savedEditorData = m_editorData;
}

bool EditorDocument::dirty() const {
    // A moved 3D cursor is an unsaved change too, though it is not undone.
    return m_dirty || m_editorData != m_savedEditorData;
}

const std::optional<std::filesystem::path>& EditorDocument::path() const {
    return m_path;
}

std::string EditorDocument::displayName() const {
    std::string name = m_path ? m_path->filename().string() : "Untitled";
    if (dirty()) {
        name += '*';
    }
    return name;
}

const SceneDocumentOptions& EditorDocument::options() const {
    return m_options;
}

const SceneEditorData& EditorDocument::editorData() const {
    return m_editorData;
}

SceneEditorData& EditorDocument::editorData() {
    return m_editorData;
}

std::string EditorDocument::takeEditorDataWarning() {
    return std::exchange(m_editorDataWarning, {});
}

void EditorDocument::setComponentTypes(std::shared_ptr<const ComponentTypeRegistry> componentTypes) {
    m_options.componentTypes = std::move(componentTypes);
}

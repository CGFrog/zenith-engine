#include "EditorDocument.hpp"

#include "engine/assets/Assets.hpp"

#include <utility>

EditorDocument::EditorDocument(std::filesystem::path assetRoot)
    : m_options{.assetRoot = std::move(assetRoot)} {}

void EditorDocument::createNew(Scene& scene) {
    scene.clear();
    m_path.reset();
    m_dirty = false;
}

SceneDocumentResult EditorDocument::open(const std::filesystem::path& path, Scene& scene, AssetManager& assets) {
    if (SceneDocumentResult result = SceneDocument::load(path, assets, m_options, scene); !result) {
        return result;
    }
    m_path = path;
    m_dirty = false;
    return {.success = true};
}

SceneDocumentResult EditorDocument::save(const Scene& scene, const AssetManager& assets) {
    if (!m_path) {
        return {.error = "The untitled scene requires Save As"};
    }
    if (SceneDocumentResult result = SceneDocument::save(*m_path, scene, assets, m_options); !result) {
        return result;
    }
    m_dirty = false;
    return {.success = true};
}

SceneDocumentResult EditorDocument::saveAs(const std::filesystem::path& path, const Scene& scene,
                                           const AssetManager& assets) {
    if (path.empty()) {
        return {.error = "A scene path is required"};
    }
    if (SceneDocumentResult result = SceneDocument::save(path, scene, assets, m_options); !result) {
        return result;
    }
    m_path = path;
    m_dirty = false;
    return {.success = true};
}

void EditorDocument::markDirty() {
    m_dirty = true;
}

void EditorDocument::setDirty(bool dirty) {
    m_dirty = dirty;
}

bool EditorDocument::dirty() const {
    return m_dirty;
}

const std::optional<std::filesystem::path>& EditorDocument::path() const {
    return m_path;
}

std::string EditorDocument::displayName() const {
    std::string name = m_path ? m_path->filename().string() : "Untitled";
    if (m_dirty) {
        name += '*';
    }
    return name;
}

const SceneDocumentOptions& EditorDocument::options() const {
    return m_options;
}

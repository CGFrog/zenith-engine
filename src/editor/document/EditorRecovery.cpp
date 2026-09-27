#include "EditorRecovery.hpp"

#include "engine/assets/Assets.hpp"

#include <utility>

EditorRecovery::EditorRecovery(std::filesystem::path path)
    : m_path(std::move(path)) {}

SceneDocumentResult EditorRecovery::autosave(const Scene& scene, const AssetManager& assets,
                                             const SceneDocumentOptions& options) const {
    return SceneDocument::save(m_path, scene, assets, options);
}

SceneDocumentResult EditorRecovery::recover(Scene& scene, AssetManager& assets,
                                            const SceneDocumentOptions& options) const {
    return SceneDocument::load(m_path, assets, options, scene);
}

bool EditorRecovery::available() const {
    std::error_code error;
    return std::filesystem::is_regular_file(m_path, error);
}

bool EditorRecovery::discard(std::string& errorMessage) const {
    std::error_code error;
    std::filesystem::remove(m_path, error);
    if (error) {
        errorMessage = "Could not remove recovery file '" + m_path.string() + "': " + error.message();
        return false;
    }
    return true;
}

const std::filesystem::path& EditorRecovery::path() const {
    return m_path;
}

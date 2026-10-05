#include "AssetReload.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/project/ProjectAssets.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/assets/TextureImportSettings.hpp"
#include "engine/audio/AudioSystem.hpp"
#include "engine/scene/SceneAssetReplacement.hpp"

#include <string>
#include <system_error>

namespace {
constexpr const char* diagnosticSource = "Asset Browser";

// The path as AssetManager records texture sources.
std::filesystem::path loadedSourcePath(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

// Points bindings that read image with another usage than its import
// settings name at the texture read with that usage.
size_t applyTextureUsage(AssetManager& assets, std::span<Scene* const> scenes, const ComponentTypeRegistry& types,
                         const AssetDatabase& database, const std::filesystem::path& image,
                         EditorDiagnostics& diagnostics) {
    const TextureImportSettings settings = textureImportSettingsFor(database, image);
    const std::filesystem::path source = loadedSourcePath(image);
    TextureHandle reread;
    bool failed = false;
    const auto replace = [&](TextureHandle handle) {
        const TextureAsset* texture = assets.texture(handle);
        const std::optional<TextureAssetSource> loadedFrom = assets.textureSource(handle);
        if (failed || texture == nullptr || texture->usage == settings.usage || !loadedFrom ||
            loadedFrom->path != source) {
            return handle;
        }
        if (!reread) {
            const AssetLoadResult<TextureHandle> loaded = assets.loadTexture(image, settings.usage);
            if (!loaded) {
                diagnostics.reportError("Could not read texture '" + utf8PathText(image.filename()) + "' as " +
                                        std::string(textureUsageName(settings.usage)) + ": " + loaded.error);
                failed = true;
                return handle;
            }
            reread = loaded.handle;
        }
        return reread;
    };
    size_t changed = 0;
    for (Scene* scene : scenes) {
        if (scene != nullptr) {
            changed += replaceSceneTextures(*scene, types, replace);
        }
    }
    return changed;
}
} // namespace

size_t reloadProjectTexture(AssetManager& assets, std::span<Scene* const> scenes, const ComponentTypeRegistry& types,
                            const AssetDatabase* database, EditorDiagnostics& diagnostics,
                            const std::filesystem::path& image) {
    const std::string name = utf8PathText(image.filename());
    const TextureReloadResult reloaded = assets.reloadTexture(image);
    if (!reloaded) {
        diagnostics.reportError("Texture reload of '" + name + "' failed: " + reloaded.error);
        return 0;
    }
    size_t changed = 0;
    for (Scene* scene : scenes) {
        if (scene != nullptr) {
            changed += replaceSceneTextures(*scene, types, reloaded.replaced);
        }
    }
    for (const std::filesystem::path& material : reloaded.materialFiles) {
        changed += reloadProjectMaterial(assets, scenes, diagnostics, material);
    }
    if (database != nullptr) {
        changed += applyTextureUsage(assets, scenes, types, *database, image, diagnostics);
    }
    diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource,
                    reloaded.replaced.empty() && changed == 0
                        ? "Texture '" + name + "' is up to date; nothing in the open scenes uses it."
                        : "Reloaded texture '" + name + "' (" + std::to_string(changed) + " users updated).");
    return changed;
}

size_t reloadProjectAudio(AudioSystem& audio, std::span<Scene* const> scenes, EditorDiagnostics& diagnostics,
                          const std::filesystem::path& clip) {
    const std::string name = utf8PathText(clip.filename());
    const AudioClipReloadResult reloaded = audio.reloadClip(clip);
    if (!reloaded) {
        diagnostics.reportError("Sound reload of '" + name + "' failed: " + reloaded.error);
        return 0;
    }
    size_t changed = 0;
    for (Scene* scene : scenes) {
        if (scene != nullptr) {
            changed += replaceAudioEmitterClips(*scene, reloaded.replaced);
        }
    }
    diagnostics.add(EditorDiagnosticSeverity::Info, diagnosticSource,
                    reloaded.replaced.empty()
                        ? "Sound '" + name + "' is up to date; it is not loaded."
                        : "Reloaded sound '" + name + "' (" + std::to_string(changed) + " emitters updated).");
    return changed;
}

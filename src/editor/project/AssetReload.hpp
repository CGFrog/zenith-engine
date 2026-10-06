#pragma once

// Reloads of textures and sounds that changed on disk or were reimported, beside reloadProjectModel and
// reloadProjectMaterial (ProjectAssets.hpp).

#include <cstddef>
#include <filesystem>
#include <span>

class AssetDatabase;
class AssetManager;
class AudioSystem;
class ComponentTypeRegistry;
class EditorDiagnostics;
class Scene;

// Reloads of textures and sounds that changed on disk or were reimported,
// beside reloadProjectModel and reloadProjectMaterial (ProjectAssets.hpp).
// Each reports its outcome through diagnostics and returns the number of
// components it pointed at the new asset in scenes (null entries skipped).

// Reloads an image through AssetManager::reloadTexture and re-points the
// texture bindings of scenes' components (decals, particles, environment and
// probe maps, and module components) at the replacements; material files
// sampling it are reloaded (reloadProjectMaterial). The image's import
// settings in database (null for none) are applied too: bindings that read
// it with another usage than its settings name load it again with that usage.
size_t reloadProjectTexture(AssetManager& assets, std::span<Scene* const> scenes, const ComponentTypeRegistry& types,
                            const AssetDatabase* database, EditorDiagnostics& diagnostics,
                            const std::filesystem::path& image);

// Reloads a sound through AudioSystem::reloadClip, stopping voices of the
// previous clip, and points scenes' audio emitters at the replacement.
size_t reloadProjectAudio(AudioSystem& audio, std::span<Scene* const> scenes, EditorDiagnostics& diagnostics,
                          const std::filesystem::path& clip);

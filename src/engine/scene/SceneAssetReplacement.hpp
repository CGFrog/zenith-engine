#pragma once

#include "engine/assets/Assets.hpp"

#include <cstddef>
#include <functional>
#include <span>

class ComponentTypeRegistry;
class Scene;

// Re-points a scene's components at replaced textures, after
// AssetManager::reloadTexture or an import-settings change loaded a texture
// again. Every texture() binding of every component type in types (decal,
// particle, environment, and probe textures among the built-ins) is offered
// to replace, which returns the handle to use (the same handle to keep it).
// Changed entities are marked renderable and light dirty. Returns the number
// of components changed. Material users are not touched: reload the material
// files TextureReloadResult lists and re-point their users instead.
size_t replaceSceneTextures(Scene& scene, const ComponentTypeRegistry& types,
                            const std::function<TextureHandle(TextureHandle)>& replace);

// replaceSceneTextures for reloadTexture's replacements: each previous handle
// becomes its current one.
size_t replaceSceneTextures(Scene& scene, const ComponentTypeRegistry& types,
                            std::span<const TextureReplacement> replacements);

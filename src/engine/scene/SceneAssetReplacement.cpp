#include "SceneAssetReplacement.hpp"

#include "engine/scene/ComponentTypes.hpp"
#include "engine/scene/Scene.hpp"

size_t replaceSceneTextures(Scene& scene, const ComponentTypeRegistry& types,
                            const std::function<TextureHandle(TextureHandle)>& replace) {
    size_t changed = 0;
    // Every saved entity has a stable id, so this visits each entity once.
    const auto entities = scene.registry().view<const SceneEntityIdComponent>();
    for (const ComponentType& type : types.types()) {
        for (const Entity entity : entities) {
            void* component = type.find(scene, entity);
            if (component == nullptr || !type.replaceTextures(component, replace)) {
                continue;
            }
            scene.markRenderableDirty(entity);
            scene.markLightDirty(entity);
            ++changed;
        }
    }
    return changed;
}

size_t replaceSceneTextures(Scene& scene, const ComponentTypeRegistry& types,
                            std::span<const TextureReplacement> replacements) {
    return replaceSceneTextures(scene, types, [replacements](TextureHandle handle) {
        for (const TextureReplacement& replacement : replacements) {
            if (replacement.previous == handle) {
                return replacement.current;
            }
        }
        return handle;
    });
}

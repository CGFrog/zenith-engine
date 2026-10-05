// Registers the engine's components. The type builders live in one file per
// domain (Render, Animation, Light, Physics, Collection, Prefab, Gameplay,
// Navigation, and Audio ComponentTypes.cpp); add a new built-in type to the family file for its
// domain and list it here.

#include "AnimationComponentTypes.hpp"
#include "AudioComponentTypes.hpp"
#include "CollectionComponentTypes.hpp"
#include "ComponentTypes.hpp"
#include "GameplayComponentTypes.hpp"
#include "LightComponentTypes.hpp"
#include "NavigationComponentTypes.hpp"
#include "PhysicsComponentTypes.hpp"
#include "Prefab.hpp"
#include "RenderComponentTypes.hpp"

#include <utility>

SceneDocumentResult registerBuiltInComponentTypes(ComponentTypeRegistry& registry) {
    // Registration order is the order documents are read in and the order the
    // editor lists types; keep it stable across the family files.
    for (ComponentType type :
         {modelComponentType(), modelNodeComponentType(), animationComponentType(), morphWeightsComponentType(),
          skinComponentType(), meshRendererComponentType(), cameraComponentType(), directionalLightComponentType(),
          environmentLightComponentType(), volumetricFogComponentType(), particleEmitterComponentType(),
          pointLightComponentType(), spotLightComponentType(), reflectionProbeComponentType(), decalComponentType(),
          irradianceVolumeComponentType(), colliderComponentType(), rigidBodyComponentType()}) {
        if (SceneDocumentResult result = registry.add(std::move(type)); !result) {
            return result;
        }
    }
    // Registered from their own translation units to keep this one small.
    if (SceneDocumentResult result = registerCollectionComponentTypes(registry); !result) {
        return result;
    }
    if (SceneDocumentResult result = registerPrefabComponentTypes(registry); !result) {
        return result;
    }
    for (ComponentType type :
         {characterControllerComponentType(), firstPersonCameraComponentType(), animatorComponentType(),
          navAgentComponentType(), tagsComponentType(), audioListenerComponentType(), audioEmitterComponentType(),
          materialOverrideComponentType()}) {
        if (SceneDocumentResult result = registry.add(std::move(type)); !result) {
            return result;
        }
    }
    return {.success = true};
}

#include "editor/theme/EntityIcon.hpp"

#include "engine/audio/AudioSystem.hpp"
#include "engine/physics/PhysicsComponents.hpp"
#include "engine/scene/ParticleEmitter.hpp"

EditorIcon entityIcon(const entt::registry& registry, Entity entity) {
    if (!registry.valid(entity)) {
        return EditorIcon::Empty;
    }
    if (registry.all_of<CameraComponent>(entity)) {
        return EditorIcon::Camera;
    }
    if (registry.all_of<DirectionalLightComponent>(entity)) {
        return EditorIcon::Sun;
    }
    if (registry.all_of<SpotLightComponent>(entity)) {
        return EditorIcon::SpotLight;
    }
    if (registry.all_of<PointLightComponent>(entity)) {
        return EditorIcon::PointLight;
    }
    if (registry.all_of<EnvironmentLightComponent>(entity)) {
        return EditorIcon::Environment;
    }
    if (registry.all_of<VolumetricFogComponent>(entity)) {
        return EditorIcon::Fog;
    }
    if (registry.all_of<ReflectionProbeComponent>(entity)) {
        return EditorIcon::ReflectionProbe;
    }
    if (registry.all_of<IrradianceVolumeComponent>(entity)) {
        return EditorIcon::IrradianceVolume;
    }
    if (registry.all_of<DecalComponent>(entity)) {
        return EditorIcon::Decal;
    }
    if (registry.all_of<ParticleEmitterComponent>(entity)) {
        return EditorIcon::Particles;
    }
    if (registry.all_of<AudioEmitterComponent>(entity)) {
        return EditorIcon::AudioEmitter;
    }
    if (registry.all_of<AudioListenerComponent>(entity)) {
        return EditorIcon::AudioListener;
    }
    if (registry.any_of<RigidBodyComponent, ColliderComponent>(entity)) {
        return EditorIcon::Physics;
    }
    if (registry.any_of<ModelComponent, ModelNodeComponent, MeshRendererComponent>(entity)) {
        return EditorIcon::Mesh;
    }
    return EditorIcon::Empty;
}

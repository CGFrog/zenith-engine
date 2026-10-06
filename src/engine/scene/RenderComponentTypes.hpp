#pragma once

// Rendering and effect components: model instances, meshes, cameras, particle emitters, decals, and material overrides.

class ComponentType;

// Rendering and effect components: model instances, meshes, cameras,
// particle emitters, decals, and material overrides.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType modelComponentType();
ComponentType modelNodeComponentType();
ComponentType meshRendererComponentType();
ComponentType cameraComponentType();
ComponentType particleEmitterComponentType();
ComponentType decalComponentType();
ComponentType materialOverrideComponentType();

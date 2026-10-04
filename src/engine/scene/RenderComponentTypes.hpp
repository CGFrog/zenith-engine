#pragma once

class ComponentType;

// Rendering and effect components: model instances, meshes, cameras,
// particle emitters, and decals.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType modelComponentType();
ComponentType modelNodeComponentType();
ComponentType meshRendererComponentType();
ComponentType cameraComponentType();
ComponentType particleEmitterComponentType();
ComponentType decalComponentType();

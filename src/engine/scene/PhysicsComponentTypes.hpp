#pragma once

class ComponentType;

// Physics components: colliders, rigid bodies and character controllers.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType colliderComponentType();
ComponentType rigidBodyComponentType();
ComponentType characterControllerComponentType();

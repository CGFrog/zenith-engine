#pragma once

class ComponentType;

// Physics components: colliders and rigid bodies.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType colliderComponentType();
ComponentType rigidBodyComponentType();

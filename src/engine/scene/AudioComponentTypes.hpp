#pragma once

class ComponentType;

// Audio components: the listener and sound emitters placed in a scene.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType audioListenerComponentType();
ComponentType audioEmitterComponentType();

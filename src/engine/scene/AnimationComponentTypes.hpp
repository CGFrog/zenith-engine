#pragma once

class ComponentType;

// Animation playback, morph-target weights, and skins.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType animationComponentType();
ComponentType morphWeightsComponentType();
ComponentType skinComponentType();

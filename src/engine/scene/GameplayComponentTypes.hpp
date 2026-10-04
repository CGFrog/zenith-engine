#pragma once

class ComponentType;

// Reusable gameplay components (engine/gameplay): first-person views and
// tags. registerBuiltInComponentTypes registers them in the established order.
ComponentType firstPersonCameraComponentType();
// "tags": TagsComponent (Tags.hpp).
ComponentType tagsComponentType();

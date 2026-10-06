#pragma once

// The icon that best names what an entity is, from its components in a fixed priority (a camera before its mesh, lights
// before probes).

#include "editor/theme/PixelIconBitmaps.hpp"

#include "engine/scene/Scene.hpp"

// The icon that best names what an entity is, from its components in a fixed
// priority (a camera before its mesh, lights before probes). Entities with
// only a transform, or only game-module components, are Empty.
EditorIcon entityIcon(const entt::registry& registry, Entity entity);

#pragma once

// The precompiled header game modules build with (Zenith::Engine's
// INTERFACE_PRECOMPILE_HEADERS, cmake/ZenithEngineConfig.cmake.in): the
// standard and third-party headers modules use, and the engine's gameplay
// API. Only headers in ZENITH_GAMEPLAY_API_HEADERS (cmake/GameplayApi.cmake,
// docs/engine/gameplay-api.md) belong here. A module never edits the engine, so unlike the engine's own
// precompiled header (ZenithEnginePch.hpp) this one holds Zenith headers too,
// which is where most of a gameplay source's compile time goes. Sources never
// rely on it: each includes what it uses, and a module configured with
// -DZENITH_MODULE_PCH=OFF must still build.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/behaviour/Behaviour.hpp"
#include "engine/gameplay/FirstPerson.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/module/ModuleApi.hpp"
#include "engine/module/ModuleRegistrar.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/ui/GameMenus.hpp"
#include "engine/ui/GameUi.hpp"

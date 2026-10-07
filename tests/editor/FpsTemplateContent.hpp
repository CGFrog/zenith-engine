#pragma once

// FpsTemplateContent: what the FPS template's committed scene and prefab hold,
// shared by the tests that check them and the tool that writes them.

#include <string_view>

namespace fpstemplate {
// Relative to the template folder, and to a generated project's folder.
inline constexpr std::string_view sceneFile = "scenes/main.zenith-scene";
// Relative to the template's assets/ folder.
inline constexpr std::string_view impactPrefab = "prefabs/impact.prefab";
inline constexpr int targetCount = 5;
} // namespace fpstemplate

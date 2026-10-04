#pragma once

// Precompiled header for zenith_engine (src/engine/CMakeLists.txt, enabled by
// the ZENITH_USE_PCH option). It lists only standard and third-party headers
// that many engine translation units include, so precompiling them saves
// parsing time without changing what any file means: every source still
// includes what it uses and builds with the option off.
//
// Leave out headers whose meaning depends on macros a translation unit defines
// first (volk/Vulkan, SDL, Jolt, Windows) and every Zenith header, which keeps
// export macros such as ZENITH_ENGINE_API and IMGUI_API resolved per target.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#pragma once

#include "engine/input/InputActions.hpp"

#include <memory>
#include <string_view>

namespace DemoAction {
inline constexpr std::string_view Move = "Move";
inline constexpr std::string_view Look = "Look";
inline constexpr std::string_view Jump = "Jump";
inline constexpr std::string_view Sprint = "Sprint";
inline constexpr std::string_view Interact = "Interact";
inline constexpr std::string_view Pause = "Pause";
inline constexpr std::string_view Reload = "Reload";
inline constexpr std::string_view MenuNext = "MenuNext";
inline constexpr std::string_view MenuPrevious = "MenuPrevious";
inline constexpr std::string_view MenuAccept = "MenuAccept";
inline constexpr std::string_view MenuLeft = "MenuLeft";
inline constexpr std::string_view MenuRight = "MenuRight";
} // namespace DemoAction

std::shared_ptr<InputActionMap> createDemoInputMap();

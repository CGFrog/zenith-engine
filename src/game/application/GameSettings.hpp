#pragma once

#include "engine/graphics/Rendering.hpp"
#include "engine/ui/Accessibility.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct GameSettings {
    AntialiasingMode antialiasing{AntialiasingMode::Fxaa};
    BloomQuality bloom{BloomQuality::Off};
    bool masterMuted{false};
    // The master bus gain, 0 to 1 (version 3).
    float masterVolume{1.0f};
    float lookSensitivity{1.0f};
    bool invertLookY{false};
    std::string locale{"en"};
    AccessibilitySettings accessibility;
    std::vector<std::string> inputProfiles{"Default"};
    std::string activeInputProfile{"Default"};
};

struct GameSettingsResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

GameSettingsResult loadGameSettings(const std::filesystem::path& path, GameSettings& settings);
GameSettingsResult saveGameSettings(const std::filesystem::path& path, const GameSettings& settings);

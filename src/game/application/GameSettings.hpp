#pragma once

#include "engine/graphics/Rendering.hpp"

#include <filesystem>
#include <string>

struct GameSettings {
    AntialiasingMode antialiasing{AntialiasingMode::Fxaa};
    BloomQuality bloom{BloomQuality::Off};
    bool masterMuted{false};
    float lookSensitivity{1.0f};
    bool invertLookY{false};
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

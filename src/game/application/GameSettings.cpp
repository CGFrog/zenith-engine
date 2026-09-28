#include "GameSettings.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {
constexpr int currentVersion = 1;
}

GameSettingsResult loadGameSettings(const std::filesystem::path& path, GameSettings& settings) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "Could not open game settings: " + path.string()};
    }
    nlohmann::json document;
    try {
        input >> document;
        if (!document.is_object() || document.value("version", 0) != currentVersion) {
            return {.error = "Unsupported game settings version: " + path.string()};
        }
        GameSettings loaded;
        const int antialiasing = document.at("antialiasing").get<int>();
        const int bloom = document.at("bloom").get<int>();
        loaded.masterMuted = document.at("masterMuted").get<bool>();
        loaded.lookSensitivity = document.at("lookSensitivity").get<float>();
        loaded.invertLookY = document.at("invertLookY").get<bool>();
        if (antialiasing < static_cast<int>(AntialiasingMode::Off) ||
            antialiasing > static_cast<int>(AntialiasingMode::Fxaa) || bloom < static_cast<int>(BloomQuality::Off) ||
            bloom > static_cast<int>(BloomQuality::Downsampled) || loaded.lookSensitivity < 0.25f ||
            loaded.lookSensitivity > 4.0f) {
            return {.error = "Game settings contain an out-of-range value: " + path.string()};
        }
        loaded.antialiasing = static_cast<AntialiasingMode>(antialiasing);
        loaded.bloom = static_cast<BloomQuality>(bloom);
        settings = loaded;
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "Could not parse game settings '" + path.string() + "': " + error.what()};
    }
}

GameSettingsResult saveGameSettings(const std::filesystem::path& path, const GameSettings& settings) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return {.error = "Could not create game settings directory: " + error.message()};
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        return {.error = "Could not write game settings: " + temporary.string()};
    }
    const nlohmann::json document = {{"version", currentVersion},
                                     {"antialiasing", static_cast<int>(settings.antialiasing)},
                                     {"bloom", static_cast<int>(settings.bloom)},
                                     {"masterMuted", settings.masterMuted},
                                     {"lookSensitivity", settings.lookSensitivity},
                                     {"invertLookY", settings.invertLookY}};
    output << document.dump(2) << '\n';
    output.close();
    if (!output) {
        return {.error = "Could not finish writing game settings: " + temporary.string()};
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        return {.error = "Could not replace game settings: " + error.message()};
    }
    return {.success = true};
}

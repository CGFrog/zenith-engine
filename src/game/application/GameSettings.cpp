#include "GameSettings.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
constexpr int currentVersion = 3;
}

GameSettingsResult loadGameSettings(const std::filesystem::path& path, GameSettings& settings) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "Could not open game settings: " + path.string()};
    }
    nlohmann::json document;
    try {
        input >> document;
        const int version = document.value("version", 0);
        if (!document.is_object() || version < 1 || version > currentVersion) {
            return {.error = "Unsupported game settings version: " + path.string()};
        }
        GameSettings loaded;
        const int antialiasing = document.at("antialiasing").get<int>();
        const int bloom = document.at("bloom").get<int>();
        loaded.masterMuted = document.at("masterMuted").get<bool>();
        loaded.lookSensitivity = document.at("lookSensitivity").get<float>();
        loaded.invertLookY = document.at("invertLookY").get<bool>();
        if (version >= 3) {
            loaded.masterVolume = document.at("masterVolume").get<float>();
        }
        if (version >= 2) {
            loaded.locale = document.value("locale", "en");
            const auto& accessibility = document.at("accessibility");
            loaded.accessibility.uiScale = accessibility.value("uiScale", 1.0f);
            loaded.accessibility.textScale = accessibility.value("textScale", 1.0f);
            loaded.accessibility.highContrast = accessibility.value("highContrast", false);
            loaded.accessibility.reducedMotion = accessibility.value("reducedMotion", false);
            loaded.accessibility.captionsEnabled = accessibility.value("captionsEnabled", true);
            loaded.accessibility.colorIndependentIndicators = accessibility.value("colorIndependentIndicators", true);
            loaded.inputProfiles = document.value("inputProfiles", std::vector<std::string>{"Default"});
            loaded.activeInputProfile = document.value("activeInputProfile", "Default");
        }
        if (antialiasing < static_cast<int>(AntialiasingMode::Off) ||
            antialiasing > static_cast<int>(AntialiasingMode::Taa) || bloom < static_cast<int>(BloomQuality::Off) ||
            bloom > static_cast<int>(BloomQuality::Downsampled) || !(loaded.masterVolume >= 0.0f) ||
            loaded.masterVolume > 1.0f || loaded.lookSensitivity < 0.25f || loaded.lookSensitivity > 4.0f ||
            loaded.accessibility.uiScale < 0.75f || loaded.accessibility.uiScale > 2.5f ||
            loaded.accessibility.textScale < 0.75f || loaded.accessibility.textScale > 3.0f || loaded.locale.empty() ||
            loaded.inputProfiles.empty() ||
            std::find(loaded.inputProfiles.begin(), loaded.inputProfiles.end(), loaded.activeInputProfile) ==
                loaded.inputProfiles.end()) {
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
    const nlohmann::json document = {
        {"version", currentVersion},
        {"antialiasing", static_cast<int>(settings.antialiasing)},
        {"bloom", static_cast<int>(settings.bloom)},
        {"masterMuted", settings.masterMuted},
        {"masterVolume", settings.masterVolume},
        {"lookSensitivity", settings.lookSensitivity},
        {"invertLookY", settings.invertLookY},
        {"locale", settings.locale},
        {"accessibility",
         {{"uiScale", settings.accessibility.uiScale},
          {"textScale", settings.accessibility.textScale},
          {"highContrast", settings.accessibility.highContrast},
          {"reducedMotion", settings.accessibility.reducedMotion},
          {"captionsEnabled", settings.accessibility.captionsEnabled},
          {"colorIndependentIndicators", settings.accessibility.colorIndependentIndicators}}},
        {"inputProfiles", settings.inputProfiles},
        {"activeInputProfile", settings.activeInputProfile}};
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

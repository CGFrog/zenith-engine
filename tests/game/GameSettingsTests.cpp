#include "game/application/GameSettings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "zenith_game_settings_tests";
    const std::filesystem::path path = directory / "settings.json";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    const GameSettings source{.antialiasing = AntialiasingMode::Off,
                              .bloom = BloomQuality::Downsampled,
                              .masterMuted = true,
                              .masterVolume = 0.35f,
                              .lookSensitivity = 1.5f,
                              .invertLookY = true,
                              .locale = "fr",
                              .accessibility = {.uiScale = 1.5f,
                                                .textScale = 1.75f,
                                                .highContrast = true,
                                                .reducedMotion = true,
                                                .captionsEnabled = true,
                                                .colorIndependentIndicators = true},
                              .inputProfiles = {"Keyboard", "Gamepad"},
                              .activeInputProfile = "Gamepad"};
    expect(static_cast<bool>(saveGameSettings(path, source)), "game settings should save atomically");
    GameSettings loaded;
    expect(static_cast<bool>(loadGameSettings(path, loaded)), "saved game settings should load");
    expect(loaded.antialiasing == source.antialiasing && loaded.bloom == source.bloom &&
               loaded.masterMuted == source.masterMuted && loaded.masterVolume == source.masterVolume &&
               loaded.lookSensitivity == source.lookSensitivity && loaded.invertLookY == source.invertLookY &&
               loaded.locale == source.locale && loaded.accessibility.uiScale == source.accessibility.uiScale &&
               loaded.accessibility.highContrast == source.accessibility.highContrast &&
               loaded.activeInputProfile == source.activeInputProfile && loaded.inputProfiles.size() == 2,
           "render, audio, and input settings should round-trip");

    std::ofstream invalid(path, std::ios::trunc);
    invalid
        << R"({"version":1,"antialiasing":12,"bloom":0,"masterMuted":false,"lookSensitivity":1.0,"invertLookY":false})";
    invalid.close();
    const GameSettings preserved = loaded;
    expect(!loadGameSettings(path, loaded), "out-of-range settings should be rejected");
    expect(loaded.lookSensitivity == preserved.lookSensitivity && loaded.masterMuted == preserved.masterMuted,
           "a failed settings load preserves the active configuration");

    std::ofstream legacy(path, std::ios::trunc);
    legacy
        << R"({"version":1,"antialiasing":1,"bloom":0,"masterMuted":false,"lookSensitivity":1.0,"invertLookY":false})";
    legacy.close();
    GameSettings migrated;
    expect(loadGameSettings(path, migrated) && migrated.locale == "en" && migrated.activeInputProfile == "Default" &&
               migrated.masterVolume == 1.0f,
           "version 1 settings migrate atomically to accessibility-safe defaults and full volume");

    std::ofstream loud(path, std::ios::trunc);
    loud << R"({"version":3,"antialiasing":1,"bloom":0,"masterMuted":false,"masterVolume":1.5,"lookSensitivity":1.0,)"
         << R"("invertLookY":false,"accessibility":{}})";
    loud.close();
    expect(!loadGameSettings(path, migrated), "a master volume above 1 is rejected");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}

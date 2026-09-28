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
                              .lookSensitivity = 1.5f,
                              .invertLookY = true};
    expect(static_cast<bool>(saveGameSettings(path, source)), "game settings should save atomically");
    GameSettings loaded;
    expect(static_cast<bool>(loadGameSettings(path, loaded)), "saved game settings should load");
    expect(loaded.antialiasing == source.antialiasing && loaded.bloom == source.bloom &&
               loaded.masterMuted == source.masterMuted && loaded.lookSensitivity == source.lookSensitivity &&
               loaded.invertLookY == source.invertLookY,
           "render, audio, and input settings should round-trip");

    std::ofstream invalid(path, std::ios::trunc);
    invalid
        << R"({"version":1,"antialiasing":12,"bloom":0,"masterMuted":false,"lookSensitivity":1.0,"invertLookY":false})";
    invalid.close();
    const GameSettings preserved = loaded;
    expect(!loadGameSettings(path, loaded), "out-of-range settings should be rejected");
    expect(loaded.lookSensitivity == preserved.lookSensitivity && loaded.masterMuted == preserved.masterMuted,
           "a failed settings load preserves the active configuration");

    std::filesystem::remove_all(directory, error);
    return failures == 0 ? 0 : 1;
}

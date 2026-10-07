// Game settings (game/application/GameSettings): render, audio, input, and
// accessibility settings round-trip, invalid files are refused without
// changing the active settings, and older versions migrate.

#include "game/application/GameSettings.hpp"

#include "support/Doctest.hpp"
#include "support/Files.hpp"
#include "support/TempDirectory.hpp"

TEST_SUITE_BEGIN("GameSettings");

TEST_CASE("GameSettings: render audio input and accessibility settings round-trip") {
    const testsupport::TempDirectory directory("game-settings-round-trip");
    const std::filesystem::path path = directory / "settings.json";
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
    CHECK_MESSAGE(static_cast<bool>(saveGameSettings(path, source)), "game settings should save atomically");
    GameSettings loaded;
    CHECK_MESSAGE(static_cast<bool>(loadGameSettings(path, loaded)), "saved game settings should load");
    CHECK_MESSAGE((loaded.antialiasing == source.antialiasing && loaded.bloom == source.bloom &&
                   loaded.masterMuted == source.masterMuted && loaded.masterVolume == source.masterVolume &&
                   loaded.lookSensitivity == source.lookSensitivity && loaded.invertLookY == source.invertLookY &&
                   loaded.locale == source.locale && loaded.accessibility.uiScale == source.accessibility.uiScale &&
                   loaded.accessibility.highContrast == source.accessibility.highContrast &&
                   loaded.activeInputProfile == source.activeInputProfile && loaded.inputProfiles.size() == 2),
                  "render, audio, and input settings should round-trip");
}

TEST_CASE("GameSettings: out-of-range settings are refused and the active ones kept") {
    const testsupport::TempDirectory directory("game-settings-invalid");
    const std::filesystem::path path = directory / "settings.json";
    testsupport::writeText(
        path,
        R"({"version":1,"antialiasing":12,"bloom":0,"masterMuted":false,"lookSensitivity":1.0,"invertLookY":false})");
    GameSettings loaded{.masterMuted = true, .lookSensitivity = 1.5f};
    const GameSettings preserved = loaded;
    CHECK_MESSAGE(!loadGameSettings(path, loaded), "out-of-range settings should be rejected");
    CHECK_MESSAGE((loaded.lookSensitivity == preserved.lookSensitivity && loaded.masterMuted == preserved.masterMuted),
                  "a failed settings load preserves the active configuration");

    testsupport::writeText(path, R"({"version":3,"antialiasing":1,"bloom":0,"masterMuted":false,"masterVolume":1.5,)"
                                 R"("lookSensitivity":1.0,"invertLookY":false,"accessibility":{}})");
    CHECK_MESSAGE(!loadGameSettings(path, loaded), "a master volume above 1 is rejected");
}

TEST_CASE("GameSettings: version 1 settings migrate to safe defaults and full volume") {
    const testsupport::TempDirectory directory("game-settings-migrate");
    const std::filesystem::path path = directory / "settings.json";
    testsupport::writeText(
        path,
        R"({"version":1,"antialiasing":1,"bloom":0,"masterMuted":false,"lookSensitivity":1.0,"invertLookY":false})");
    GameSettings migrated;
    CHECK_MESSAGE((loadGameSettings(path, migrated) && migrated.locale == "en" &&
                   migrated.activeInputProfile == "Default" && migrated.masterVolume == 1.0f),
                  "version 1 settings migrate atomically to accessibility-safe defaults and full volume");
}

TEST_SUITE_END();

#pragma once

#include "engine/platform/PlatformServices.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Reading and writing a JSON object whose top level carries its format
// version: the shape of the settings and state files the engine, games, and
// editor keep. The helpers check the envelope (the file, the object, the
// version); what the object holds, and how an older version migrates, stays
// with the caller.

enum class MissingJsonFile : uint8_t {
    // A missing file is not an error; the caller uses its defaults.
    UseDefaults,
    Error,
};

enum class NewerJsonVersions : uint8_t {
    Reject,
    // For files a newer build may write and an older one can still read, such
    // as per-scene editor data kept under version control.
    Read,
};

struct VersionedJsonFormat {
    // What the file holds, for error messages ("game settings").
    std::string_view description;
    std::string_view versionKey = "version";
    int firstVersion = 1;
    int currentVersion = 1;
    MissingJsonFile missing = MissingJsonFile::UseDefaults;
    NewerJsonVersions newer = NewerJsonVersions::Reject;
    UserDataBackup backup = UserDataBackup::None;
};

struct VersionedJsonResult {
    bool success{false};
    // False when the file does not exist; with MissingJsonFile::UseDefaults
    // that still succeeds, with an empty document.
    bool found{false};
    // The file's version: from firstVersion to currentVersion (or above it,
    // with NewerJsonVersions::Read). Below currentVersion the caller migrates.
    int version{0};
    nlohmann::json document;
    // "Cannot read <description> <path>: <reason>" or
    // "Invalid <description> in <path> (<reason>)".
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

VersionedJsonResult readVersionedJson(const std::filesystem::path& path, const VersionedJsonFormat& format);

// Writes document (an object) with format.currentVersion under the version
// key, indented by two spaces with a final newline, through
// atomicWriteUserData. Invalid UTF-8 in strings is replaced, not thrown.
PlatformResult writeVersionedJson(const std::filesystem::path& path, const VersionedJsonFormat& format,
                                  nlohmann::json document);

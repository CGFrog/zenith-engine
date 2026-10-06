#pragma once

// User-data directories, atomic writes and backups, crash reports, diagnostic bundles, provenance.

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct PlatformResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Whether atomicWriteUserData copies the file it replaces to `<path>.bak`
// first, for recoverAtomicUserData.
enum class UserDataBackup : uint8_t {
    Keep,
    None,
};

std::filesystem::path userDataDirectory(std::string_view organization, std::string_view application);
// Writes bytes to `<path>.tmp` and renames it over path, creating the parent
// folder, so an interrupted write never leaves a truncated file.
PlatformResult atomicWriteUserData(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                                   UserDataBackup backup = UserDataBackup::Keep);
PlatformResult recoverAtomicUserData(const std::filesystem::path& path);

struct CrashReport {
    std::string timestampUtc;
    std::string productVersion;
    std::string reason;
    std::map<std::string, std::string> context;
};

class DiagnosticBundleWriter {
  public:
    explicit DiagnosticBundleWriter(std::filesystem::path directory);
    PlatformResult write(const CrashReport& report, std::span<const std::filesystem::path> attachments,
                         std::filesystem::path& output) const;

    static constexpr bool automaticallyUploads() {
        return false;
    }

  private:
    std::filesystem::path m_directory;
};

struct ReleaseProvenance {
    int formatVersion{1};
    std::string sourceRevision;
    std::string dependencyRevision;
    std::string toolchain;
    std::string contentManifestDigest;
    std::string targetPlatform;
};

PlatformResult writeReleaseProvenance(const std::filesystem::path& path, const ReleaseProvenance& provenance);
PlatformResult readReleaseProvenance(const std::filesystem::path& path, ReleaseProvenance& provenance);

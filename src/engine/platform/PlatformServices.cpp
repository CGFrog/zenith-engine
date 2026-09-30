#include "engine/platform/PlatformServices.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
std::filesystem::path environmentPath(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    const std::filesystem::path path(value);
    std::free(value);
    return path;
#else
    const char* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}
} // namespace

std::filesystem::path userDataDirectory(std::string_view organization, std::string_view application) {
#ifdef _WIN32
    std::filesystem::path root = environmentPath("LOCALAPPDATA");
    if (root.empty()) {
        root = environmentPath("APPDATA");
    }
#elif defined(__APPLE__)
    std::filesystem::path root = environmentPath("HOME") / "Library" / "Application Support";
#else
    std::filesystem::path root = environmentPath("XDG_DATA_HOME");
    if (root.empty()) {
        root = environmentPath("HOME") / ".local" / "share";
    }
#endif
    return root / std::filesystem::path(organization) / std::filesystem::path(application);
}

PlatformResult atomicWriteUserData(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return {.error = "could not create user-data directory: " + error.message()};
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    const std::filesystem::path backup = path.string() + ".bak";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return {.error = "could not finish user-data write: " + temporary.string()};
    }
    if (std::filesystem::exists(path)) {
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return {.error = "could not preserve user-data backup: " + error.message()};
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return error ? PlatformResult{.error = "could not atomically replace user data: " + error.message()}
                 : PlatformResult{.success = true};
}

PlatformResult recoverAtomicUserData(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        return {.success = true};
    }
    const std::filesystem::path backup = path.string() + ".bak";
    if (!std::filesystem::exists(backup)) {
        return {.error = "user data and its recovery backup are missing"};
    }
    std::error_code error;
    std::filesystem::copy_file(backup, path, std::filesystem::copy_options::overwrite_existing, error);
    return error ? PlatformResult{.error = "could not recover user data: " + error.message()}
                 : PlatformResult{.success = true};
}

DiagnosticBundleWriter::DiagnosticBundleWriter(std::filesystem::path directory)
    : m_directory(std::move(directory)) {}

PlatformResult DiagnosticBundleWriter::write(const CrashReport& report,
                                             std::span<const std::filesystem::path> attachments,
                                             std::filesystem::path& output) const {
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        return {.error = "could not create diagnostic directory: " + error.message()};
    }
    output = m_directory / ("diagnostic-" + report.timestampUtc + ".json");
    nlohmann::json files = nlohmann::json::array();
    for (const std::filesystem::path& attachment : attachments) {
        if (attachment.is_absolute()) {
            files.push_back(attachment.filename().generic_string());
        } else {
            files.push_back(attachment.generic_string());
        }
    }
    const nlohmann::json document = {
        {"formatVersion", 1},      {"timestampUtc", report.timestampUtc}, {"productVersion", report.productVersion},
        {"reason", report.reason}, {"context", report.context},           {"attachments", files},
        {"uploaded", false}};
    const std::string encoded = document.dump(2) + '\n';
    return atomicWriteUserData(output, {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()});
}

PlatformResult writeReleaseProvenance(const std::filesystem::path& path, const ReleaseProvenance& provenance) {
    const nlohmann::json document = {{"formatVersion", provenance.formatVersion},
                                     {"sourceRevision", provenance.sourceRevision},
                                     {"dependencyRevision", provenance.dependencyRevision},
                                     {"toolchain", provenance.toolchain},
                                     {"contentManifestDigest", provenance.contentManifestDigest},
                                     {"targetPlatform", provenance.targetPlatform}};
    const std::string encoded = document.dump(2) + '\n';
    return atomicWriteUserData(path, {reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size()});
}

PlatformResult readReleaseProvenance(const std::filesystem::path& path, ReleaseProvenance& provenance) {
    std::ifstream input(path);
    if (!input) {
        return {.error = "could not open release provenance: " + path.string()};
    }
    try {
        nlohmann::json document;
        input >> document;
        ReleaseProvenance loaded;
        loaded.formatVersion = document.value("formatVersion", 0);
        if (loaded.formatVersion != 1) {
            return {.error = "unsupported release provenance version"};
        }
        loaded.sourceRevision = document.at("sourceRevision").get<std::string>();
        loaded.dependencyRevision = document.at("dependencyRevision").get<std::string>();
        loaded.toolchain = document.at("toolchain").get<std::string>();
        loaded.contentManifestDigest = document.at("contentManifestDigest").get<std::string>();
        loaded.targetPlatform = document.at("targetPlatform").get<std::string>();
        provenance = std::move(loaded);
        return {.success = true};
    } catch (const nlohmann::json::exception& error) {
        return {.error = "could not parse release provenance: " + std::string(error.what())};
    }
}

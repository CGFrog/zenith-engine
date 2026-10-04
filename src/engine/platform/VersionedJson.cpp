#include "engine/platform/VersionedJson.hpp"

#include "engine/platform/FileContents.hpp"

#include <system_error>

namespace {
std::string pathText(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

VersionedJsonResult invalid(const std::filesystem::path& path, const VersionedJsonFormat& format,
                            const std::string& reason) {
    return {.found = true,
            .error = "Invalid " + std::string(format.description) + " in " + pathText(path) + " (" + reason + ")"};
}
} // namespace

VersionedJsonResult readVersionedJson(const std::filesystem::path& path, const VersionedJsonFormat& format) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return {.error = "Cannot read " + std::string(format.description) + " " + pathText(path) + ": " +
                             error.message()};
        }
        if (format.missing == MissingJsonFile::UseDefaults) {
            return {.success = true};
        }
        return {.error = "Cannot read " + std::string(format.description) + " " + pathText(path) +
                         ": the file does not exist"};
    }
    std::string text;
    if (readFileText(path, text) != FileReadStatus::Success) {
        return {.found = true,
                .error = "Cannot read " + std::string(format.description) + " " + pathText(path) +
                         ": the file could not be read"};
    }
    nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return invalid(path, format, "the file is not a JSON object");
    }
    const auto version = document.find(format.versionKey);
    if (version == document.end() || !version->is_number_integer()) {
        return invalid(path, format, "it has no integer \"" + std::string(format.versionKey) + "\"");
    }
    const int64_t value = version->get<int64_t>();
    const bool tooNew = value > format.currentVersion && format.newer == NewerJsonVersions::Reject;
    if (value < format.firstVersion || tooNew) {
        return invalid(path, format,
                       "unsupported version " + std::to_string(value) + "; this build reads " +
                           std::to_string(format.firstVersion) + " to " + std::to_string(format.currentVersion));
    }
    return {.success = true, .found = true, .version = static_cast<int>(value), .document = std::move(document)};
}

PlatformResult writeVersionedJson(const std::filesystem::path& path, const VersionedJsonFormat& format,
                                  nlohmann::json document) {
    if (!document.is_object()) {
        return {.error = "Cannot write " + std::string(format.description) + ": the document is not a JSON object"};
    }
    document[std::string(format.versionKey)] = format.currentVersion;
    const std::string text = document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
    PlatformResult written =
        atomicWriteUserData(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, format.backup);
    if (!written) {
        written.error = "Cannot write " + std::string(format.description) + " " + pathText(path) + ": " + written.error;
    }
    return written;
}

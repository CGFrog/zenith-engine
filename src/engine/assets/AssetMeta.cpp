#include "engine/assets/AssetMeta.hpp"
#include "engine/platform/FileContents.hpp"

#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
AssetMetaResult replaceFile(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {.error = "could not replace asset meta (Windows error " + std::to_string(GetLastError()) + ")"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return {.error = "could not replace asset meta: " + error.message()};
    }
#endif
    return {.success = true};
}
} // namespace

std::filesystem::path assetMetaPath(const std::filesystem::path& assetPath) {
    std::filesystem::path metaPath = assetPath;
    metaPath += assetMetaExtension;
    return metaPath;
}

AssetMetaResult parseAssetMeta(std::string_view text, AssetMeta& meta) {
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "asset meta is not a JSON object"};
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer()) {
        return {.error = "asset meta has no integer version"};
    }
    const int parsedVersion = version->get<int>();
    if (parsedVersion < 1 || parsedVersion > AssetMeta::currentVersion) {
        return {.error = "unsupported asset meta version " + std::to_string(parsedVersion)};
    }
    const auto guidText = document.find("guid");
    if (guidText == document.end() || !guidText->is_string()) {
        return {.error = "asset meta has no guid"};
    }
    const std::optional<AssetGuid> guid = AssetGuid::parse(guidText->get<std::string>());
    if (!guid || guid->isNull()) {
        return {.error = "asset meta guid is not 32 hexadecimal digits"};
    }
    nlohmann::json importSettings = nlohmann::json::object();
    if (const auto settings = document.find("importSettings"); settings != document.end()) {
        if (!settings->is_object()) {
            return {.error = "asset meta importSettings is not an object"};
        }
        importSettings = *settings;
    }
    meta = {.version = parsedVersion, .guid = *guid, .importSettings = std::move(importSettings)};
    return {.success = true};
}

std::string serializeAssetMeta(const AssetMeta& meta) {
    nlohmann::ordered_json document;
    document["version"] = AssetMeta::currentVersion;
    document["guid"] = meta.guid.toString();
    document["importSettings"] = meta.importSettings.is_object() ? meta.importSettings : nlohmann::json::object();
    return document.dump(4) + "\n";
}

AssetMetaResult readAssetMeta(const std::filesystem::path& metaPath, AssetMeta& meta) {
    std::string text;
    if (const FileReadStatus status = readFileText(metaPath, text); status != FileReadStatus::Success) {
        return {.error =
                    status == FileReadStatus::OpenFailed ? "could not open asset meta" : "could not read asset meta"};
    }
    return parseAssetMeta(text, meta);
}

AssetMetaResult writeAssetMeta(const std::filesystem::path& metaPath, const AssetMeta& meta) {
    if (meta.guid.isNull()) {
        return {.error = "refusing to write an asset meta with a null guid"};
    }
    const std::string text = serializeAssetMeta(meta);
    std::filesystem::path temporary = metaPath;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return {.error = "could not write asset meta"};
        }
    }
    if (AssetMetaResult result = replaceFile(temporary, metaPath); !result) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return result;
    }
    return {.success = true};
}

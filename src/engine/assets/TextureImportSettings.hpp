#pragma once

#include "engine/assets/Assets.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

class AssetDatabase;

// How a standalone texture (an image a scene component or a tool loads by
// path) is imported, stored in its `.meta` sidecar's `importSettings` object
// (see AssetMeta.hpp):
//
//   {"usage": "color"}     // or "normal" or "data"
//
// usage decides how the texels are read: Color is sRGB-encoded, Normal and
// Data are linear. A component slot that samples colour (a decal's albedo)
// samples a Normal or Data texture linearly. Material files keep the usage of
// the slot that names the image. Missing keys keep their defaults; other keys
// are left alone, so other importers' settings survive a round trip.
struct TextureImportSettings {
    TextureUsage usage{TextureUsage::Color};

    bool operator==(const TextureImportSettings&) const = default;
};

// "color", "normal", or "data".
std::string_view textureUsageName(TextureUsage usage);
std::optional<TextureUsage> parseTextureUsage(std::string_view name);

// Reads the texture keys of an import settings object. Returns an empty
// string on success, else why the settings are invalid (output then holds the
// defaults).
std::string readTextureImportSettings(const nlohmann::json& settings, TextureImportSettings& output);
// Writes the texture keys into settings, which must be a JSON object; other
// keys are kept.
void writeTextureImportSettings(const TextureImportSettings& input, nlohmann::json& settings);

// The import settings the database holds for an absolute image path, or the
// defaults when the database does not track it or its settings are invalid.
TextureImportSettings textureImportSettingsFor(const AssetDatabase& database, const std::filesystem::path& image);

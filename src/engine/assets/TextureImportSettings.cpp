#include "TextureImportSettings.hpp"

#include "engine/assets/AssetDatabase.hpp"

#include <nlohmann/json.hpp>

namespace {
constexpr const char* usageKey = "usage";
} // namespace

std::string_view textureUsageName(TextureUsage usage) {
    switch (usage) {
    case TextureUsage::Color:
        return "color";
    case TextureUsage::Normal:
        return "normal";
    case TextureUsage::Data:
        return "data";
    }
    return "color";
}

std::optional<TextureUsage> parseTextureUsage(std::string_view name) {
    for (const TextureUsage usage : {TextureUsage::Color, TextureUsage::Normal, TextureUsage::Data}) {
        if (textureUsageName(usage) == name) {
            return usage;
        }
    }
    return std::nullopt;
}

std::string readTextureImportSettings(const nlohmann::json& settings, TextureImportSettings& output) {
    output = {};
    if (!settings.is_object()) {
        return "import settings must be a JSON object";
    }
    const auto usage = settings.find(usageKey);
    if (usage == settings.end()) {
        return {};
    }
    const std::optional<TextureUsage> parsed =
        usage->is_string() ? parseTextureUsage(usage->get<std::string>()) : std::nullopt;
    if (!parsed) {
        return "texture usage must be \"color\", \"normal\", or \"data\"";
    }
    output.usage = *parsed;
    return {};
}

void writeTextureImportSettings(const TextureImportSettings& input, nlohmann::json& settings) {
    if (!settings.is_object()) {
        settings = nlohmann::json::object();
    }
    settings[usageKey] = std::string(textureUsageName(input.usage));
}

TextureImportSettings textureImportSettingsFor(const AssetDatabase& database, const std::filesystem::path& image) {
    const std::filesystem::path relative = image.lexically_normal().lexically_relative(database.root());
    if (relative.empty() || *relative.begin() == "..") {
        return {};
    }
    const std::u8string text = relative.generic_u8string();
    const std::optional<AssetGuid> guid = database.guidForPath(std::string(text.begin(), text.end()));
    const AssetRecord* record = guid ? database.find(*guid) : nullptr;
    TextureImportSettings settings;
    if (record == nullptr || !readTextureImportSettings(record->importSettings, settings).empty()) {
        return {};
    }
    return settings;
}

#include "MaterialAssetFile.hpp"

#include "engine/platform/FileContents.hpp"
#include "engine/platform/PlatformServices.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>

namespace {
using Json = nlohmann::json;

constexpr std::array<const char*, materialTextureSlotCount> textureKeys{
    "base_color_texture", "normal_texture", "metallic_roughness_texture", "occlusion_texture", "emissive_texture"};

struct AlphaModeName {
    MaterialAlphaMode mode;
    const char* name;
};

constexpr std::array<AlphaModeName, 3> alphaModeNames{
    {{MaterialAlphaMode::Opaque, "opaque"}, {MaterialAlphaMode::Mask, "mask"}, {MaterialAlphaMode::Blend, "blend"}}};

bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

template <size_t Count> void readFloats(const Json& document, const char* key, float* output) {
    const auto found = document.find(key);
    if (found == document.end()) {
        return;
    }
    if (!found->is_array() || found->size() != Count) {
        throw std::invalid_argument(std::string(key) + " must be an array of " + std::to_string(Count) + " numbers");
    }
    for (size_t index = 0; index < Count; ++index) {
        output[index] = (*found)[index].get<float>();
    }
}

void readFloat(const Json& document, const char* key, float& output) {
    if (const auto found = document.find(key); found != document.end()) {
        output = found->get<float>();
    }
}

MaterialTextureReference readTexture(const Json& reference, const char* key) {
    if (!reference.is_object()) {
        throw std::invalid_argument(std::string(key) + " must be an object with a path");
    }
    MaterialTextureReference texture{.path = reference.at("path").get<std::string>()};
    if (const auto guid = reference.find("guid"); guid != reference.end()) {
        const std::optional<AssetGuid> parsed = AssetGuid::parse(guid->get<std::string>());
        if (!parsed) {
            throw std::invalid_argument(std::string(key) + " guid must be 32 hexadecimal digits");
        }
        texture.guid = *parsed;
    }
    return texture;
}

std::string validate(const MaterialFile& material) {
    const bool finiteColors = std::isfinite(material.baseColor.r) && std::isfinite(material.baseColor.g) &&
                              std::isfinite(material.baseColor.b) && std::isfinite(material.baseColor.a) &&
                              std::isfinite(material.emissive.r) && std::isfinite(material.emissive.g) &&
                              std::isfinite(material.emissive.b);
    const bool validFactors = finiteNonNegative(material.emissiveStrength) && finiteNonNegative(material.metallic) &&
                              finiteNonNegative(material.roughness) && finiteNonNegative(material.normalScale) &&
                              finiteNonNegative(material.occlusionStrength) && finiteNonNegative(material.alphaCutoff);
    return finiteColors && validFactors ? std::string{} : "material factors must be finite and not negative";
}
} // namespace

const char* materialTextureKey(MaterialTextureSlot slot) {
    return textureKeys[static_cast<size_t>(slot)];
}

TextureUsage materialTextureUsage(MaterialTextureSlot slot) {
    switch (slot) {
    case MaterialTextureSlot::BaseColor:
    case MaterialTextureSlot::Emissive:
        return TextureUsage::Color;
    case MaterialTextureSlot::Normal:
        return TextureUsage::Normal;
    case MaterialTextureSlot::MetallicRoughness:
    case MaterialTextureSlot::Occlusion:
        return TextureUsage::Data;
    }
    return TextureUsage::Color;
}

MaterialFileResult parseMaterialFile(std::string_view text, MaterialFile& output) {
    MaterialFile material;
    try {
        const Json document = Json::parse(text);
        if (!document.is_object()) {
            return {.error = "A material file must be a JSON object"};
        }
        const uint32_t version = document.value("version", 1u);
        if (version == 0 || version > materialFileVersion) {
            return {.error = "Material file version " + std::to_string(version) + " is not supported (newest is " +
                             std::to_string(materialFileVersion) + ")"};
        }
        readFloats<4>(document, "base_color", &material.baseColor.x);
        readFloats<3>(document, "emissive", &material.emissive.x);
        readFloat(document, "emissive_strength", material.emissiveStrength);
        readFloat(document, "metallic", material.metallic);
        readFloat(document, "roughness", material.roughness);
        readFloat(document, "normal_scale", material.normalScale);
        readFloat(document, "occlusion_strength", material.occlusionStrength);
        readFloat(document, "alpha_cutoff", material.alphaCutoff);
        material.doubleSided = document.value("double_sided", false);
        if (const auto mode = document.find("alpha_mode"); mode != document.end()) {
            const std::string name = mode->get<std::string>();
            const auto found =
                std::ranges::find_if(alphaModeNames, [&](const AlphaModeName& entry) { return name == entry.name; });
            if (found == alphaModeNames.end()) {
                return {.error = "Unknown material alpha mode '" + name + "'"};
            }
            material.alphaMode = found->mode;
        }
        for (size_t slot = 0; slot < materialTextureSlotCount; ++slot) {
            if (const auto reference = document.find(textureKeys[slot]); reference != document.end()) {
                material.textures[slot] = readTexture(*reference, textureKeys[slot]);
            }
        }
    } catch (const std::exception& error) {
        return {.error = std::string("Invalid material file: ") + error.what()};
    }
    if (std::string error = validate(material); !error.empty()) {
        return {.error = "Invalid material file: " + error};
    }
    output = std::move(material);
    return {.success = true};
}

std::string serializeMaterialFile(const MaterialFile& material) {
    const char* alphaMode = "opaque";
    for (const AlphaModeName& entry : alphaModeNames) {
        if (entry.mode == material.alphaMode) {
            alphaMode = entry.name;
        }
    }
    Json document = {
        {"version", materialFileVersion},
        {"base_color", {material.baseColor.r, material.baseColor.g, material.baseColor.b, material.baseColor.a}},
        {"emissive", {material.emissive.r, material.emissive.g, material.emissive.b}},
        {"emissive_strength", material.emissiveStrength},
        {"metallic", material.metallic},
        {"roughness", material.roughness},
        {"normal_scale", material.normalScale},
        {"occlusion_strength", material.occlusionStrength},
        {"alpha_mode", alphaMode},
        {"alpha_cutoff", material.alphaCutoff},
        {"double_sided", material.doubleSided},
    };
    for (size_t slot = 0; slot < materialTextureSlotCount; ++slot) {
        const MaterialTextureReference& texture = material.textures[slot];
        if (texture.path.empty()) {
            continue;
        }
        Json reference = {{"path", texture.path}};
        if (texture.guid) {
            reference["guid"] = texture.guid.toString();
        }
        document[textureKeys[slot]] = std::move(reference);
    }
    return document.dump(2) + '\n';
}

MaterialFileResult readMaterialFile(const std::filesystem::path& path, MaterialFile& output) {
    std::string text;
    if (readFileText(path, text) != FileReadStatus::Success) {
        const std::u8string name = path.u8string();
        return {.error = "Could not read material file " + std::string(name.begin(), name.end())};
    }
    return parseMaterialFile(text, output);
}

MaterialFileResult writeMaterialFile(const std::filesystem::path& path, const MaterialFile& material) {
    const std::string text = serializeMaterialFile(material);
    const PlatformResult written = atomicWriteUserData(
        path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), UserDataBackup::None);
    if (!written) {
        return {.error = written.error};
    }
    return {.success = true};
}

#pragma once

#include "AssetGuid.hpp"
#include "Assets.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// A material asset file (.zmat): one standalone metallic-roughness material,
// independent of any model, written as JSON:
//
//   {"version": 1, "base_color": [1, 1, 1, 1], "emissive": [0, 0, 0],
//    "emissive_strength": 1, "metallic": 0, "roughness": 0.5,
//    "normal_scale": 1, "occlusion_strength": 1, "alpha_mode": "opaque",
//    "alpha_cutoff": 0.5, "double_sided": false,
//    "base_color_texture": {"guid": "0f8e...", "path": "stone.png"}}
//
// Factors are linear. Each texture slot is optional and names an image by its
// path relative to the material file's folder (forward slashes, like a glTF
// URI), so a folder holding a material and its textures can move as a whole.
// The GUID is the image's identity in the project's asset database when the
// tool that wrote the file knew it; loading uses the path, and editors use the
// GUID to follow a texture that moved on its own. Unknown keys are ignored and
// missing values keep MaterialFile's defaults.
inline constexpr uint32_t materialFileVersion = 1;
inline constexpr const char* materialFileExtension = ".zmat";

enum class MaterialTextureSlot : uint8_t {
    BaseColor,
    Normal,
    MetallicRoughness,
    Occlusion,
    Emissive,
};

inline constexpr size_t materialTextureSlotCount = 5;

struct MaterialTextureReference {
    // Relative to the material file's folder; empty for no texture.
    std::string path;
    AssetGuid guid;

    bool operator==(const MaterialTextureReference&) const = default;
};

struct MaterialFile {
    glm::vec4 baseColor{1.0f};
    glm::vec3 emissive{0.0f};
    float emissiveStrength{1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
    float alphaCutoff{0.5f};
    bool doubleSided{false};
    // Indexed by MaterialTextureSlot.
    std::array<MaterialTextureReference, materialTextureSlotCount> textures;

    bool operator==(const MaterialFile&) const = default;
};

struct MaterialFileResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// The document key of a slot, for example "base_color_texture".
const char* materialTextureKey(MaterialTextureSlot slot);
// How a slot's image is read: base color and emissive as sRGB color, normal
// as a tangent-space normal, metallic-roughness and occlusion as linear data.
TextureUsage materialTextureUsage(MaterialTextureSlot slot);

// Reads a document; fails on malformed JSON, a wrong value type, non-finite
// or negative factors, or a version newer than materialFileVersion.
MaterialFileResult parseMaterialFile(std::string_view text, MaterialFile& output);
// The document text for material, at materialFileVersion; absent textures are
// left out.
std::string serializeMaterialFile(const MaterialFile& material);
MaterialFileResult readMaterialFile(const std::filesystem::path& path, MaterialFile& output);
// Writes the document atomically (a temporary file renamed over path).
MaterialFileResult writeMaterialFile(const std::filesystem::path& path, const MaterialFile& material);

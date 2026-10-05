#include "MaterialDocument.hpp"

#include "editor/project/AssetCatalog.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "engine/assets/AssetDatabase.hpp"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace {
using TextureMember = std::string MaterialDocument::*;

// Indexed by MaterialTextureSlot.
constexpr std::array<TextureMember, materialTextureSlotCount> textureMembers{
    &MaterialDocument::baseColorTexture, &MaterialDocument::normalTexture, &MaterialDocument::metallicRoughnessTexture,
    &MaterialDocument::occlusionTexture, &MaterialDocument::emissiveTexture};

std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::vector<FieldDescriptor> buildFields() {
    using D = MaterialDocument;
    Fields<D> fields;
    fields.add("base_color", &D::baseColor)
        .color()
        .add("base_color_texture", &D::baseColorTexture)
        .label("Base color map")
        .asset("texture")
        .add("metallic", &D::metallic)
        .slider(0.0, 1.0)
        .add("roughness", &D::roughness)
        .slider(0.0, 1.0)
        .add("metallic_roughness_texture", &D::metallicRoughnessTexture)
        .label("Metallic-roughness map")
        .asset("texture")
        .tooltip("Roughness in green, metallic in blue (glTF convention)")
        .add("normal_texture", &D::normalTexture)
        .label("Normal map")
        .asset("texture")
        .add("normal_scale", &D::normalScale)
        .drag(0.01f, 0.0, 10.0)
        .add("occlusion_texture", &D::occlusionTexture)
        .label("Occlusion map")
        .asset("texture")
        .add("occlusion_strength", &D::occlusionStrength)
        .slider(0.0, 1.0)
        .add("emissive", &D::emissive)
        .color()
        .add("emissive_strength", &D::emissiveStrength)
        .drag(0.05f, 0.0, 10000.0)
        .add("emissive_texture", &D::emissiveTexture)
        .label("Emissive map")
        .asset("texture")
        .add("alpha_mode", &D::alphaMode)
        .options({{"opaque", "Opaque", static_cast<int64_t>(MaterialAlphaMode::Opaque)},
                  {"mask", "Mask", static_cast<int64_t>(MaterialAlphaMode::Mask)},
                  {"blend", "Blend", static_cast<int64_t>(MaterialAlphaMode::Blend)}})
        .add("alpha_cutoff", &D::alphaCutoff)
        .slider(0.0, 1.0)
        .add("double_sided", &D::doubleSided);
    return std::move(fields).build();
}

// The absolute file a texture reference names: the GUID's current path when
// the database knows it, else the path relative to the material's folder.
std::filesystem::path resolveTexture(const std::filesystem::path& file, const MaterialTextureReference& reference,
                                     const AssetDatabase* database) {
    if (database != nullptr && reference.guid) {
        if (const std::optional<std::string> current = database->pathForGuid(reference.guid)) {
            return normalizedCatalogPath(database->root() / pathFromUtf8Text(*current));
        }
    }
    return normalizedCatalogPath(file.parent_path() / pathFromUtf8Text(reference.path));
}
} // namespace

std::span<const FieldDescriptor> materialDocumentFields() {
    static const std::vector<FieldDescriptor> fields = buildFields();
    return fields;
}

MaterialFileResult loadMaterialDocument(const std::filesystem::path& file, const MaterialDocumentLocation& location,
                                        MaterialDocument& output) {
    MaterialFile material;
    if (MaterialFileResult read = readMaterialFile(file, material); !read) {
        return read;
    }
    MaterialDocument document{.baseColor = material.baseColor,
                              .emissive = material.emissive,
                              .emissiveStrength = material.emissiveStrength,
                              .metallic = material.metallic,
                              .roughness = material.roughness,
                              .normalScale = material.normalScale,
                              .occlusionStrength = material.occlusionStrength,
                              .alphaMode = material.alphaMode,
                              .alphaCutoff = material.alphaCutoff,
                              .doubleSided = material.doubleSided};
    const std::filesystem::path root = normalizedCatalogPath(location.assetRoot);
    for (size_t slot = 0; slot < materialTextureSlotCount; ++slot) {
        const MaterialTextureReference& reference = material.textures[slot];
        if (reference.path.empty() && !reference.guid) {
            continue;
        }
        document.*textureMembers[slot] = genericUtf8(
            resolveTexture(normalizedCatalogPath(file), reference, location.database).lexically_relative(root));
    }
    output = std::move(document);
    return {.success = true};
}

MaterialFileResult saveMaterialDocument(const std::filesystem::path& file, const MaterialDocumentLocation& location,
                                        const MaterialDocument& document) {
    MaterialFile material{.baseColor = document.baseColor,
                          .emissive = document.emissive,
                          .emissiveStrength = document.emissiveStrength,
                          .metallic = document.metallic,
                          .roughness = document.roughness,
                          .normalScale = document.normalScale,
                          .occlusionStrength = document.occlusionStrength,
                          .alphaMode = document.alphaMode,
                          .alphaCutoff = document.alphaCutoff,
                          .doubleSided = document.doubleSided};
    const std::filesystem::path folder = normalizedCatalogPath(file).parent_path();
    for (size_t slot = 0; slot < materialTextureSlotCount; ++slot) {
        const std::string& value = document.*textureMembers[slot];
        if (value.empty()) {
            continue;
        }
        const std::filesystem::path texture = normalizedCatalogPath(location.assetRoot / pathFromUtf8Text(value));
        MaterialTextureReference& reference = material.textures[slot];
        reference.path = genericUtf8(texture.lexically_relative(folder));
        if (location.database != nullptr) {
            const std::filesystem::path inDatabase =
                texture.lexically_relative(normalizedCatalogPath(location.database->root()));
            if (!inDatabase.empty() && *inDatabase.begin() != "..") {
                reference.guid = location.database->guidForPath(genericUtf8(inDatabase)).value_or(AssetGuid{});
            }
        }
    }
    return writeMaterialFile(file, material);
}

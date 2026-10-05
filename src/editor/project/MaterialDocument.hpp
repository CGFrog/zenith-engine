#pragma once

#include "engine/assets/MaterialAssetFile.hpp"
#include "engine/reflection/Reflection.hpp"

#include <filesystem>
#include <span>
#include <string>

class AssetDatabase;

// A material file (.zmat) as the editor edits it: MaterialFile's values with
// each texture slot as a path relative to the project's asset root (forward
// slashes, UTF-8), like a scene's asset fields, so the inspector edits it with
// reflected fields and asset pickers (materialDocumentFields).
struct MaterialDocument {
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
    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string occlusionTexture;
    std::string emissiveTexture;

    bool operator==(const MaterialDocument&) const = default;
};

// The inspector's fields for a MaterialDocument; texture slots are asset
// fields of kind "texture".
std::span<const FieldDescriptor> materialDocumentFields();

// Where a material file's references resolve: the asset root its document's
// paths are relative to, and the project's asset database (null for none)
// whose GUIDs locate textures that moved since the file was written.
struct MaterialDocumentLocation {
    std::filesystem::path assetRoot;
    const AssetDatabase* database{nullptr};
};

// Reads file into output. A texture whose GUID the database knows resolves
// to its current path; otherwise the file's relative path is used.
MaterialFileResult loadMaterialDocument(const std::filesystem::path& file, const MaterialDocumentLocation& location,
                                        MaterialDocument& output);
// Writes document to file atomically, with texture paths relative to the
// file's folder and each texture's GUID when the database knows it.
MaterialFileResult saveMaterialDocument(const std::filesystem::path& file, const MaterialDocumentLocation& location,
                                        const MaterialDocument& document);

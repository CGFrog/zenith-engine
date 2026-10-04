#include "ComponentTypeHelpers.hpp"

#include "SceneAssetReferences.hpp"

#include <nlohmann/json.hpp>

namespace component_type_helpers {
namespace {
using Json = nlohmann::json;

// Resolves a reference object (by GUID, then path) to an existing asset file.
SceneDocumentResult resolveAssetReference(const Json& json, const ComponentLoadContext& context,
                                          std::filesystem::path& path) {
    std::string reference;
    if (SceneDocumentResult result = readAssetReference(json, context.options, reference); !result) {
        return result;
    }
    return resolveProjectAssetPath(reference, context.options, path);
}
} // namespace

SceneDocumentResult modelReference(const AssetManager& assets, ModelHandle handle, const SceneDocumentOptions& options,
                                   const char* component, std::string& output) {
    const std::optional<ModelAssetSource> source = assets.modelSource(handle);
    if (!source) {
        return {.error = std::string("Cannot save a ") + component + " component with an unknown runtime handle"};
    }
    return projectRelativeAssetPath(source->path, options, output);
}

SceneDocumentResult loadModelReference(const Json& json, const ComponentLoadContext& context, ModelHandle& output) {
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetReference(json, context, path); !result) {
        return result;
    }
    const AssetLoadResult<ModelHandle> loaded = context.assets.loadModel(path);
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}

SceneDocumentResult meshReference(const AssetManager& assets, MeshHandle handle, const SceneDocumentOptions& options,
                                  Json& output) {
    const std::optional<MeshAssetSource> source = assets.meshSource(handle);
    if (!source) {
        return {.error = "Cannot save a mesh component with an unknown runtime handle"};
    }
    std::string relative;
    if (SceneDocumentResult result = projectRelativeAssetPath(source->path, options, relative); !result) {
        return result;
    }
    output = {{"mesh", source->meshIndex}};
    writeAssetReference(relative, options, output);
    return {.success = true};
}

SceneDocumentResult readMesh(const Json& json, const ComponentLoadContext& context, MeshHandle& output) {
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveAssetReference(json, context, path); !result) {
        return result;
    }
    const AssetLoadResult<MeshHandle> loaded = context.assets.loadMesh(path, json.at("mesh").get<uint32_t>());
    if (!loaded) {
        return {.error = loaded.error};
    }
    output = loaded.handle;
    return {.success = true};
}
} // namespace component_type_helpers

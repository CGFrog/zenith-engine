#pragma once

// Helpers shared by the built-in component type families (Render, Animation,
// Light and Physics ComponentTypes.cpp). Internal to the engine; not public API.

#include "ComponentTypes.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cmath>
#include <string>

namespace component_type_helpers {

inline bool finiteNonNegative(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

inline bool finitePositive(float value) {
    return std::isfinite(value) && value > 0.0f;
}

inline bool unitInterval(float value) {
    return value >= 0.0f && value <= 1.0f;
}

// Converts a loaded model's handle to its project-relative path. component
// names the saving component in the error for an unknown handle.
SceneDocumentResult modelReference(const AssetManager& assets, ModelHandle handle, const SceneDocumentOptions& options,
                                   const char* component, std::string& output);

// Loads the model named by a reference object (by GUID, then path).
SceneDocumentResult loadModelReference(const nlohmann::json& json, const ComponentLoadContext& context,
                                       ModelHandle& output);

// Writes a mesh reference: the asset reference plus the "mesh" index.
SceneDocumentResult meshReference(const AssetManager& assets, MeshHandle handle, const SceneDocumentOptions& options,
                                  nlohmann::json& output);

// Loads the mesh named by a reference written by meshReference.
SceneDocumentResult readMesh(const nlohmann::json& json, const ComponentLoadContext& context, MeshHandle& output);

} // namespace component_type_helpers

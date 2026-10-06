#pragma once

// The Inspector's mesh renderer field: names and picks a model's meshes and materials.

#include "engine/assets/ModelImporter.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct EditorContext;

// Model outlines (readModelOutline) by file, read again when a file's write
// time changes, so an inspector can name a model's meshes and materials every
// frame without parsing the file every frame.
class ModelOutlineCache {
  public:
    // The outline of the model at an absolute path; its error is set when the
    // file cannot be read.
    const ModelOutline& outline(const std::filesystem::path& model);

  private:
    struct Entry {
        std::optional<std::filesystem::file_time_type> writeTime;
        ModelOutline outline;
    };

    std::unordered_map<std::string, Entry> m_entries;
};

// One mesh the Mesh row offers.
struct MeshChoice {
    // The glTF mesh index (object.mesh_assign's mesh).
    uint32_t index{0};
    // The mesh's name ("Mesh <index>" without one), marked "(skinned)" or
    // "(morph)" when it needs a skin or morph weights.
    std::string label;
};

// The meshes of an outline a mesh renderer can draw: those with triangle
// primitives (the importer loads no others), in glTF order.
std::vector<MeshChoice> meshChoices(const ModelOutline& outline);

// The material name of each of a mesh's primitives in the outline, "Default
// material" for primitives without one and "Material <index>" for an unnamed
// material. Empty when the outline has no such mesh.
std::vector<std::string> materialSlotNames(const ModelOutline& outline, uint32_t mesh);

// The Mesh row of the inspector for entities[0] (the active entity), below
// the Model row: the model file its Mesh Renderer's mesh comes from (the
// AssetReferenceField picker and drop target, model kind) and a combo of
// that model's meshes, then the mesh's material slots, read-only, with the
// material each primitive draws with (what a Material Override changes). A
// pick runs object.mesh_assign on every listed entity that has a Mesh
// Renderer (entities[0] always); choosing another model assigns its first
// mesh. A mesh made by code (AssetManager::createMesh) shows "(runtime
// mesh)" read-only, since it has no file to name or save. An entity without
// a Mesh Renderer that neither holds nor belongs to a model gets an empty
// row when it is the only one listed, so picking a model adds the renderer;
// model roots and nodes without one get none. Nothing without project assets
// (EditorContext::projectAssets).
void drawMeshRendererField(EditorContext& context, std::span<const SceneEntityId> entities,
                           ModelOutlineCache& outlines);

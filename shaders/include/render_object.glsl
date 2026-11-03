// One drawn object's transforms and skinning record.
// Mirrors mesh_pass_detail::ObjectData in
// engine/graphics/passes/MeshPassInternal.hpp (std430).
#ifndef ZENITH_RENDER_OBJECT_GLSL
#define ZENITH_RENDER_OBJECT_GLSL

struct RenderObject {
    mat4 model;
    mat4 previousModel;
    uvec4 skin; // x: skin palette offset, y: joint count, z: material record index
};

#endif

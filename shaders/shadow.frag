#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "material.glsl"

layout(std430, set = 0, binding = 4) readonly buffer Materials {
    MaterialRecord values[];
} materials;

// Bindless material textures; slots come from MaterialRecord.
#include "material_textures.glsl"

layout(location = 0) in vec2 texCoord;
layout(location = 1) flat in uint materialIndex;

void main() {
    MaterialRecord material = materials.values[materialIndex];
    bool masked = material.surface.y > 0.5 && material.surface.y < 1.5;
    if (masked && sampleMaterialTexture(material.textures.x, texCoord).a * material.baseColor.a < material.surface.x) {
        discard;
    }
}

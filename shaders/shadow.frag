#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Mirrors MaterialRecord in MeshPass.cpp (std430).
struct MaterialRecord {
    vec4 baseColor;
    vec4 factors;  // metallic, roughness, normal scale, occlusion strength
    vec4 emissive; // rgb radiance; w unused
    vec4 surface;  // alpha cutoff, alpha mode (0 opaque, 1 mask, 2 blend)
    uvec4 textures; // bindless slots: base color, normal, metallic-roughness, occlusion
    uvec4 extraTextures; // bindless slots: emissive
};

layout(std430, set = 0, binding = 4) readonly buffer Materials {
    MaterialRecord values[];
} materials;

// Bindless material textures; slots come from MaterialRecord.
layout(set = 1, binding = 0) uniform sampler materialSampler;
layout(set = 1, binding = 1) uniform texture2D materialTextures[];

vec4 sampleMaterialTexture(uint slot, vec2 uv) {
    return texture(sampler2D(materialTextures[nonuniformEXT(slot)], materialSampler), uv);
}

layout(location = 0) in vec2 texCoord;
layout(location = 1) flat in uint materialIndex;

void main() {
    MaterialRecord material = materials.values[materialIndex];
    bool masked = material.surface.y > 0.5 && material.surface.y < 1.5;
    if (masked && sampleMaterialTexture(material.textures.x, texCoord).a * material.baseColor.a < material.surface.x) {
        discard;
    }
}

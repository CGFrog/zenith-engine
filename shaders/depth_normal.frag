#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Writes the depth prepass normal target (PostProcessPass::normalFormat,
// A2B10G10R10_UNORM); src/engine/graphics/SceneNormalEncoding.hpp mirrors the
// encoding:
//   rg: octahedral view-space normal (+Z towards the camera), [-1, 1] -> [0, 1]
//   b:  perceptual roughness, as the shading pass clamps it
//   a:  1 for a written opaque surface (the clear value is 0)
// and the specular target (PostProcessPass::specularFormat, R8G8B8A8_SRGB);
// src/engine/graphics/SceneSpecularEncoding.hpp mirrors it:
//   rgb: linear F0, mix(0.04, base color, metallic) as mesh.frag computes it
//   a:   1 for a written opaque surface (the clear value is 0)
// The specular attachment is VK_NULL_HANDLE on frames without screen-space
// reflections, which discards that output.

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

layout(location = 0) in vec3 viewNormal;
layout(location = 1) in vec2 texCoord;
layout(location = 2) in vec4 viewTangent;
layout(location = 3) flat in uint materialIndex;
layout(location = 0) out vec4 outNormalRoughness;
layout(location = 1) out vec4 outSpecular;

vec2 signNotZero(vec2 value) {
    return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

vec2 encodeOctahedral(vec3 normal) {
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    vec2 encoded = normal.xy;
    if (normal.z < 0.0) {
        encoded = (1.0 - abs(encoded.yx)) * signNotZero(encoded);
    }
    return encoded;
}

void main() {
    MaterialRecord material = materials.values[materialIndex];
    // Same alpha test as mesh.frag, so masked texels the shading pass would
    // discard never reach the depth it tests against.
    vec4 sampledBase = sampleMaterialTexture(material.textures.x, texCoord);
    if (material.surface.y > 0.5 && material.surface.y < 1.5 &&
        sampledBase.a * material.baseColor.a < material.surface.x) {
        discard;
    }
    // Same normal mapping as mesh.frag; the basis is rotated into view space,
    // which leaves the construction unchanged.
    vec3 tangent = normalize(viewTangent.xyz);
    vec3 normal = normalize(viewNormal);
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = cross(normal, tangent) * viewTangent.w;
    // Normal maps may be two-channel (BC5), so Z is rebuilt from unit length.
    vec2 mappedNormalXY = sampleMaterialTexture(material.textures.y, texCoord).xy * 2.0 - 1.0;
    vec3 mappedNormal = vec3(mappedNormalXY, sqrt(clamp(1.0 - dot(mappedNormalXY, mappedNormalXY), 0.0, 1.0)));
    mappedNormal.xy *= material.factors.z;
    normal = normalize(mat3(tangent, bitangent, normal) * mappedNormal);
    // Back faces of double-sided surfaces face the camera through their back.
    if (!gl_FrontFacing) {
        normal = -normal;
    }
    vec4 orm = sampleMaterialTexture(material.textures.z, texCoord);
    float roughness = clamp(orm.g * material.factors.y, 0.045, 1.0);
    float metallic = clamp(orm.b * material.factors.x, 0.0, 1.0);
    outNormalRoughness = vec4(encodeOctahedral(normal) * 0.5 + 0.5, roughness, 1.0);
    outSpecular = vec4(mix(vec3(0.04), sampledBase.rgb * material.baseColor.rgb, metallic), 1.0);
}

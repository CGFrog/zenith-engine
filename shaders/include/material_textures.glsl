// The bindless material texture set (set 1) and sampling by MaterialRecord
// slot. The includer enables GL_EXT_nonuniform_qualifier.
#ifndef ZENITH_MATERIAL_TEXTURES_GLSL
#define ZENITH_MATERIAL_TEXTURES_GLSL

layout(set = 1, binding = 0) uniform sampler materialSampler;
layout(set = 1, binding = 1) uniform texture2D materialTextures[];

vec4 sampleMaterialTexture(uint slot, vec2 uv) {
    return texture(sampler2D(materialTextures[nonuniformEXT(slot)], materialSampler), uv);
}

#endif

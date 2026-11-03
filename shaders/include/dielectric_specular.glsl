// Dielectric reflectance from KHR_materials_ior and KHR_materials_specular.
// Include material.glsl and material_textures.glsl first.
#ifndef ZENITH_DIELECTRIC_SPECULAR_GLSL
#define ZENITH_DIELECTRIC_SPECULAR_GLSL

// Dielectric reflectance at normal incidence (rgb) and the specular weight
// (a), which scales the dielectric Fresnel term including its grazing value.
// ior 1.5 with the default specular gives (0.04, 1), the metallic-roughness
// model.
vec4 dielectricSpecular(MaterialRecord material, vec2 uv) {
    float ior = material.surface.z;
    float reflectance = (ior - 1.0) / (ior + 1.0);
    vec3 color = material.specular.rgb;
    float weight = material.specular.w;
    if ((material.extraTextures.y & LOBE_SPECULAR_TEXTURES) != 0u) {
        weight *= sampleMaterialTexture(material.layerTextures.y, uv).a;
        color *= sampleMaterialTexture(material.layerTextures.z, uv).rgb;
    }
    return vec4(min(vec3(reflectance * reflectance) * color, vec3(1.0)), weight);
}

#endif

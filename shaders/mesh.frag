#version 450
#extension GL_EXT_nonuniform_qualifier : require

const float PI = 3.14159265359;

// Quality variants baked in by MeshPass::meshPipelines so disabled paths compile out.
layout(constant_id = 0) const bool PHYSICALLY_BASED_LIGHTING = true;
layout(constant_id = 1) const int SHADOW_CASCADE_COUNT = 3;

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 lightViewProjections[3];
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 shadowSettings;
    vec4 cascadeSplits;
    vec4 renderingSettings;
    vec4 environmentSpecular[6];
    vec4 localLightPositionRange[64];
    vec4 localLightDirectionType[64];
    vec4 localLightColorIntensity[64];
    vec4 localLightCone[64];
    uvec4 localLightCount;
} camera;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowMap;

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

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) in vec4 currentClipPosition;
layout(location = 5) in vec4 previousClipPosition;
layout(location = 6) flat in uint materialIndex;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outMotionReactive;

float distributionGGX(vec3 normal, vec3 halfVector, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

vec3 fresnelSchlick(float cosine, vec3 reflectance) {
    return reflectance + (1.0 - reflectance) * pow(1.0 - cosine, 5.0);
}

vec3 fresnelSchlickRoughness(float cosine, vec3 reflectance, float roughness) {
    return reflectance + (max(vec3(1.0 - roughness), reflectance) - reflectance) * pow(1.0 - cosine, 5.0);
}

float sampleShadowCascade(vec3 position, vec3 normal, vec3 lightDirection, int cascade) {
    float normalOffset = camera.shadowSettings.x * (1.0 - max(dot(normal, lightDirection), 0.0));
    vec4 clip = camera.lightViewProjections[cascade] * vec4(position + normal * normalOffset, 1.0);
    vec3 projected = clip.xyz / clip.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 || any(lessThan(uv, vec2(0.0))) ||
        any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    const vec2 poissonDisk[16] = vec2[16](
        vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
        vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
        vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
        vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
        vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
        vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
        vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
        vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));
    float angle = fract(sin(dot(position.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    const int sampleCount = SHADOW_CASCADE_COUNT > 2 ? 16 : SHADOW_CASCADE_COUNT > 1 ? 8 : 4;
    float visibility = 0.0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        vec2 offset = rotation * poissonDisk[sampleIndex] * texel * 1.75;
        visibility += texture(shadowMap, vec4(uv + offset, float(cascade), projected.z));
    }
    return visibility / float(sampleCount);
}

int shadowCascade(vec3 position) {
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = 0;
    if (SHADOW_CASCADE_COUNT > 1 && cameraDistance > camera.cascadeSplits.x) {
        cascade = 1;
    }
    if (SHADOW_CASCADE_COUNT > 2 && cameraDistance > camera.cascadeSplits.y) {
        cascade = 2;
    }
    return cascade;
}

float shadowVisibility(vec3 position, vec3 normal, vec3 lightDirection) {
    if (SHADOW_CASCADE_COUNT == 0 || camera.shadowSettings.y < 0.5) {
        return 1.0;
    }
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = shadowCascade(position);
    float visibility = sampleShadowCascade(position, normal, lightDirection, cascade);
    const int cascadeCount = SHADOW_CASCADE_COUNT;
    if (cascade + 1 < cascadeCount) {
        float splitNear = cascade == 0 ? 0.0 : camera.cascadeSplits[cascade - 1];
        float splitFar = camera.cascadeSplits[cascade];
        float blendWidth = max((splitFar - splitNear) * camera.shadowSettings.w, 0.001);
        float blend = smoothstep(splitFar - blendWidth, splitFar, cameraDistance);
        if (blend > 0.0) {
            visibility = mix(visibility, sampleShadowCascade(position, normal, lightDirection, cascade + 1), blend);
        }
    } else if (cascadeCount > 1) {
        float lastSplit = camera.cascadeSplits[cascadeCount - 1];
        float fadeWidth = max(lastSplit * 0.1, 0.001);
        visibility = mix(visibility, 1.0, smoothstep(lastSplit - fadeWidth, lastSplit, cameraDistance));
    }
    return visibility;
}

// Reflected fraction of one light's irradiance, excluding radiance and the cosine
// term. The fast variant is unnormalized Lambert, matching its tuned brightness.
vec3 directResponse(vec3 normal, vec3 viewDirection, vec3 lightDirection, float nDotV, float nDotL, vec3 albedo,
                    float metallic, float roughness, vec3 reflectance, float viewGeometry) {
    if (!PHYSICALLY_BASED_LIGHTING) {
        return albedo;
    }
    vec3 halfVector = normalize(viewDirection + lightDirection);
    vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), reflectance);
    float distribution = distributionGGX(normal, halfVector, roughness);
    float geometry = viewGeometry * geometrySchlickGGX(nDotL, roughness);
    vec3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 0.0001);
    vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo / PI;
    return diffuse + specular;
}

void main() {
    MaterialRecord material = materials.values[materialIndex];
    vec4 sampledBase = sampleMaterialTexture(material.textures.x, texCoord);
    float alpha = sampledBase.a * material.baseColor.a;
    if (material.surface.y > 0.5 && material.surface.y < 1.5 && alpha < material.surface.x) {
        discard;
    }
    vec3 albedo = sampledBase.rgb * material.baseColor.rgb;
    vec4 orm = sampleMaterialTexture(material.textures.z, texCoord);
    float metallic = clamp(orm.b * material.factors.x, 0.0, 1.0);
    float roughness = clamp(orm.g * material.factors.y, 0.045, 1.0);
    float occlusion = mix(1.0, sampleMaterialTexture(material.textures.w, texCoord).r, material.factors.w);

    vec3 tangent = normalize(worldTangent.xyz);
    vec3 geometricNormal = normalize(worldNormal);
    vec3 normal = geometricNormal;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = cross(normal, tangent) * worldTangent.w;
    // Normal maps may be two-channel (BC5), so Z is rebuilt from unit length.
    vec2 mappedNormalXY = sampleMaterialTexture(material.textures.y, texCoord).xy * 2.0 - 1.0;
    vec3 mappedNormal = vec3(mappedNormalXY, sqrt(clamp(1.0 - dot(mappedNormalXY, mappedNormalXY), 0.0, 1.0)));
    mappedNormal.xy *= material.factors.z;
    normal = normalize(mat3(tangent, bitangent, normal) * mappedNormal);

    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    vec3 reflectance = mix(vec3(0.04), albedo, metallic);
    // The view half of the Smith geometry term is shared by every light.
    float viewGeometry = geometrySchlickGGX(nDotV, roughness);
    float shadow = shadowVisibility(worldPosition, geometricNormal, lightDirection);
    vec3 sunRadiance = camera.lightColor.rgb * camera.lightDirection.w;
    vec3 direct = directResponse(normal, viewDirection, lightDirection, nDotV, nDotL, albedo, metallic, roughness,
                                 reflectance, viewGeometry) *
                  sunRadiance * nDotL * shadow;
    for (uint lightIndex = 0; lightIndex < camera.localLightCount.x; ++lightIndex) {
        vec3 toLight = camera.localLightPositionRange[lightIndex].xyz - worldPosition;
        float distanceToLight = length(toLight);
        float range = camera.localLightPositionRange[lightIndex].w;
        if (distanceToLight >= range || distanceToLight <= 0.0001) {
            continue;
        }
        vec3 localDirection = toLight / distanceToLight;
        float attenuation = pow(max(1.0 - distanceToLight / range, 0.0), 2.0) /
                            max(distanceToLight * distanceToLight, 0.01);
        if (camera.localLightDirectionType[lightIndex].w > 0.5) {
            float coneCosine = dot(-localDirection, normalize(camera.localLightDirectionType[lightIndex].xyz));
            attenuation *= smoothstep(camera.localLightCone[lightIndex].y,
                                      camera.localLightCone[lightIndex].x, coneCosine);
        }
        float localNDotL = max(dot(normal, localDirection), 0.0);
        vec3 localRadiance = camera.localLightColorIntensity[lightIndex].rgb *
                             camera.localLightColorIntensity[lightIndex].w * attenuation;
        direct += directResponse(normal, viewDirection, localDirection, nDotV, localNDotL, albedo, metallic,
                                 roughness, reflectance, viewGeometry) *
                  localRadiance * localNDotL;
    }
    float hemisphereWeight = normal.y * 0.5 + 0.5;
    vec3 diffuseEnvironment = mix(camera.environmentGround.rgb, camera.environmentSky.rgb, hemisphereWeight);
    vec3 indirect = albedo * diffuseEnvironment * camera.environmentSky.w;
    if (PHYSICALLY_BASED_LIGHTING) {
        vec3 indirectFresnel = fresnelSchlickRoughness(nDotV, reflectance, roughness);
        vec3 indirectDiffuse = (vec3(1.0) - indirectFresnel) * (1.0 - metallic) * indirect;
        float specularLevel = roughness * 5.0;
        int specularLow = int(floor(specularLevel));
        int specularHigh = min(specularLow + 1, 5);
        vec3 specularEnvironment = mix(camera.environmentSpecular[specularLow].rgb,
                                       camera.environmentSpecular[specularHigh].rgb, fract(specularLevel));
        vec3 indirectSpecular = indirectFresnel * specularEnvironment * camera.environmentGround.w *
                                mix(1.0, 0.35, roughness);
        indirect = indirectDiffuse + indirectSpecular;
    }
    indirect *= occlusion;
    vec3 emissive = sampleMaterialTexture(material.extraTextures.x, texCoord).rgb * material.emissive.rgb;
    vec3 color = direct + indirect + emissive;
    if (SHADOW_CASCADE_COUNT > 1 && camera.renderingSettings.w > 0.5) {
        const vec3 cascadeColors[3] = vec3[3](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35),
                                              vec3(0.35, 0.55, 1.0));
        color = mix(color, color * cascadeColors[shadowCascade(worldPosition)], 0.45);
    }
    outColor = vec4(color, alpha);
    vec2 currentUv = currentClipPosition.xy / max(abs(currentClipPosition.w), 0.00001) * 0.5 + 0.5;
    vec2 previousUv = previousClipPosition.xy / max(abs(previousClipPosition.w), 0.00001) * 0.5 + 0.5;
    float reactive = max(material.surface.y > 1.5 ? alpha : 0.0,
                         smoothstep(1.0, 4.0, dot(emissive, vec3(0.2126, 0.7152, 0.0722))));
    // Surfaces revealed by camera motion are detected in post.frag against the
    // previous frame's depth (stored in .w); only off-screen history is known here.
    bool previousOffscreen = previousClipPosition.w <= 0.0 || any(lessThan(previousUv, vec2(0.0))) ||
                             any(greaterThan(previousUv, vec2(1.0)));
    outMotionReactive = vec4(currentUv - previousUv, previousOffscreen ? 1.0 : reactive, gl_FragCoord.z);
}

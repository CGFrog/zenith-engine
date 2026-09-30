#version 450

const float PI = 3.14159265359;

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
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

layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 3) uniform sampler2D occlusionMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 baseColor;
    vec4 material;
    vec4 emissive;
    vec4 surface;
} drawData;

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;
layout(location = 3) in vec4 worldTangent;
layout(location = 0) out vec4 outColor;

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
    float visibility = 0.0;
    for (int sampleIndex = 0; sampleIndex < 16; ++sampleIndex) {
        vec2 offset = rotation * poissonDisk[sampleIndex] * texel * 1.75;
        visibility += texture(shadowMap, vec4(uv + offset, float(cascade), projected.z));
    }
    return visibility / 16.0;
}

int shadowCascade(vec3 position) {
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = 0;
    if (camera.shadowSettings.z > 1.5 && cameraDistance > camera.cascadeSplits.x) {
        cascade = 1;
    }
    if (camera.shadowSettings.z > 2.5 && cameraDistance > camera.cascadeSplits.y) {
        cascade = 2;
    }
    return cascade;
}

float shadowVisibility(vec3 position, vec3 normal, vec3 lightDirection) {
    if (camera.shadowSettings.y < 0.5) {
        return 1.0;
    }
    float cameraDistance = length(position - camera.cameraPosition.xyz);
    int cascade = shadowCascade(position);
    float visibility = sampleShadowCascade(position, normal, lightDirection, cascade);
    int cascadeCount = int(camera.shadowSettings.z + 0.5);
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

void main() {
    vec4 sampledBase = texture(baseColorMap, texCoord);
    float alpha = sampledBase.a * drawData.baseColor.a;
    if (drawData.surface.y > 0.5 && drawData.surface.y < 1.5 && alpha < drawData.surface.x) {
        discard;
    }
    vec3 albedo = sampledBase.rgb * drawData.baseColor.rgb;
    vec4 orm = texture(metallicRoughnessMap, texCoord);
    float metallic = clamp(orm.b * drawData.material.x, 0.0, 1.0);
    float roughness = clamp(orm.g * drawData.material.y, 0.045, 1.0);
    float occlusion = mix(1.0, texture(occlusionMap, texCoord).r, drawData.material.w);

    vec3 tangent = normalize(worldTangent.xyz);
    vec3 geometricNormal = normalize(worldNormal);
    vec3 normal = geometricNormal;
    tangent = normalize(tangent - normal * dot(normal, tangent));
    vec3 bitangent = cross(normal, tangent) * worldTangent.w;
    vec3 mappedNormal = texture(normalMap, texCoord).xyz * 2.0 - 1.0;
    mappedNormal.xy *= drawData.material.z;
    normal = normalize(mat3(tangent, bitangent, normal) * mappedNormal);

    vec3 viewDirection = normalize(camera.cameraPosition.xyz - worldPosition);
    vec3 lightDirection = normalize(camera.lightDirection.xyz);
    vec3 halfVector = normalize(viewDirection + lightDirection);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    vec3 reflectance = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), reflectance);
    float distribution = distributionGGX(normal, halfVector, roughness);
    float geometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
    vec3 specular = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 0.0001);
    vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo / PI;
    float shadow = shadowVisibility(worldPosition, geometricNormal, lightDirection);
    vec3 physicallyBasedDirect = (diffuse + specular) * camera.lightColor.rgb * camera.lightDirection.w * nDotL;
    vec3 fastDirect = albedo * camera.lightColor.rgb * camera.lightDirection.w * nDotL;
    vec3 direct = mix(fastDirect, physicallyBasedDirect, camera.renderingSettings.x) * shadow;
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
        vec3 localHalf = normalize(viewDirection + localDirection);
        vec3 localFresnel = fresnelSchlick(max(dot(localHalf, viewDirection), 0.0), reflectance);
        float localDistribution = distributionGGX(normal, localHalf, roughness);
        float localGeometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(localNDotL, roughness);
        vec3 localSpecular = localDistribution * localGeometry * localFresnel /
                             max(4.0 * nDotV * localNDotL, 0.0001);
        vec3 localDiffuse = (vec3(1.0) - localFresnel) * (1.0 - metallic) * albedo / PI;
        vec3 localRadiance = camera.localLightColorIntensity[lightIndex].rgb *
                             camera.localLightColorIntensity[lightIndex].w * attenuation;
        direct += mix(albedo * localRadiance * localNDotL,
                      (localDiffuse + localSpecular) * localRadiance * localNDotL,
                      camera.renderingSettings.x);
    }
    float hemisphereWeight = normal.y * 0.5 + 0.5;
    vec3 diffuseEnvironment = mix(camera.environmentGround.rgb, camera.environmentSky.rgb, hemisphereWeight);
    vec3 indirectFresnel = fresnelSchlickRoughness(nDotV, reflectance, roughness);
    vec3 indirectDiffuse = (vec3(1.0) - indirectFresnel) * (1.0 - metallic) * albedo * diffuseEnvironment *
                           camera.environmentSky.w;
    float specularLevel = roughness * 5.0;
    int specularLow = int(floor(specularLevel));
    int specularHigh = min(specularLow + 1, 5);
    vec3 specularEnvironment = mix(camera.environmentSpecular[specularLow].rgb,
                                   camera.environmentSpecular[specularHigh].rgb, fract(specularLevel));
    vec3 indirectSpecular = indirectFresnel * specularEnvironment * camera.environmentGround.w *
                            mix(1.0, 0.35, roughness);
    vec3 physicallyBasedIndirect = indirectDiffuse + indirectSpecular;
    vec3 fastIndirect = albedo * diffuseEnvironment * camera.environmentSky.w;
    vec3 indirect = mix(fastIndirect, physicallyBasedIndirect, camera.renderingSettings.x) * occlusion;
    vec3 emissive = texture(emissiveMap, texCoord).rgb * drawData.emissive.rgb;
    vec3 color = direct + indirect + emissive;
    if (camera.renderingSettings.w > 0.5 && camera.shadowSettings.z > 1.5) {
        const vec3 cascadeColors[3] = vec3[3](vec3(1.0, 0.35, 0.35), vec3(0.35, 1.0, 0.35),
                                              vec3(0.35, 0.55, 1.0));
        color = mix(color, color * cascadeColors[shadowCascade(worldPosition)], 0.45);
    }
    outColor = vec4(color, alpha);
}

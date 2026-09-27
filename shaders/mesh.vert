#version 450

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
} camera;

layout(std430, set = 0, binding = 2) readonly buffer ObjectData {
    mat4 models[];
} objects;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 baseColor;
    vec4 material;
    vec4 emissive;
} drawData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 texCoord;
layout(location = 3) out vec4 worldTangent;

void main() {
    mat4 model = objects.models[gl_InstanceIndex];
    vec4 world = model * vec4(inPosition, 1.0);
    // The cofactor matrix transforms normals correctly for affine transforms;
    // normalization removes the determinant scale without a per-vertex inverse.
    mat3 linearModel = mat3(model);
    mat3 normalMatrix = mat3(cross(linearModel[1], linearModel[2]), cross(linearModel[2], linearModel[0]),
                             cross(linearModel[0], linearModel[1]));
    worldPosition = world.xyz;
    worldNormal = normalize(normalMatrix * inNormal);
    worldTangent = vec4(normalize(linearModel * inTangent.xyz), inTangent.w);
    texCoord = inTexCoord;
    gl_Position = camera.viewProjection * world;
}

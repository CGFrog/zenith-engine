#version 450

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
} camera;

layout(push_constant) uniform ShadowDrawData {
    vec4 settings;
} drawData;

struct RenderObject {
    mat4 model;
    mat4 previousModel;
    uvec4 skin;
};

layout(set = 0, binding = 2, std430) readonly buffer ObjectBuffer {
    RenderObject values[];
} objects;

layout(set = 0, binding = 3, std430) readonly buffer SkinPalettes {
    mat4 joints[];
} skinPalettes;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;
layout(location = 0) out vec2 texCoord;
// skin.z carries the object's material record index.
layout(location = 1) flat out uint materialIndex;

void main() {
    RenderObject object = objects.values[gl_InstanceIndex];
    mat4 skin = mat4(1.0);
    if (object.skin.y > 0 && dot(inWeights, vec4(1.0)) > 0.0001) {
        uvec4 joint = min(inJoints, uvec4(object.skin.y - 1));
        skin = skinPalettes.joints[object.skin.x + joint.x] * inWeights.x +
               skinPalettes.joints[object.skin.x + joint.y] * inWeights.y +
               skinPalettes.joints[object.skin.x + joint.z] * inWeights.z +
               skinPalettes.joints[object.skin.x + joint.w] * inWeights.w;
    }
    uint cascadeIndex = uint(drawData.settings.x + 0.5);
    gl_Position = camera.lightViewProjections[cascadeIndex] * object.model * skin * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
    materialIndex = object.skin.z;
}

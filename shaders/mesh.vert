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
} camera;

struct RenderObject {
    mat4 model;
    mat4 previousModel;
    uvec4 skin;
};

layout(std430, set = 0, binding = 2) readonly buffer ObjectData {
    RenderObject values[];
} objects;

layout(std430, set = 0, binding = 3) readonly buffer SkinPalettes {
    mat4 joints[];
} skinPalettes;

// The previous frame's palettes at the same offsets, for motion vectors.
layout(std430, set = 0, binding = 12) readonly buffer PreviousSkinPalettes {
    mat4 joints[];
} previousSkinPalettes;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 texCoord;
layout(location = 3) out vec4 worldTangent;
layout(location = 4) out vec4 currentClipPosition;
layout(location = 5) out vec4 previousClipPosition;
// skin.z carries the object's material record index.
layout(location = 6) flat out uint materialIndex;

void main() {
    RenderObject object = objects.values[gl_InstanceIndex];
    mat4 skin = mat4(1.0);
    mat4 previousSkin = mat4(1.0);
    float weightSum = dot(inWeights, vec4(1.0));
    if (object.skin.y > 0 && weightSum > 0.0001) {
        uvec4 joint = min(inJoints, uvec4(object.skin.y - 1));
        skin = skinPalettes.joints[object.skin.x + joint.x] * inWeights.x +
               skinPalettes.joints[object.skin.x + joint.y] * inWeights.y +
               skinPalettes.joints[object.skin.x + joint.z] * inWeights.z +
               skinPalettes.joints[object.skin.x + joint.w] * inWeights.w;
        previousSkin = previousSkinPalettes.joints[object.skin.x + joint.x] * inWeights.x +
                       previousSkinPalettes.joints[object.skin.x + joint.y] * inWeights.y +
                       previousSkinPalettes.joints[object.skin.x + joint.z] * inWeights.z +
                       previousSkinPalettes.joints[object.skin.x + joint.w] * inWeights.w;
    }
    mat4 model = object.model * skin;
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
    currentClipPosition = camera.viewProjection * world;
    previousClipPosition = camera.previousViewProjection * object.previousModel * previousSkin * vec4(inPosition, 1.0);
    gl_Position = currentClipPosition;
    materialIndex = object.skin.z;
}

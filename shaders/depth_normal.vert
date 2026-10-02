#version 450

// Depth prepass. The clip-space position must match mesh.vert bit for bit so
// the shading pass can test EQUAL against this depth: keep the position math
// below identical to mesh.vert (same operations in the same order, `precise`,
// and an invariant gl_Position).
invariant gl_Position;

layout(set = 0, binding = 0) uniform CameraData {
    mat4 viewProjection;
    mat4 previousViewProjection;
} camera;

// Rigid world-to-view transform of the frame (no projection).
layout(push_constant) uniform DepthNormalData {
    mat4 view;
} drawData;

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

layout(location = 0) in vec3 inPosition;
// Packed streams (engine/assets/PackedVertexStreams.hpp), decoded like mesh.vert.
layout(location = 1) in vec2 inNormalOctahedral;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inTangentOctahedral;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;
layout(location = 0) out vec3 viewNormal;
layout(location = 1) out vec2 texCoord;
layout(location = 2) out vec4 viewTangent;
// skin.z carries the object's material record index.
layout(location = 3) flat out uint materialIndex;

// Mirrors vertex_packing::decodeOctahedral.
vec3 decodeOctahedral(vec2 encoded) {
    vec3 direction = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    float fold = max(-direction.z, 0.0);
    direction.x += direction.x >= 0.0 ? -fold : fold;
    direction.y += direction.y >= 0.0 ? -fold : fold;
    return normalize(direction);
}

// Mirrors vertex_packing::unpackTangent.
vec4 decodeTangent(vec2 packedTangent) {
    const float minimumFoldedMagnitude = 1.0 / 32767.0;
    float magnitude = (abs(packedTangent.y) - minimumFoldedMagnitude) / (1.0 - minimumFoldedMagnitude);
    vec2 encoded = vec2(packedTangent.x, clamp(magnitude * 2.0 - 1.0, -1.0, 1.0));
    return vec4(decodeOctahedral(encoded), packedTangent.y < 0.0 ? -1.0 : 1.0);
}

void main() {
    vec3 inNormal = decodeOctahedral(inNormalOctahedral);
    vec4 inTangent = decodeTangent(inTangentOctahedral);
    RenderObject object = objects.values[gl_InstanceIndex];
    mat4 skin = mat4(1.0);
    float weightSum = dot(inWeights, vec4(1.0));
    if (object.skin.y > 0 && weightSum > 0.0001) {
        uvec4 joint = min(inJoints, uvec4(object.skin.y - 1));
        skin = skinPalettes.joints[object.skin.x + joint.x] * inWeights.x +
               skinPalettes.joints[object.skin.x + joint.y] * inWeights.y +
               skinPalettes.joints[object.skin.x + joint.z] * inWeights.z +
               skinPalettes.joints[object.skin.x + joint.w] * inWeights.w;
    }
    mat4 model = object.model * skin;
    precise vec4 world = model * vec4(inPosition, 1.0);
    // Same cofactor normal transform as mesh.vert, then rotated into view space.
    mat3 linearModel = mat3(model);
    mat3 normalMatrix = mat3(cross(linearModel[1], linearModel[2]), cross(linearModel[2], linearModel[0]),
                             cross(linearModel[0], linearModel[1]));
    mat3 viewRotation = mat3(drawData.view);
    viewNormal = viewRotation * normalize(normalMatrix * inNormal);
    viewTangent = vec4(viewRotation * normalize(linearModel * inTangent.xyz), inTangent.w);
    texCoord = inTexCoord;
    precise vec4 clipPosition = camera.viewProjection * world;
    gl_Position = clipPosition;
    materialIndex = object.skin.z;
}

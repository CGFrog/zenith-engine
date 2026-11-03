#version 450
#extension GL_GOOGLE_include_directive : require

// The depth prepass (depth_normal.vert) repeats the position math below and the
// shading pass tests EQUAL against its depth, so keep both bit-identical.
invariant gl_Position;

#include "camera.glsl"

#include "render_object.glsl"

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
// Packed streams (engine/assets/PackedVertexStreams.hpp): octahedral normal,
// octahedral tangent with the bitangent sign folded into y, float UV.
layout(location = 1) in vec2 inNormalOctahedral;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec2 inTangentOctahedral;
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
// The vertex's weights, unused by unskinned meshes, are a terrain material's
// layer weights (MaterialTerrain).
layout(location = 7) out vec4 surfaceWeights;

#include "vertex_packing.glsl"

void main() {
    vec3 inNormal = decodeOctahedral(inNormalOctahedral);
    vec4 inTangent = decodeTangent(inTangentOctahedral);
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
    precise vec4 world = model * vec4(inPosition, 1.0);
    // The cofactor matrix transforms normals correctly for affine transforms;
    // normalization removes the determinant scale without a per-vertex inverse.
    mat3 linearModel = mat3(model);
    mat3 normalMatrix = mat3(cross(linearModel[1], linearModel[2]), cross(linearModel[2], linearModel[0]),
                             cross(linearModel[0], linearModel[1]));
    worldPosition = world.xyz;
    worldNormal = normalize(normalMatrix * inNormal);
    worldTangent = vec4(normalize(linearModel * inTangent.xyz), inTangent.w);
    texCoord = inTexCoord;
    precise vec4 clipPosition = camera.viewProjection * world;
    currentClipPosition = clipPosition;
    previousClipPosition = camera.previousViewProjection * object.previousModel * previousSkin * vec4(inPosition, 1.0);
    gl_Position = clipPosition;
    materialIndex = object.skin.z;
    surfaceWeights = inWeights;
}

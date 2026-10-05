#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// A transform split into translation, rotation, and scale. It composes as
// translate(position) * rotate(rotation) * scale(scale), so scale applies in
// the transform's own axes before rotation.
struct TransformTrs {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Splits an affine matrix; the projective row is ignored.
// - position is the translation column, exactly.
// - scale is the length of each basis column. A mirrored basis (negative
//   determinant) negates scale.x.
// - rotation is the basis made orthonormal in X, Y, Z order (Gram-Schmidt),
//   so the X axis keeps its direction exactly.
// A sheared basis (non-perpendicular columns) loses its shear: composeTrs of
// the result reproduces the matrix only when it has none. A zero-length axis
// gives a zero scale and an arbitrary perpendicular direction for that axis.
TransformTrs decomposeTrs(const glm::mat4& transform);

glm::mat4 composeTrs(const TransformTrs& trs);

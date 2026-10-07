#pragma once

// Tolerant comparisons of glm vectors, matrices, and quaternions, for tests and smoke checks.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace testsupport {

// The default tolerance: float noise of a few operations, not a design margin.
inline constexpr float defaultEpsilon = 1e-5f;

inline bool approxEqual(float a, float b, float epsilon = defaultEpsilon) {
    return std::abs(a - b) <= epsilon;
}

// Component by component.
template <glm::length_t Length, typename Value, glm::qualifier Q>
bool approxEqual(const glm::vec<Length, Value, Q>& a, const glm::vec<Length, Value, Q>& b,
                 Value epsilon = Value(defaultEpsilon)) {
    for (glm::length_t i = 0; i < Length; ++i) {
        if (std::abs(a[i] - b[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

template <glm::length_t Columns, glm::length_t Rows, typename Value, glm::qualifier Q>
bool approxEqual(const glm::mat<Columns, Rows, Value, Q>& a, const glm::mat<Columns, Rows, Value, Q>& b,
                 Value epsilon = Value(defaultEpsilon)) {
    for (glm::length_t column = 0; column < Columns; ++column) {
        if (!approxEqual(a[column], b[column], epsilon)) {
            return false;
        }
    }
    return true;
}

// The same rotation: q and -q are equal.
template <typename Value, glm::qualifier Q>
bool approxEqual(const glm::qua<Value, Q>& a, const glm::qua<Value, Q>& b, Value epsilon = Value(defaultEpsilon)) {
    return std::abs(std::abs(glm::dot(a, b)) - Value(1)) <= epsilon;
}

} // namespace testsupport

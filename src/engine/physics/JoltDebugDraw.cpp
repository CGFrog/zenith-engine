#include "JoltDebugDraw.hpp"

#include <string>

namespace {
glm::vec3 toGlm(JPH::RVec3Arg value) {
    return {static_cast<float>(value.GetX()), static_cast<float>(value.GetY()), static_cast<float>(value.GetZ())};
}

glm::vec4 toGlm(JPH::ColorArg color) {
    return glm::vec4(color.r, color.g, color.b, color.a) / 255.0f;
}
} // namespace

void JoltDebugDraw::DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) {
    if (m_target != nullptr) {
        m_target->line(toGlm(from), toGlm(to), {.color = toGlm(color), .depth = m_depth});
    }
}

void JoltDebugDraw::DrawText3D(JPH::RVec3Arg position, const std::string_view& text, JPH::ColorArg color, float) {
    if (m_target != nullptr) {
        m_target->label(toGlm(position), std::string(text), toGlm(color));
    }
}

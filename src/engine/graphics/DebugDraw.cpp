#include "DebugDraw.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Two unit vectors perpendicular to `normal` (unit length) and each other.
std::pair<glm::vec3, glm::vec3> perpendicularBasis(const glm::vec3& normal) {
    const glm::vec3 reference = std::abs(normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 first = glm::normalize(glm::cross(reference, normal));
    return {first, glm::cross(normal, first)};
}

uint8_t unitToByte(float value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
} // namespace

uint32_t packDebugColor(const glm::vec4& color) {
    return static_cast<uint32_t>(unitToByte(color.r)) | (static_cast<uint32_t>(unitToByte(color.g)) << 8u) |
           (static_cast<uint32_t>(unitToByte(color.b)) << 16u) | (static_cast<uint32_t>(unitToByte(color.a)) << 24u);
}

void DebugDraw::addLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, DebugDepth depth) {
    // A non-finite position would rasterize unpredictably; drop the segment.
    if (!finite(from) || !finite(to)) {
        return;
    }
    std::vector<DebugLineVertex>& lines = m_lines[static_cast<size_t>(depth)];
    lines.push_back({from, color});
    lines.push_back({to, color});
}

void DebugDraw::addArc(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius,
                       float beginRadians, float endRadians, uint32_t segments, uint32_t color, DebugDepth depth) {
    const uint32_t count = std::max(segments, 1u);
    const auto point = [&](uint32_t index) {
        const float angle =
            beginRadians + (endRadians - beginRadians) * static_cast<float>(index) / static_cast<float>(count);
        return center + radius * (std::cos(angle) * axisX + std::sin(angle) * axisY);
    };
    glm::vec3 previous = point(0);
    for (uint32_t index = 1; index <= count; ++index) {
        const glm::vec3 next = point(index);
        addLine(previous, next, color, depth);
        previous = next;
    }
}

void DebugDraw::line(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style) {
    addLine(from, to, packDebugColor(style.color), style.depth);
}

void DebugDraw::arrow(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style, float headLength) {
    const uint32_t color = packDebugColor(style.color);
    addLine(from, to, color, style.depth);
    const glm::vec3 shaft = to - from;
    const float length = glm::length(shaft);
    if (!(length > 0.0f)) {
        return;
    }
    const glm::vec3 direction = shaft / length;
    const float head = std::min(headLength > 0.0f ? headLength : length * 0.2f, length);
    const auto [side, up] = perpendicularBasis(direction);
    const glm::vec3 base = to - direction * head;
    const float spread = head * 0.35f;
    for (const glm::vec3& offset : {side, -side, up, -up}) {
        addLine(to, base + offset * spread, color, style.depth);
    }
}

void DebugDraw::aabb(const glm::vec3& minimum, const glm::vec3& maximum, const DebugStyle& style) {
    box(glm::translate(glm::mat4(1.0f), (minimum + maximum) * 0.5f), (maximum - minimum) * 0.5f, style);
}

void DebugDraw::box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                    const DebugStyle& style) {
    box(glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(rotation), halfExtents, style);
}

void DebugDraw::box(const glm::mat4& transform, const glm::vec3& halfExtents, const DebugStyle& style) {
    std::array<glm::vec3, 8> corners;
    for (uint32_t index = 0; index < corners.size(); ++index) {
        const glm::vec3 sign{(index & 1u) != 0u ? 1.0f : -1.0f, (index & 2u) != 0u ? 1.0f : -1.0f,
                             (index & 4u) != 0u ? 1.0f : -1.0f};
        corners[index] = glm::vec3(transform * glm::vec4(sign * halfExtents, 1.0f));
    }
    const uint32_t color = packDebugColor(style.color);
    // Each edge joins two corners whose index differs in exactly one bit.
    for (uint32_t index = 0; index < corners.size(); ++index) {
        for (uint32_t bit = 1; bit < 8u; bit <<= 1u) {
            if ((index & bit) == 0u) {
                addLine(corners[index], corners[index | bit], color, style.depth);
            }
        }
    }
}

void DebugDraw::sphere(const glm::vec3& center, float radius, const DebugStyle& style, uint32_t segments) {
    circle(center, {1.0f, 0.0f, 0.0f}, radius, style, segments);
    circle(center, {0.0f, 1.0f, 0.0f}, radius, style, segments);
    circle(center, {0.0f, 0.0f, 1.0f}, radius, style, segments);
}

void DebugDraw::circle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugStyle& style,
                       uint32_t segments) {
    if (!(glm::length(normal) > 0.0f)) {
        return;
    }
    const auto [axisX, axisY] = perpendicularBasis(glm::normalize(normal));
    addArc(center, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, packDebugColor(style.color),
           style.depth);
}

void DebugDraw::frustum(const glm::mat4& viewProjection, const DebugStyle& style) {
    const std::array<glm::vec3, 8> corners = frustumCorners(glm::inverse(viewProjection));
    const uint32_t color = packDebugColor(style.color);
    for (uint32_t index = 0; index < 4u; ++index) {
        const uint32_t next = (index + 1u) % 4u;
        addLine(corners[index], corners[next], color, style.depth);
        addLine(corners[index + 4u], corners[next + 4u], color, style.depth);
        addLine(corners[index], corners[index + 4u], color, style.depth);
    }
}

void DebugDraw::cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngleRadians,
                     const DebugStyle& style, uint32_t segments) {
    if (!(glm::length(direction) > 0.0f)) {
        return;
    }
    const glm::vec3 axis = glm::normalize(direction);
    const glm::vec3 baseCenter = apex + axis * length;
    const float baseRadius = length * std::tan(halfAngleRadians);
    const auto [axisX, axisY] = perpendicularBasis(axis);
    const uint32_t color = packDebugColor(style.color);
    addArc(baseCenter, axisX, axisY, baseRadius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    for (const glm::vec3& offset : {axisX, -axisX, axisY, -axisY}) {
        addLine(apex, baseCenter + offset * baseRadius, color, style.depth);
    }
}

void DebugDraw::capsule(const glm::vec3& from, const glm::vec3& to, float radius, const DebugStyle& style,
                        uint32_t segments) {
    const glm::vec3 spine = to - from;
    if (!(glm::length(spine) > 0.0f)) {
        sphere(from, radius, style, segments);
        return;
    }
    const glm::vec3 axis = glm::normalize(spine);
    const auto [axisX, axisY] = perpendicularBasis(axis);
    const uint32_t color = packDebugColor(style.color);
    const uint32_t halfSegments = std::max(segments / 2u, 1u);
    addArc(from, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    addArc(to, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    for (const glm::vec3& offset : {axisX, -axisX, axisY, -axisY}) {
        addLine(from + offset * radius, to + offset * radius, color, style.depth);
    }
    // Arcs from one side of the spine over the cap to the other.
    for (const glm::vec3& side : {axisX, axisY}) {
        addArc(to, side, axis, radius, 0.0f, glm::pi<float>(), halfSegments, color, style.depth);
        addArc(from, side, -axis, radius, 0.0f, glm::pi<float>(), halfSegments, color, style.depth);
    }
}

void DebugDraw::label(const glm::vec3& position, std::string text, const glm::vec4& color) {
    if (!finite(position)) {
        return;
    }
    m_labels.push_back({.position = position, .text = std::move(text), .color = color});
}

void DebugDraw::clear() {
    for (std::vector<DebugLineVertex>& lines : m_lines) {
        lines.clear();
    }
    m_labels.clear();
}

bool DebugDraw::empty() const {
    return m_lines[0].empty() && m_lines[1].empty() && m_labels.empty();
}

std::array<glm::vec3, 8> frustumCorners(const glm::mat4& inverseViewProjection) {
    constexpr std::array<glm::vec2, 4> ndc{glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f),
                                           glm::vec2(-1.0f, 1.0f)};
    std::array<glm::vec3, 8> corners;
    for (uint32_t index = 0; index < corners.size(); ++index) {
        const glm::vec2 xy = ndc[index % 4u];
        const glm::vec4 world = inverseViewProjection * glm::vec4(xy, index < 4u ? 0.0f : 1.0f, 1.0f);
        corners[index] = glm::vec3(world) / world.w;
    }
    return corners;
}

std::optional<ProjectedDebugPoint> projectDebugPoint(const glm::mat4& viewProjection, const glm::vec2& viewportSize,
                                                     const glm::vec3& world) {
    const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
    // For a perspective projection clip.w is the distance along the forward axis.
    if (!(clip.w > 1e-6f)) {
        return std::nullopt;
    }
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    if (!(std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f)) {
        return std::nullopt;
    }
    // Vulkan clip space: y = -1 is the top row of the viewport.
    return ProjectedDebugPoint{.position = (ndc * 0.5f + 0.5f) * viewportSize, .distance = clip.w};
}

void projectDebugLabels(std::span<const DebugLabel> labels, const glm::mat4& viewProjection,
                        const glm::vec2& viewportSize, std::vector<ProjectedDebugLabel>& output) {
    for (const DebugLabel& label : labels) {
        if (const std::optional<ProjectedDebugPoint> point =
                projectDebugPoint(viewProjection, viewportSize, label.position)) {
            output.push_back(
                {.position = point->position, .distance = point->distance, .text = label.text, .color = label.color});
        }
    }
}

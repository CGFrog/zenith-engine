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

// The shapes, tessellated into one primitive list.
void addLine(DebugPrimitives& output, const glm::vec3& from, const glm::vec3& to, uint32_t color, DebugDepth depth) {
    // A non-finite position would rasterize unpredictably; drop the segment.
    if (!finite(from) || !finite(to)) {
        return;
    }
    std::vector<DebugLineVertex>& lines = output.lines[static_cast<size_t>(depth)];
    lines.push_back({from, color});
    lines.push_back({to, color});
}

void addArc(DebugPrimitives& output, const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY,
            float radius, float beginRadians, float endRadians, uint32_t segments, uint32_t color, DebugDepth depth) {
    const uint32_t count = std::max(segments, 1u);
    const auto point = [&](uint32_t index) {
        const float angle =
            beginRadians + (endRadians - beginRadians) * static_cast<float>(index) / static_cast<float>(count);
        return center + radius * (std::cos(angle) * axisX + std::sin(angle) * axisY);
    };
    glm::vec3 previous = point(0);
    for (uint32_t index = 1; index <= count; ++index) {
        const glm::vec3 next = point(index);
        addLine(output, previous, next, color, depth);
        previous = next;
    }
}

void addBox(DebugPrimitives& output, const glm::mat4& transform, const glm::vec3& halfExtents,
            const DebugStyle& style) {
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
                addLine(output, corners[index], corners[index | bit], color, style.depth);
            }
        }
    }
}

void addCircle(DebugPrimitives& output, const glm::vec3& center, const glm::vec3& normal, float radius,
               const DebugStyle& style, uint32_t segments) {
    if (!(glm::length(normal) > 0.0f)) {
        return;
    }
    const auto [axisX, axisY] = perpendicularBasis(glm::normalize(normal));
    addArc(output, center, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, packDebugColor(style.color),
           style.depth);
}

void addSphere(DebugPrimitives& output, const glm::vec3& center, float radius, const DebugStyle& style,
               uint32_t segments) {
    addCircle(output, center, {1.0f, 0.0f, 0.0f}, radius, style, segments);
    addCircle(output, center, {0.0f, 1.0f, 0.0f}, radius, style, segments);
    addCircle(output, center, {0.0f, 0.0f, 1.0f}, radius, style, segments);
}

void addArrow(DebugPrimitives& output, const glm::vec3& from, const glm::vec3& to, const DebugStyle& style,
              float headLength) {
    const uint32_t color = packDebugColor(style.color);
    addLine(output, from, to, color, style.depth);
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
        addLine(output, to, base + offset * spread, color, style.depth);
    }
}

void addFrustum(DebugPrimitives& output, const glm::mat4& viewProjection, const DebugStyle& style) {
    const std::array<glm::vec3, 8> corners = frustumCorners(glm::inverse(viewProjection));
    const uint32_t color = packDebugColor(style.color);
    for (uint32_t index = 0; index < 4u; ++index) {
        const uint32_t next = (index + 1u) % 4u;
        addLine(output, corners[index], corners[next], color, style.depth);
        addLine(output, corners[index + 4u], corners[next + 4u], color, style.depth);
        addLine(output, corners[index], corners[index + 4u], color, style.depth);
    }
}

void addCone(DebugPrimitives& output, const glm::vec3& apex, const glm::vec3& direction, float length,
             float halfAngleRadians, const DebugStyle& style, uint32_t segments) {
    if (!(glm::length(direction) > 0.0f)) {
        return;
    }
    const glm::vec3 axis = glm::normalize(direction);
    const glm::vec3 baseCenter = apex + axis * length;
    const float baseRadius = length * std::tan(halfAngleRadians);
    const auto [axisX, axisY] = perpendicularBasis(axis);
    const uint32_t color = packDebugColor(style.color);
    addArc(output, baseCenter, axisX, axisY, baseRadius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    for (const glm::vec3& offset : {axisX, -axisX, axisY, -axisY}) {
        addLine(output, apex, baseCenter + offset * baseRadius, color, style.depth);
    }
}

void addCapsule(DebugPrimitives& output, const glm::vec3& from, const glm::vec3& to, float radius,
                const DebugStyle& style, uint32_t segments) {
    const glm::vec3 spine = to - from;
    if (!(glm::length(spine) > 0.0f)) {
        addSphere(output, from, radius, style, segments);
        return;
    }
    const glm::vec3 axis = glm::normalize(spine);
    const auto [axisX, axisY] = perpendicularBasis(axis);
    const uint32_t color = packDebugColor(style.color);
    const uint32_t halfSegments = std::max(segments / 2u, 1u);
    addArc(output, from, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    addArc(output, to, axisX, axisY, radius, 0.0f, glm::two_pi<float>(), segments, color, style.depth);
    for (const glm::vec3& offset : {axisX, -axisX, axisY, -axisY}) {
        addLine(output, from + offset * radius, to + offset * radius, color, style.depth);
    }
    // Arcs from one side of the spine over the cap to the other.
    for (const glm::vec3& side : {axisX, axisY}) {
        addArc(output, to, side, axis, radius, 0.0f, glm::pi<float>(), halfSegments, color, style.depth);
        addArc(output, from, side, -axis, radius, 0.0f, glm::pi<float>(), halfSegments, color, style.depth);
    }
}

void addLabel(DebugPrimitives& output, const glm::vec3& position, std::string text, const glm::vec4& color) {
    if (!finite(position)) {
        return;
    }
    output.labels.push_back({.position = position, .text = std::move(text), .color = color});
}
} // namespace

uint32_t packDebugColor(const glm::vec4& color) {
    return static_cast<uint32_t>(unitToByte(color.r)) | (static_cast<uint32_t>(unitToByte(color.g)) << 8u) |
           (static_cast<uint32_t>(unitToByte(color.b)) << 16u) | (static_cast<uint32_t>(unitToByte(color.a)) << 24u);
}

bool DebugPrimitives::empty() const {
    return lines[0].empty() && lines[1].empty() && labels.empty();
}

void DebugPrimitives::clear() {
    for (std::vector<DebugLineVertex>& list : lines) {
        list.clear();
    }
    labels.clear();
}

void DebugPrimitives::append(const DebugPrimitives& other) {
    for (size_t depth = 0; depth < lines.size(); ++depth) {
        lines[depth].insert(lines[depth].end(), other.lines[depth].begin(), other.lines[depth].end());
    }
    labels.insert(labels.end(), other.labels.begin(), other.labels.end());
}

DebugDrawRecorder DebugDrawRecorder::forSeconds(float seconds) const {
    return {*m_owner, m_channel, DebugLifetime::Timed, std::isfinite(seconds) ? std::max(seconds, 0.0f) : 0.0f};
}

DebugDrawRecorder DebugDrawRecorder::persistent() const {
    return {*m_owner, m_channel, DebugLifetime::Persistent, 0.0f};
}

DebugPrimitives* DebugDrawRecorder::target() const {
    return m_owner->target(m_channel, m_lifetime, m_seconds);
}

void DebugDrawRecorder::line(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style) const {
    if (DebugPrimitives* output = target()) {
        addLine(*output, from, to, packDebugColor(style.color), style.depth);
    }
}

void DebugDrawRecorder::arrow(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style,
                              float headLength) const {
    if (DebugPrimitives* output = target()) {
        addArrow(*output, from, to, style, headLength);
    }
}

void DebugDrawRecorder::aabb(const glm::vec3& minimum, const glm::vec3& maximum, const DebugStyle& style) const {
    box(glm::translate(glm::mat4(1.0f), (minimum + maximum) * 0.5f), (maximum - minimum) * 0.5f, style);
}

void DebugDrawRecorder::box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                            const DebugStyle& style) const {
    box(glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(rotation), halfExtents, style);
}

void DebugDrawRecorder::box(const glm::mat4& transform, const glm::vec3& halfExtents, const DebugStyle& style) const {
    if (DebugPrimitives* output = target()) {
        addBox(*output, transform, halfExtents, style);
    }
}

void DebugDrawRecorder::sphere(const glm::vec3& center, float radius, const DebugStyle& style,
                               uint32_t segments) const {
    if (DebugPrimitives* output = target()) {
        addSphere(*output, center, radius, style, segments);
    }
}

void DebugDrawRecorder::circle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugStyle& style,
                               uint32_t segments) const {
    if (DebugPrimitives* output = target()) {
        addCircle(*output, center, normal, radius, style, segments);
    }
}

void DebugDrawRecorder::frustum(const glm::mat4& viewProjection, const DebugStyle& style) const {
    if (DebugPrimitives* output = target()) {
        addFrustum(*output, viewProjection, style);
    }
}

void DebugDrawRecorder::cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngleRadians,
                             const DebugStyle& style, uint32_t segments) const {
    if (DebugPrimitives* output = target()) {
        addCone(*output, apex, direction, length, halfAngleRadians, style, segments);
    }
}

void DebugDrawRecorder::capsule(const glm::vec3& from, const glm::vec3& to, float radius, const DebugStyle& style,
                                uint32_t segments) const {
    if (DebugPrimitives* output = target()) {
        addCapsule(*output, from, to, radius, style, segments);
    }
}

void DebugDrawRecorder::label(const glm::vec3& position, std::string text, const glm::vec4& color) const {
    if (DebugPrimitives* output = target()) {
        addLabel(*output, position, std::move(text), color);
    }
}

void DebugDraw::line(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style) {
    recorder().line(from, to, style);
}

void DebugDraw::arrow(const glm::vec3& from, const glm::vec3& to, const DebugStyle& style, float headLength) {
    recorder().arrow(from, to, style, headLength);
}

void DebugDraw::aabb(const glm::vec3& minimum, const glm::vec3& maximum, const DebugStyle& style) {
    recorder().aabb(minimum, maximum, style);
}

void DebugDraw::box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                    const DebugStyle& style) {
    recorder().box(center, halfExtents, rotation, style);
}

void DebugDraw::box(const glm::mat4& transform, const glm::vec3& halfExtents, const DebugStyle& style) {
    recorder().box(transform, halfExtents, style);
}

void DebugDraw::sphere(const glm::vec3& center, float radius, const DebugStyle& style, uint32_t segments) {
    recorder().sphere(center, radius, style, segments);
}

void DebugDraw::circle(const glm::vec3& center, const glm::vec3& normal, float radius, const DebugStyle& style,
                       uint32_t segments) {
    recorder().circle(center, normal, radius, style, segments);
}

void DebugDraw::frustum(const glm::mat4& viewProjection, const DebugStyle& style) {
    recorder().frustum(viewProjection, style);
}

void DebugDraw::cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngleRadians,
                     const DebugStyle& style, uint32_t segments) {
    recorder().cone(apex, direction, length, halfAngleRadians, style, segments);
}

void DebugDraw::capsule(const glm::vec3& from, const glm::vec3& to, float radius, const DebugStyle& style,
                        uint32_t segments) {
    recorder().capsule(from, to, radius, style, segments);
}

void DebugDraw::label(const glm::vec3& position, std::string text, const glm::vec4& color) {
    recorder().label(position, std::move(text), color);
}

DebugDrawRecorder DebugDraw::channel(std::string_view name) {
    return {*this, channelIndex(name), DebugLifetime::Frame, 0.0f};
}

DebugDrawRecorder DebugDraw::forSeconds(float seconds) {
    return recorder().forSeconds(seconds);
}

DebugDrawRecorder DebugDraw::persistent() {
    return recorder().persistent();
}

uint32_t DebugDraw::channelIndex(std::string_view name) {
    for (uint32_t index = 0; index < m_channels.size(); ++index) {
        if (m_channels[index].name == name) {
            return index;
        }
    }
    m_channels.push_back({.name = std::string(name)});
    return static_cast<uint32_t>(m_channels.size() - 1);
}

const DebugDraw::Channel* DebugDraw::findChannel(std::string_view name) const {
    const auto found = std::ranges::find(m_channels, name, &Channel::name);
    return found == m_channels.end() ? nullptr : &*found;
}

DebugPrimitives* DebugDraw::target(uint32_t channel, DebugLifetime lifetime, float seconds) {
    Channel& owner = m_channels[channel];
    switch (lifetime) {
    case DebugLifetime::Frame:
        // Hidden one-frame shapes would be discarded unseen.
        if (!owner.visible) {
            return nullptr;
        }
        m_composedDirty = true;
        return &owner.frame;
    case DebugLifetime::Persistent:
        m_composedDirty = true;
        return &owner.persistent;
    case DebugLifetime::Timed:
        break;
    }
    m_composedDirty = true;
    if (m_timed.empty() || !m_timed.back().fresh || m_timed.back().channel != channel ||
        m_timed.back().remainingSeconds != seconds) {
        m_timed.push_back({.channel = channel, .remainingSeconds = seconds});
    }
    return &m_timed.back().primitives;
}

void DebugDraw::setChannelVisible(std::string_view name, bool visible) {
    Channel& channel = m_channels[channelIndex(name)];
    if (channel.visible != visible) {
        channel.visible = visible;
        m_composedDirty = true;
    }
}

bool DebugDraw::channelVisible(std::string_view name) const {
    const Channel* channel = findChannel(name);
    return channel == nullptr || channel->visible;
}

void DebugDraw::clearChannel(std::string_view name) {
    const auto found = std::ranges::find(m_channels, name, &Channel::name);
    if (found == m_channels.end()) {
        return;
    }
    const auto index = static_cast<uint32_t>(found - m_channels.begin());
    found->frame.clear();
    found->persistent.clear();
    std::erase_if(m_timed, [index](const TimedBatch& batch) { return batch.channel == index; });
    m_composedDirty = true;
}

std::vector<DebugDrawChannelInfo> DebugDraw::channels() const {
    std::vector<DebugDrawChannelInfo> named;
    for (uint32_t index = 1; index < m_channels.size(); ++index) {
        const Channel& channel = m_channels[index];
        const bool timed =
            std::ranges::any_of(m_timed, [index](const TimedBatch& batch) { return batch.channel == index; });
        named.push_back(
            {.name = channel.name,
             .visible = channel.visible,
             .active = channel.drewLastFrame || !channel.frame.empty() || !channel.persistent.empty() || timed});
    }
    std::ranges::sort(named, {}, &DebugDrawChannelInfo::name);
    return named;
}

void DebugDraw::endFrame(float elapsedSeconds) {
    for (Channel& channel : m_channels) {
        channel.drewLastFrame = !channel.frame.empty();
        channel.frame.clear();
    }
    const float elapsed = std::isfinite(elapsedSeconds) ? std::max(elapsedSeconds, 0.0f) : 0.0f;
    for (TimedBatch& batch : m_timed) {
        batch.remainingSeconds -= elapsed;
        batch.fresh = false;
    }
    // A batch is drawn in at least the frame it was recorded for, so one
    // whose time is up after that frame goes now.
    std::erase_if(m_timed, [](const TimedBatch& batch) { return batch.remainingSeconds <= 0.0f; });
    m_composedDirty = true;
}

void DebugDraw::clear() {
    for (Channel& channel : m_channels) {
        channel.frame.clear();
        channel.persistent.clear();
        channel.drewLastFrame = false;
    }
    m_timed.clear();
    m_composed.clear();
    m_composedDirty = false;
}

bool DebugDraw::empty() const {
    return std::ranges::all_of(
               m_channels,
               [](const Channel& channel) { return channel.frame.empty() && channel.persistent.empty(); }) &&
           std::ranges::all_of(m_timed, [](const TimedBatch& batch) { return batch.primitives.empty(); });
}

const DebugPrimitives& DebugDraw::composed() const {
    if (!m_composedDirty) {
        return m_composed;
    }
    m_composed.clear();
    for (const Channel& channel : m_channels) {
        if (channel.visible) {
            m_composed.append(channel.frame);
            m_composed.append(channel.persistent);
        }
    }
    for (const TimedBatch& batch : m_timed) {
        if (m_channels[batch.channel].visible) {
            m_composed.append(batch.primitives);
        }
    }
    m_composedDirty = false;
    return m_composed;
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

#include "GpuCullLayout.hpp"

std::array<glm::vec4, 6> clipVolumePlanes(const glm::mat4& viewProjection) {
    const glm::mat4 rows = glm::transpose(viewProjection);
    return {rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]};
}

bool sphereReachesClipVolume(const std::array<glm::vec4, 6>& planes, const glm::vec3& center, float radius) {
    for (const glm::vec4& plane : planes) {
        const glm::vec3 normal(plane);
        if (glm::dot(normal, center) + plane.w < -radius * glm::length(normal)) {
            return false;
        }
    }
    return true;
}

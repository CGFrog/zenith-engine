#include "NavigationCrowd.hpp"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cstring>

namespace {
// Agents accelerate almost at once: a character controller smooths its own
// motion, and a transform-driven agent should start and stop crisply.
constexpr float agentAcceleration = 100.0f;

const float* floats(const glm::vec3& value) {
    return &value.x;
}

struct CrowdDeleter {
    void operator()(dtCrowd* crowd) const {
        dtFreeCrowd(crowd);
    }
};
} // namespace

class NavigationCrowd::Impl {
  public:
    std::unique_ptr<dtCrowd, CrowdDeleter> crowd;
    uint64_t meshGeneration{0};
    NavCrowdSettings settings;
    std::vector<uint64_t> owners;
    std::vector<uint8_t> marked;
};

NavigationCrowd::NavigationCrowd()
    : m_impl(std::make_unique<Impl>()) {}

NavigationCrowd::~NavigationCrowd() = default;

NavigationResult NavigationCrowd::reset(const NavigationWorld& navigation, const NavCrowdSettings& settings) {
    m_impl->crowd.reset();
    m_impl->owners.clear();
    m_impl->marked.clear();
    m_impl->meshGeneration = navigation.generation();
    const dtNavMesh* navMesh = navigation.nativeNavMesh();
    if (navMesh == nullptr) {
        return {.error = "The navigation mesh is not baked"};
    }
    const int maximumAgents = static_cast<int>(std::max(settings.maximumAgents, 1u));
    std::unique_ptr<dtCrowd, CrowdDeleter> crowd(dtAllocCrowd());
    // Detour reads the mesh only; its API is not const-correct.
    if (crowd == nullptr ||
        !crowd->init(maximumAgents, std::max(settings.maximumAgentRadius, 0.01f), const_cast<dtNavMesh*>(navMesh))) {
        return {.error = "The navigation crowd could not be created"};
    }
    m_impl->crowd = std::move(crowd);
    m_impl->settings = settings;
    m_impl->owners.assign(static_cast<size_t>(maximumAgents), 0);
    m_impl->marked.assign(static_cast<size_t>(maximumAgents), 0);
    return {.success = true};
}

bool NavigationCrowd::ready() const {
    return m_impl->crowd != nullptr;
}

uint64_t NavigationCrowd::meshGeneration() const {
    return m_impl->meshGeneration;
}

bool NavigationCrowd::valid(int agent) const {
    return ready() && agent >= 0 && agent < m_impl->crowd->getAgentCount() && m_impl->crowd->getAgent(agent)->active;
}

int NavigationCrowd::addAgent(const glm::vec3& position, const NavCrowdAgentParams& params, uint64_t owner) {
    if (!ready()) {
        return -1;
    }
    dtCrowdAgentParams detour{};
    detour.radius = std::clamp(params.radius, 0.0f, m_impl->settings.maximumAgentRadius);
    detour.height = std::max(params.height, 0.01f);
    detour.maxAcceleration = agentAcceleration;
    detour.maxSpeed = std::max(params.maximumSpeed, 0.0f);
    detour.collisionQueryRange = std::max(detour.radius, 0.1f) * 12.0f;
    detour.pathOptimizationRange = std::max(detour.radius, 0.1f) * 30.0f;
    detour.separationWeight = std::max(m_impl->settings.separationWeight, 0.0f);
    detour.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_OPTIMIZE_VIS |
                         DT_CROWD_OPTIMIZE_TOPO | (detour.separationWeight > 0.0f ? DT_CROWD_SEPARATION : 0);
    // Detour's default avoidance profile, set up by dtCrowd::init.
    detour.obstacleAvoidanceType = 0;
    const int agent = m_impl->crowd->addAgent(floats(position), &detour);
    if (agent >= 0) {
        m_impl->owners[static_cast<size_t>(agent)] = owner;
        m_impl->marked[static_cast<size_t>(agent)] = 1;
    }
    return agent;
}

void NavigationCrowd::removeAgent(int agent) {
    if (valid(agent)) {
        m_impl->crowd->removeAgent(agent);
        m_impl->owners[static_cast<size_t>(agent)] = 0;
    }
}

bool NavigationCrowd::owns(int agent, uint64_t owner) const {
    return valid(agent) && m_impl->owners[static_cast<size_t>(agent)] == owner;
}

bool NavigationCrowd::requestMove(int agent, const glm::vec3& target) {
    if (!valid(agent)) {
        return false;
    }
    const dtNavMeshQuery* query = m_impl->crowd->getNavMeshQuery();
    const glm::vec3 extents(2.0f, 4.0f, 2.0f);
    dtPolyRef polygon = 0;
    glm::vec3 nearest(0.0f);
    if (dtStatusFailed(query->findNearestPoly(floats(target), floats(extents), m_impl->crowd->getFilter(0), &polygon,
                                              &nearest.x)) ||
        polygon == 0) {
        return false;
    }
    return m_impl->crowd->requestMoveTarget(agent, polygon, &nearest.x);
}

void NavigationCrowd::stop(int agent) {
    if (valid(agent)) {
        m_impl->crowd->resetMoveTarget(agent);
    }
}

void NavigationCrowd::placeAgent(int agent, const glm::vec3& position) {
    if (valid(agent)) {
        // The corridor follows on the next update, which moves its start to
        // the agent's position along the mesh.
        dtCrowdAgent* detour = m_impl->crowd->getEditableAgent(agent);
        std::memcpy(detour->npos, floats(position), sizeof(detour->npos));
    }
}

void NavigationCrowd::update(float deltaSeconds) {
    if (ready() && deltaSeconds > 0.0f) {
        m_impl->crowd->update(deltaSeconds, nullptr);
    }
}

NavCrowdAgentState NavigationCrowd::state(int agent) const {
    if (!valid(agent)) {
        return {};
    }
    const dtCrowdAgent& detour = *m_impl->crowd->getAgent(agent);
    const float* end = detour.corridor.getTarget();
    return {.position = {detour.npos[0], detour.npos[1], detour.npos[2]},
            .velocity = {detour.vel[0], detour.vel[1], detour.vel[2]},
            .pathEnd = {end[0], end[1], end[2]},
            .partial = detour.partial,
            .moving = detour.targetState == DT_CROWDAGENT_TARGET_VALID};
}

void NavigationCrowd::mark(int agent) {
    if (valid(agent)) {
        m_impl->marked[static_cast<size_t>(agent)] = 1;
    }
}

void NavigationCrowd::sweepUnmarked() {
    if (!ready()) {
        return;
    }
    for (int agent = 0; agent < m_impl->crowd->getAgentCount(); ++agent) {
        uint8_t& marked = m_impl->marked[static_cast<size_t>(agent)];
        if (!marked) {
            removeAgent(agent);
        }
        marked = 0;
    }
}

size_t NavigationCrowd::agentCount() const {
    if (!ready()) {
        return 0;
    }
    size_t count = 0;
    for (int agent = 0; agent < m_impl->crowd->getAgentCount(); ++agent) {
        count += m_impl->crowd->getAgent(agent)->active ? 1 : 0;
    }
    return count;
}

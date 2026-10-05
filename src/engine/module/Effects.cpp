#include "Effects.hpp"

#include "EntityCommandBuffer.hpp"
#include "Timers.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/scene/ParticleEmitter.hpp"

#include <algorithm>
#include <utility>

namespace {
template <typename Visit> void forEachInSubtree(const Scene& scene, Entity root, const Visit& visit) {
    visit(root);
    if (const auto* relationship = scene.registry().try_get<RelationshipComponent>(root)) {
        for (const Entity child : relationship->children) {
            if (scene.registry().valid(child)) {
                forEachInSubtree(scene, child, visit);
            }
        }
    }
}

// Starts (restart) or stops every emitter of the effect.
void setEmitting(Scene& scene, Entity root, bool emitting, bool restart) {
    forEachInSubtree(scene, root, [&](Entity entity) {
        if (auto* emitter = scene.registry().try_get<ParticleEmitterComponent>(entity)) {
            emitter->emitting = emitting;
            if (restart) {
                ++emitter->restartGeneration;
            }
        }
    });
}
} // namespace

std::optional<float> derivedEffectLifetime(const Scene& scene, Entity root, std::string& error) {
    std::optional<float> lifetime;
    bool repeating = false;
    forEachInSubtree(scene, root, [&](Entity entity) {
        if (const auto* emitter = scene.registry().try_get<ParticleEmitterComponent>(entity)) {
            repeating = repeating || emitter->spawnRate > 0.0f || emitter->burstInterval > 0.0f;
            lifetime = std::max(lifetime.value_or(0.0f), emitter->lifetimeMax);
        }
    });
    if (repeating) {
        error = "it has a continuous or repeating particle emitter";
        return std::nullopt;
    }
    if (!lifetime) {
        error = "it has no particle emitter";
    }
    return lifetime;
}

Effects::Effects(EntityCommandBuffer& commands, Timers& timers, const LogChannel& log)
    : m_commands(commands)
    , m_timers(timers)
    , m_log(log) {}

void Effects::spawn(std::filesystem::path prefab, const glm::mat4& transform, EffectOptions options) {
    m_requests.push_back({.prefab = std::move(prefab), .transform = transform, .options = options});
}

size_t Effects::activeCount() const {
    return m_active.size();
}

size_t Effects::parkedCount(const std::filesystem::path& prefab) const {
    const auto parked = m_parked.find(prefab.generic_string());
    return parked != m_parked.end() ? parked->second.size() : 0;
}

void Effects::flush(Scene& scene) {
    // Forget effects whose entities were destroyed early.
    std::erase_if(m_active, [&scene](uint64_t root) { return scene.findEntity(SceneEntityId{root}) == entt::null; });
    std::vector<Request> requests = std::exchange(m_requests, {});
    for (Request& request : requests) {
        std::string pool = request.prefab.generic_string();
        if (request.options.pooled) {
            std::vector<Parked>& parked = m_parked[pool];
            Entity reused = entt::null;
            Parked instance;
            while (reused == entt::null && !parked.empty()) {
                instance = parked.back();
                parked.pop_back();
                reused = scene.findEntity(instance.root);
            }
            if (reused != entt::null) {
                scene.setWorldTransform(reused, request.transform);
                setEmitting(scene, reused, true, true);
                play(scene, instance.root, pool, request.options.lifetime.value_or(instance.lifetime), true);
                continue;
            }
        }
        m_commands.spawn(request.prefab, {.localTransform = request.transform},
                         [this, pool = std::move(pool), options = request.options](Scene& spawnedScene, Entity root) {
                             spawned(spawnedScene, root, pool, options);
                         });
    }
}

void Effects::clear() {
    m_requests.clear();
    m_active.clear();
    m_parked.clear();
}

void Effects::spawned(Scene& scene, Entity root, const std::string& pool, const EffectOptions& options) {
    std::string error;
    const std::optional<float> lifetime =
        options.lifetime ? options.lifetime : derivedEffectLifetime(scene, root, error);
    if (!lifetime) {
        m_log.error("Effect '" + pool + "' needs an explicit lifetime: " + error);
        m_commands.destroy(scene.id(root));
        return;
    }
    play(scene, scene.id(root), pool, *lifetime, options.pooled);
}

void Effects::play(Scene&, SceneEntityId root, const std::string& pool, float lifetime, bool pooled) {
    m_active.insert(root.value);
    m_timers.start(
        std::max(lifetime, 0.0f), TimerRepeat::Once,
        [this, root, pool, lifetime, pooled](TimerContext& context) {
            finish(context.scene, root, pool, lifetime, pooled);
        },
        root);
}

void Effects::finish(Scene& scene, SceneEntityId root, const std::string& pool, float lifetime, bool pooled) {
    m_active.erase(root.value);
    const Entity entity = scene.findEntity(root);
    if (entity == entt::null) {
        return;
    }
    if (!pooled) {
        m_commands.destroy(root);
        return;
    }
    setEmitting(scene, entity, false, false);
    m_parked[pool].push_back({.root = root, .lifetime = lifetime});
}

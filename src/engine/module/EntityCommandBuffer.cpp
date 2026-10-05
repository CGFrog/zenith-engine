#include "EntityCommandBuffer.hpp"

#include "EventQueue.hpp"
#include "GameEvents.hpp"
#include "engine/diagnostics/Logger.hpp"
#include "engine/scene/Prefab.hpp"

#include <utility>

namespace {
// Callbacks that keep recording commands could otherwise flush forever.
constexpr int maximumFlushRounds = 16;

std::string describe(SceneEntityId id) {
    return "entity " + std::to_string(id.value);
}

// Physics adds bodies for entities it is told about (or finds in its periodic
// audit); a spawned body should simulate from the next step.
void markBodiesForPhysics(Scene& scene, Entity root) {
    std::vector<Entity> pending{root};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        if (!scene.registry().valid(entity)) {
            continue;
        }
        if (scene.registry().any_of<ColliderComponent, RigidBodyComponent>(entity)) {
            scene.markPhysicsDirty(entity);
        }
        const auto& children = scene.registry().get<RelationshipComponent>(entity).children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
}
} // namespace

void EntityCommandBuffer::spawn(std::filesystem::path prefab, SpawnPlacement placement, SpawnedCallback onSpawned) {
    m_commands.emplace_back(Spawn{
        .prefab = std::move(prefab), .name = {}, .placement = std::move(placement), .onSpawned = std::move(onSpawned)});
}

void EntityCommandBuffer::spawn(std::filesystem::path prefab, SpawnPlacement placement,
                                std::vector<PrefabOverride> overrides, SpawnedCallback onSpawned) {
    m_commands.emplace_back(Spawn{.prefab = std::move(prefab),
                                  .name = {},
                                  .placement = std::move(placement),
                                  .onSpawned = std::move(onSpawned),
                                  .overrides = std::move(overrides)});
}

void EntityCommandBuffer::spawnEmpty(std::string name, SpawnPlacement placement, SpawnedCallback onSpawned) {
    m_commands.emplace_back(Spawn{
        .prefab = {}, .name = std::move(name), .placement = std::move(placement), .onSpawned = std::move(onSpawned)});
}

void EntityCommandBuffer::destroy(SceneEntityId entity) {
    m_commands.emplace_back(Destroy{entity});
}

void EntityCommandBuffer::reparent(SceneEntityId child, SceneEntityId parent) {
    m_commands.emplace_back(Reparent{child, parent});
}

bool EntityCommandBuffer::empty() const {
    return m_commands.empty();
}

size_t EntityCommandBuffer::size() const {
    return m_commands.size();
}

void EntityCommandBuffer::clear() {
    m_commands.clear();
}

void EntityCommandBuffer::flush(Scene& scene, PrefabSystem& prefabs, EventQueue& events, const LogChannel& log,
                                const DestroyingCallback& destroying) {
    for (int round = 0; !m_commands.empty(); ++round) {
        if (round == maximumFlushRounds) {
            log.error("Entity commands kept recording more commands while flushing; dropped " +
                      std::to_string(m_commands.size()));
            m_commands.clear();
            return;
        }
        // Taken out first: callbacks record into a fresh list for the next round.
        std::vector<Command> commands = std::exchange(m_commands, {});
        for (Command& command : commands) {
            if (auto* spawn = std::get_if<Spawn>(&command)) {
                apply(*spawn, scene, prefabs, events, log);
            } else if (const auto* destroy = std::get_if<Destroy>(&command)) {
                Entity entity = scene.findEntity(destroy->entity);
                if (entity != entt::null && destroying) {
                    destroying(scene, entity);
                    // The callback may have destroyed it already.
                    entity = scene.findEntity(destroy->entity);
                }
                if (entity != entt::null) {
                    scene.destroyEntity(entity);
                    events.publish(EntityDestroyed{destroy->entity});
                }
            } else if (const auto* reparent = std::get_if<Reparent>(&command)) {
                const Entity child = scene.findEntity(reparent->child);
                if (child == entt::null) {
                    continue;
                }
                const Entity parent = reparent->parent ? scene.findEntity(reparent->parent) : entt::null;
                if (reparent->parent && parent == entt::null) {
                    log.warning("Cannot reparent " + describe(reparent->child) + " under missing " +
                                describe(reparent->parent));
                    continue;
                }
                if (const SceneResult result = scene.reparent(child, parent, ReparentMode::PreserveWorldTransform);
                    !result) {
                    log.warning("Cannot reparent " + describe(reparent->child) + ": " + result.error);
                }
            }
        }
    }
}

void EntityCommandBuffer::apply(Spawn& command, Scene& scene, PrefabSystem& prefabs, EventQueue& events,
                                const LogChannel& log) {
    const Entity parent = command.placement.parent ? scene.findEntity(command.placement.parent) : entt::null;
    Entity root = entt::null;
    if (command.prefab.empty()) {
        root = scene.createEntity(std::move(command.name));
        if (root != entt::null && parent != entt::null) {
            scene.setParent(root, parent);
        }
    } else {
        const PrefabInstanceResult result = prefabs.instantiate(command.prefab, scene, parent, command.overrides);
        if (!result) {
            log.warning("Cannot spawn prefab '" + command.prefab.generic_string() + "': " + result.error);
            return;
        }
        root = result.root;
    }
    if (root == entt::null) {
        log.warning("Cannot spawn an entity: the scene has no free entity id");
        return;
    }
    if (command.placement.localTransform) {
        scene.setLocalTransform(root, *command.placement.localTransform);
    }
    events.publish(EntitySpawned{scene.id(root)});
    if (command.onSpawned) {
        command.onSpawned(scene, root);
    }
    markBodiesForPhysics(scene, root);
}

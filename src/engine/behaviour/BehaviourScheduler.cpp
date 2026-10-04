#include "BehaviourScheduler.hpp"

#include "engine/module/GameServices.hpp"

#include <algorithm>
#include <vector>

// The engine side of Behaviour's lifecycle state, which behaviours themselves
// cannot change.
class BehaviourLifecycle {
  public:
    static void create(Behaviour& behaviour) {
        behaviour.m_created = true;
    }

    static bool active(const Behaviour& behaviour) {
        return behaviour.m_active;
    }

    static void setActive(Behaviour& behaviour, bool active) {
        behaviour.m_active = active;
    }

    static void start(Behaviour& behaviour) {
        behaviour.m_started = true;
    }
};

namespace {
struct Entry {
    const BehaviourType* type{nullptr};
    Entity entity{entt::null};
};

// Every behaviour that runs in mode: by type in registration order, then by
// scene id.
std::vector<Entry> gatherBehaviours(const BehaviourRegistry& behaviours, const Scene& scene, SimulationMode mode) {
    std::vector<Entry> entries;
    std::vector<Entity> entities;
    for (const BehaviourType& type : behaviours.types()) {
        if (!runsIn(type.runIn, mode)) {
            continue;
        }
        entities.clear();
        type.collect(scene, entities);
        std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });
        for (const Entity entity : entities) {
            entries.push_back({&type, entity});
        }
    }
    return entries;
}

// The behaviour again: a hook may have destroyed its entity or changed the
// entity's components since the last call.
Behaviour* behaviourOf(Scene& scene, const Entry& entry) {
    return scene.registry().valid(entry.entity) ? entry.type->find(scene, entry.entity) : nullptr;
}

bool running(const Behaviour& behaviour) {
    return BehaviourLifecycle::active(behaviour) && behaviour.enabled;
}

class Dispatcher {
  public:
    explicit Dispatcher(const SystemContext& context)
        : m_context(context) {}

    BehaviourContext contextFor(Entity entity) const {
        return {.scene = m_context.scene,
                .entity = entity,
                .deltaSeconds = m_context.deltaSeconds,
                .services = m_context.services,
                .actions = m_context.actions,
                .logger = m_context.logger};
    }

    void lifecycle(const std::vector<Entry>& entries) const {
        for (const Entry& entry : entries) {
            Behaviour* behaviour = behaviourOf(m_context.scene, entry);
            if (behaviour == nullptr) {
                continue;
            }
            BehaviourContext context = contextFor(entry.entity);
            if (!behaviour->created()) {
                BehaviourLifecycle::create(*behaviour);
                behaviour->onCreate(context);
                behaviour = behaviourOf(m_context.scene, entry);
            }
            if (behaviour != nullptr && behaviour->enabled != BehaviourLifecycle::active(*behaviour)) {
                BehaviourLifecycle::setActive(*behaviour, behaviour->enabled);
                if (behaviour->enabled) {
                    behaviour->onEnable(context);
                } else {
                    behaviour->onDisable(context);
                }
            }
        }
        for (const Entry& entry : entries) {
            Behaviour* behaviour = behaviourOf(m_context.scene, entry);
            if (behaviour != nullptr && running(*behaviour) && !behaviour->started()) {
                BehaviourLifecycle::start(*behaviour);
                BehaviourContext context = contextFor(entry.entity);
                behaviour->onStart(context);
            }
        }
    }

    // Calls hook on each running behaviour of entity, in registration order.
    template <typename Hook> void forEntity(Entity entity, const Hook& hook) const {
        const BehaviourRegistry& behaviours = *m_context.behaviours;
        for (const BehaviourType& type : behaviours.types()) {
            if (!runsIn(type.runIn, m_context.mode)) {
                continue;
            }
            Behaviour* behaviour = behaviourOf(m_context.scene, {&type, entity});
            if (behaviour != nullptr && running(*behaviour)) {
                BehaviourContext context = contextFor(entity);
                hook(*behaviour, context);
            }
        }
    }

    void physicsEvents(const GameplayPhysicsEvents& events) const {
        const Scene& scene = m_context.scene;
        for (const GameplayPhysicsEvents::Trigger& trigger : events.triggers) {
            const Entity sensor = scene.findEntity(trigger.sensor);
            const Entity other = scene.findEntity(trigger.other);
            const auto notify = [&](Entity entity, Entity counterpart) {
                if (entity == entt::null) {
                    return;
                }
                forEntity(entity, [&](Behaviour& behaviour, BehaviourContext& context) {
                    if (trigger.entered) {
                        behaviour.onTriggerEnter(context, counterpart);
                    } else {
                        behaviour.onTriggerExit(context, counterpart);
                    }
                });
            };
            notify(sensor, other);
            notify(other, sensor);
        }
        for (const GameplayPhysicsEvents::Collision& collision : events.collisions) {
            const Entity first = scene.findEntity(collision.first);
            const Entity second = scene.findEntity(collision.second);
            if (first == entt::null || second == entt::null) {
                continue;
            }
            forEntity(first, [&](Behaviour& behaviour, BehaviourContext& context) {
                behaviour.onCollision(context,
                                      {.other = second, .otherId = collision.second, .normal = collision.normal});
            });
            forEntity(second, [&](Behaviour& behaviour, BehaviourContext& context) {
                behaviour.onCollision(context,
                                      {.other = first, .otherId = collision.first, .normal = -collision.normal});
            });
        }
    }

    void update(SystemPhase phase, const std::vector<Entry>& entries) const {
        for (const Entry& entry : entries) {
            Behaviour* behaviour = behaviourOf(m_context.scene, entry);
            if (behaviour == nullptr || !running(*behaviour)) {
                continue;
            }
            BehaviourContext context = contextFor(entry.entity);
            if (phase == SystemPhase::Update) {
                behaviour->onUpdate(context);
            } else if (phase == SystemPhase::FixedUpdate) {
                behaviour->onFixedUpdate(context);
            } else {
                behaviour->onLateUpdate(context);
            }
        }
    }

  private:
    const SystemContext& m_context;
};

void collectSubtree(const Scene& scene, Entity entity, std::vector<Entity>& entities) {
    entities.push_back(entity);
    if (const auto* relationship = scene.registry().try_get<RelationshipComponent>(entity)) {
        for (const Entity child : relationship->children) {
            if (scene.registry().valid(child)) {
                collectSubtree(scene, child, entities);
            }
        }
    }
}
} // namespace

void runBehaviours(SystemPhase phase, const SystemContext& context) {
    if (context.behaviours == nullptr || context.behaviours->empty() || phase == SystemPhase::Ui) {
        return;
    }
    const Dispatcher dispatcher(context);
    const std::vector<Entry> entries = gatherBehaviours(*context.behaviours, context.scene, context.mode);
    dispatcher.lifecycle(entries);
    if (phase == SystemPhase::FixedUpdate && context.services != nullptr) {
        dispatcher.physicsEvents(context.services->takePhysicsEvents());
    }
    dispatcher.update(phase, entries);
}

void destroyBehaviours(const BehaviourRegistry& behaviours, Scene& scene, Entity root, GameServices* services,
                       Logger* logger) {
    if (behaviours.empty() || !scene.registry().valid(root)) {
        return;
    }
    std::vector<Entity> entities;
    collectSubtree(scene, root, entities);
    for (const Entity entity : entities) {
        for (const BehaviourType& type : behaviours.types()) {
            Behaviour* behaviour = scene.registry().valid(entity) ? type.find(scene, entity) : nullptr;
            if (behaviour == nullptr || !behaviour->created()) {
                continue;
            }
            BehaviourContext context{.scene = scene, .entity = entity, .services = services, .logger = logger};
            if (BehaviourLifecycle::active(*behaviour)) {
                BehaviourLifecycle::setActive(*behaviour, false);
                behaviour->onDisable(context);
                behaviour = scene.registry().valid(entity) ? type.find(scene, entity) : nullptr;
            }
            if (behaviour != nullptr) {
                behaviour->onDestroy(context);
            }
        }
    }
}

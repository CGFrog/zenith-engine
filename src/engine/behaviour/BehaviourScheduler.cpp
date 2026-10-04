#include "BehaviourScheduler.hpp"

#include "engine/application/Application.hpp"

#include "engine/module/GameServices.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
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

    // After onDestroy at the end of a session: a later session creates and
    // starts it again.
    static void reset(Behaviour& behaviour) {
        behaviour.m_created = false;
        behaviour.m_started = false;
        behaviour.m_active = false;
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

void logHookError(Logger* logger, const std::string& message) {
    if (logger != nullptr) {
        logger->log(LogLevel::Error, "gameplay", message);
    } else {
        std::cerr << message << '\n';
    }
}

// Runs call, a hook of the behaviour of type on entity. A hook that throws is
// logged with the behaviour and its entity; with disableOnFailure the
// behaviour, if it still exists, is disabled so it stops running, and the
// lifecycle calls its onDisable next.
template <typename Call>
void callHook(Scene& scene, Logger* logger, const BehaviourType& type, Entity entity, std::string_view hook,
              bool disableOnFailure, const Call& call) {
    std::string what;
    try {
        call();
        return;
    } catch (const std::exception& error) {
        what = error.what();
    } catch (...) {
        what = "an unknown exception";
    }
    const bool exists = scene.registry().valid(entity);
    std::string message = "Behaviour '" + type.key + "'";
    if (exists) {
        message += " on entity " + std::to_string(scene.id(entity).value);
        if (const auto* name = scene.registry().try_get<NameComponent>(entity)) {
            message += " (" + name->name + ")";
        }
    }
    message += " threw in " + std::string(hook) + ": " + what;
    Behaviour* behaviour = exists ? type.find(scene, entity) : nullptr;
    if (disableOnFailure && behaviour != nullptr) {
        behaviour->enabled = false;
        message += "; it is disabled";
    }
    logHookError(logger, message);
}

// Calls onDisable (when active) and onDestroy on the created behaviour of
// type on entity, as when it is destroyed.
void endBehaviour(const BehaviourType& type, Scene& scene, Entity entity, GameServices* services, Logger* logger) {
    Behaviour* behaviour = scene.registry().valid(entity) ? type.find(scene, entity) : nullptr;
    if (behaviour == nullptr || !behaviour->created()) {
        return;
    }
    BehaviourContext context{.scene = scene, .entity = entity, .services = services, .logger = logger};
    if (BehaviourLifecycle::active(*behaviour)) {
        BehaviourLifecycle::setActive(*behaviour, false);
        callHook(scene, logger, type, entity, "onDisable", false, [&] { behaviour->onDisable(context); });
        behaviour = scene.registry().valid(entity) ? type.find(scene, entity) : nullptr;
    }
    if (behaviour != nullptr) {
        callHook(scene, logger, type, entity, "onDestroy", false, [&] { behaviour->onDestroy(context); });
    }
}

class Dispatcher {
  public:
    explicit Dispatcher(const SystemContext& context)
        : m_context(context) {}

    BehaviourContext contextFor(Entity entity) const {
        return {.scene = m_context.scene,
                .entity = entity,
                .deltaSeconds = m_context.deltaSeconds,
                .unscaledDeltaSeconds = m_context.unscaledDeltaSeconds,
                .services = m_context.services,
                .actions = m_context.actions,
                .logger = m_context.logger,
                .physics = m_context.application != nullptr ? &m_context.application->physics : nullptr};
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
                call(entry, "onCreate", [&] { behaviour->onCreate(context); });
                behaviour = behaviourOf(m_context.scene, entry);
            }
            if (behaviour != nullptr && behaviour->enabled != BehaviourLifecycle::active(*behaviour)) {
                BehaviourLifecycle::setActive(*behaviour, behaviour->enabled);
                if (behaviour->enabled) {
                    call(entry, "onEnable", [&] { behaviour->onEnable(context); });
                } else {
                    call(entry, "onDisable", [&] { behaviour->onDisable(context); });
                }
            }
        }
        for (const Entry& entry : entries) {
            Behaviour* behaviour = behaviourOf(m_context.scene, entry);
            if (behaviour != nullptr && running(*behaviour) && !behaviour->started()) {
                BehaviourLifecycle::start(*behaviour);
                BehaviourContext context = contextFor(entry.entity);
                call(entry, "onStart", [&] { behaviour->onStart(context); });
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
                hook(*behaviour, context, Entry{&type, entity});
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
                forEntity(entity, [&](Behaviour& behaviour, BehaviourContext& context, const Entry& entry) {
                    if (trigger.entered) {
                        call(entry, "onTriggerEnter", [&] { behaviour.onTriggerEnter(context, counterpart); });
                    } else {
                        call(entry, "onTriggerExit", [&] { behaviour.onTriggerExit(context, counterpart); });
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
            forEntity(first, [&](Behaviour& behaviour, BehaviourContext& context, const Entry& entry) {
                call(entry, "onCollision", [&] {
                    behaviour.onCollision(context,
                                          {.other = second, .otherId = collision.second, .normal = collision.normal});
                });
            });
            forEntity(second, [&](Behaviour& behaviour, BehaviourContext& context, const Entry& entry) {
                call(entry, "onCollision", [&] {
                    behaviour.onCollision(context,
                                          {.other = first, .otherId = collision.first, .normal = -collision.normal});
                });
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
                call(entry, "onUpdate", [&] { behaviour->onUpdate(context); });
            } else if (phase == SystemPhase::FixedUpdate) {
                call(entry, "onFixedUpdate", [&] { behaviour->onFixedUpdate(context); });
            } else {
                call(entry, "onLateUpdate", [&] { behaviour->onLateUpdate(context); });
            }
        }
    }

  private:
    // Runs a hook of entry's behaviour, disabling the behaviour if it throws.
    template <typename Call> void call(const Entry& entry, std::string_view hook, const Call& hookCall) const {
        callHook(m_context.scene, m_context.logger, *entry.type, entry.entity, hook, true, hookCall);
    }

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
            endBehaviour(type, scene, entity, services, logger);
        }
    }
}

void destroyAllBehaviours(const BehaviourRegistry& behaviours, Scene& scene, GameServices* services, Logger* logger) {
    std::vector<Entity> entities;
    for (const BehaviourType& type : behaviours.types()) {
        entities.clear();
        type.collect(scene, entities);
        std::ranges::sort(entities, {}, [&scene](Entity entity) { return scene.id(entity).value; });
        for (const Entity entity : entities) {
            endBehaviour(type, scene, entity, services, logger);
            if (Behaviour* behaviour = scene.registry().valid(entity) ? type.find(scene, entity) : nullptr) {
                BehaviourLifecycle::reset(*behaviour);
            }
        }
    }
}

#include "SceneVisibility.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>

namespace {
uint8_t flagBit(CollectionFlag flag) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(flag));
}

constexpr std::array<CollectionFlag, 3> allFlags{CollectionFlag::Visible, CollectionFlag::Selectable,
                                                 CollectionFlag::Renderable};

const VisibilityComponent& visibilityOf(const Scene& scene, Entity entity) {
    static constexpr VisibilityComponent defaults{};
    const auto* visibility = scene.registry().try_get<VisibilityComponent>(entity);
    return visibility == nullptr ? defaults : *visibility;
}
} // namespace

SceneVisibility::SceneVisibility(const Scene& scene, SceneVisibilityMode mode)
    : m_scene(scene)
    , m_mode(mode) {
    const SceneCollections& collections = scene.collections();
    m_allowed.reserve(collections.all().size());
    for (const SceneCollection& collection : collections.all()) {
        uint8_t allowed = 0;
        for (const CollectionFlag flag : allFlags) {
            if (collections.effectiveFlag(collection.id, flag)) {
                allowed |= flagBit(flag);
            }
        }
        m_allowed.emplace(collection.id.value, allowed);
    }
}

bool SceneVisibility::collectionsAllow(Entity entity, CollectionFlag flag) const {
    const auto* membership = m_scene.registry().try_get<CollectionMembershipComponent>(entity);
    if (membership == nullptr) {
        return true;
    }
    // Documents reject unknown collection ids, but runtime edits may leave
    // one behind; such ids are ignored, as if the entity were not a member.
    bool member = false;
    for (const SceneCollectionId id : membership->collections) {
        const auto found = m_allowed.find(id.value);
        if (found == m_allowed.end()) {
            continue;
        }
        if ((found->second & flagBit(flag)) != 0) {
            return true;
        }
        member = true;
    }
    return !member;
}

bool SceneVisibility::drawn(Entity entity) const {
    if (!drawnInInstance(entity)) {
        return false;
    }
    if (m_mode == SceneVisibilityMode::Runtime) {
        return true;
    }
    return !visibilityOf(m_scene, entity).hidden && collectionsAllow(entity, CollectionFlag::Visible);
}

bool SceneVisibility::drawnInInstance(Entity entity) const {
    return visibilityOf(m_scene, entity).renderable && collectionsAllow(entity, CollectionFlag::Renderable);
}

bool SceneVisibility::selectable(Entity entity) const {
    return drawn(entity) && visibilityOf(m_scene, entity).selectable &&
           collectionsAllow(entity, CollectionFlag::Selectable);
}

namespace {
struct StableEntity {
    uint64_t id{0};
    Entity entity{entt::null};
};

// Expands instances depth-first. Members of a collection include the members
// of every collection nested in it, each listed once.
class InstanceExpander {
  public:
    InstanceExpander(const Scene& scene, const SceneVisibility& visibility, std::vector<CollectionInstanceDraw>& draws)
        : m_scene(scene)
        , m_draws(draws) {
        const entt::registry& registry = scene.registry();
        for (const auto [entity, membership] : registry.view<const CollectionMembershipComponent>().each()) {
            if (!visibility.drawnInInstance(entity)) {
                continue;
            }
            for (const SceneCollectionId id : membership.collections) {
                m_directMembers[id.value].push_back({scene.id(entity).value, entity});
            }
        }
    }

    void expand(Entity instancer, const CollectionInstanceComponent& instance, const glm::mat4& instancerWorld) {
        m_stack.clear();
        expandCollection(instancer, instance.collection, instancerWorld, 1);
    }

  private:
    // Members of the collection and its nested collections, by stable id.
    const std::vector<StableEntity>& membersOf(SceneCollectionId collection) {
        const auto cached = m_subtreeMembers.find(collection.value);
        if (cached != m_subtreeMembers.end()) {
            return cached->second;
        }
        std::vector<StableEntity> members;
        for (const SceneCollectionId nested : m_scene.collections().subtree(collection)) {
            if (const auto direct = m_directMembers.find(nested.value); direct != m_directMembers.end()) {
                members.insert(members.end(), direct->second.begin(), direct->second.end());
            }
        }
        std::ranges::sort(members, {}, &StableEntity::id);
        const auto duplicates = std::ranges::unique(members, {}, &StableEntity::id);
        members.erase(duplicates.begin(), duplicates.end());
        return m_subtreeMembers.emplace(collection.value, std::move(members)).first->second;
    }

    void expandCollection(Entity instancer, SceneCollectionId collection, const glm::mat4& placement, uint32_t depth) {
        const SceneCollection* definition = m_scene.collections().find(collection);
        if (definition == nullptr || depth > maximumCollectionInstanceDepth ||
            std::ranges::find(m_stack, collection) != m_stack.end()) {
            return;
        }
        m_stack.push_back(collection);
        const glm::mat4 origin = placement * glm::translate(glm::mat4(1.0f), -definition->instanceOffset);
        // Copied: expanding a nested instance may add to the member cache.
        const std::vector<StableEntity> members = membersOf(collection);
        const entt::registry& registry = m_scene.registry();
        for (const StableEntity& member : members) {
            const auto* transform = registry.try_get<TransformComponent>(member.entity);
            if (transform == nullptr) {
                continue;
            }
            const glm::mat4 world = origin * transform->world;
            m_draws.push_back({.member = member.entity, .instancer = instancer, .world = world});
            if (const auto* nested = registry.try_get<CollectionInstanceComponent>(member.entity)) {
                expandCollection(instancer, nested->collection, world, depth + 1);
            }
        }
        m_stack.pop_back();
    }

    const Scene& m_scene;
    std::vector<CollectionInstanceDraw>& m_draws;
    std::unordered_map<uint64_t, std::vector<StableEntity>> m_directMembers;
    std::unordered_map<uint64_t, std::vector<StableEntity>> m_subtreeMembers;
    std::vector<SceneCollectionId> m_stack;
};
} // namespace

void expandCollectionInstances(const Scene& scene, const SceneVisibility& visibility,
                               std::vector<CollectionInstanceDraw>& draws) {
    const entt::registry& registry = scene.registry();
    const auto instances = registry.view<const CollectionInstanceComponent, const TransformComponent>();
    std::vector<StableEntity> instancers;
    for (const Entity entity : instances) {
        if (visibility.drawn(entity)) {
            instancers.push_back({scene.id(entity).value, entity});
        }
    }
    if (instancers.empty()) {
        return;
    }
    std::ranges::sort(instancers, {}, &StableEntity::id);
    InstanceExpander expander(scene, visibility, draws);
    for (const StableEntity& instancer : instancers) {
        expander.expand(instancer.entity, instances.get<const CollectionInstanceComponent>(instancer.entity),
                        instances.get<const TransformComponent>(instancer.entity).world);
    }
}

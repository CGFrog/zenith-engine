// Prefab instance queries: the instance an entity belongs to, its members,
// the instances of a prefab, and unpacking an instance.
#include "PrefabSystemInternal.hpp"

using namespace prefab_detail;

Entity prefabInstanceRoot(const Scene& scene, Entity entity) {
    for (Entity current = entity; current != entt::null && scene.registry().valid(current);
         current = parentOf(scene, current)) {
        if (scene.registry().all_of<PrefabInstanceComponent>(current)) {
            return current;
        }
    }
    return entt::null;
}

std::vector<Entity> prefabInstanceMembers(const Scene& scene, Entity root, std::vector<Entity>* nestedRoots) {
    std::vector<Entity> members;
    std::vector<Entity> pending{root};
    while (!pending.empty()) {
        const Entity current = pending.back();
        pending.pop_back();
        if (current != root && scene.registry().all_of<PrefabInstanceComponent>(current)) {
            if (nestedRoots != nullptr) {
                nestedRoots->push_back(current);
            }
            continue;
        }
        members.push_back(current);
        const auto& children = scene.registry().get<RelationshipComponent>(current).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return members;
}

std::vector<Entity> prefabInstancesOf(const Scene& scene, const std::filesystem::path& reference) {
    const std::string wanted = normalizedReference(reference);
    std::vector<Entity> roots;
    const auto view = scene.registry().view<const PrefabInstanceComponent>();
    for (const Entity entity : view) {
        if (normalizedReference(view.get<const PrefabInstanceComponent>(entity).prefab) == wanted) {
            roots.push_back(entity);
        }
    }
    std::ranges::sort(roots, {}, [&scene](Entity entity) { return scene.id(entity).value; });
    return roots;
}

void unpackPrefabInstance(Scene& scene, Entity root) {
    std::vector<Entity> nestedRoots;
    for (const Entity member : prefabInstanceMembers(scene, root, &nestedRoots)) {
        scene.registry().remove<PrefabEntityComponent>(member);
    }
    scene.registry().remove<PrefabInstanceComponent>(root);
    for (const Entity nested : nestedRoots) {
        scene.registry().get<PrefabInstanceComponent>(nested).nestedId = 0;
    }
}

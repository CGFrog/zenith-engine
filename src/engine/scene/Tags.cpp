#include "Tags.hpp"

#include <algorithm>
#include <utility>

bool TagsComponent::has(std::string_view tag) const {
    return std::ranges::find(tags, tag) != tags.end();
}

bool TagsComponent::add(std::string tag) {
    if (has(tag)) {
        return false;
    }
    tags.push_back(std::move(tag));
    return true;
}

bool TagsComponent::remove(std::string_view tag) {
    const auto found = std::ranges::find(tags, tag);
    if (found == tags.end()) {
        return false;
    }
    tags.erase(found);
    return true;
}

bool hasTag(const Scene& scene, Entity entity, std::string_view tag) {
    const auto* tags = scene.registry().valid(entity) ? scene.registry().try_get<TagsComponent>(entity) : nullptr;
    return tags != nullptr && tags->has(tag);
}

std::vector<Entity> findEntitiesWithTag(const Scene& scene, std::string_view tag) {
    std::vector<Entity> found;
    for (const auto [entity, tags] : scene.registry().view<const TagsComponent>().each()) {
        if (tags.has(tag)) {
            found.push_back(entity);
        }
    }
    std::ranges::sort(found,
                      [&scene](Entity first, Entity second) { return scene.id(first).value < scene.id(second).value; });
    return found;
}

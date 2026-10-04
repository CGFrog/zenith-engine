#include "BehaviourRegistry.hpp"

#include <algorithm>
#include <utility>

SystemRegistryResult BehaviourRegistry::add(BehaviourType type) {
    if (type.key.empty()) {
        return {.error = "A behaviour type needs a key"};
    }
    if (!type.collect || !type.find) {
        return {.error = "Behaviour type '" + type.key + "' has no functions"};
    }
    if (find(type.key) != nullptr) {
        return {.error = "Behaviour type '" + type.key + "' is already registered"};
    }
    m_types.push_back(std::move(type));
    return {.success = true};
}

const BehaviourType* BehaviourRegistry::find(std::string_view key) const {
    const auto found = std::ranges::find(m_types, key, &BehaviourType::key);
    return found != m_types.end() ? &*found : nullptr;
}

std::span<const BehaviourType> BehaviourRegistry::types() const {
    return m_types;
}

bool BehaviourRegistry::empty() const {
    return m_types.empty();
}

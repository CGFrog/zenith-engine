#include "ModuleRegistrar.hpp"

#include <utility>

ModuleRegistrar::ModuleRegistrar(ComponentTypeRegistry& componentTypes, SystemRegistry& systems,
                                 InputActionMap& inputActions)
    : m_componentTypes(componentTypes)
    , m_systems(systems)
    , m_inputActions(inputActions) {}

SceneDocumentResult ModuleRegistrar::addComponentType(ComponentType type) {
    std::string key = type.key();
    SceneDocumentResult result = m_componentTypes.add(std::move(type));
    if (result) {
        m_componentKeys.push_back(std::move(key));
    } else {
        recordFailure(result.error);
    }
    return result;
}

SystemRegistryResult ModuleRegistrar::addSystem(SystemDescriptor system) {
    std::string name = system.name;
    SystemRegistryResult result = m_systems.add(std::move(system));
    if (result) {
        m_systemNames.push_back(std::move(name));
    } else {
        recordFailure(result.error);
    }
    return result;
}

bool ModuleRegistrar::addInputActions(const InputActionMap& actions) {
    std::string error;
    if (!m_inputActions.merge(actions, error)) {
        recordFailure(error);
        return false;
    }
    return true;
}

const std::vector<std::string>& ModuleRegistrar::componentKeys() const {
    return m_componentKeys;
}

const std::vector<std::string>& ModuleRegistrar::systemNames() const {
    return m_systemNames;
}

const std::string& ModuleRegistrar::firstError() const {
    return m_firstError;
}

void ModuleRegistrar::recordFailure(const std::string& error) {
    if (m_firstError.empty()) {
        m_firstError = error;
    }
}

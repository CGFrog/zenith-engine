#include "ModuleRegistrar.hpp"

#include <utility>

ModuleRegistrar::ModuleRegistrar(ComponentTypeRegistry& componentTypes, SystemRegistry& systems,
                                 InputActionMap& inputActions, BehaviourRegistry& behaviours,
                                 DefinitionSchemaRegistry* definitionSchemas)
    : m_componentTypes(componentTypes)
    , m_systems(systems)
    , m_inputActions(inputActions)
    , m_behaviours(behaviours)
    , m_definitionSchemas(definitionSchemas) {}

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

bool ModuleRegistrar::addDefinitionSchema(DefinitionSchema schema) {
    if (m_definitionSchemas == nullptr) {
        return true;
    }
    if (DefinitionTableResult result = m_definitionSchemas->add(std::move(schema)); !result) {
        recordFailure(result.error);
        return false;
    }
    return true;
}

bool ModuleRegistrar::addBehaviourType(ComponentType type, BehaviourType behaviour) {
    if (!addComponentType(std::move(type))) {
        return false;
    }
    std::string key = behaviour.key;
    if (SystemRegistryResult result = m_behaviours.add(std::move(behaviour)); !result) {
        recordFailure(result.error);
        return false;
    }
    m_behaviourKeys.push_back(std::move(key));
    return true;
}

const std::vector<std::string>& ModuleRegistrar::componentKeys() const {
    return m_componentKeys;
}

const std::vector<std::string>& ModuleRegistrar::systemNames() const {
    return m_systemNames;
}

const std::vector<std::string>& ModuleRegistrar::behaviourKeys() const {
    return m_behaviourKeys;
}

const std::string& ModuleRegistrar::firstError() const {
    return m_firstError;
}

void ModuleRegistrar::recordFailure(const std::string& error) {
    if (m_firstError.empty()) {
        m_firstError = error;
    }
}

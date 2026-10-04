#pragma once

#include "SystemScheduler.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <string>
#include <vector>

// What a game module's zenith_module_register receives: the host's component
// type and system registries and input action map, which the module extends. The registrar records
// what the module added and the first failure, which fails the module's load
// even if the module ignored the returned result.
//
// A registrar only lives for the duration of the registration call; a module
// must not keep a reference to it.
class ModuleRegistrar {
  public:
    ModuleRegistrar(ComponentTypeRegistry& componentTypes, SystemRegistry& systems, InputActionMap& inputActions);

    ModuleRegistrar(const ModuleRegistrar&) = delete;
    ModuleRegistrar& operator=(const ModuleRegistrar&) = delete;

    SceneDocumentResult addComponentType(ComponentType type);
    SystemRegistryResult addSystem(SystemDescriptor system);
    // Declares the game's input actions and their default bindings. The host
    // evaluates them once per frame (and per fixed step) and hands systems
    // the result as SystemContext::actions. An action declared twice fails.
    // The map is copied: plain data, it holds no module code.
    bool addInputActions(const InputActionMap& actions);

    // Keys and names the module added, in registration order.
    const std::vector<std::string>& componentKeys() const;
    const std::vector<std::string>& systemNames() const;
    // Empty while every registration succeeded.
    const std::string& firstError() const;

  private:
    ComponentTypeRegistry& m_componentTypes;
    SystemRegistry& m_systems;
    InputActionMap& m_inputActions;
    std::vector<std::string> m_componentKeys;
    std::vector<std::string> m_systemNames;
    std::string m_firstError;

    void recordFailure(const std::string& error);
};

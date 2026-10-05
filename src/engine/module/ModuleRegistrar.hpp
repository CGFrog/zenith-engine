#pragma once

#include "SystemScheduler.hpp"
#include "engine/behaviour/BehaviourRegistry.hpp"
#include "engine/gameplay/DefinitionSchemas.hpp"
#include "engine/input/InputActions.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <concepts>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

// How a behaviour type registers: the modes it runs in (Play, as for systems,
// unless it opts into Edit) and its category in the editor's Add Component.
struct BehaviourOptions {
    RunIn runIn{RunIn::Play};
    std::string category{"Behaviours"};
};

// What a game module's zenith_module_register receives: the host's component
// type, system, and behaviour registries and input action map, which the
// module extends. The registrar records
// what the module added and the first failure, which fails the module's load
// even if the module ignored the returned result.
//
// A registrar only lives for the duration of the registration call; a module
// must not keep a reference to it.
class ModuleRegistrar {
  public:
    // definitionSchemas receives the module's definition table schemas; null
    // when the host has no use for them (a packaged game), which makes
    // addDefinitionSchema accept and drop them.
    ModuleRegistrar(ComponentTypeRegistry& componentTypes, SystemRegistry& systems, InputActionMap& inputActions,
                    BehaviourRegistry& behaviours, DefinitionSchemaRegistry* definitionSchemas = nullptr);

    ModuleRegistrar(const ModuleRegistrar&) = delete;
    ModuleRegistrar& operator=(const ModuleRegistrar&) = delete;

    SceneDocumentResult addComponentType(ComponentType type);
    SystemRegistryResult addSystem(SystemDescriptor system);
    // Declares the game's input actions and their default bindings. The host
    // evaluates them once per frame (and per fixed step) and hands systems
    // the result as SystemContext::actions. An action declared twice fails.
    // The map is copied: plain data, it holds no module code.
    bool addInputActions(const InputActionMap& actions);

    // Registers Behaviour type T as a component type (key, displayName, and
    // its fields, after the "enabled" field every behaviour has) and as a
    // behaviour the engine runs. location records where the type was
    // registered, for the editor's Edit Script.
    template <typename T>
        requires std::derived_from<T, Behaviour> && std::default_initializable<T> && std::copy_constructible<T>
    bool addBehaviour(std::string key, std::string displayName, Fields<T> fields = {}, BehaviourOptions options = {},
                      std::source_location location = std::source_location::current()) {
        Fields<T> allFields;
        allFields.add("enabled", static_cast<bool T::*>(&Behaviour::enabled))
            .optional()
            .tooltip("Whether the behaviour's update, trigger, and collision hooks run");
        allFields.append(std::move(fields));
        ComponentType type = ComponentTypeBuilder<T>(key, std::move(displayName))
                                 .category(std::move(options.category))
                                 .fields(std::move(allFields))
                                 .build();
        BehaviourType behaviour{
            .key = std::move(key),
            .runIn = options.runIn,
            .source = {.file = location.file_name(), .line = location.line(), .function = location.function_name()},
            .collect =
                [](const Scene& scene, std::vector<Entity>& entities) {
                    for (const Entity entity : scene.registry().view<const T>()) {
                        entities.push_back(entity);
                    }
                },
            .find = [](Scene& scene, Entity entity) -> Behaviour* { return scene.registry().try_get<T>(entity); }};
        return addBehaviourType(std::move(type), std::move(behaviour));
    }

    // Declares the layout of the game's definition table files, so tools such
    // as the editor's asset browser can validate them without the game's
    // types (see DefinitionSchemas.hpp). A name registered twice or unusable
    // fields fail the registration.
    bool addDefinitionSchema(DefinitionSchema schema);

    // addDefinitionSchema for a DefinitionTable<Entry> with these fields.
    template <typename Entry> bool addDefinitionTable(std::string name, Fields<Entry> fields, std::string idKey = "id") {
        return addDefinitionSchema(makeDefinitionSchema<Entry>(std::move(name), std::move(fields), std::move(idKey)));
    }

    // Keys and names the module added, in registration order.
    const std::vector<std::string>& componentKeys() const;
    const std::vector<std::string>& systemNames() const;
    const std::vector<std::string>& behaviourKeys() const;
    // Empty while every registration succeeded.
    const std::string& firstError() const;

  private:
    ComponentTypeRegistry& m_componentTypes;
    SystemRegistry& m_systems;
    InputActionMap& m_inputActions;
    BehaviourRegistry& m_behaviours;
    DefinitionSchemaRegistry* m_definitionSchemas{nullptr};
    std::vector<std::string> m_componentKeys;
    std::vector<std::string> m_systemNames;
    std::vector<std::string> m_behaviourKeys;
    std::string m_firstError;

    // Adds the behaviour's component type, then the behaviour.
    bool addBehaviourType(ComponentType type, BehaviourType behaviour);
    void recordFailure(const std::string& error);
};

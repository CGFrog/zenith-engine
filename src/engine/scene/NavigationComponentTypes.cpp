// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added.

#include "NavigationComponentTypes.hpp"

#include "ComponentTypes.hpp"
#include "engine/navigation/NavAgent.hpp"

#include <string>

ComponentType navAgentComponentType() {
    using C = NavAgentComponent;
    return ComponentTypeBuilder<C>("nav_agent", "Navigation Agent")
        .category("Navigation")
        .fields(Fields<C>()
                    .add("speed", &C::speed)
                    .drag(0.05f, 0.0, 100.0)
                    .format("%.2f m/s")
                    .add("stopping_distance", &C::stoppingDistance)
                    .drag(0.01f, 0.0, 10.0)
                    .format("%.2f m")
                    .tooltip("How close to its destination the agent stops")
                    .add("radius", &C::radius)
                    .optional()
                    .drag(0.01f, 0.0, 5.0)
                    .format("%.2f m")
                    .tooltip("How much room the agent keeps from other agents"))
        .validate([](const C& agent) -> std::string {
            return agent.speed >= 0.0f && agent.stoppingDistance >= 0.0f && agent.radius >= 0.0f
                       ? std::string{}
                       : "navigation agent speed, stopping distance, and radius must not be negative";
        })
        .build();
}

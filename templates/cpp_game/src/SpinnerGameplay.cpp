#include "SpinnerGameplay.hpp"

#include "engine/module/ModuleRegistrar.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <string>

namespace {
ComponentType spinnerComponentType() {
    return ComponentTypeBuilder<SpinnerComponent>(std::string(SpinnerComponentKey), "Spinner")
        .category("Gameplay")
        .fields(Fields<SpinnerComponent>()
                    .add("degrees_per_second", &SpinnerComponent::degreesPerSecond)
                    .drag(1.0f, -720.0, 720.0)
                    .format("%.0f deg/s")
                    .tooltip("How fast the entity turns about its local up axis"))
        .build();
}

void spin(const SystemContext& context) {
    Scene& scene = context.scene;
    const auto spinners = scene.registry().view<const SpinnerComponent, TransformComponent>();
    for (const Entity entity : spinners) {
        const float degrees = spinners.get<const SpinnerComponent>(entity).degreesPerSecond * context.deltaSeconds;
        glm::mat4& local = spinners.get<TransformComponent>(entity).local;
        local = glm::rotate(local, glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
        scene.markTransformDirty(entity);
    }
}
} // namespace

void registerSpinnerGameplay(ModuleRegistrar& registrar) {
    registrar.addComponentType(spinnerComponentType());
    registrar.addSystem(
        {.name = std::string(SpinSystemName), .phase = SystemPhase::Update, .runIn = RunIn::Play, .run = spin});
}

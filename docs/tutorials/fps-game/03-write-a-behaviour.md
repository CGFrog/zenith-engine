# 3. Write a behaviour

[Back to the outline](README.md)

A **behaviour** is per-entity C++ logic: a class derived from `Behaviour`
with hooks (`onStart`, `onUpdate`, `onFixedUpdate`, `onLateUpdate`,
`onTriggerEnter`, `onCollision`, and others) and reflected fields. It is also
an ordinary component type, so scenes save it and the inspector edits it. Use
a **system** instead for logic over many entities at once
([Game modules](../../engine/modules.md)).

## Generate it

Make the targets slide from side to side. Choose **Module > New
Behaviour...** and enter:

- Name: `Slider`
- Fields: `distance` (float) and `speed` (float)

**Create** (`module.generate_code`) writes `src/Slider.hpp` and
`src/Slider.cpp` from the editor's snippets, adds `Slider.cpp` above
`# zenith:sources` in `src/CMakeLists.txt`, adds the include and
`registerSlider(registrar);` above the `zenith:` markers in
`src/GameModule.cpp`, opens `Slider.cpp` in your IDE, and starts a build.

## Write the logic

Give the fields defaults in `Slider.hpp`, and keep where the slide started:

```cpp
class Slider final : public Behaviour {
  public:
    float distance{2.0f};
    float speed{1.0f};

    void onStart(BehaviourContext& context) override;
    void onUpdate(BehaviourContext& context) override;

  private:
    glm::vec3 m_start{0.0f};
    float m_time{0.0f};
};
```

and move the entity in `Slider.cpp`:

```cpp
#include "engine/scene/Scene.hpp"

#include <cmath>

void Slider::onStart(BehaviourContext& context) {
    m_start = glm::vec3(context.scene.registry().get<TransformComponent>(context.entity).local[3]);
}

void Slider::onUpdate(BehaviourContext& context) {
    m_time += context.deltaSeconds;
    auto& transform = context.scene.registry().get<TransformComponent>(context.entity);
    const float offset = std::sin(m_time * speed) * distance;
    transform.local[3] = glm::vec4(m_start + glm::vec3(offset, 0.0f, 0.0f), 1.0f);
    context.scene.markTransformDirty(context.entity);
}
```

Change the targets' rigid bodies to **Kinematic** so physics follows the
moved transform.

## Build and add it

Press **Ctrl B**. After the reload, select a target, choose **Add Component >
Behaviours > Slider**, and press F5. If you build during Play, the status bar
says **Reload pending - stop to apply**; the new code loads on Stop.

Compiler errors appear in the Build panel and the Diagnostics panel; clicking
one opens the IDE at that line.

Next: [Input, layers, and tags](04-input-layers-and-tags.md).

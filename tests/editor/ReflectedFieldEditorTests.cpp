#include "HeadlessImGui.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>

#include <array>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

struct Sample {
    glm::vec3 offset{0.0f};
    float scale{1.0f};
    bool enabled{false};
};

// Editing several objects copies only the edited field, and of a vector only
// the edited element, to every object.
void testMultipleObjects() {
    const std::vector<FieldDescriptor> fields = std::move(Fields<Sample>()
                                                              .add("offset", &Sample::offset)
                                                              .add("scale", &Sample::scale)
                                                              .add("enabled", &Sample::enabled))
                                                    .build();
    Sample first{.offset = {1.0f, 2.0f, 3.0f}, .scale = 1.0f, .enabled = true};
    Sample second{.offset = {4.0f, 5.0f, 6.0f}, .scale = 2.0f, .enabled = false};
    std::array<void*, 2> objects{&first, &second};
    HeadlessImGuiContext imgui;
    const auto draw = [&] {
        HeadlessImGuiFrame frame("Multiple objects");
        return drawReflectedFields(objects, fields);
    };
    expect(!draw(), "drawing several objects without input should change nothing");
    const ImGuiWindow* window = HeadlessImGuiContext::findWindow("Multiple objects");
    expect(window != nullptr, "the test window should exist");
    if (window == nullptr) {
        return;
    }
    // The vector's elements are pushed by index, as DragScalarN does.
    ImGuiID elementY = HeadlessImGuiContext::itemId(*window, "offset", "Offset");
    const int index = 1;
    elementY = ImHashData(&index, sizeof(index), elementY);
    elementY = ImHashStr("", 0, elementY);
    HeadlessImGuiContext::activateNextFrame(elementY);
    draw();
    HeadlessImGuiContext::typeNextFrame("9");
    expect(draw(), "typing into an element should report a change");
    expect(first.offset == glm::vec3(1.0f, 9.0f, 3.0f) && second.offset == glm::vec3(4.0f, 9.0f, 6.0f),
           "only the edited element should be copied to every object");
    expect(first.scale == 1.0f && second.scale == 2.0f, "fields that were not edited should keep their values");

    // A mixed checkbox checks every object when clicked.
    HeadlessImGuiContext::activateNextFrame(HeadlessImGuiContext::itemId(*window, "enabled", "Enabled"));
    draw();
    expect(first.enabled && second.enabled, "clicking a mixed checkbox should check every object");
}
} // namespace

int main() {
    expect(matchesInspectorFilter("Shadow resolution", "RESOL"), "filters should ignore case");
    expect(!matchesInspectorFilter("Intensity", "range"), "filters should reject unrelated labels");
    expect(matchesInspectorFilter("Anything", ""), "an empty filter should match everything");

    // Every built-in component's fields draw without input and report no change.
    Scene scene;
    const Entity entity = scene.createEntity("All components");
    const std::shared_ptr<const ComponentTypeRegistry> types = builtInComponentTypes();
    {
        HeadlessFrame frame;
        for (const ComponentType& type : types->types()) {
            type.addDefault(scene, entity);
            void* component = type.find(scene, entity);
            ImGui::PushID(type.key().c_str());
            const bool changed = drawReflectedFields(component, type.fields());
            const bool filteredChanged =
                drawReflectedFields(component, type.fields(), ReflectedFieldEditorOptions{.filter = "shadow"});
            ImGui::PopID();
            expect(!changed && !filteredChanged, "drawing without input should not change a component");
        }
    }
    testMultipleObjects();
    if (failures != 0) {
        std::cerr << failures << " reflected field editor test(s) failed\n";
        return 1;
    }
    return 0;
}

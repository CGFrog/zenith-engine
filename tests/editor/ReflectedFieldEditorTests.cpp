#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "engine/scene/ComponentTypes.hpp"

#include <imgui.h>

#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// A headless ImGui frame: no renderer, just enough state for widgets to run.
class HeadlessFrame {
  public:
    HeadlessFrame() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        ImGui::Begin("Inspector test");
    }

    ~HeadlessFrame() {
        ImGui::End();
        ImGui::Render();
        ImGui::DestroyContext();
    }

    HeadlessFrame(const HeadlessFrame&) = delete;
    HeadlessFrame& operator=(const HeadlessFrame&) = delete;
};
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
    if (failures != 0) {
        std::cerr << failures << " reflected field editor test(s) failed\n";
        return 1;
    }
    return 0;
}

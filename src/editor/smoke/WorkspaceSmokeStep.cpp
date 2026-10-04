// Areas and workspaces: open a second Hierarchy, switch workspace and back,
// check the area survived, and restore the areas the step changed (a manual
// smoke run may use the user's workspaces.json; ctest gives it its own).

#include "editor/areas/Workspace.hpp"
#include "editor/smoke/EditorSmokeHooks.hpp"
#include "editor/smoke/EditorSmokeSteps.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace {

class WorkspaceSmokeStep final : public SmokeStep {
  public:
    std::string_view name() const override {
        return "workspaces";
    }

    SmokeStepResult update(SmokeStepContext& context, uint32_t stepFrame) override {
        EditorSmokeHooks& editor = context.editor;
        switch (stepFrame) {
        case 0:
            return openSecondHierarchy(editor);
        case 1: {
            // The workspace switch applies between frames.
            const std::string other = m_workspace == "Debug" ? "Layout" : "Debug";
            editor.invokeOperator("wm.workspace_activate", nlohmann::json{{"workspace", other}}.dump(),
                                  KeymapRegion::Global);
            return SmokeStepResult::running();
        }
        case 2:
            if (editor.activeWorkspace().name() == m_workspace) {
                return SmokeStepResult::failed("the workspace did not switch");
            }
            editor.invokeOperator("wm.workspace_activate", nlohmann::json{{"workspace", m_workspace}}.dump(),
                                  KeymapRegion::Global);
            return SmokeStepResult::running();
        default: {
            const AreaInstance* area = editor.activeWorkspace().find(m_area);
            if (editor.activeWorkspace().name() != m_workspace || area == nullptr || !area->visible) {
                return SmokeStepResult::failed("switching back did not restore the second Hierarchy");
            }
            editor.invokeOperator("wm.area_close", nlohmann::json{{"area", m_area}}.dump(), KeymapRegion::Global);
            if (m_showedHierarchy) {
                editor.invokeOperator("wm.toggle_panel", R"({"panel":"hierarchy"})", KeymapRegion::Global);
            }
            return SmokeStepResult::done();
        }
        }
    }

  private:
    static std::vector<std::string> hierarchyAreas(const Workspace& workspace) {
        std::vector<std::string> ids;
        for (const AreaInstance& area : workspace.areas()) {
            if (area.typeId == "hierarchy" && area.visible) {
                ids.push_back(area.id());
            }
        }
        return ids;
    }

    SmokeStepResult openSecondHierarchy(EditorSmokeHooks& editor) {
        m_workspace = editor.activeWorkspace().name();
        if (!editor.activeWorkspace().anyVisible("hierarchy")) {
            m_showedHierarchy = true;
            editor.invokeOperator("wm.toggle_panel", R"({"panel":"hierarchy"})", KeymapRegion::Global);
        }
        const std::vector<std::string> before = hierarchyAreas(editor.activeWorkspace());
        editor.invokeOperator("wm.area_new", R"({"type":"hierarchy"})", KeymapRegion::Global);
        for (const std::string& id : hierarchyAreas(editor.activeWorkspace())) {
            if (std::ranges::find(before, id) == before.end()) {
                m_area = id;
            }
        }
        if (m_area.empty()) {
            return SmokeStepResult::failed("wm.area_new did not open a second Hierarchy");
        }
        return SmokeStepResult::running();
    }

    // The workspace the step started in and the Hierarchy area it opened.
    std::string m_workspace;
    std::string m_area;
    bool m_showedHierarchy{false};
};

} // namespace

std::unique_ptr<SmokeStep> makeWorkspaceSmokeStep() {
    return std::make_unique<WorkspaceSmokeStep>();
}

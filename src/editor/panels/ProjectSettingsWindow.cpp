#include "ProjectSettingsWindow.hpp"

#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/input/InputActionsJson.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace {
constexpr const char* windowTitle = "Project Settings";
constexpr const char* projectActionsFileName = "input_actions.json";
// Static, Dynamic, and Player: engine code refers to them by index.
constexpr uint32_t builtInLayerCount = 3;

bool sameValues(const ProjectSettingsValues& first, const ProjectSettingsValues& second) {
    return first.layers == second.layers && first.tags == second.tags && first.navigation == second.navigation &&
           serializeInputActionMap(first.inputActions) == serializeInputActionMap(second.inputActions);
}

// Replaces action's definition, keeping its place among the others.
void setAction(InputActionMap& actions, const std::string& name, InputActionValueType type, bool clamp,
               std::vector<InputBinding> bindings) {
    actions.removeAction(name);
    actions.addAction(name, type, clamp);
    for (InputBinding& binding : bindings) {
        actions.addBinding(name, std::move(binding));
    }
}

// A copy of action from source as one map, to override with.
InputActionMap actionFrom(const InputActionMap& source, const std::string& name) {
    InputActionMap single;
    single.addAction(name, source.actionType(name).value_or(InputActionValueType::Button), source.clampsValue(name));
    for (const InputBinding& binding : source.bindings(name)) {
        single.addBinding(name, binding);
    }
    return single;
}

std::string uniqueLayerName(const PhysicsLayerTable& layers) {
    for (size_t number = layers.size();; ++number) {
        std::string name = "Layer " + std::to_string(number);
        if (!layers.find(name)) {
            return name;
        }
    }
}

bool drawSourceCombo(const char* label, InputBindingSource& source) {
    bool changed = false;
    const std::string current = inputSourceName(source);
    if (ImGui::BeginCombo(label, current.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const InputBindingSource& candidate : allInputSources()) {
            const std::string name = inputSourceName(candidate);
            if (ImGui::Selectable(name.c_str(), candidate == source)) {
                source = candidate;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace

ProjectConfigResult loadProjectSettingsValues(const std::filesystem::path& projectFile, ProjectSettingsValues& values) {
    ProjectConfig project;
    if (ProjectConfigResult loaded = loadProjectConfig(projectFile, project); !loaded) {
        return loaded;
    }
    ProjectSettingsValues read{
        .layers = project.physicsLayerTable(), .tags = project.tags, .navigation = project.navigationSettings()};
    if (ProjectConfigResult actions = loadProjectInputActions(project, read.inputActions); !actions) {
        return actions;
    }
    values = std::move(read);
    return {.success = true};
}

ProjectConfigResult saveProjectSettingsValues(const std::filesystem::path& projectFile,
                                              const ProjectSettingsValues& values) {
    if (const std::string problem = validateProjectSettings(values); !problem.empty()) {
        return {.error = problem};
    }
    ProjectConfig project;
    if (ProjectConfigResult loaded = loadProjectConfig(projectFile, project); !loaded) {
        return loaded;
    }
    project.physicsLayers = values.layers;
    project.tags = values.tags;
    if (project.navigation || values.navigation != NavMeshBuildSettings{}) {
        project.navigation = values.navigation;
    }
    if (project.inputActions.empty() && !values.inputActions.actionNames().empty()) {
        project.inputActions = projectFile.parent_path() / projectActionsFileName;
    }
    if (!project.inputActions.empty()) {
        if (const InputActionsJsonResult saved = saveInputActionMap(project.inputActions, values.inputActions);
            !saved) {
            return {.error = saved.error};
        }
    }
    return saveProjectConfig(projectFile, project);
}

std::string validateProjectSettings(const ProjectSettingsValues& values) {
    if (const std::string problem = values.layers.validate(); !problem.empty()) {
        return "Physics layers: " + problem;
    }
    if (const std::string problem = validateProjectTags(values.tags); !problem.empty()) {
        return "Tags: " + problem;
    }
    if (const std::string problem = validateNavMeshBuildSettings(values.navigation); !problem.empty()) {
        return "Navigation: " + problem;
    }
    return {};
}

std::optional<ProjectSettingsTab> projectSettingsTab(std::string_view key) {
    constexpr std::array keys{std::pair{"physics_layers", ProjectSettingsTab::PhysicsLayers},
                              std::pair{"input_actions", ProjectSettingsTab::InputActions},
                              std::pair{"tags", ProjectSettingsTab::Tags},
                              std::pair{"navigation", ProjectSettingsTab::Navigation}};
    for (const auto& [name, tab] : keys) {
        if (key == name) {
            return tab;
        }
    }
    return std::nullopt;
}

void ProjectSettingsWindow::open(ProjectSettingsValues values, InputActionMap moduleActions) {
    m_saved = values;
    m_draft = std::move(values);
    m_moduleActions = std::move(moduleActions);
    m_selectedAction.clear();
    m_error.clear();
    m_modified = false;
    m_open = true;
}

bool ProjectSettingsWindow::isOpen() const {
    return m_open;
}

void ProjectSettingsWindow::close() {
    m_open = false;
}

ProjectSettingsValues& ProjectSettingsWindow::draft() {
    return m_draft;
}

bool ProjectSettingsWindow::modified() const {
    return m_modified;
}

ProjectSettingsTab ProjectSettingsWindow::tab() const {
    return m_tab;
}

void ProjectSettingsWindow::setTab(ProjectSettingsTab tab) {
    m_requestedTab = tab;
    m_selectRequestedTab = true;
}

void ProjectSettingsWindow::finish(const std::string& error) {
    m_error = error;
    if (error.empty()) {
        m_saved = m_draft;
        m_modified = false;
    }
}

std::optional<ProjectSettingsValues> ProjectSettingsWindow::draw(bool canApply, const std::string& reason) {
    if (!m_open) {
        return std::nullopt;
    }
    ImGui::SetNextWindowSize({640.0f, 480.0f}, ImGuiCond_FirstUseEver);
    std::optional<ProjectSettingsValues> applied;
    if (ImGui::Begin(windowTitle, &m_open)) {
        applied = drawContents(canApply, reason);
    }
    ImGui::End();
    return applied;
}

std::optional<ProjectSettingsValues> ProjectSettingsWindow::drawContents(bool canApply, const std::string& reason) {
    constexpr std::array tabs{std::pair{ProjectSettingsTab::PhysicsLayers, "Physics Layers"},
                              std::pair{ProjectSettingsTab::InputActions, "Input Actions"},
                              std::pair{ProjectSettingsTab::Tags, "Tags"},
                              std::pair{ProjectSettingsTab::Navigation, "Navigation"}};
    if (ImGui::BeginTabBar("##ProjectSettingsTabs")) {
        for (const auto& [tab, label] : tabs) {
            const bool select = m_selectRequestedTab && m_requestedTab == tab;
            if (ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0)) {
                m_tab = tab;
                switch (tab) {
                case ProjectSettingsTab::PhysicsLayers:
                    drawPhysicsLayers();
                    break;
                case ProjectSettingsTab::InputActions:
                    drawInputActions();
                    break;
                case ProjectSettingsTab::Tags:
                    drawTags();
                    break;
                case ProjectSettingsTab::Navigation:
                    drawNavigation();
                    break;
                }
                ImGui::EndTabItem();
            }
        }
        m_selectRequestedTab = false;
        ImGui::EndTabBar();
    }
    m_modified = !sameValues(m_draft, m_saved);

    ImGui::Separator();
    const std::string problem = validateProjectSettings(m_draft);
    if (!problem.empty()) {
        widgets::CopyableText(problem, palette::warning);
    }
    if (!m_error.empty()) {
        widgets::CopyableText(m_error, palette::error);
    }
    std::optional<ProjectSettingsValues> applied;
    const bool applicable = canApply && problem.empty() && m_modified;
    ImGui::BeginDisabled(!applicable);
    // Checked again: keyboard navigation can activate a disabled button.
    if (widgets::Button("Apply") && applicable) {
        applied = m_draft;
    }
    ImGui::EndDisabled();
    if (!canApply && !reason.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", reason.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_modified);
    if (widgets::Button("Revert")) {
        m_draft = m_saved;
        m_error.clear();
    }
    ImGui::EndDisabled();
    return applied;
}

void ProjectSettingsWindow::drawPhysicsLayers() {
    PhysicsLayerTable& layers = m_draft.layers;
    ImGui::TextWrapped("Bodies name their layer by index, so layers are added and removed at the end. Two layers "
                       "collide when their box is checked.");
    const uint32_t count = static_cast<uint32_t>(layers.size());
    for (uint32_t index = 0; index < count; ++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%2u", index);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::BeginDisabled(index < builtInLayerCount);
        ImGui::InputText("##name", &layers.names[index]);
        ImGui::EndDisabled();
        if (index < builtInLayerCount) {
            ImGui::SetItemTooltip("A built-in layer engine code refers to");
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(count >= maxPhysicsLayers);
    if (widgets::Button("Add Layer")) {
        layers.add(uniqueLayerName(layers));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(count <= builtInLayerCount);
    if (widgets::Button("Remove Last Layer")) {
        const PhysicsLayer last{count - 1};
        for (uint32_t other = 0; other < count; ++other) {
            layers.setCollides(last, {other}, false);
        }
        layers.names.pop_back();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Collision Matrix");
    const uint32_t shown = static_cast<uint32_t>(layers.size());
    if (ImGui::BeginTable("##CollisionMatrix", static_cast<int>(shown) + 1,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("##Layer");
        for (uint32_t column = 0; column < shown; ++column) {
            ImGui::TableSetupColumn(layers.names[column].c_str());
        }
        ImGui::TableHeadersRow();
        for (uint32_t row = 0; row < shown; ++row) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(layers.names[row].c_str());
            // The matrix is symmetric, so only one triangle is shown.
            for (uint32_t column = 0; column < shown; ++column) {
                ImGui::TableNextColumn();
                if (column > row) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(row * maxPhysicsLayers + column));
                bool collide = layers.collides({row}, {column});
                if (ImGui::Checkbox("##collides", &collide)) {
                    layers.setCollides({row}, {column}, collide);
                }
                ImGui::SetItemTooltip("%s and %s", layers.names[row].c_str(), layers.names[column].c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void ProjectSettingsWindow::drawInputActions() {
    InputActionMap& actions = m_draft.inputActions;
    ImGui::TextWrapped("The project's actions replace the game module's defaults of the same name and add new "
                       "ones. They are saved to the project's input action file.");
    const std::vector<std::string> names = actions.actionNames();
    if (!names.empty() && std::ranges::find(names, m_selectedAction) == names.end()) {
        m_selectedAction = names.front();
    }
    if (ImGui::BeginChild("##Actions", {180.0f, 0.0f}, ImGuiChildFlags_Borders)) {
        for (const std::string& name : names) {
            if (ImGui::Selectable(name.c_str(), name == m_selectedAction)) {
                m_selectedAction = name;
            }
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##NewAction", "New action", &m_newActionName);
        if (widgets::Button("Add Action") && actions.addAction(m_newActionName, InputActionValueType::Button)) {
            m_selectedAction = std::exchange(m_newActionName, {});
        }
        std::vector<std::string> overridable;
        for (const std::string& name : m_moduleActions.actionNames()) {
            if (!actions.contains(name)) {
                overridable.push_back(name);
            }
        }
        if (!overridable.empty() && ImGui::BeginCombo("##Override", "Override...")) {
            for (const std::string& name : overridable) {
                if (ImGui::Selectable(name.c_str())) {
                    actions.overrideWith(actionFrom(m_moduleActions, name));
                    m_selectedAction = name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Copy one of the game module's actions here to change its bindings");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##ActionDetails")) {
        if (actions.contains(m_selectedAction)) {
            drawActionDetails(m_selectedAction);
        } else {
            ImGui::TextDisabled("No project actions yet. Add one, or override one of the game module's.");
        }
    }
    ImGui::EndChild();
}

void ProjectSettingsWindow::drawActionDetails(const std::string& action) {
    InputActionMap& actions = m_draft.inputActions;
    InputActionValueType type = actions.actionType(action).value_or(InputActionValueType::Button);
    bool clamp = actions.clampsValue(action);
    const std::span<const InputBinding> current = actions.bindings(action);
    std::vector<InputBinding> bindings(current.begin(), current.end());
    bool changed = false;

    ImGui::Text("%s", action.c_str());
    if (m_moduleActions.contains(action)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(overrides the game module's)");
    }
    const std::string typeName(inputActionTypeName(type));
    if (ImGui::BeginCombo("Type", typeName.c_str())) {
        for (const InputActionValueType candidate :
             {InputActionValueType::Button, InputActionValueType::Axis1D, InputActionValueType::Axis2D}) {
            const std::string name(inputActionTypeName(candidate));
            if (ImGui::Selectable(name.c_str(), candidate == type)) {
                type = candidate;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed = ImGui::Checkbox("Clamp", &clamp) || changed;
    ImGui::SetItemTooltip("Limit the combined value to -1..1 per axis");

    ImGui::SeparatorText("Bindings");
    std::optional<size_t> removed;
    for (size_t index = 0; index < bindings.size(); ++index) {
        InputBinding& binding = bindings[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(170.0f);
        changed = drawSourceCombo("##source", binding.source) || changed;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        changed = ImGui::DragFloat2("Scale", &binding.scale.x, 0.05f, -10.0f, 10.0f, "%.2f") || changed;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        changed = ImGui::DragFloat("Dead zone", &binding.deadZone, 0.01f, 0.0f, 0.95f, "%.2f") || changed;
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            removed = index;
        }
        std::string modifiers;
        for (const Key key : binding.modifiers) {
            modifiers += (modifiers.empty() ? "" : " + ") + inputSourceName(key);
        }
        ImGui::TextDisabled("Modifiers: %s", modifiers.empty() ? "none" : modifiers.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            ImGui::OpenPopup("##AddModifier");
        }
        if (ImGui::BeginPopup("##AddModifier")) {
            for (size_t key = 0; key < static_cast<size_t>(Key::Count); ++key) {
                const std::string name = inputSourceName(static_cast<Key>(key));
                if (ImGui::MenuItem(name.c_str())) {
                    binding.modifiers.push_back(static_cast<Key>(key));
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        if (!binding.modifiers.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                binding.modifiers.clear();
                changed = true;
            }
        }
        ImGui::PopID();
    }
    if (removed) {
        bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(*removed));
        changed = true;
    }
    if (widgets::Button("Add Binding")) {
        bindings.push_back({});
        changed = true;
    }
    ImGui::SameLine();
    if (widgets::Button("Remove Action")) {
        actions.removeAction(action);
        return;
    }
    if (changed) {
        setAction(actions, action, type, clamp, std::move(bindings));
    }
}

void ProjectSettingsWindow::drawNavigation() {
    NavMeshBuildSettings& settings = m_draft.navigation;
    ImGui::TextWrapped("How navigation meshes are built: the editor's Bake Navigation Mesh preview, and the game's "
                       "own bake when it plays (OutsideServices::setDefaultNavigationSettings).");

    struct Setting {
        const char* label;
        float NavMeshBuildSettings::*value;
        float speed;
        float minimum;
        float maximum;
        const char* format;
        const char* tooltip;
    };

    constexpr std::array agent{Setting{"Agent height", &NavMeshBuildSettings::agentHeight, 0.01f, 0.01f, 100.0f,
                                       "%.2f m", "The clearance an agent needs overhead"},
                               Setting{"Agent radius", &NavMeshBuildSettings::agentRadius, 0.01f, 0.0f, 100.0f,
                                       "%.2f m", "How far the mesh keeps from walls"},
                               Setting{"Agent max climb", &NavMeshBuildSettings::agentMaxClimb, 0.01f, 0.0f, 100.0f,
                                       "%.2f m", "The tallest step an agent walks up"},
                               Setting{"Agent max slope", &NavMeshBuildSettings::agentMaxSlopeDegrees, 0.5f, 0.0f,
                                       89.9f, "%.1f deg", "The steepest ground an agent walks on"}};
    constexpr std::array detail{
        Setting{"Cell size", &NavMeshBuildSettings::cellSize, 0.005f, 0.01f, 10.0f, "%.3f m",
                "Horizontal voxel size; smaller follows the geometry closer and bakes slower"},
        Setting{"Cell height", &NavMeshBuildSettings::cellHeight, 0.005f, 0.01f, 10.0f, "%.3f m",
                "Vertical voxel size"},
        Setting{"Region minimum area", &NavMeshBuildSettings::regionMinimumArea, 0.5f, 0.0f, 10000.0f, "%.0f cells",
                "Smaller islands are dropped"},
        Setting{"Region merge area", &NavMeshBuildSettings::regionMergeArea, 0.5f, 0.0f, 10000.0f, "%.0f cells",
                "Smaller regions merge into their neighbours"},
        Setting{"Edge max length", &NavMeshBuildSettings::edgeMaxLength, 0.1f, 0.0f, 1000.0f, "%.1f m", nullptr},
        Setting{"Edge max error", &NavMeshBuildSettings::edgeMaxError, 0.01f, 0.01f, 100.0f, "%.2f", nullptr},
        Setting{"Detail sample distance", &NavMeshBuildSettings::detailSampleDistance, 0.1f, 0.0f, 100.0f, "%.1f",
                nullptr},
        Setting{"Detail sample max error", &NavMeshBuildSettings::detailSampleMaxError, 0.1f, 0.0f, 100.0f, "%.1f",
                nullptr}};
    const auto drawSettings = [&settings](std::span<const Setting> group) {
        for (const Setting& setting : group) {
            ImGui::SetNextItemWidth(160.0f);
            ImGui::DragFloat(setting.label, &(settings.*setting.value), setting.speed, setting.minimum, setting.maximum,
                             setting.format, ImGuiSliderFlags_AlwaysClamp);
            if (setting.tooltip != nullptr) {
                ImGui::SetItemTooltip("%s", setting.tooltip);
            }
        }
    };
    ImGui::SeparatorText("Agent");
    drawSettings(agent);
    ImGui::SeparatorText("Walkable Layers");
    ImGui::TextDisabled("Static colliders on these layers are walked on.");
    for (uint32_t layer = 0; layer < m_draft.layers.size(); ++layer) {
        ImGui::PushID(static_cast<int>(layer));
        bool walkable = (settings.layerMask & (1u << layer)) != 0;
        if (ImGui::Checkbox(m_draft.layers.names[layer].c_str(), &walkable)) {
            settings.layerMask = walkable ? settings.layerMask | (1u << layer) : settings.layerMask & ~(1u << layer);
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("Mesh Detail");
    drawSettings(detail);
    if (widgets::Button("Reset to Defaults")) {
        settings = NavMeshBuildSettings{};
    }
}

void ProjectSettingsWindow::drawTags() {
    std::vector<std::string>& tags = m_draft.tags;
    ImGui::TextWrapped("Tags the inspector offers for an entity's Tags component. Game code finds entities by tag "
                       "with findEntitiesWithTag.");
    std::optional<size_t> removed;
    for (size_t index = 0; index < tags.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText("##tag", &tags[index]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(*removed));
    }
    ImGui::SetNextItemWidth(220.0f);
    const bool entered =
        ImGui::InputTextWithHint("##NewTag", "New tag", &m_newTag, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((widgets::Button("Add Tag") || entered) && !m_newTag.empty()) {
        tags.push_back(std::exchange(m_newTag, {}));
    }
}

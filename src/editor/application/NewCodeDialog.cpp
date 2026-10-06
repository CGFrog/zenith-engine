#include "NewCodeDialog.hpp"

#include "editor/operators/Operator.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <array>

namespace {
constexpr const char* popupId = "###NewCodeDialog";

const char* kindTitle(GeneratedCodeKind kind) {
    switch (kind) {
    case GeneratedCodeKind::Behaviour:
        return "New Behaviour";
    case GeneratedCodeKind::Component:
        return "New Component";
    case GeneratedCodeKind::System:
        return "New System";
    }
    return "New";
}

const char* kindHint(GeneratedCodeKind kind) {
    switch (kind) {
    case GeneratedCodeKind::Behaviour:
        return "Per-entity logic with hooks (onStart, onUpdate, triggers, ...), added to entities like a component.";
    case GeneratedCodeKind::Component:
        return "Data on an entity that systems read and write.";
    case GeneratedCodeKind::System:
        return "Logic that runs once per phase over the whole scene.";
    }
    return "";
}

constexpr std::array fieldTypes{GeneratedFieldType::Float, GeneratedFieldType::Int,    GeneratedFieldType::Bool,
                                GeneratedFieldType::Vec3,  GeneratedFieldType::String, GeneratedFieldType::Entity};
} // namespace

void NewCodeDialog::open(GeneratedCodeKind kind) {
    m_kind = kind;
    m_name.clear();
    m_category.clear();
    m_phase = SystemPhase::Update;
    m_fields.clear();
    m_failure.clear();
    m_open = true;
    m_openPopup = true;
    m_focusName = true;
}

bool NewCodeDialog::isOpen() const {
    return m_open;
}

GenerateCodeProperties NewCodeDialog::properties() const {
    std::vector<GeneratedField> fields;
    for (const FieldRow& row : m_fields) {
        fields.push_back({.name = row.name, .type = row.type});
    }
    return {.kind = m_kind,
            .name = m_name,
            .category = m_category,
            .phase = m_phase,
            .fields = formatGeneratedFields(fields)};
}

std::string NewCodeDialog::problem() const {
    if (const GeneratedTypeName type = generatedTypeName(m_kind, m_name); !type.error.empty()) {
        return type.error;
    }
    for (const FieldRow& row : m_fields) {
        if (std::string error = validateGeneratedFieldName(row.name); !error.empty()) {
            return error;
        }
    }
    return {};
}

void NewCodeDialog::finish(const OperatorResult& result) {
    if (result) {
        m_open = false;
        m_failure.clear();
    } else {
        m_failure = result.message.empty() ? "The code was not generated." : result.message;
    }
}

std::optional<GenerateCodeProperties> NewCodeDialog::draw() {
    if (!m_open) {
        return std::nullopt;
    }
    const std::string title = std::string(kindTitle(m_kind)) + popupId;
    if (std::exchange(m_openPopup, false)) {
        ImGui::OpenPopup(title.c_str());
    }
    ImGui::SetNextWindowSize({480.0f, 0.0f}, ImGuiCond_Appearing);
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &keepOpen)) {
        m_open = false;
        return std::nullopt;
    }
    std::optional<GenerateCodeProperties> create;
    ImGui::TextWrapped("%s", kindHint(m_kind));
    if (std::exchange(m_focusName, false)) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("Name", "Health, PlayerMovement, ...", &m_name);
    const GeneratedTypeName type = generatedTypeName(m_kind, m_name);
    if (!m_name.empty() && type.error.empty()) {
        ImGui::TextDisabled("Creates src/%s.hpp and src/%s.cpp", type.typeName.c_str(), type.typeName.c_str());
    }
    if (m_kind == GeneratedCodeKind::System) {
        const std::string current(systemPhaseName(m_phase));
        if (ImGui::BeginCombo("Phase", current.c_str())) {
            for (const SystemPhase phase : systemPhases) {
                const std::string name(systemPhaseName(phase));
                if (ImGui::Selectable(name.c_str(), phase == m_phase)) {
                    m_phase = phase;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::InputTextWithHint("Category", m_kind == GeneratedCodeKind::Behaviour ? "Behaviours" : "Gameplay",
                                 &m_category);
        ImGui::SetItemTooltip("Where Add Component lists it.");
        drawFields();
    }

    const std::string problem = this->problem();
    if (!problem.empty() && !m_name.empty()) {
        widgets::CopyableText(problem, palette::warning);
    }
    if (!m_failure.empty()) {
        widgets::CopyableText(m_failure, palette::error);
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!problem.empty());
    if (widgets::Button("Create") || (problem.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        create = properties();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (widgets::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        keepOpen = false;
    }
    if (!keepOpen) {
        m_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return create;
}

void NewCodeDialog::drawFields() {
    ImGui::SeparatorText("Fields");
    std::optional<size_t> removed;
    for (size_t index = 0; index < m_fields.size(); ++index) {
        FieldRow& row = m_fields[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##name", "fieldName", &row.name);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const std::string current(generatedFieldTypeName(row.type));
        if (ImGui::BeginCombo("##type", current.c_str())) {
            for (const GeneratedFieldType fieldType : fieldTypes) {
                const std::string name(generatedFieldTypeName(fieldType));
                if (ImGui::Selectable(name.c_str(), fieldType == row.type)) {
                    row.type = fieldType;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            removed = index;
        }
        ImGui::PopID();
    }
    if (removed) {
        m_fields.erase(m_fields.begin() + static_cast<std::ptrdiff_t>(*removed));
    }
    if (ImGui::SmallButton("Add Field")) {
        m_fields.push_back({});
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Reflected, so the inspector edits them and scenes save them.");
}

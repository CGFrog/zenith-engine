#include "ReflectedFieldEditor.hpp"

#include "editor/theme/EditorTheme.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <string>

namespace {
const ImVec4 problemColor = toImVec4(palette::error);

// ImGui treats equal drag bounds as unbounded; one-sided ranges are filled
// with the type's extreme so the given side still clamps.
struct FloatBounds {
    float minimum{0.0f};
    float maximum{0.0f};
    ImGuiSliderFlags flags{ImGuiSliderFlags_None};
};

FloatBounds floatBounds(const FieldPresentation& presentation) {
    if (!presentation.minimum && !presentation.maximum) {
        return {};
    }
    return {.minimum = static_cast<float>(presentation.minimum.value_or(-FLT_MAX)),
            .maximum = static_cast<float>(presentation.maximum.value_or(FLT_MAX)),
            .flags = ImGuiSliderFlags_AlwaysClamp};
}

struct IntBounds {
    int minimum{0};
    int maximum{0};
    ImGuiSliderFlags flags{ImGuiSliderFlags_None};
};

IntBounds intBounds(const FieldPresentation& presentation, int lowest) {
    if (!presentation.minimum && !presentation.maximum) {
        if (lowest == INT_MIN) {
            return {};
        }
        return {.minimum = lowest, .maximum = INT_MAX, .flags = ImGuiSliderFlags_AlwaysClamp};
    }
    const auto clampToInt = [](double value) {
        return static_cast<int>(std::clamp(value, static_cast<double>(INT_MIN), static_cast<double>(INT_MAX)));
    };
    return {.minimum = std::max(lowest, clampToInt(presentation.minimum.value_or(INT_MIN))),
            .maximum = clampToInt(presentation.maximum.value_or(INT_MAX)),
            .flags = ImGuiSliderFlags_AlwaysClamp};
}

const char* formatOr(const FieldPresentation& presentation, const char* fallback) {
    return presentation.format.empty() ? fallback : presentation.format.c_str();
}

bool drawFloat(const char* label, float& value, const FieldPresentation& presentation) {
    const FloatBounds bounds = floatBounds(presentation);
    if (presentation.angle) {
        float degrees = glm::degrees(value);
        const char* format = formatOr(presentation, "%.1f deg");
        const bool changed =
            presentation.widget == NumberWidget::Slider
                ? ImGui::SliderFloat(label, &degrees, bounds.minimum, bounds.maximum, format, bounds.flags)
                : ImGui::DragFloat(label, &degrees, presentation.speed, bounds.minimum, bounds.maximum, format,
                                   bounds.flags);
        if (changed) {
            value = glm::radians(degrees);
        }
        return changed;
    }
    const char* format = formatOr(presentation, "%.3f");
    if (presentation.widget == NumberWidget::Slider) {
        return ImGui::SliderFloat(label, &value, bounds.minimum, bounds.maximum, format, bounds.flags);
    }
    return ImGui::DragFloat(label, &value, presentation.speed, bounds.minimum, bounds.maximum, format, bounds.flags);
}

bool drawFloats(const char* label, float* values, int count, const FieldPresentation& presentation) {
    if (presentation.color) {
        constexpr ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
        return count == 3 ? ImGui::ColorEdit3(label, values, flags) : ImGui::ColorEdit4(label, values, flags);
    }
    const FloatBounds bounds = floatBounds(presentation);
    const char* format = formatOr(presentation, "%.3f");
    if (presentation.widget == NumberWidget::Slider) {
        return ImGui::SliderScalarN(label, ImGuiDataType_Float, values, count, &bounds.minimum, &bounds.maximum, format,
                                    bounds.flags);
    }
    return ImGui::DragScalarN(label, ImGuiDataType_Float, values, count, presentation.speed,
                              bounds.flags != 0 ? &bounds.minimum : nullptr,
                              bounds.flags != 0 ? &bounds.maximum : nullptr, format, bounds.flags);
}

bool drawInt(const char* label, int& value, const FieldPresentation& presentation, int lowest) {
    const IntBounds bounds = intBounds(presentation, lowest);
    const char* format = formatOr(presentation, "%d");
    if (presentation.widget == NumberWidget::Slider) {
        return ImGui::SliderInt(label, &value, bounds.minimum, bounds.maximum, format, bounds.flags);
    }
    return ImGui::DragInt(label, &value, presentation.speed, bounds.minimum, bounds.maximum, format, bounds.flags);
}

bool drawEnum(const char* label, void* object, const FieldDescriptor& field) {
    const int64_t current = field.enumValue(object);
    const EnumOption* selected = field.findOption(current);
    const std::string preview = selected != nullptr ? selected->label : std::to_string(current);
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (const EnumOption& option : field.options) {
            const bool isSelected = option.value == current;
            if (ImGui::Selectable(option.label.c_str(), isSelected) && !isSelected) {
                field.setEnumValue(object, option.value);
                changed = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Edits a [minimum, maximum] pair, keeping the maximum at least the minimum.
bool drawRange(const char* label, float& minimum, float& maximum, const FieldPresentation& presentation) {
    float range[2]{minimum, maximum};
    const FloatBounds bounds = floatBounds(presentation);
    if (!ImGui::DragFloat2(label, range, presentation.speed, bounds.minimum, bounds.maximum,
                           formatOr(presentation, "%.3f"), bounds.flags)) {
        return false;
    }
    minimum = range[0];
    maximum = std::max(range[1], minimum);
    return true;
}

void drawReadOnlyText(const char* label, const std::string& value, std::optional<bool> assetLoaded) {
    if (value.empty()) {
        ImGui::TextDisabled("%s: (none)", label);
    } else if (assetLoaded == false) {
        ImGui::TextColored(problemColor, "%s missing: %s", label, value.c_str());
    } else {
        ImGui::TextDisabled("%s: %s", label, value.c_str());
    }
}

bool drawField(void* object, const FieldDescriptor& field, const ReflectedFieldEditorOptions& options) {
    const FieldPresentation& presentation = field.presentation;
    const char* label = presentation.label.c_str();
    if (field.kind == FieldKind::String && presentation.readOnly) {
        const std::optional<bool> loaded = options.assetLoaded ? options.assetLoaded(field.key) : std::nullopt;
        drawReadOnlyText(label, field.value<std::string>(object), loaded);
        return false;
    }
    bool changed = false;
    ImGui::BeginDisabled(presentation.readOnly);
    switch (field.kind) {
    case FieldKind::Bool:
        changed = ImGui::Checkbox(label, &field.value<bool>(object));
        break;
    case FieldKind::Int32:
        changed = drawInt(label, field.value<int32_t>(object), presentation, INT_MIN);
        break;
    case FieldKind::UInt32: {
        uint32_t& stored = field.value<uint32_t>(object);
        int value = static_cast<int>(std::min<uint32_t>(stored, INT_MAX));
        if (drawInt(label, value, presentation, 0)) {
            stored = static_cast<uint32_t>(std::max(value, 0));
            changed = true;
        }
        break;
    }
    case FieldKind::Float:
        changed = drawFloat(label, field.value<float>(object), presentation);
        break;
    case FieldKind::Vec3:
        changed = drawFloats(label, &field.value<glm::vec3>(object).x, 3, presentation);
        break;
    case FieldKind::Vec4:
        changed = drawFloats(label, &field.value<glm::vec4>(object).x, 4, presentation);
        break;
    case FieldKind::UVec3: {
        glm::uvec3& stored = field.value<glm::uvec3>(object);
        glm::ivec3 value(glm::min(stored, glm::uvec3(INT_MAX)));
        const IntBounds bounds = intBounds(presentation, 0);
        if (ImGui::DragInt3(label, &value.x, presentation.speed, bounds.minimum, bounds.maximum,
                            formatOr(presentation, "%d"), bounds.flags)) {
            stored = glm::uvec3(glm::max(value, glm::ivec3(0)));
            changed = true;
        }
        break;
    }
    case FieldKind::String:
        changed = ImGui::InputText(label, &field.value<std::string>(object));
        break;
    case FieldKind::Enum:
        changed = drawEnum(label, object, field);
        break;
    case FieldKind::FloatRange:
        changed = drawRange(label, *static_cast<float*>(field.address(object)),
                            *static_cast<float*>(field.secondAddress(object)), presentation);
        break;
    case FieldKind::Struct:
        if (ImGui::TreeNode(label)) {
            // A struct matched by its own label shows all of its fields.
            ReflectedFieldEditorOptions childOptions = options;
            if (matchesInspectorFilter(presentation.label, options.filter)) {
                childOptions.filter = {};
            }
            changed = drawReflectedFields(field.address(object), field.children, childOptions);
            ImGui::TreePop();
        }
        break;
    }
    ImGui::EndDisabled();
    if (!presentation.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", presentation.tooltip.c_str());
    }
    return changed;
}

bool anyFieldMatches(std::span<const FieldDescriptor> fields, std::string_view filter) {
    return std::ranges::any_of(fields, [filter](const FieldDescriptor& field) {
        return !field.presentation.hidden &&
               (matchesInspectorFilter(field.presentation.label, filter) || anyFieldMatches(field.children, filter));
    });
}
} // namespace

bool matchesInspectorFilter(std::string_view label, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    const auto lower = [](unsigned char character) { return static_cast<char>(std::tolower(character)); };
    std::string lowerLabel(label);
    std::string lowerFilter(filter);
    std::ranges::transform(lowerLabel, lowerLabel.begin(), lower);
    std::ranges::transform(lowerFilter, lowerFilter.begin(), lower);
    return lowerLabel.find(lowerFilter) != std::string::npos;
}

bool drawReflectedFields(void* object, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options) {
    bool changed = false;
    for (const FieldDescriptor& field : fields) {
        if (field.presentation.hidden) {
            continue;
        }
        // A struct stays visible when any of its children match.
        const bool visible = matchesInspectorFilter(field.presentation.label, options.filter) ||
                             (field.kind == FieldKind::Struct && anyFieldMatches(field.children, options.filter));
        if (!visible) {
            continue;
        }
        ImGui::PushID(field.key.c_str());
        changed |= drawField(object, field, options);
        ImGui::PopID();
    }
    return changed;
}

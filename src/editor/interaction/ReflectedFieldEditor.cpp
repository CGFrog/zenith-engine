#include "ReflectedFieldEditor.hpp"

#include "editor/interaction/EntityReferenceField.hpp"
#include "editor/theme/EditorTheme.hpp"
#include "editor/theme/EditorWidgets.hpp"
#include "engine/reflection/FieldJson.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <climits>
#include <optional>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;

const ImVec4 problemColor = toImVec4(palette::error);

// Alt applies an edit to every selected object (see EntityPropertiesView), so
// it must not also slow drags down as ImGui's speed tweak does.
ImGuiSliderFlags modifierFlags() {
    return ImGui::GetIO().KeyAlt ? ImGuiSliderFlags_NoSpeedTweaks : ImGuiSliderFlags_None;
}

// Which of a field's values differ between the edited objects. Vectors and
// ranges also record which elements differ, so each can show its own mark.
struct MixedValue {
    bool any{false};
    std::array<bool, 4> elements{};
};

// Elements of a mixed field whose edit the user committed without changing
// the shown value, for example by typing the first object's own value: ImGui
// reports no change then, but the value must still apply to every object.
using CommittedElements = std::array<bool, 4>;

// Whether the user just finished typing into the item drawn last (with Enter
// or by leaving it, not with Escape) while it shows a mixed value. ImGui
// reports no change, and no edit, when the typed number equals the shown
// object's own, so typing is tracked in the window's state storage as the
// last frame the item was being typed into.
bool committedMixedEdit(bool mixed) {
    const ImGuiID id = ImGui::GetItemID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImHashStr("##TypedIntoMixedValue", 0, id);
    constexpr int notTyping = INT_MIN;
    const int frame = ImGui::GetFrameCount();
    const int typedFrame = storage->GetInt(key, notTyping);
    // A frame older than the previous one is stale: the item was not drawn
    // when it was left.
    const bool typing = typedFrame != notTyping && typedFrame >= frame - 1;
    if (ImGui::IsItemActive()) {
        const ImGuiInputTextState* text = ImGui::TempInputIsActive(id) ? ImGui::GetInputTextState(id) : nullptr;
        if (text != nullptr && (typing || (mixed && text->Edited))) {
            storage->SetInt(key, frame);
        }
        return false;
    }
    if (typedFrame != notTyping) {
        storage->SetInt(key, notTyping);
    }
    return typing && mixed && !ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

// ImGui treats equal drag bounds as unbounded; one-sided ranges are filled
// with the type's extreme so the given side still clamps.
struct FloatBounds {
    bool bounded{false};
    float minimum{0.0f};
    float maximum{0.0f};
    ImGuiSliderFlags flags{ImGuiSliderFlags_None};
};

FloatBounds floatBounds(const FieldPresentation& presentation) {
    if (!presentation.minimum && !presentation.maximum) {
        return {.flags = modifierFlags()};
    }
    return {.bounded = true,
            .minimum = static_cast<float>(presentation.minimum.value_or(-FLT_MAX)),
            .maximum = static_cast<float>(presentation.maximum.value_or(FLT_MAX)),
            .flags = ImGuiSliderFlags_AlwaysClamp | modifierFlags()};
}

struct IntBounds {
    int minimum{0};
    int maximum{0};
    ImGuiSliderFlags flags{ImGuiSliderFlags_None};
};

IntBounds intBounds(const FieldPresentation& presentation, int lowest) {
    if (!presentation.minimum && !presentation.maximum) {
        if (lowest == INT_MIN) {
            return {.flags = modifierFlags()};
        }
        return {.minimum = lowest, .maximum = INT_MAX, .flags = ImGuiSliderFlags_AlwaysClamp | modifierFlags()};
    }
    const auto clampToInt = [](double value) {
        return static_cast<int>(std::clamp(value, static_cast<double>(INT_MIN), static_cast<double>(INT_MAX)));
    };
    return {.minimum = std::max(lowest, clampToInt(presentation.minimum.value_or(INT_MIN))),
            .maximum = clampToInt(presentation.maximum.value_or(INT_MAX)),
            .flags = ImGuiSliderFlags_AlwaysClamp | modifierFlags()};
}

// The number format, or the mixed mark when the values differ. A format
// without a conversion makes ImGui show it verbatim; typing a value still
// parses with the type's default format.
const char* numberFormat(const FieldPresentation& presentation, const char* fallback, bool mixed) {
    if (mixed) {
        return mixedValueText;
    }
    return presentation.format.empty() ? fallback : presentation.format.c_str();
}

bool drawFloat(const char* label, float& value, const FieldPresentation& presentation, bool mixed) {
    const FloatBounds bounds = floatBounds(presentation);
    if (presentation.angle) {
        float degrees = glm::degrees(value);
        const char* format = numberFormat(presentation, "%.1f deg", mixed);
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
    const char* format = numberFormat(presentation, "%.3f", mixed);
    if (presentation.widget == NumberWidget::Slider) {
        return ImGui::SliderFloat(label, &value, bounds.minimum, bounds.maximum, format, bounds.flags);
    }
    return ImGui::DragFloat(label, &value, presentation.speed, bounds.minimum, bounds.maximum, format, bounds.flags);
}

// A drag or slider per element laid out as ImGui's DragScalarN and
// SliderScalarN lay them out (with the same ids), so each element can show
// its own mixed mark.
struct ElementWidget {
    ImGuiDataType type{ImGuiDataType_Float};
    bool slider{false};
    float speed{0.01f};
    const void* minimum{nullptr};
    const void* maximum{nullptr};
    const char* format{"%.3f"};
    ImGuiSliderFlags flags{ImGuiSliderFlags_None};
};

bool drawElements(const char* label, void* values, int count, const ElementWidget& widget, const MixedValue& mixed,
                  CommittedElements& committed) {
    if (ImGui::GetCurrentWindow()->SkipItems) {
        return false;
    }
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const size_t elementSize = ImGui::DataTypeGetInfo(widget.type)->Size;
    bool changed = false;
    ImGui::BeginGroup();
    ImGui::PushID(label);
    ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
    for (int index = 0; index < count; ++index) {
        ImGui::PushID(index);
        if (index > 0) {
            ImGui::SameLine(0.0f, spacing);
        }
        void* element = static_cast<char*>(values) + elementSize * static_cast<size_t>(index);
        const char* format = mixed.elements[static_cast<size_t>(index)] ? mixedValueText : widget.format;
        changed |= widget.slider ? ImGui::SliderScalar("", widget.type, element, widget.minimum, widget.maximum, format,
                                                       widget.flags)
                                 : ImGui::DragScalar("", widget.type, element, widget.speed, widget.minimum,
                                                     widget.maximum, format, widget.flags);
        if (committedMixedEdit(mixed.elements[static_cast<size_t>(index)])) {
            committed[static_cast<size_t>(index)] = true;
            changed = true;
        }
        ImGui::PopID();
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    if (const char* labelEnd = ImGui::FindRenderedTextEnd(label); labelEnd != label) {
        ImGui::SameLine(0.0f, spacing);
        ImGui::TextEx(label, labelEnd);
    }
    ImGui::EndGroup();
    return changed;
}

bool drawFloats(const char* label, float* values, int count, const FieldPresentation& presentation,
                const MixedValue& mixed, CommittedElements& committed) {
    if (presentation.color) {
        constexpr ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
        const bool changed =
            count == 3 ? ImGui::ColorEdit3(label, values, flags) : ImGui::ColorEdit4(label, values, flags);
        if (mixed.any) {
            // Colour pickers have no mixed state; mark the row instead.
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mixedValueText);
        }
        return changed;
    }
    const FloatBounds bounds = floatBounds(presentation);
    const bool slider = presentation.widget == NumberWidget::Slider;
    return drawElements(label, values, count,
                        {.type = ImGuiDataType_Float,
                         .slider = slider,
                         .speed = presentation.speed,
                         .minimum = slider || bounds.bounded ? &bounds.minimum : nullptr,
                         .maximum = slider || bounds.bounded ? &bounds.maximum : nullptr,
                         .format = numberFormat(presentation, "%.3f", false),
                         .flags = bounds.flags},
                        mixed, committed);
}

bool drawInt(const char* label, int& value, const FieldPresentation& presentation, int lowest, bool mixed) {
    const IntBounds bounds = intBounds(presentation, lowest);
    const char* format = numberFormat(presentation, "%d", mixed);
    if (presentation.widget == NumberWidget::Slider) {
        return ImGui::SliderInt(label, &value, bounds.minimum, bounds.maximum, format, bounds.flags);
    }
    return ImGui::DragInt(label, &value, presentation.speed, bounds.minimum, bounds.maximum, format, bounds.flags);
}

bool drawEnum(const char* label, void* object, const FieldDescriptor& field, bool mixed) {
    const int64_t current = field.enumValue(object);
    const EnumOption* selected = field.findOption(current);
    const std::string preview = mixed                 ? std::string(mixedValueText)
                                : selected != nullptr ? selected->label
                                                      : std::to_string(current);
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (const EnumOption& option : field.options) {
            const bool isSelected = !mixed && option.value == current;
            // Choosing the shown value of a mixed field still applies it to all.
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
bool drawRange(const char* label, float& minimum, float& maximum, const FieldPresentation& presentation,
               const MixedValue& mixed, CommittedElements& committed) {
    float range[2]{minimum, maximum};
    const FloatBounds bounds = floatBounds(presentation);
    if (!drawElements(label, range, 2,
                      {.type = ImGuiDataType_Float,
                       .speed = presentation.speed,
                       .minimum = &bounds.minimum,
                       .maximum = &bounds.maximum,
                       .format = numberFormat(presentation, "%.3f", false),
                       .flags = bounds.flags},
                      mixed, committed)) {
        return false;
    }
    minimum = range[0];
    maximum = std::max(range[1], minimum);
    return true;
}

bool drawString(const char* label, std::string& value, bool mixed) {
    if (!mixed) {
        return ImGui::InputText(label, &value);
    }
    // Typing replaces every object's differing text.
    std::string typed;
    if (!ImGui::InputTextWithHint(label, mixedValueText, &typed)) {
        return false;
    }
    value = std::move(typed);
    return true;
}

void drawReadOnlyText(const char* label, const std::string& value, std::optional<bool> assetLoaded, bool mixed) {
    if (mixed) {
        ImGui::TextDisabled("%s: %s", label, mixedValueText);
    } else if (value.empty()) {
        ImGui::TextDisabled("%s: (none)", label);
    } else if (assetLoaded == false) {
        ImGui::TextColored(problemColor, "%s missing: %s", label, value.c_str());
    } else {
        ImGui::TextDisabled("%s: %s", label, value.c_str());
    }
}

// The field's value in its document form, which every field kind has, so
// values can be compared and copied without knowing the kind.
Json fieldValue(const void* object, const FieldDescriptor& field) {
    Json output = Json::object();
    writeReflectedFields(object, std::span(&field, 1), output);
    return output[field.key];
}

MixedValue mixedValue(std::span<void* const> objects, const FieldDescriptor& field) {
    MixedValue mixed;
    if (objects.size() < 2 || field.kind == FieldKind::Struct || field.kind == FieldKind::List) {
        return mixed;
    }
    const Json shown = fieldValue(objects.front(), field);
    for (const void* object : objects.subspan(1)) {
        const Json other = fieldValue(object, field);
        if (other == shown) {
            continue;
        }
        mixed.any = true;
        const bool elementwise = shown.is_array() && other.is_array() && shown.size() == other.size();
        for (size_t index = 0; index < mixed.elements.size(); ++index) {
            mixed.elements[index] |= !elementwise || (index < shown.size() && shown[index] != other[index]);
        }
    }
    return mixed;
}

// Copies the edit the first object received (from before to after) to the
// others: the whole value, or only the changed and committed elements of an
// array.
void applyToOthers(std::span<void* const> objects, const FieldDescriptor& field, const Json& before, const Json& after,
                   const CommittedElements& committed) {
    const bool elementwise = before.is_array() && after.is_array() && before.size() == after.size();
    for (void* object : objects.subspan(1)) {
        Json value = fieldValue(object, field);
        if (elementwise && value.is_array() && value.size() == after.size()) {
            for (size_t index = 0; index < after.size(); ++index) {
                if (before[index] != after[index] || (index < committed.size() && committed[index])) {
                    value[index] = after[index];
                }
            }
        } else {
            value = after;
        }
        // The value came from an object described by the same field, so it
        // reads back; a failure would leave this object unchanged.
        const std::string error =
            readReflectedFields(Json{{field.key, std::move(value)}}, object, std::span(&field, 1));
        if (error.empty() && field.kind == FieldKind::FloatRange) {
            float& maximum = *static_cast<float*>(field.secondAddress(object));
            maximum = std::max(maximum, *static_cast<const float*>(field.address(object)));
        }
    }
}

std::string escapePointerToken(std::string_view key) {
    std::string escaped;
    for (const char character : key) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

// A combo choosing an index by name. An index past the names (a layer the
// project removed) shows as its number until another is chosen.
bool drawNamedIndex(const char* label, uint32_t& index, const std::vector<std::string>& names, bool mixed) {
    const std::string preview = mixed                  ? std::string(mixedValueText)
                                : index < names.size() ? names[index]
                                                       : "#" + std::to_string(index) + " (not in the project)";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (uint32_t candidate = 0; candidate < names.size(); ++candidate) {
            ImGui::PushID(static_cast<int>(candidate));
            if (ImGui::Selectable(names[candidate].c_str(), !mixed && candidate == index)) {
                index = candidate;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Marks the field just drawn when it is a prefab override, and gives it a
// context menu (##FieldMenu) with Revert to Prefab for an override and Watch
// Field when the options watch. Struct fields (tree nodes) and read-only text
// draw their label at the left, so only the bar marks them.
void decorateField(const FieldDescriptor& field, const std::string& pointer,
                   const ReflectedFieldEditorOptions& options) {
    const bool overridden = options.overridden && options.overridden(pointer);
    if (overridden) {
        const bool labelAtRight = field.kind != FieldKind::Struct && field.kind != FieldKind::List &&
                                  !(field.kind == FieldKind::String && field.presentation.readOnly);
        markPrefabOverride(labelAtRight ? std::string_view(field.presentation.label) : std::string_view{});
    }
    const bool canRevert = overridden && options.revertOverride;
    const bool canWatch =
        static_cast<bool>(options.watch) && field.kind != FieldKind::Struct && field.kind != FieldKind::List;
    if ((!canRevert && !canWatch) || !ImGui::BeginPopupContextItem("##FieldMenu")) {
        return;
    }
    if (canRevert && ImGui::MenuItem("Revert to Prefab")) {
        options.revertOverride(pointer);
    }
    if (canWatch && ImGui::MenuItem("Watch Field")) {
        options.watch(pointer, field.presentation.label);
    }
    ImGui::EndPopup();
}

// A list: a tree node with one node per element, a button to remove each, and
// one to add an element at the end. Several objects show only their counts,
// since their lists may differ in length.
bool drawList(std::span<void* const> objects, const FieldDescriptor& field, const std::string& pointer,
              const ReflectedFieldEditorOptions& options) {
    void* object = objects.front();
    const size_t size = field.listSize(object);
    const std::string& label = field.presentation.label;
    if (objects.size() > 1) {
        ImGui::TextDisabled("%s: %zu entries (select one object to edit)", label.c_str(), size);
        decorateField(field, pointer, options);
        return false;
    }
    const std::string title = label + " (" + std::to_string(size) + ")###" + field.key;
    const bool open = ImGui::TreeNode(title.c_str());
    decorateField(field, pointer, options);
    if (!open) {
        return false;
    }
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < size; ++index) {
        ImGui::PushID(static_cast<int>(index));
        const bool elementOpen = ImGui::TreeNode("##element", "#%zu", index);
        ImGui::SameLine();
        if (widgets::Button("Remove")) {
            removed = index;
        }
        if (elementOpen) {
            ReflectedFieldEditorOptions elementOptions = options;
            elementOptions.pointerPrefix = pointer + "/" + std::to_string(index);
            changed |= drawReflectedFields(field.listElement(object, index), field.children, elementOptions);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (widgets::Button("Add")) {
        field.resizeList(object, size + 1);
        changed = true;
    }
    if (removed) {
        // The descriptor has no erase: rewrite the list without the element.
        Json document = Json::object();
        writeReflectedFields(object, std::span(&field, 1), document);
        document[field.key].erase(*removed);
        changed |= readReflectedFields(document, object, std::span(&field, 1)).empty();
    }
    ImGui::TreePop();
    return changed;
}

bool drawField(std::span<void* const> objects, const FieldDescriptor& field,
               const ReflectedFieldEditorOptions& options) {
    void* object = objects.front();
    const FieldPresentation& presentation = field.presentation;
    const char* label = presentation.label.c_str();
    const MixedValue mixed = options.showMixedValues ? mixedValue(objects, field) : MixedValue{};
    const std::string pointer = options.pointerPrefix + "/" + escapePointerToken(field.key);
    if (field.kind == FieldKind::String && presentation.readOnly) {
        const std::optional<bool> loaded = options.assetLoaded ? options.assetLoaded(field.key) : std::nullopt;
        drawReadOnlyText(label, field.value<std::string>(object), loaded, mixed.any);
        decorateField(field, pointer, options);
        return false;
    }
    const Json before = objects.size() > 1 ? fieldValue(object, field) : Json{};
    bool changed = false;
    CommittedElements committed{};
    ImGui::BeginDisabled(presentation.readOnly);
    switch (field.kind) {
    case FieldKind::Bool: {
        bool& value = field.value<bool>(object);
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed.any);
        changed = ImGui::Checkbox(label, &value);
        ImGui::PopItemFlag();
        if (changed && mixed.any) {
            // Clicking a mixed checkbox checks every object.
            value = true;
        }
        break;
    }
    case FieldKind::Int32:
        changed = drawInt(label, field.value<int32_t>(object), presentation, INT_MIN, mixed.any);
        changed |= committedMixedEdit(mixed.any);
        break;
    case FieldKind::UInt32: {
        uint32_t& stored = field.value<uint32_t>(object);
        const std::vector<std::string>* names =
            !presentation.nameList.empty() && options.nameList ? options.nameList(presentation.nameList) : nullptr;
        if (names != nullptr) {
            changed = drawNamedIndex(label, stored, *names, mixed.any);
            break;
        }
        int value = static_cast<int>(std::min<uint32_t>(stored, INT_MAX));
        if (drawInt(label, value, presentation, 0, mixed.any)) {
            stored = static_cast<uint32_t>(std::max(value, 0));
            changed = true;
        }
        changed |= committedMixedEdit(mixed.any);
        break;
    }
    case FieldKind::UInt64:
        changed = ImGui::InputScalar(label, ImGuiDataType_U64, &field.value<uint64_t>(object));
        changed |= committedMixedEdit(mixed.any);
        break;
    case FieldKind::Float:
        changed = drawFloat(label, field.value<float>(object), presentation, mixed.any);
        changed |= committedMixedEdit(mixed.any);
        break;
    case FieldKind::Vec3:
        changed = drawFloats(label, &field.value<glm::vec3>(object).x, 3, presentation, mixed, committed);
        break;
    case FieldKind::Vec4:
        changed = drawFloats(label, &field.value<glm::vec4>(object).x, 4, presentation, mixed, committed);
        break;
    case FieldKind::UVec3: {
        glm::uvec3& stored = field.value<glm::uvec3>(object);
        glm::ivec3 value(glm::min(stored, glm::uvec3(INT_MAX)));
        const IntBounds bounds = intBounds(presentation, 0);
        if (drawElements(label, &value.x, 3,
                         {.type = ImGuiDataType_S32,
                          .speed = presentation.speed,
                          .minimum = &bounds.minimum,
                          .maximum = &bounds.maximum,
                          .format = numberFormat(presentation, "%d", false),
                          .flags = bounds.flags},
                         mixed, committed)) {
            stored = glm::uvec3(glm::max(value, glm::ivec3(0)));
            changed = true;
        }
        break;
    }
    case FieldKind::String:
        changed = drawString(label, field.value<std::string>(object), mixed.any);
        break;
    case FieldKind::Enum:
        changed = drawEnum(label, object, field, mixed.any);
        break;
    case FieldKind::FloatRange:
        changed = drawRange(label, *static_cast<float*>(field.address(object)),
                            *static_cast<float*>(field.secondAddress(object)), presentation, mixed, committed);
        break;
    case FieldKind::EntityReference:
        changed = drawEntityReferenceField(label, field.value<SceneEntityId>(object), field, options.entityReferences);
        break;
    case FieldKind::Struct: {
        const bool open = ImGui::TreeNode(label);
        // Marked before the children are drawn, while the node is the last item.
        decorateField(field, pointer, options);
        if (open) {
            // A struct matched by its own label shows all of its fields.
            ReflectedFieldEditorOptions childOptions = options;
            childOptions.pointerPrefix = pointer;
            if (matchesInspectorFilter(presentation.label, options.filter)) {
                childOptions.filter = {};
            }
            // The children copy their own edits to every object.
            std::vector<void*> members;
            members.reserve(objects.size());
            for (void* owner : objects) {
                members.push_back(field.address(owner));
            }
            changed = drawReflectedFields(members, field.children, childOptions);
            ImGui::TreePop();
        }
        break;
    }
    case FieldKind::List:
        changed = drawList(objects, field, pointer, options);
        break;
    }
    ImGui::EndDisabled();
    if (!presentation.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", presentation.tooltip.c_str());
    }
    if (field.kind != FieldKind::Struct && field.kind != FieldKind::List) {
        decorateField(field, pointer, options);
    }
    if (changed && objects.size() > 1 && field.kind != FieldKind::Struct && field.kind != FieldKind::List) {
        applyToOthers(objects, field, before, fieldValue(object, field), committed);
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

void markPrefabOverride(std::string_view label) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const float pixel = editorArtPixel();
    // The bar sits in the gutter left of the row; the window padding is
    // partly clipped, so it gets its own clip rectangle.
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    drawList->PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max, false);
    const float barRight = itemMin.x - pixel;
    drawList->AddRectFilled({barRight - pixel * 2.0f, itemMin.y}, {barRight, itemMax.y},
                            ImGui::GetColorU32(toImVec4(palette::prefabOverride)));
    drawList->PopClipRect();
    if (label.empty()) {
        return;
    }
    const char* text = label.data();
    const char* textEnd = ImGui::FindRenderedTextEnd(text, text + label.size());
    if (textEnd == text) {
        return;
    }
    // Pixel fonts have no bold face: the label is drawn again in override ink,
    // once in place and once an art pixel to the right.
    const float labelWidth = ImGui::CalcTextSize(text, textEnd).x;
    const ImVec2 position{itemMax.x - labelWidth, itemMin.y + ImGui::GetStyle().FramePadding.y};
    const ImU32 ink = ImGui::GetColorU32(toImVec4(palette::prefabOverrideInk));
    drawList->AddText(position, ink, text, textEnd);
    drawList->AddText({position.x + pixel, position.y}, ink, text, textEnd);
}

bool prefabOverrideMenu(const char* id) {
    bool chosen = false;
    if (ImGui::BeginPopupContextItem(id)) {
        chosen = ImGui::MenuItem("Revert to Prefab");
        ImGui::EndPopup();
    }
    return chosen;
}

bool drawReflectedFields(void* object, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options) {
    return drawReflectedFields(std::span<void* const>(&object, 1), fields, options);
}

bool drawReflectedFields(std::span<void* const> objects, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options) {
    if (objects.empty()) {
        return false;
    }
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
        changed |= drawField(objects, field, options);
        ImGui::PopID();
    }
    return changed;
}

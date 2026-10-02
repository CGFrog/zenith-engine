#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Plain-data reflection: describes the fields of a C++ struct so generic code
// can serialize, inspect, and edit it without knowing the type. Descriptors
// are built explicitly from member pointers (see Fields<Object>); there is no
// code generation, so registration is ordinary code a debugger can step into.

enum class FieldKind : uint8_t {
    Bool,
    Int32,
    UInt32,
    Float,
    Vec3,
    Vec4,
    UVec3,
    String,
    // A scoped or unscoped enum whose values are listed in FieldDescriptor::options.
    Enum,
    // Two floats, a minimum and a maximum, kept as separate members.
    FloatRange,
    // A nested struct described by FieldDescriptor::children.
    Struct,
};

// How an enum is written to documents.
enum class EnumEncoding : uint8_t {
    Name,
    Integer,
};

struct EnumOption {
    // Stable document name, for example "additive".
    std::string key;
    // Inspector label, for example "Additive".
    std::string label;
    int64_t value{0};
};

enum class NumberWidget : uint8_t {
    Drag,
    Slider,
};

// Editing hints. They shape the inspector only; documents are validated by the
// owning type, so a range here never rejects a saved value.
struct FieldPresentation {
    std::string label;
    std::string tooltip;
    NumberWidget widget{NumberWidget::Drag};
    float speed{0.01f};
    std::optional<double> minimum;
    std::optional<double> maximum;
    // printf-style format for numbers, for example "%.2f s".
    std::string format;
    // Float stored in radians and shown in degrees; minimum and maximum are in degrees.
    bool angle{false};
    // Vec3/Vec4 shown as a linear color.
    bool color{false};
    bool readOnly{false};
    bool hidden{false};
};

struct FieldDescriptor {
    // Stable document key, for example "shadow_resolution".
    std::string key;
    FieldKind kind{FieldKind::Float};
    // Optional fields may be missing from a document; the object keeps the
    // value it had, normally its default.
    bool optional{false};
    FieldPresentation presentation;
    // Maps the owning object to this field. FloatRange uses it for the minimum
    // and secondAddress for the maximum.
    std::function<void*(void*)> address;
    std::function<void*(void*)> secondAddress;
    // Enum only.
    std::vector<EnumOption> options;
    EnumEncoding encoding{EnumEncoding::Name};
    std::function<int64_t(const void*)> readEnum;
    std::function<void(void*, int64_t)> writeEnum;
    // Struct only.
    std::vector<FieldDescriptor> children;

    template <typename Value> Value& value(void* object) const {
        return *static_cast<Value*>(address(object));
    }

    template <typename Value> const Value& value(const void* object) const {
        // Descriptors address members; reading through them never mutates.
        return *static_cast<const Value*>(address(const_cast<void*>(object)));
    }

    int64_t enumValue(const void* object) const;
    void setEnumValue(void* object, int64_t value) const;
    const EnumOption* findOption(int64_t value) const;
};

// "shadow_resolution" -> "Shadow resolution".
std::string labelFromKey(std::string_view key);

namespace reflection_detail {
template <typename Value> constexpr FieldKind kindOf() {
    if constexpr (std::is_same_v<Value, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_same_v<Value, int32_t>) {
        return FieldKind::Int32;
    } else if constexpr (std::is_same_v<Value, uint32_t>) {
        return FieldKind::UInt32;
    } else if constexpr (std::is_same_v<Value, float>) {
        return FieldKind::Float;
    } else if constexpr (std::is_same_v<Value, glm::vec3>) {
        return FieldKind::Vec3;
    } else if constexpr (std::is_same_v<Value, glm::vec4>) {
        return FieldKind::Vec4;
    } else if constexpr (std::is_same_v<Value, glm::uvec3>) {
        return FieldKind::UVec3;
    } else if constexpr (std::is_same_v<Value, std::string>) {
        return FieldKind::String;
    } else if constexpr (std::is_enum_v<Value>) {
        return FieldKind::Enum;
    } else {
        static_assert(sizeof(Value) == 0, "Unsupported reflected field type; describe it with structure() or a "
                                          "custom component codec");
        return FieldKind::Bool;
    }
}
} // namespace reflection_detail

// Builds the field list of Object. Each add() starts a field; the following
// modifiers apply to the most recent field:
//
//   Fields<PointLightComponent>()
//       .add("color", &PointLightComponent::color).color()
//       .add("range", &PointLightComponent::range).drag(0.05f, 0.01f, 10000.0f)
//       .add("casts_shadows", &PointLightComponent::castsShadows).optional()
template <typename Object> class Fields {
  public:
    template <typename Value> Fields& add(std::string key, Value Object::* member) {
        FieldDescriptor field;
        field.presentation.label = labelFromKey(key);
        field.key = std::move(key);
        field.kind = reflection_detail::kindOf<Value>();
        field.address = [member](void* object) -> void* { return &(static_cast<Object*>(object)->*member); };
        if constexpr (std::is_enum_v<Value>) {
            field.readEnum = [](const void* value) { return static_cast<int64_t>(*static_cast<const Value*>(value)); };
            field.writeEnum = [](void* value, int64_t raw) { *static_cast<Value*>(value) = static_cast<Value>(raw); };
        }
        m_fields.push_back(std::move(field));
        return *this;
    }

    // A [minimum, maximum] pair written as a two-element array.
    Fields& range(std::string key, float Object::* minimum, float Object::* maximum) {
        FieldDescriptor field;
        field.presentation.label = labelFromKey(key);
        field.key = std::move(key);
        field.kind = FieldKind::FloatRange;
        field.address = [minimum](void* object) -> void* { return &(static_cast<Object*>(object)->*minimum); };
        field.secondAddress = [maximum](void* object) -> void* { return &(static_cast<Object*>(object)->*maximum); };
        m_fields.push_back(std::move(field));
        return *this;
    }

    // A nested struct written as an object.
    template <typename Nested> Fields& structure(std::string key, Nested Object::* member, Fields<Nested> nested) {
        FieldDescriptor field;
        field.presentation.label = labelFromKey(key);
        field.key = std::move(key);
        field.kind = FieldKind::Struct;
        field.address = [member](void* object) -> void* { return &(static_cast<Object*>(object)->*member); };
        field.children = std::move(nested).build();
        m_fields.push_back(std::move(field));
        return *this;
    }

    Fields& optional() {
        last().optional = true;
        return *this;
    }

    Fields& label(std::string text) {
        last().presentation.label = std::move(text);
        return *this;
    }

    Fields& tooltip(std::string text) {
        last().presentation.tooltip = std::move(text);
        return *this;
    }

    Fields& drag(float speed, std::optional<double> minimum = std::nullopt,
                 std::optional<double> maximum = std::nullopt) {
        FieldPresentation& presentation = last().presentation;
        presentation.widget = NumberWidget::Drag;
        presentation.speed = speed;
        presentation.minimum = minimum;
        presentation.maximum = maximum;
        return *this;
    }

    Fields& slider(double minimum, double maximum) {
        FieldPresentation& presentation = last().presentation;
        presentation.widget = NumberWidget::Slider;
        presentation.minimum = minimum;
        presentation.maximum = maximum;
        return *this;
    }

    Fields& format(std::string text) {
        last().presentation.format = std::move(text);
        return *this;
    }

    Fields& angle() {
        last().presentation.angle = true;
        return *this;
    }

    Fields& color() {
        last().presentation.color = true;
        return *this;
    }

    Fields& readOnly() {
        last().presentation.readOnly = true;
        return *this;
    }

    Fields& hidden() {
        last().presentation.hidden = true;
        return *this;
    }

    // Lists an enum's values; every enum field needs them.
    Fields& options(std::vector<EnumOption> values, EnumEncoding encoding = EnumEncoding::Name) {
        last().options = std::move(values);
        last().encoding = encoding;
        return *this;
    }

    std::vector<FieldDescriptor> build() && {
        return std::move(m_fields);
    }

  private:
    FieldDescriptor& last() {
        return m_fields.back();
    }

    std::vector<FieldDescriptor> m_fields;
};

// Empty when the descriptors are usable, otherwise the first problem found,
// such as an enum without options or a duplicate key.
std::string validateFieldDescriptors(const std::vector<FieldDescriptor>& fields);

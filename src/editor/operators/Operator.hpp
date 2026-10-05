#pragma once

#include "editor/application/EditorPlayMode.hpp"
#include "editor/application/SceneEditing.hpp"
#include "editor/interaction/Selection.hpp"
#include "editor/operators/KeymapRegion.hpp"
#include "engine/reflection/Reflection.hpp"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct ApplicationContext;

// Blender-style operators: every editor action (menu item, button, hotkey, F3
// search entry) is a named operator that menus, panels, and the keymap invoke
// by id. Operators run against an OperatorContext, so the ones that only touch
// the document and selection run headless in tests.

enum class OperatorFlags : uint8_t {
    None = 0,
    // The runner records the operator's scene change as one undo step named
    // after its label. Undo operators must not commit history themselves.
    Undo = 1u << 0u,
    // The runner remembers the operator and its properties so the Adjust Last
    // Operation panel can undo it and run it again with edited properties.
    Register = 1u << 1u,
    // Hidden from F3 search because it needs properties a caller supplies,
    // for example a component key from the inspector.
    Internal = 1u << 2u,
};

constexpr OperatorFlags operator|(OperatorFlags left, OperatorFlags right) {
    return static_cast<OperatorFlags>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr bool hasFlag(OperatorFlags flags, OperatorFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

enum class OperatorStatus : uint8_t {
    // The operator did its work (a modal operator: it started).
    Finished,
    // Nothing happened, for example because poll failed; not an error. A
    // cancelled operator must leave the scene unchanged.
    Cancelled,
    // The operator could not do its work; the runner reports the message.
    Failed,
};

struct OperatorResult {
    OperatorStatus status{OperatorStatus::Failed};
    std::string message;

    static OperatorResult finished() {
        return {.status = OperatorStatus::Finished};
    }

    static OperatorResult cancelled(std::string message = {}) {
        return {.status = OperatorStatus::Cancelled, .message = std::move(message)};
    }

    static OperatorResult failed(std::string message) {
        return {.status = OperatorStatus::Failed, .message = std::move(message)};
    }

    explicit operator bool() const {
        return status == OperatorStatus::Finished;
    }
};

// A copyable value holding an operator's property struct together with the
// reflected fields that describe it, so generic code (the Adjust Last
// Operation panel, keymap bindings) can edit and serialize properties without
// knowing their type. An empty value means the operator has no properties.
class OperatorProperties {
  public:
    OperatorProperties() = default;

    template <typename Properties> static OperatorProperties make(Properties defaults, Fields<Properties> fields) {
        return fromDescriptors(std::move(defaults), std::move(fields).build());
    }

    // The same for descriptors built at run time (a game module's operator
    // properties), whose addresses resolve inside a Properties object.
    template <typename Properties>
    static OperatorProperties fromDescriptors(Properties defaults, std::vector<FieldDescriptor> fields) {
        OperatorProperties properties;
        properties.m_value = std::move(defaults);
        properties.m_address = [](std::any& value) -> void* { return std::any_cast<Properties>(&value); };
        properties.m_fields = std::make_shared<const std::vector<FieldDescriptor>>(std::move(fields));
        return properties;
    }

    bool empty() const {
        return !m_value.has_value();
    }

    // Whether both hold the same property struct type (or both are empty).
    bool sameType(const OperatorProperties& other) const {
        return m_value.type() == other.m_value.type();
    }

    void* object() {
        return m_address == nullptr ? nullptr : m_address(m_value);
    }

    const void* object() const {
        return m_address == nullptr ? nullptr : m_address(const_cast<std::any&>(m_value));
    }

    std::span<const FieldDescriptor> fields() const {
        return m_fields == nullptr ? std::span<const FieldDescriptor>{} : std::span<const FieldDescriptor>(*m_fields);
    }

    // nullptr when the properties are not a Properties.
    template <typename Properties> Properties* get() {
        return std::any_cast<Properties>(&m_value);
    }

    template <typename Properties> const Properties* get() const {
        return std::any_cast<Properties>(&m_value);
    }

    // Overwrites fields named in a JSON object (compact text, for example
    // {"action":"deselect"}). Empty text changes nothing. Returns an empty
    // string on success, otherwise what was wrong.
    std::string applyJson(std::string_view json);
    // The properties as a compact JSON object; "{}" when there are none.
    std::string toJson() const;

  private:
    std::any m_value;
    void* (*m_address)(std::any&) = nullptr;
    std::shared_ptr<const std::vector<FieldDescriptor>> m_fields;
};

// Everything an operator may use while it runs. It holds references only and
// is built per invocation by whoever invokes the operator.
struct OperatorContext {
    SceneEditing editing;
    Selection& selection;
    EditorMode mode{EditorMode::Edit};
    // Where the operator was invoked from; menus, buttons, and F3 use Global.
    KeymapRegion region{KeymapRegion::Global};
    // The running editor application. Headless callers leave it null, so
    // operators that need a window or renderer must poll false without it.
    ApplicationContext* app{nullptr};

    bool editMode() const {
        return mode == EditorMode::Edit;
    }
};

struct Operator {
    // Stable id used by menus, the keymap, and keymap.json, for example
    // "object.duplicate". Changing it breaks saved user keymaps.
    std::string id;
    // Menu, search, and undo-step name, for example "Duplicate Objects".
    std::string label;
    // One sentence for tooltips and the F3 search.
    std::string description;
    OperatorFlags flags{OperatorFlags::None};
    // Default property values; invocations start from a copy of these.
    OperatorProperties properties;
    // Whether the operator can run now. Empty means always.
    std::function<bool(const OperatorContext&)> poll;
    // Does the work. It may write back properties it resolved (for example the
    // name it used) so the redo panel shows them.
    std::function<OperatorResult(OperatorContext&, OperatorProperties&)> execute;
    // Optional, Blender's invoke: fills defaults that depend on the editor's
    // state when the operator is started (the add operators start at the 3D
    // cursor). The runner calls it on a copy of `properties` before a
    // caller's JSON values are applied, and not when a caller passes complete
    // properties (Adjust Last Operation's re-run), so edited values stick.
    std::function<void(const OperatorContext&, OperatorProperties&)> invokeDefaults;

    bool canRun(const OperatorContext& context) const {
        return !poll || poll(context);
    }
};

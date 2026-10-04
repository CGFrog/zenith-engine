#include "DebugOperators.hpp"

#include "editor/interaction/WatchList.hpp"
#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ExternalEditor.hpp"
#include "editor/settings/EditorSettings.hpp"
#include "engine/platform/ChildProcess.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace {
Fields<WatchFieldProperties> watchFieldFields() {
    return std::move(Fields<WatchFieldProperties>()
                         .add("entity", &WatchFieldProperties::entity)
                         .add("component", &WatchFieldProperties::component)
                         .add("field", &WatchFieldProperties::field)
                         .add("label", &WatchFieldProperties::label));
}

bool blank(const std::string& text) {
    return std::ranges::all_of(text, [](char character) { return character == ' ' || character == '\t'; });
}

// The watched field the properties name, or nullopt with error set.
std::optional<WatchedField> watchedField(const WatchFieldProperties& properties, std::string& error) {
    uint64_t value = 0;
    const std::string& entity = properties.entity;
    const auto [end, parseError] = std::from_chars(entity.data(), entity.data() + entity.size(), value);
    if (parseError != std::errc{} || end != entity.data() + entity.size() || value == 0) {
        error = "'" + entity + "' is not an entity id";
        return std::nullopt;
    }
    if (properties.component.empty() || !properties.field.starts_with('/')) {
        error = "A watch needs a component key and a field pointer such as /range";
        return std::nullopt;
    }
    return WatchedField{.entity = SceneEntityId{value},
                        .component = properties.component,
                        .pointer = properties.field,
                        .label = properties.label.empty() ? properties.field : properties.label};
}

Operator attachOperator(const EditorToolSettings& tools) {
    return {.id = "debug.attach",
            .label = "Attach Debugger",
            .description = "Attach the debugger (ide.debug_command) to the editor, so breakpoints in the game module "
                           "hit",
            .poll = [&tools](const OperatorContext&) { return !blank(tools.debugCommand); },
            .execute =
                [&tools](OperatorContext&, OperatorProperties&) {
                    if (const ChildProcessResult started = attachDebugger(tools.debugCommand, currentProcessId());
                        !started) {
                        return OperatorResult::failed("Could not start the debugger (ide.debug_command): " +
                                                      started.error);
                    }
                    return OperatorResult::finished();
                }};
}

Operator watchOperator(WatchList& watches, bool add) {
    return {.id = add ? "debug.watch_field" : "debug.unwatch_field",
            .label = add ? "Watch Field" : "Stop Watching Field",
            .description = add ? "Show a component field's running value at the top of the Properties panel"
                               : "Remove a field from the Properties panel's watch list",
            .properties = OperatorProperties::make(WatchFieldProperties{}, watchFieldFields()),
            .execute = [&watches, add](OperatorContext&, OperatorProperties& properties) {
                const auto* values = properties.get<WatchFieldProperties>();
                if (values == nullptr) {
                    return OperatorResult::failed("Operator properties have the wrong type");
                }
                std::string error;
                std::optional<WatchedField> field = watchedField(*values, error);
                if (!field) {
                    return OperatorResult::failed(error);
                }
                const bool changed = add ? watches.add(std::move(*field)) : watches.remove(*field);
                return changed ? OperatorResult::finished()
                               : OperatorResult::cancelled(add ? "The field is already watched"
                                                               : "The field is not watched");
            }};
}

Operator clearOperator(WatchList& watches) {
    return {.id = "debug.clear_watches",
            .label = "Clear Watched Fields",
            .description = "Remove every field from the Properties panel's watch list",
            .poll = [&watches](const OperatorContext&) { return !watches.fields().empty(); },
            .execute =
                [&watches](OperatorContext&, OperatorProperties&) {
                    watches.clear();
                    return OperatorResult::finished();
                }};
}
} // namespace

OperatorRegistrationResult registerDebugOperators(OperatorRegistry& registry, DebugOperatorServices services) {
    for (Operator op : {attachOperator(services.tools), watchOperator(services.watches, true),
                        watchOperator(services.watches, false), clearOperator(services.watches)}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerDebugOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services) {
    return registerDebugOperators(registry, {.tools = services.tools, .watches = services.watches});
}

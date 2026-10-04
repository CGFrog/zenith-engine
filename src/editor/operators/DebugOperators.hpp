#pragma once

#include "editor/operators/OperatorRegistry.hpp"

#include <string>

class WatchList;
struct EditorOperatorServices;
struct EditorToolSettings;

// Properties of debug.watch_field and debug.unwatch_field. entity is the
// stable id as decimal text, as play.apply_to_edit takes it; field is a JSON
// pointer into the component's document object.
struct WatchFieldProperties {
    std::string entity;
    std::string component;
    std::string field;
    // What the watch list shows the field as; empty uses the pointer.
    std::string label;
};

struct DebugOperatorServices {
    // ide.debug_command, which debug.attach runs.
    const EditorToolSettings& tools;
    WatchList& watches;
};

// Debugging the game in the editor:
//
//   debug.attach         run ide.debug_command with the editor's process id,
//                        so breakpoints in the game module hit
//   debug.watch_field    add a component field to the watch list
//   debug.unwatch_field  remove one
//   debug.clear_watches  remove them all
//
// None records undo (the watch list is editor state) and all run headless.
OperatorRegistrationResult registerDebugOperators(OperatorRegistry& registry, DebugOperatorServices services);

OperatorRegistrationResult registerDebugOperatorFamily(OperatorRegistry& registry,
                                                       const EditorOperatorServices& services);

#pragma once

#include "engine/reflection/Reflection.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>

struct ReflectedFieldEditorOptions {
    // Case-insensitive label filter; empty shows every field.
    std::string_view filter;
    // For read-only path fields: whether the asset they name loaded, so a
    // missing file can be flagged. nullopt means the field is not an asset.
    std::function<std::optional<bool>(std::string_view fieldKey)> assetLoaded;
};

// Draws one ImGui widget per visible field of object, using each field's
// presentation hints, and returns whether any value changed. Edits are applied
// in place; callers record undo around the call.
bool drawReflectedFields(void* object, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options = {});

// Whether a label matches an inspector filter, ignoring case.
bool matchesInspectorFilter(std::string_view label, std::string_view filter);

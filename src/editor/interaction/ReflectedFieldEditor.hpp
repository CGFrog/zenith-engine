#pragma once

#include "engine/reflection/Reflection.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>

struct EntityReferenceFieldContext;

// Shown in place of a value that differs between the objects being edited.
inline constexpr const char* mixedValueText = "\xE2\x80\x94";

struct ReflectedFieldEditorOptions {
    // Case-insensitive label filter; empty shows every field.
    std::string_view filter;
    // For read-only path fields: whether the asset they name loaded, so a
    // missing file can be flagged. nullopt means the field is not an asset.
    std::function<std::optional<bool>(std::string_view fieldKey)> assetLoaded;
    // Names referenced entities and offers the eyedropper; without it entity
    // reference fields show their id only.
    const EntityReferenceFieldContext* entityReferences{nullptr};
    // With several objects, show mixedValueText for a field whose values
    // differ. When false the first object's values are shown and the others
    // are only edit targets (an Alt-applied edit).
    bool showMixedValues{true};
};

// Draws one ImGui widget per visible field of object, using each field's
// presentation hints, and returns whether any value changed. Edits are applied
// in place; callers record undo around the call.
bool drawReflectedFields(void* object, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options = {});

// Edits the same fields of several objects of one type together. The widgets
// show objects.front(); a field whose values differ shows mixedValueText (a
// checkbox shows ImGui's mixed mark). An edit is copied to every object, but
// only the edited field, and of a vector or range only the edited elements, so
// the objects' other differences survive. Fields without a dedicated widget
// path are compared and copied through their JSON form (FieldJson), so every
// field kind supports mixed display and multi-object edits.
bool drawReflectedFields(std::span<void* const> objects, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options = {});

// Whether a label matches an inspector filter, ignoring case.
bool matchesInspectorFilter(std::string_view label, std::string_view filter);

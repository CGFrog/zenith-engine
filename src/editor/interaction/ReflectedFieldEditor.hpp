#pragma once

// Draws any reflected field (Fields<T>) as an Inspector widget, including multi-entity editing.

#include "engine/reflection/Reflection.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct AssetReferenceFieldContext;
struct EntityReferenceFieldContext;

// Shown in place of a value that differs between the objects being edited.
inline constexpr const char* mixedValueText = "\xE2\x80\x94";

struct ReflectedFieldEditorOptions {
    // Case-insensitive label filter; empty shows every field.
    std::string_view filter;
    // For path fields: whether the asset they name loaded, so a missing file
    // can be flagged. nullopt means the field has no loaded asset.
    std::function<std::optional<bool>(std::string_view fieldKey)> assetLoaded;
    // Offers project assets to String fields with FieldPresentation::
    // assetKind (an asset picker, thumbnails, and drops from the Asset
    // Browser); without it they are edited as text.
    const AssetReferenceFieldContext* assets{nullptr};
    // Names referenced entities and offers the eyedropper; without it entity
    // reference fields show their id only.
    const EntityReferenceFieldContext* entityReferences{nullptr};
    // The names a UInt32 field with FieldPresentation::nameList indexes
    // (the project's physics layers); null, or no callback, edits the number.
    std::function<const std::vector<std::string>*(std::string_view listName)> nameList;
    // With several objects, show mixedValueText for a field whose values
    // differ. When false the first object's values are shown and the others
    // are only edit targets (an Alt-applied edit).
    bool showMixedValues{true};
    // Prefab override highlighting. A field is addressed by its RFC 6901
    // pointer inside the component's document object: pointerPrefix (the
    // pointer of the object whose fields are drawn, empty at the top level)
    // followed by "/" and its key, so nested struct fields get full paths.
    // A field for which overridden returns true is marked (markPrefabOverride)
    // and right-clicking it offers "Revert to Prefab", which calls
    // revertOverride with its pointer. Without overridden nothing is marked.
    std::string pointerPrefix;
    std::function<bool(std::string_view pointer)> overridden;
    std::function<void(std::string_view pointer)> revertOverride;
    // When set, each field's context menu offers Watch Field, which calls
    // it with the field's pointer and label.
    std::function<void(std::string_view pointer, std::string_view label)> watch;
};

// Marks the last drawn item as a prefab override: a bar in the override
// accent beside it and, when label (its visible text, drawn at the item's
// right end as ImGui widgets draw labels) is not empty, the label in bold
// override ink. Pass an empty label for items whose text sits elsewhere
// (headers, tree nodes).
void markPrefabOverride(std::string_view label);
// The last item's right-click menu offering "Revert to Prefab"; returns
// whether it was chosen. id names the popup and must be unique per item.
bool prefabOverrideMenu(const char* id);

// Draws one ImGui widget per visible field of object, using each field's
// presentation hints, and returns whether any value changed. Edits are applied
// in place; callers record undo around the call.
bool drawReflectedFields(void* object, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options = {});

// Edits the same fields of several objects of one type together. The widgets
// show objects.front(); a field whose values differ shows mixedValueText (a
// checkbox shows ImGui's mixed mark). An edit is copied to every object, but
// only the edited field, and of a vector or range only the edited elements, so
// the objects' other differences survive. Finishing an edit of a mixed
// number (typing and pressing Enter, or leaving the field) applies it even
// when the value equals the shown object's own. Fields without a dedicated widget
// path are compared and copied through their JSON form (FieldJson), so every
// field kind supports mixed display and multi-object edits.
bool drawReflectedFields(std::span<void* const> objects, std::span<const FieldDescriptor> fields,
                         const ReflectedFieldEditorOptions& options = {});

// Whether a label matches an inspector filter, ignoring case.
bool matchesInspectorFilter(std::string_view label, std::string_view filter);

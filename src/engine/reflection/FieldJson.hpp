#pragma once

#include "Reflection.hpp"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>

// Whether writeReflectedFields writes runtime-state fields (save games) or
// leaves them out (scene documents).
enum class RuntimeStateFields {
    Include,
    Skip,
};

// Writes the fields of object into output, which becomes a JSON object.
// Floats keep float precision, enums use their declared encoding, ranges and
// vectors become arrays, structs become nested objects, lists become arrays
// of objects, and entity references become their id, or null for none.
void writeReflectedFields(const void* object, std::span<const FieldDescriptor> fields, nlohmann::json& output,
                          RuntimeStateFields runtimeState = RuntimeStateFields::Include);

// Reads fields from a JSON object into object. Required fields must be
// present; optional and runtime-state ones keep the object's current value
// when missing. Keys
// the descriptors do not name are ignored, so newer documents stay readable.
// Returns an empty string on success, otherwise what was wrong. On failure
// object may be partially updated.
std::string readReflectedFields(const nlohmann::json& input, void* object, std::span<const FieldDescriptor> fields);

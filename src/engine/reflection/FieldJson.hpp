#pragma once

#include "Reflection.hpp"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>

// Writes every field of object into output, which becomes a JSON object.
// Floats keep float precision, enums use their declared encoding, ranges and
// vectors become arrays, structs become nested objects, and entity references
// become their id, or null for none.
void writeReflectedFields(const void* object, std::span<const FieldDescriptor> fields, nlohmann::json& output);

// Reads fields from a JSON object into object. Required fields must be
// present; optional ones keep the object's current value when missing. Keys
// the descriptors do not name are ignored, so newer documents stay readable.
// Returns an empty string on success, otherwise what was wrong. On failure
// object may be partially updated.
std::string readReflectedFields(const nlohmann::json& input, void* object, std::span<const FieldDescriptor> fields);

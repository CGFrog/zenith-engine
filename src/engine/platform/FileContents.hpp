#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class FileReadStatus {
    Success,
    OpenFailed,
    ReadFailed,
};

// Reads a whole file with one sized read. Prefer these to constructing a
// container from std::istreambuf_iterator, which copies one byte per call and
// dominates asset loading in Debug builds. On failure `output` is left empty.
FileReadStatus readFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& output);
FileReadStatus readFileText(const std::filesystem::path& path, std::string& output);

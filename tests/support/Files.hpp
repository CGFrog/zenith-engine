#pragma once

// Whole-file reads and writes for tests, which throw (failing the test with the reason) instead of returning errors.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace testsupport {

// The file's bytes as text; throws std::runtime_error when it cannot be read.
std::string readText(const std::filesystem::path& file);
std::vector<uint8_t> readBytes(const std::filesystem::path& file);
// Writes text as the whole file, creating its parent directories; throws
// std::runtime_error when it cannot.
void writeText(const std::filesystem::path& file, std::string_view text);
void writeBytes(const std::filesystem::path& file, std::span<const uint8_t> bytes);

// Where a test keeps a file that explains a failure (a capture that did not
// match, a diff): test-temp/failures/<fileName> in the build tree, which
// outlives the test, unlike a TempDirectory. The directory is created.
std::filesystem::path failureArtifactPath(std::string_view fileName);

} // namespace testsupport

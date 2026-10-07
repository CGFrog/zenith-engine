#include "support/Files.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace testsupport {

std::string readText(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not read " + file.string());
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> readBytes(const std::filesystem::path& file) {
    const std::string text = readText(file);
    return {text.begin(), text.end()};
}

void writeText(const std::filesystem::path& file, std::string_view text) {
    if (file.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            throw std::runtime_error("Could not create " + file.parent_path().string() + ": " + error.message());
        }
    }
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("Could not write " + file.string());
    }
}

void writeBytes(const std::filesystem::path& file, std::span<const uint8_t> bytes) {
    writeText(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::filesystem::path failureArtifactPath(std::string_view fileName) {
#ifdef ZENITH_TEST_TEMP_DIR
    const std::filesystem::path root = std::filesystem::path(ZENITH_TEST_TEMP_DIR) / "failures";
#else
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith-test-failures";
#endif
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    return root / fileName;
}

} // namespace testsupport

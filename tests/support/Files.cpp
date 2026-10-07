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

} // namespace testsupport

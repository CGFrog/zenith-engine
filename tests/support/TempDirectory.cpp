#include "support/TempDirectory.hpp"

#include <atomic>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace testsupport {
namespace {

std::filesystem::path temporaryRoot() {
#ifdef ZENITH_TEST_TEMP_DIR
    const std::filesystem::path root(ZENITH_TEST_TEMP_DIR);
    if (std::error_code error; std::filesystem::is_directory(root, error)) {
        return root;
    }
#endif
    return std::filesystem::temp_directory_path();
}

// Unique across processes (a random start) and within one (a counter).
std::string uniqueSuffix() {
    static const uint64_t processSeed = std::random_device{}() ^ (uint64_t{std::random_device{}()} << 32);
    static std::atomic<uint64_t> counter{0};
    return std::to_string(processSeed) + "-" + std::to_string(counter.fetch_add(1));
}

} // namespace

TempDirectory::TempDirectory(std::string_view name) {
    const std::filesystem::path component(name);
    if (name.empty() || component.has_parent_path() || component.has_root_path()) {
        throw std::invalid_argument("TempDirectory needs a single path component, not '" + std::string(name) + "'");
    }
    m_path = temporaryRoot() / (std::string(name) + "-" + uniqueSuffix());
    std::error_code error;
    std::filesystem::create_directories(m_path, error);
    if (error) {
        throw std::runtime_error("Could not create " + m_path.string() + ": " + error.message());
    }
}

TempDirectory::~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
}

const std::filesystem::path& TempDirectory::path() const {
    return m_path;
}

std::filesystem::path TempDirectory::operator/(const std::filesystem::path& relative) const {
    return m_path / relative;
}

} // namespace testsupport

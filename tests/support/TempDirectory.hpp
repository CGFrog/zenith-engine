#pragma once

// TempDirectory: a test's own scratch directory under the build tree's test-temp/, removed when it goes.

#include <filesystem>
#include <string_view>

namespace testsupport {

// A fresh, empty directory that only this object owns: test-temp/<name>-<unique>
// under the build tree (ZENITH_TEST_TEMP_DIR), or under the system temporary
// directory outside a build tree. Two tests, or two runs of one test, never
// share it, so a test needs no cleanup of its own and never clears another's
// files. The destructor removes it and everything in it.
class TempDirectory {
  public:
    // name says which test owns it ("prefab-save"); it must be a single path
    // component.
    explicit TempDirectory(std::string_view name);
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const;
    // path() / relative.
    std::filesystem::path operator/(const std::filesystem::path& relative) const;

  private:
    std::filesystem::path m_path;
};

} // namespace testsupport

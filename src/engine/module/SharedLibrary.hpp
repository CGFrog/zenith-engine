#pragma once

#include <filesystem>
#include <string>

struct SharedLibraryResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Owns one handle to a shared library loaded with LoadLibrary or dlopen and
// closes it on destruction. Code and data reached through symbol() are valid
// only while the handle is open.
class SharedLibrary {
  public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    // Replaces any library this object holds. On Windows, dependencyDirectory
    // (when not empty) is also searched for the libraries this one imports,
    // for a library loaded from a copy outside its original directory. Other
    // platforms resolve dependencies through the library's own search paths.
    SharedLibraryResult open(const std::filesystem::path& path, const std::filesystem::path& dependencyDirectory = {});
    void close();
    // Gives up the handle without closing it, so the library stays mapped for
    // the rest of the process. For code that may still be referenced.
    void leak();

    bool isOpen() const;
    // The exported symbol's address, or null when the library does not export
    // it.
    void* symbol(const char* name) const;

  private:
    void* m_handle{nullptr};
};

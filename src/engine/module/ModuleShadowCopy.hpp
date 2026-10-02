#pragma once

#include <filesystem>
#include <string>

struct ModuleShadowCopyResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// A uniquely named copy of a module library, loaded in place of the original
// so the original can be rebuilt while the module is in use.
//
// When the library names a program database (a Windows PDB) that exists, the
// database is copied beside the library under the copy's unique name and the
// copy is patched to name it. A debugger then locks the copied database, not
// the one the linker rewrites on the next build.
//
// The copy deletes its files when destroyed, unless keep() was called; destroy
// it only after the library has been unloaded.
class ModuleShadowCopy {
  public:
    ModuleShadowCopy() = default;
    ~ModuleShadowCopy();

    ModuleShadowCopy(ModuleShadowCopy&& other) noexcept;
    ModuleShadowCopy& operator=(ModuleShadowCopy&& other) noexcept;
    ModuleShadowCopy(const ModuleShadowCopy&) = delete;
    ModuleShadowCopy& operator=(const ModuleShadowCopy&) = delete;

    // Copies source into directory, creating directory when needed. On
    // Windows, copies of source left in directory by earlier processes are
    // deleted first unless a process still has them loaded.
    static ModuleShadowCopyResult create(const std::filesystem::path& source, const std::filesystem::path& directory,
                                         ModuleShadowCopy& copy);

    const std::filesystem::path& library() const;
    // The copied program database; empty when the library names none or the
    // named file does not exist.
    const std::filesystem::path& debugSymbols() const;

    // Leaves the files on disk when this object is destroyed, for a library
    // that could not be unloaded.
    void keep();

  private:
    std::filesystem::path m_library;
    std::filesystem::path m_debugSymbols;

    void remove();
};

#include "SharedLibrary.hpp"

#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
#if defined(_WIN32)
std::string lastErrorMessage() {
    const DWORD code = GetLastError();
    LPSTR buffer = nullptr;
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = length > 0 && buffer != nullptr ? std::string(buffer, length) : std::string();
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return "error " + std::to_string(code) + (message.empty() ? std::string() : ": " + message);
}
#endif
} // namespace

SharedLibrary::~SharedLibrary() {
    close();
}

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
}

SharedLibraryResult SharedLibrary::open(const std::filesystem::path& path,
                                        const std::filesystem::path& dependencyDirectory) {
    close();
#if defined(_WIN32)
    // Resolve the library's own dependencies from its directory, the extra
    // dependency directory, then the application directory and system paths.
    const std::filesystem::path libraryPath = path.lexically_normal();
    DLL_DIRECTORY_COOKIE cookie = nullptr;
    if (!dependencyDirectory.empty()) {
        cookie = AddDllDirectory(dependencyDirectory.lexically_normal().c_str());
    }
    DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (cookie != nullptr) {
        flags |= LOAD_LIBRARY_SEARCH_USER_DIRS;
    }
    HMODULE handle = LoadLibraryExW(libraryPath.c_str(), nullptr, flags);
    const std::string loadError = handle == nullptr ? lastErrorMessage() : std::string();
    if (cookie != nullptr) {
        RemoveDllDirectory(cookie);
    }
    if (handle == nullptr) {
        return {.error = "Could not load '" + path.string() + "' (" + loadError + ")"};
    }
    m_handle = handle;
#else
    static_cast<void>(dependencyDirectory);
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        return {.error =
                    "Could not load '" + path.string() + "' (" + (error != nullptr ? error : "unknown error") + ")"};
    }
    m_handle = handle;
#endif
    return {.success = true};
}

void SharedLibrary::close() {
    if (m_handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void SharedLibrary::leak() {
    m_handle = nullptr;
}

bool SharedLibrary::isOpen() const {
    return m_handle != nullptr;
}

void* SharedLibrary::symbol(const char* name) const {
    if (m_handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    return dlsym(m_handle, name);
#endif
}

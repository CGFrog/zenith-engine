#include "ResourceLocator.hpp"

#include <SDL3/SDL_stdinc.h>
#include <mutex>
#include <system_error>
#include <vector>

namespace {
std::mutex locatorMutex;
std::filesystem::path locatedRoot;

bool isResourceRoot(const std::filesystem::path& candidate) {
    std::error_code error;
    return std::filesystem::is_directory(candidate / "assets", error) &&
           std::filesystem::is_directory(candidate / "shaders", error);
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}
} // namespace

void initializeResourceLocator(const std::filesystem::path& executablePath) {
    std::scoped_lock lock(locatorMutex);
    std::vector<std::filesystem::path> candidates;
    if (const char* overrideRoot = SDL_getenv("ZENITH_RESOURCE_ROOT"); overrideRoot && *overrideRoot) {
        candidates.emplace_back(overrideRoot);
    }

    std::filesystem::path executableDirectory = executablePath;
    if (executableDirectory.has_filename()) {
        executableDirectory = executableDirectory.parent_path();
    }
    if (executableDirectory.empty()) {
        executableDirectory = std::filesystem::current_path();
    } else if (executableDirectory.is_relative()) {
        executableDirectory = std::filesystem::absolute(executableDirectory);
    }
    candidates.push_back(executableDirectory);
    candidates.push_back(executableDirectory.parent_path());
#ifdef __APPLE__
    candidates.push_back(executableDirectory.parent_path() / "Resources");
    candidates.push_back(executableDirectory.parent_path().parent_path().parent_path());
#endif
    candidates.push_back(std::filesystem::current_path());

    for (const std::filesystem::path& candidate : candidates) {
        if (isResourceRoot(candidate)) {
            locatedRoot = normalized(candidate);
            return;
        }
    }
    locatedRoot = normalized(executableDirectory);
}

std::filesystem::path resourceRoot() {
    std::scoped_lock lock(locatorMutex);
    if (locatedRoot.empty()) {
        locatedRoot = normalized(std::filesystem::current_path());
    }
    return locatedRoot;
}

std::filesystem::path assetRoot() {
    return resourceRoot() / "assets";
}

std::filesystem::path shaderRoot() {
    return resourceRoot() / "shaders";
}

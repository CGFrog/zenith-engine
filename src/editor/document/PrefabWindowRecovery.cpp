#include "PrefabWindowRecovery.hpp"

#include "engine/assets/Assets.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
constexpr std::string_view recoveryExtension = ".prefab-edit";

// The number a recovery file is named with, or 0 for any other file.
unsigned long recoveryNumber(const std::filesystem::path& file) {
    if (file.extension() != recoveryExtension) {
        return 0;
    }
    const std::string stem = file.stem().string();
    char* end = nullptr;
    const unsigned long number = std::strtoul(stem.c_str(), &end, 10);
    return end != nullptr && *end == '\0' ? number : 0;
}

// The recovery files in directory, by number.
std::vector<std::filesystem::path> recoveryFiles(const std::filesystem::path& directory) {
    std::vector<std::pair<unsigned long, std::filesystem::path>> numbered;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (const unsigned long number = recoveryNumber(it->path()); number > 0 && it->is_regular_file(error)) {
            numbered.emplace_back(number, it->path());
        }
    }
    std::ranges::sort(numbered, {}, &std::pair<unsigned long, std::filesystem::path>::first);
    std::vector<std::filesystem::path> files;
    files.reserve(numbered.size());
    for (auto& entry : numbered) {
        files.push_back(std::move(entry.second));
    }
    return files;
}
} // namespace

PrefabWindowRecovery::PrefabWindowRecovery(std::filesystem::path directory)
    : m_directory(std::move(directory)) {}

SceneDocumentResult PrefabWindowRecovery::autosave(std::span<const PrefabWindowAutosave> windows,
                                                   const AssetManager& assets) const {
    SceneDocumentResult result{.success = true};
    if (std::string error; !discard(error)) {
        result = {.error = std::move(error)};
    }
    if (windows.empty()) {
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        return {.error = "Could not create '" + m_directory.string() + "': " + error.message()};
    }
    size_t number = 1;
    for (const PrefabWindowAutosave& window : windows) {
        const PrefabEditRecovery recovery(m_directory /
                                          (std::to_string(number++) + std::string(recoveryExtension)));
        if (SceneDocumentResult saved = recovery.autosave(window.scene, assets, window.options, window.note);
            !saved && result) {
            result = std::move(saved);
        }
    }
    return result;
}

std::vector<PrefabEditRecovery> PrefabWindowRecovery::available() const {
    std::vector<PrefabEditRecovery> recoveries;
    for (std::filesystem::path& file : recoveryFiles(m_directory)) {
        PrefabEditRecovery recovery(std::move(file));
        if (recovery.available()) {
            recoveries.push_back(std::move(recovery));
        }
    }
    return recoveries;
}

bool PrefabWindowRecovery::discard(std::string& error) const {
    bool discarded = true;
    for (std::filesystem::path& file : recoveryFiles(m_directory)) {
        if (std::string fileError; !PrefabEditRecovery(std::move(file)).discard(fileError) && discarded) {
            error = std::move(fileError);
            discarded = false;
        }
    }
    return discarded;
}

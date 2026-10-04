// The file watcher's shared path helpers and tree walk.
#include "engine/platform/FileWatchBackends.hpp"

#include <system_error>

namespace file_watch_detail {
std::string toGenericUtf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path pathFromUtf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string joinRelative(const std::string& directory, std::string_view name) {
    if (directory.empty()) {
        return std::string(name);
    }
    std::string joined = directory;
    joined += '/';
    joined += name;
    return joined;
}

bool isSameOrDescendant(std::string_view path, std::string_view directory) {
    if (!path.starts_with(directory)) {
        return false;
    }
    return path.size() == directory.size() || path[directory.size()] == '/';
}

bool isDescendant(std::string_view path, std::string_view directory) {
    return path.size() > directory.size() && isSameOrDescendant(path, directory);
}

// Replaces the `from` prefix of a path known to be `from` or below it with `to`.
std::string rebase(const std::string& path, const std::string& from, const std::string& to) {
    return to + path.substr(from.size());
}

#if defined(_WIN32) || defined(__linux__)
std::string systemErrorMessage(int code) {
    return std::system_category().message(code);
}
#endif

bool forEachEntryBelow(const fs::path& root, const std::string& relativeDirectory, const ChangeSink& sink,
                       const EntryVisitor& visit) {
    bool complete = true;
    std::vector<std::string> pendingDirectories{relativeDirectory};
    while (!pendingDirectories.empty()) {
        const std::string directory = std::move(pendingDirectories.back());
        pendingDirectories.pop_back();
        const fs::path directoryPath = directory.empty() ? root : root / pathFromUtf8(directory);
        std::error_code error;
        fs::directory_iterator iterator(directoryPath, fs::directory_options::skip_permission_denied, error);
        for (; !error && iterator != fs::directory_iterator(); iterator.increment(error)) {
            const fs::directory_entry& entry = *iterator;
            const std::string relativePath = joinRelative(directory, toGenericUtf8(entry.path().filename()));
            if (sink.isIgnored(relativePath)) {
                continue;
            }
            std::error_code typeError;
            const bool isDirectory = entry.is_directory(typeError);
            visit(relativePath, entry, isDirectory);
            if (isDirectory && !entry.is_symlink(typeError)) {
                pendingDirectories.push_back(relativePath);
            }
        }
        std::error_code existsError;
        if (error && fs::is_directory(directoryPath, existsError)) {
            complete = false;
        }
    }
    return complete;
}
} // namespace file_watch_detail

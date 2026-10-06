#include "NativeFileDialog.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keyboard.h>

#include <mutex>
#include <utility>

namespace {
// SDL exchanges paths as UTF-8; std::filesystem::path uses the native
// encoding, which differs on Windows.
std::filesystem::path pathFromUtf8(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}
} // namespace

struct NativeFileDialog::State {
    std::mutex mutex;
    std::optional<FileDialogOutcome> outcome;
};

struct NativeFileDialog::CallbackContext {
    std::shared_ptr<State> state;
    FileDialogKind kind{};
};

std::filesystem::path normalizeSavePath(std::filesystem::path path, const std::filesystem::path& extension) {
    if (!path.empty() && path.extension() != extension) {
        path += extension;
    }
    return path;
}

NativeFileDialog::NativeFileDialog(std::string filterName, std::filesystem::path extension)
    : m_extension(std::move(extension))
    , m_state(std::make_shared<State>()) {
    if (!m_extension.empty()) {
        m_filters.push_back({.name = std::move(filterName), .extensions = {utf8FromPath(m_extension).substr(1)}});
    }
}

NativeFileDialog::NativeFileDialog(std::vector<FileDialogFilter> filters)
    : m_filters(std::move(filters))
    , m_state(std::make_shared<State>()) {}

std::string fileDialogPattern(const std::vector<std::string>& extensions) {
    std::string pattern;
    for (const std::string& extension : extensions) {
        if (!pattern.empty()) {
            pattern += ';';
        }
        pattern += extension;
    }
    return pattern;
}

bool NativeFileDialog::request(FileDialogKind kind, const std::filesystem::path& defaultLocation, bool allowMany) {
    if (m_pending) {
        return false;
    }
    m_pending = true;
    // SDL may call back on another thread; the heap-allocated context keeps the
    // shared state alive until the callback runs and deletes it.
    auto* context = new CallbackContext{.state = m_state, .kind = kind};
    const SDL_DialogFileCallback callback = [](void* userdata, const char* const* files, int) {
        const std::unique_ptr<CallbackContext> owned(static_cast<CallbackContext*>(userdata));
        FileDialogOutcome outcome{.kind = owned->kind};
        if (files == nullptr) {
            outcome.status = FileDialogStatus::Failed;
            outcome.error = std::string("File dialog failed: ") + SDL_GetError();
        } else if (files[0] == nullptr) {
            outcome.status = FileDialogStatus::Cancelled;
        } else {
            outcome.status = FileDialogStatus::Chosen;
            for (const char* const* file = files; *file != nullptr; ++file) {
                outcome.paths.push_back(pathFromUtf8(*file));
            }
            outcome.path = outcome.paths.front();
        }
        const std::scoped_lock lock(owned->state->mutex);
        owned->state->outcome = std::move(outcome);
    };

    // An empty pattern is SDL's "every file"; it is NULL-terminated per filter.
    std::vector<std::string> patterns;
    patterns.reserve(m_filters.size());
    std::vector<SDL_DialogFileFilter> filters;
    for (const FileDialogFilter& filter : m_filters) {
        patterns.push_back(filter.extensions.empty() ? std::string("*") : fileDialogPattern(filter.extensions));
        filters.push_back({.name = filter.name.c_str(), .pattern = patterns.back().c_str()});
    }
    const std::string location = utf8FromPath(defaultLocation);
    const char* locationArgument = location.empty() ? nullptr : location.c_str();
    // The focused editor window becomes the dialog's parent so it stays on top.
    SDL_Window* parent = SDL_GetKeyboardFocus();
    const int filterCount = static_cast<int>(filters.size());
    if (kind == FileDialogKind::Save) {
        SDL_ShowSaveFileDialog(callback, context, parent, filters.empty() ? nullptr : filters.data(), filterCount,
                               locationArgument);
    } else {
        SDL_ShowOpenFileDialog(callback, context, parent, filters.empty() ? nullptr : filters.data(), filterCount,
                               locationArgument, allowMany);
    }
    return true;
}

bool NativeFileDialog::pending() const {
    return m_pending;
}

std::optional<FileDialogOutcome> NativeFileDialog::poll() {
    if (!m_pending) {
        return std::nullopt;
    }
    std::optional<FileDialogOutcome> outcome;
    {
        const std::scoped_lock lock(m_state->mutex);
        outcome = std::exchange(m_state->outcome, std::nullopt);
    }
    if (!outcome) {
        return std::nullopt;
    }
    m_pending = false;
    if (outcome->status == FileDialogStatus::Chosen && outcome->kind == FileDialogKind::Save && !m_extension.empty()) {
        outcome->path = normalizeSavePath(std::move(outcome->path), m_extension);
        outcome->paths = {outcome->path};
    }
    return outcome;
}

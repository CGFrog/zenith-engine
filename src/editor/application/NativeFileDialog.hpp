#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

enum class FileDialogKind {
    Open,
    Save,
};

enum class FileDialogStatus {
    Chosen,
    Cancelled,
    Failed,
};

struct FileDialogOutcome {
    FileDialogKind kind{};
    FileDialogStatus status{};
    std::filesystem::path path;
    std::string error;
};

// Returns the path a Save dialog should write, appending the required
// extension when the user typed a bare file name.
std::filesystem::path normalizeSavePath(std::filesystem::path path, const std::filesystem::path& extension);

// Shows the operating system's open/save dialog for one file type. Dialogs
// are asynchronous: the platform may report the choice from another thread,
// so the result is handed to the owner through poll() on the main thread.
// The shared state outlives this object if it is destroyed while a dialog is
// still open.
class NativeFileDialog {
  public:
    // extension includes the leading dot, for example ".zenith-scene".
    NativeFileDialog(std::string filterName, std::filesystem::path extension);

    // Returns false when a dialog is already open.
    bool request(FileDialogKind kind, const std::filesystem::path& defaultLocation);
    bool pending() const;
    std::optional<FileDialogOutcome> poll();

  private:
    struct State;
    struct CallbackContext;

    std::string m_filterName;
    std::filesystem::path m_extension;
    std::string m_filterPattern;
    std::shared_ptr<State> m_state;
    bool m_pending{false};
};

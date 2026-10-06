#pragma once

// Shows the operating system's open/save dialog for one file type.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class FileDialogKind {
    Open,
    Save,
    // Chooses an existing folder; filters do not apply.
    Folder,
};

enum class FileDialogStatus {
    Chosen,
    Cancelled,
    Failed,
};

struct FileDialogOutcome {
    FileDialogKind kind{};
    FileDialogStatus status{};
    // The first file chosen; paths holds every one (several only when the
    // request allowed it).
    std::filesystem::path path;
    std::vector<std::filesystem::path> paths;
    std::string error;
};

// One entry of a file dialog's type list. Extensions have no leading dot and
// are matched case-insensitively by the platform; none means every file.
struct FileDialogFilter {
    std::string name;
    std::vector<std::string> extensions;
};

// The platform filter pattern for extensions: "png;jpg".
std::string fileDialogPattern(const std::vector<std::string>& extensions);

// Returns the path a Save dialog should write, appending the required
// extension when the user typed a bare file name.
std::filesystem::path normalizeSavePath(std::filesystem::path path, const std::filesystem::path& extension);

// The folder a native dialog should start in: absolute, with a trailing
// separator that marks it as a folder with no suggested file name. Empty
// (the platform default) when folder is empty or not an existing directory.
std::filesystem::path dialogStartFolder(const std::filesystem::path& folder);

// Shows the operating system's open/save dialog for one file type. Dialogs
// are asynchronous: the platform may report the choice from another thread,
// so the result is handed to the owner through poll() on the main thread.
// The shared state outlives this object if it is destroyed while a dialog is
// still open.
class NativeFileDialog {
  public:
    // extension includes the leading dot, for example ".zenith-scene"; an
    // empty extension offers every file.
    NativeFileDialog(std::string filterName, std::filesystem::path extension);

    // Offers each filter in turn (the first is selected). Open dialogs only:
    // a Save dialog's result is not given an extension.
    explicit NativeFileDialog(std::vector<FileDialogFilter> filters);

    // Returns false when a dialog is already open. allowMany lets an Open
    // or Folder dialog choose several entries.
    bool request(FileDialogKind kind, const std::filesystem::path& defaultLocation, bool allowMany = false);
    bool pending() const;
    std::optional<FileDialogOutcome> poll();

  private:
    struct State;
    struct CallbackContext;

    std::vector<FileDialogFilter> m_filters;
    // The Save dialog's required extension; empty when built from filters.
    std::filesystem::path m_extension;
    std::shared_ptr<State> m_state;
    bool m_pending{false};
};

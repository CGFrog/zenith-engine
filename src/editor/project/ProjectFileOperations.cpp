#include "ProjectFileOperations.hpp"

#include "engine/scene/Scene.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {
constexpr int maximumUniqueNameAttempts = 10000;

std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

ProjectFileResult failure(std::string message) {
    return {.error = std::move(message)};
}

ProjectFileResult succeeded(std::filesystem::path path) {
    return {.success = true, .path = std::move(path)};
}

std::string quoted(const std::filesystem::path& path) {
    return "'" + utf8PathText(path) + "'";
}

// Moves without replacing an existing destination.
std::error_code moveWithoutReplacing(const std::filesystem::path& source, const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (MoveFileExW(source.c_str(), destination.c_str(), 0) == 0) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    return {};
#else
    // POSIX rename replaces files, so the caller's existence check is the
    // guard; the window between the check and the rename is accepted.
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    return error;
#endif
}

#if defined(_WIN32)
std::string windowsErrorText(HRESULT result) {
    return std::system_category().message(static_cast<int>(result));
}

// Releases a COM interface when it leaves scope.
template <typename Interface> class ComReference {
  public:
    ComReference() = default;

    ~ComReference() {
        if (m_pointer != nullptr) {
            m_pointer->Release();
        }
    }

    ComReference(const ComReference&) = delete;
    ComReference& operator=(const ComReference&) = delete;

    Interface* operator->() const {
        return m_pointer;
    }

    Interface* get() const {
        return m_pointer;
    }

    void** out() {
        return reinterpret_cast<void**>(&m_pointer);
    }

  private:
    Interface* m_pointer{nullptr};
};

// Initializes COM for this thread when nobody has yet. A thread already in
// another apartment mode keeps it; shell operations work in either.
class ComApartment {
  public:
    ComApartment()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

    ~ComApartment() {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool usable() const {
        return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }

    HRESULT result() const {
        return m_result;
    }

  private:
    HRESULT m_result;
};

ProjectFileResult moveToRecycleBin(const std::filesystem::path& target) {
    const ComApartment apartment;
    if (!apartment.usable()) {
        return failure("Could not start the Windows shell: " + windowsErrorText(apartment.result()));
    }
    ComReference<IFileOperation> operation;
    HRESULT result =
        CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, __uuidof(IFileOperation), operation.out());
    if (FAILED(result)) {
        return failure("Could not create a Windows file operation: " + windowsErrorText(result));
    }
    // FOF_WANTNUKEWARNING makes the shell ask before deleting permanently when
    // the item cannot go to the Recycle Bin (for example on a network share).
    result = operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                                          FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE);
    ComReference<IShellItem> item;
    if (SUCCEEDED(result)) {
        result = SHCreateItemFromParsingName(target.c_str(), nullptr, __uuidof(IShellItem), item.out());
    }
    if (SUCCEEDED(result)) {
        result = operation->DeleteItem(item.get(), nullptr);
    }
    if (SUCCEEDED(result)) {
        result = operation->PerformOperations();
    }
    if (FAILED(result)) {
        return failure("Could not move " + quoted(target) + " to the Recycle Bin: " + windowsErrorText(result));
    }
    BOOL aborted = FALSE;
    std::error_code error;
    if (FAILED(operation->GetAnyOperationsAborted(&aborted)) || aborted || std::filesystem::exists(target, error)) {
        return failure("Moving " + quoted(target) + " to the Recycle Bin was cancelled or did not complete.");
    }
    return succeeded(target);
}

ProjectFileResult revealWithExplorer(const std::filesystem::path& target) {
    const std::wstring parameters = L"/select,\"" + target.native() + L"\"";
    const HINSTANCE launched =
        ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        return failure("Could not open Explorer for " + quoted(target) + ".");
    }
    return succeeded(target);
}
#elif defined(__linux__)
std::filesystem::path xdgTrashDirectory() {
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && dataHome[0] == '/') {
        return std::filesystem::path(dataHome) / "Trash";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
        return std::filesystem::path(home) / ".local" / "share" / "Trash";
    }
    return {};
}

// Percent-encodes a path for the trashinfo Path key, keeping separators.
std::string trashInfoPath(const std::filesystem::path& path) {
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : path.native()) {
        if (std::isalnum(character) || character == '/' || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            encoded += static_cast<char>(character);
        } else {
            encoded += '%';
            encoded += hexDigits[character >> 4];
            encoded += hexDigits[character & 0x0F];
        }
    }
    return encoded;
}

std::string deletionDate() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &local);
    return text.data();
}

// Claims a unique name in the trash by creating its info file exclusively,
// as the XDG trash specification requires.
ProjectFileResult claimTrashName(const std::filesystem::path& trash, const std::filesystem::path& target,
                                 std::string& name) {
    const std::string stem = target.stem().native();
    const std::string extension = target.extension().native();
    const std::string contents =
        "[Trash Info]\nPath=" + trashInfoPath(target) + "\nDeletionDate=" + deletionDate() + "\n";
    for (int attempt = 1; attempt <= maximumUniqueNameAttempts; ++attempt) {
        name = attempt == 1 ? target.filename().native() : stem + "." + std::to_string(attempt) + extension;
        const std::filesystem::path info = trash / "info" / (name + ".trashinfo");
        const int descriptor = open(info.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST) {
                continue;
            }
            return failure("Could not write trash information in " + quoted(trash) + ": " +
                           std::generic_category().message(errno));
        }
        const ssize_t written = write(descriptor, contents.data(), contents.size());
        close(descriptor);
        if (written != static_cast<ssize_t>(contents.size())) {
            unlink(info.c_str());
            return failure("Could not write trash information in " + quoted(trash) + ".");
        }
        return succeeded(info);
    }
    return failure("Could not find a free name in the trash for " + quoted(target) + ".");
}

ProjectFileResult moveToXdgTrash(const std::filesystem::path& target) {
    const std::filesystem::path trash = xdgTrashDirectory();
    if (trash.empty()) {
        return failure("Could not find the trash: neither XDG_DATA_HOME nor HOME is set.");
    }
    std::error_code error;
    std::filesystem::create_directories(trash / "files", error);
    if (!error) {
        std::filesystem::create_directories(trash / "info", error);
    }
    if (error) {
        return failure("Could not create the trash at " + quoted(trash) + ": " + error.message());
    }
    std::string name;
    const ProjectFileResult claimed = claimTrashName(trash, target, name);
    if (!claimed) {
        return claimed;
    }
    std::filesystem::rename(target, trash / "files" / name, error);
    if (error) {
        unlink(claimed.path.c_str());
        if (error == std::errc::cross_device_link) {
            return failure(quoted(target) + " is on a different file system from the trash at " + quoted(trash) +
                           "; it was not deleted.");
        }
        return failure("Could not move " + quoted(target) + " to the trash: " + error.message());
    }
    return succeeded(target);
}

ProjectFileResult revealWithXdgOpen(const std::filesystem::path& target) {
    const std::filesystem::path folder = target.parent_path();
    // The shell starts xdg-open in the background and exits at once, so the
    // editor never waits on the file manager. The folder is passed as an
    // argument, never interpolated into the command.
    const std::string folderText = folder.native();
    const char* arguments[] = {"sh", "-c", "xdg-open \"$1\" >/dev/null 2>&1 &", "sh", folderText.c_str(), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawnp(&child, "sh", nullptr, nullptr, const_cast<char* const*>(arguments), environ);
    if (spawned != 0) {
        return failure("Could not run xdg-open: " + std::generic_category().message(spawned));
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return failure("Could not run xdg-open for " + quoted(folder) + ".");
    }
    return succeeded(folder);
}
#endif

bool existsOrFails(const std::filesystem::path& path, ProjectFileResult& result) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        result = failure("Could not inspect " + quoted(path) + ": " + error.message());
    }
    return exists || error;
}
} // namespace

std::string utf8PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path pathFromUtf8Text(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string invalidFileNameReason(std::string_view utf8Name) {
    if (utf8Name.empty()) {
        return "A name cannot be empty.";
    }
    if (utf8Name == "." || utf8Name == "..") {
        return "'" + std::string(utf8Name) + "' is reserved.";
    }
    for (const char character : utf8Name) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20 || std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos) {
            return "A name cannot contain control characters or any of < > : \" / \\ | ? *";
        }
    }
    if (utf8Name.back() == ' ' || utf8Name.back() == '.') {
        return "A name cannot end with a space or a period.";
    }
    // Windows reserves device names regardless of extension.
    static constexpr std::array<std::string_view, 22> deviceNames{
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    const std::string stem = lowercase(std::string(utf8Name.substr(0, utf8Name.find('.'))));
    if (std::ranges::find(deviceNames, stem) != deviceNames.end()) {
        return "'" + std::string(utf8Name) + "' is a reserved device name.";
    }
    return {};
}

ProjectFileResult uniqueChildPath(const std::filesystem::path& folder, std::string_view utf8Base,
                                  const std::filesystem::path& extension) {
    for (int attempt = 1; attempt <= maximumUniqueNameAttempts; ++attempt) {
        std::string name(utf8Base);
        if (attempt > 1) {
            name += " " + std::to_string(attempt);
        }
        std::filesystem::path candidate = folder / pathFromUtf8Text(name);
        candidate += extension;
        ProjectFileResult result;
        if (!existsOrFails(candidate, result)) {
            return succeeded(std::move(candidate));
        }
        if (!result.error.empty()) {
            return result;
        }
    }
    return failure("Could not find a free name in " + quoted(folder) + ".");
}

ProjectFileResult createProjectFolder(const std::filesystem::path& parent) {
    ProjectFileResult target = uniqueChildPath(parent, "New Folder", {});
    if (!target) {
        return target;
    }
    std::error_code error;
    if (!std::filesystem::create_directory(target.path, error) || error) {
        return failure("Could not create folder " + quoted(target.path) + ": " +
                       (error ? error.message() : std::string("it already exists")));
    }
    return target;
}

ProjectFileResult createProjectScene(const std::filesystem::path& folder, const std::filesystem::path& assetRoot) {
    ProjectFileResult target = uniqueChildPath(folder, "New Scene", ".zenith-scene");
    if (!target) {
        return target;
    }
    const Scene scene;
    AssetManager assets;
    if (const SceneDocumentResult saved = SceneDocument::save(target.path, scene, assets, {.assetRoot = assetRoot});
        !saved) {
        return failure("Could not create scene " + quoted(target.path) + ": " + saved.error);
    }
    return target;
}

ProjectFileResult renameProjectItem(const std::filesystem::path& source, std::string_view utf8NewName) {
    if (const std::string reason = invalidFileNameReason(utf8NewName); !reason.empty()) {
        return failure("Cannot rename " + quoted(source) + ": " + reason);
    }
    std::error_code error;
    const bool isFolder = std::filesystem::is_directory(source, error);
    if (error || (!isFolder && !std::filesystem::exists(source, error))) {
        return failure("Cannot rename " + quoted(source) + ": it no longer exists.");
    }
    std::filesystem::path name = pathFromUtf8Text(utf8NewName);
    const std::filesystem::path extension = source.extension();
    if (!isFolder && lowercase(utf8PathText(name.extension())) != lowercase(utf8PathText(extension))) {
        name += extension;
    }
    const std::filesystem::path destination = source.parent_path() / name;
    if (destination == source) {
        return succeeded(destination);
    }
    // A name differing only in case is the same item on case-insensitive file
    // systems; renaming it is allowed.
    const bool sameItem = std::filesystem::equivalent(source, destination, error);
    if (!sameItem) {
        ProjectFileResult inspected;
        if (existsOrFails(destination, inspected)) {
            return inspected.error.empty()
                       ? failure("Cannot rename " + quoted(source) + ": " + quoted(destination) + " already exists.")
                       : inspected;
        }
    }
    if (const std::error_code moved = moveWithoutReplacing(source, destination)) {
        return failure("Could not rename " + quoted(source) + ": " + moved.message());
    }
    return succeeded(destination);
}

ProjectFileResult duplicateProjectItem(const std::filesystem::path& source) {
    std::error_code error;
    const bool isFolder = std::filesystem::is_directory(source, error);
    if (error || (!isFolder && !std::filesystem::is_regular_file(source, error))) {
        return failure("Cannot duplicate " + quoted(source) + ": it is not a file or folder.");
    }
    const std::filesystem::path extension = isFolder ? std::filesystem::path{} : source.extension();
    const std::string base = utf8PathText(isFolder ? source.filename() : source.stem()) + " copy";
    ProjectFileResult target = uniqueChildPath(source.parent_path(), base, extension);
    if (!target) {
        return target;
    }
    if (!isFolder) {
        // copy_options::none refuses to replace an existing file.
        std::filesystem::copy_file(source, target.path, std::filesystem::copy_options::none, error);
    } else if (std::filesystem::create_directory(target.path, error) && !error) {
        std::filesystem::copy(source, target.path, std::filesystem::copy_options::recursive, error);
        if (error) {
            // Only the incomplete copy this call created is removed.
            std::error_code ignored;
            std::filesystem::remove_all(target.path, ignored);
        }
    } else if (!error) {
        error = std::make_error_code(std::errc::file_exists);
    }
    if (error) {
        return failure("Could not duplicate " + quoted(source) + ": " + error.message());
    }
    return target;
}

ProjectFileResult moveProjectItemToTrash(const std::filesystem::path& target) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(target, error).lexically_normal();
    if (error || !std::filesystem::exists(absolute, error) || error) {
        return failure("Cannot delete " + quoted(target) + ": it does not exist.");
    }
#if defined(_WIN32)
    return moveToRecycleBin(absolute);
#elif defined(__linux__)
    return moveToXdgTrash(absolute);
#else
    return failure("Moving " + quoted(target) + " to the trash is not supported on this platform.");
#endif
}

ProjectFileResult revealInFileBrowser(const std::filesystem::path& target) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(target, error).lexically_normal();
    if (error || !std::filesystem::exists(absolute, error) || error) {
        return failure("Cannot show " + quoted(target) + ": it does not exist.");
    }
#if defined(_WIN32)
    return revealWithExplorer(absolute.has_filename() ? absolute : absolute.parent_path());
#elif defined(__linux__)
    return revealWithXdgOpen(absolute.has_filename() ? absolute : absolute.parent_path());
#else
    return failure("Showing files in the file browser is not supported on this platform.");
#endif
}

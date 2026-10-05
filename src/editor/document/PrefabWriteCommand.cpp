#include "PrefabWriteCommand.hpp"

#include "engine/platform/FileContents.hpp"
#include "engine/platform/PlatformServices.hpp"

#include <span>
#include <system_error>
#include <utility>

namespace {
std::string fileLabel(const std::filesystem::path& file) {
    return file.filename().generic_string();
}

// Puts content into file atomically, or removes the file for nullopt.
EditorCommandResult writeContent(const std::filesystem::path& file, const std::optional<std::string>& content) {
    if (!content) {
        std::error_code error;
        std::filesystem::remove(file, error);
        if (error) {
            return {.error = "Could not remove " + fileLabel(file) + ": " + error.message()};
        }
        return {.success = true};
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(content->data());
    if (const PlatformResult written =
            atomicWriteUserData(file, std::span<const uint8_t>(bytes, content->size()), UserDataBackup::None);
        !written) {
        return {.error = "Could not write " + fileLabel(file) + ": " + written.error};
    }
    return {.success = true};
}
} // namespace

bool readPrefabFileBytes(const std::filesystem::path& file, std::optional<std::string>& bytes, std::string& error) {
    std::error_code status;
    if (!std::filesystem::exists(file, status)) {
        if (status) {
            error = "Could not inspect " + fileLabel(file) + ": " + status.message();
            return false;
        }
        bytes.reset();
        return true;
    }
    std::string content;
    if (readFileText(file, content) != FileReadStatus::Success) {
        error = "Could not read " + fileLabel(file);
        return false;
    }
    bytes = std::move(content);
    return true;
}

PrefabWriteCommand::PrefabWriteCommand(std::string name, PrefabFileWrite write, SceneChanges changes,
                                       PrefabWriteHooks hooks)
    : m_name(name)
    , m_write(std::move(write))
    , m_scene(std::move(name), std::move(changes))
    , m_hooks(std::move(hooks)) {}

std::string_view PrefabWriteCommand::name() const {
    return m_name;
}

EditorCommandResult PrefabWriteCommand::execute(EditorCommandContext& context) {
    return apply(context, m_write.before, m_write.after, true, "redo");
}

EditorCommandResult PrefabWriteCommand::undo(EditorCommandContext& context) {
    return apply(context, m_write.after, m_write.before, false, "undo");
}

EditorCommandResult PrefabWriteCommand::apply(EditorCommandContext& context, const std::optional<std::string>& from,
                                              const std::optional<std::string>& to, bool forward,
                                              std::string_view action) {
    std::optional<std::string> current;
    std::string error;
    if (!readPrefabFileBytes(m_write.file, current, error)) {
        return {.error = error + "; " + std::string(action) + " skipped"};
    }
    const bool atTarget = current == to;
    if (!atTarget && current != from) {
        return {.error =
                    fileLabel(m_write.file) + " changed on disk since this step; " + std::string(action) + " skipped"};
    }
    if (EditorCommandResult patched = forward ? m_scene.execute(context) : m_scene.undo(context); !patched) {
        return patched;
    }
    if (!atTarget) {
        if (EditorCommandResult written = writeContent(m_write.file, to); !written) {
            // Best effort: the scene goes back to match the file it kept.
            static_cast<void>(forward ? m_scene.undo(context) : m_scene.execute(context));
            return written;
        }
    }
    m_hooks.notify(m_write.file, to);
    return {.success = true};
}

size_t PrefabWriteCommand::storedBytes() const {
    const auto optionalBytes = [](const std::optional<std::string>& value) { return value ? value->capacity() : 0; };
    return sizeof(*this) + m_name.capacity() + optionalBytes(m_write.before) + optionalBytes(m_write.after) +
           m_scene.storedBytes();
}

std::string PrefabWriteLedger::key(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(file, error);
    if (error) {
        absolute = file;
    }
    return absolute.lexically_normal().generic_string();
}

void PrefabWriteLedger::record(const std::filesystem::path& file, std::optional<std::string> bytes) {
    m_contents.insert_or_assign(key(file), std::move(bytes));
}

bool PrefabWriteLedger::matches(const std::filesystem::path& file, const std::optional<std::string>& bytes) const {
    const std::optional<std::string>* known = recorded(file);
    return known != nullptr && *known == bytes;
}

const std::optional<std::string>* PrefabWriteLedger::recorded(const std::filesystem::path& file) const {
    const auto found = m_contents.find(key(file));
    return found == m_contents.end() ? nullptr : &found->second;
}

void PrefabWriteLedger::forget(const std::filesystem::path& file) {
    m_contents.erase(key(file));
}

void PrefabWriteLedger::clear() {
    m_contents.clear();
}

PrefabResult classifyPrefabFileChange(PrefabWriteLedger& ledger, const std::filesystem::path& file,
                                      const std::filesystem::path& assetRoot, PrefabFileChange& output) {
    std::error_code error;
    std::filesystem::path root = std::filesystem::absolute(assetRoot, error).lexically_normal();
    std::filesystem::path absolute = std::filesystem::absolute(file, error).lexically_normal();
    const std::filesystem::path reference = absolute.lexically_relative(root);
    if (reference.empty() || *reference.begin() == "..") {
        return {.error = fileLabel(file) + " is not inside the project's asset root"};
    }
    std::optional<std::string> bytes;
    std::string readError;
    if (!readPrefabFileBytes(absolute, bytes, readError)) {
        return {.error = readError};
    }
    PrefabFileChange change{.external = !ledger.matches(absolute, bytes), .reference = reference};
    change.displayName = absolute.stem().generic_string();
    PrefabDocument current;
    if (bytes && deserializePrefab(*bytes, current).success && !current.displayName.empty()) {
        change.displayName = current.displayName;
    }
    if (change.external) {
        if (const std::optional<std::string>* known = ledger.recorded(absolute); known != nullptr && *known) {
            PrefabDocument previous;
            if (deserializePrefab(**known, previous).success) {
                change.previous = std::move(previous);
            }
        }
        ledger.record(absolute, std::move(bytes));
    }
    output = std::move(change);
    return {.success = true};
}

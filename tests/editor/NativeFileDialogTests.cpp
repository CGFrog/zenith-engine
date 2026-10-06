#include "editor/application/FolderPicker.hpp"
#include "editor/application/NativeFileDialog.hpp"

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const std::filesystem::path extension = ".zenith-scene";
    const std::filesystem::path folder = std::filesystem::path("scenes") / "levels";

    expect(normalizeSavePath(folder / "arena", extension) == folder / "arena.zenith-scene",
           "a bare save name should gain the scene extension");
    expect(normalizeSavePath(folder / "arena.zenith-scene", extension) == folder / "arena.zenith-scene",
           "a name that already has the scene extension should be kept as typed");
    expect(normalizeSavePath(folder / "arena.backup", extension) == folder / "arena.backup.zenith-scene",
           "a different extension should be preserved as part of the file name");
    expect(normalizeSavePath({}, extension).empty(), "an empty choice should stay empty");

    NativeFileDialog dialog("Zenith scene", extension);
    expect(!dialog.pending() && !dialog.poll(), "a dialog that was never requested should report nothing");

    expect(dialogStartFolder({}).empty(), "no folder should leave the platform's default start location");
    expect(dialogStartFolder(folder / "missing").empty(), "a missing folder should not be offered as a start location");
    const std::filesystem::path here = dialogStartFolder(std::filesystem::current_path());
    expect(!here.empty() && here.is_absolute() && here.filename().empty(),
           "an existing folder should become an absolute path ending in a separator");

    // Choosing a folder fills the field; cancelling and failing leave it, and
    // a failure is reported rather than dropped.
    FolderPicker picker;
    std::string field = "typed";
    const std::filesystem::path chosen = std::filesystem::path("games") / "demo";
    FileDialogOutcome outcome{.kind = FileDialogKind::Folder, .status = FileDialogStatus::Chosen, .path = chosen};
    expect(picker.apply(outcome, field) && field == chosen.string(), "a chosen folder should replace the field");
    expect(!picker.apply(outcome, field), "choosing the folder already shown is not a change");
    outcome.status = FileDialogStatus::Cancelled;
    outcome.path = "other";
    expect(!picker.apply(outcome, field) && field == chosen.string(), "cancelling should keep the field");
    outcome.status = FileDialogStatus::Failed;
    outcome.error = "no portal";
    expect(!picker.apply(outcome, field) && field == chosen.string() &&
               picker.error().find("no portal") != std::string::npos,
           "a failed dialog should keep the field and report why");

    const std::filesystem::path base = std::filesystem::path("work") / "build";
    expect(relativeIfInside(base / "release", base) == std::filesystem::path("release"),
           "a folder inside the base should be stored relative to it");
    expect(relativeIfInside(std::filesystem::path("work") / "other", base) == std::filesystem::path("work") / "other",
           "a folder outside the base should be stored as chosen");

    return failures == 0 ? 0 : 1;
}

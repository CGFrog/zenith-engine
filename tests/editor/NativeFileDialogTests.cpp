// The native file dialog's pure parts: save-name normalization, the start
// folder it offers, the folder picker that fills a settings field, and paths
// stored relative to a base.

#include "editor/application/FolderPicker.hpp"
#include "editor/application/NativeFileDialog.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>

namespace {

const std::filesystem::path sceneExtension = ".zenith-scene";
const std::filesystem::path levelsFolder = std::filesystem::path("scenes") / "levels";

} // namespace

TEST_SUITE_BEGIN("NativeFileDialog");

TEST_CASE("NativeFileDialog: a save name gains the scene extension") {
    const std::filesystem::path& extension = sceneExtension;
    const std::filesystem::path& folder = levelsFolder;
    CHECK_MESSAGE(normalizeSavePath(folder / "arena", extension) == folder / "arena.zenith-scene",
                  "a bare save name should gain the scene extension");
    CHECK_MESSAGE(normalizeSavePath(folder / "arena.zenith-scene", extension) == folder / "arena.zenith-scene",
                  "a name that already has the scene extension should be kept as typed");
    CHECK_MESSAGE(normalizeSavePath(folder / "arena.backup", extension) == folder / "arena.backup.zenith-scene",
                  "a different extension should be preserved as part of the file name");
    CHECK_MESSAGE(normalizeSavePath({}, extension).empty(), "an empty choice should stay empty");
}

TEST_CASE("NativeFileDialog: a dialog that was never requested reports nothing") {
    NativeFileDialog dialog("Zenith scene", sceneExtension);
    CHECK_MESSAGE((!dialog.pending() && !dialog.poll()), "a dialog that was never requested should report nothing");
}

TEST_CASE("NativeFileDialog: only an existing folder is offered as the start location") {
    const testsupport::TempDirectory root("native-dialog-start-folder");
    CHECK_MESSAGE(dialogStartFolder({}).empty(), "no folder should leave the platform's default start location");
    CHECK_MESSAGE(dialogStartFolder(root / levelsFolder / "missing").empty(),
                  "a missing folder should not be offered as a start location");
    const std::filesystem::path here = dialogStartFolder(root.path());
    CHECK_MESSAGE((!here.empty() && here.is_absolute() && here.filename().empty()),
                  "an existing folder should become an absolute path ending in a separator");
}

// Choosing a folder fills the field; cancelling and failing leave it, and
// a failure is reported rather than dropped.
TEST_CASE("NativeFileDialog: the folder picker fills the field only with a chosen folder") {
    FolderPicker picker;
    std::string field = "typed";
    const std::filesystem::path chosen = std::filesystem::path("games") / "demo";
    FileDialogOutcome outcome{.kind = FileDialogKind::Folder, .status = FileDialogStatus::Chosen, .path = chosen};
    CHECK_MESSAGE((picker.apply(outcome, field) && field == chosen.string()),
                  "a chosen folder should replace the field");
    CHECK_MESSAGE(!picker.apply(outcome, field), "choosing the folder already shown is not a change");
    outcome.status = FileDialogStatus::Cancelled;
    outcome.path = "other";
    CHECK_MESSAGE((!picker.apply(outcome, field) && field == chosen.string()), "cancelling should keep the field");
    outcome.status = FileDialogStatus::Failed;
    outcome.error = "no portal";
    CHECK_MESSAGE((!picker.apply(outcome, field) && field == chosen.string() &&
                   picker.error().find("no portal") != std::string::npos),
                  "a failed dialog should keep the field and report why");
}

TEST_CASE("NativeFileDialog: a folder inside the base is stored relative to it") {
    const std::filesystem::path base = std::filesystem::path("work") / "build";
    CHECK_MESSAGE(relativeIfInside(base / "release", base) == std::filesystem::path("release"),
                  "a folder inside the base should be stored relative to it");
    CHECK_MESSAGE(relativeIfInside(std::filesystem::path("work") / "other", base) ==
                      std::filesystem::path("work") / "other",
                  "a folder outside the base should be stored as chosen");
}

TEST_SUITE_END();

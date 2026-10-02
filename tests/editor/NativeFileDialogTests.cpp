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

    return failures == 0 ? 0 : 1;
}

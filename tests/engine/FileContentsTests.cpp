#include "engine/platform/FileContents.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace fs = std::filesystem;

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void write(const fs::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "zenith_file_contents_tests";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    // Binary content survives unchanged: embedded zeros and CR/LF pairs are
    // not text-translated.
    const std::string binary("a\0b\r\nc\xff", 7);
    write(root / "binary.bin", binary);
    std::vector<uint8_t> bytes{1, 2, 3};
    expect(readFileBytes(root / "binary.bin", bytes) == FileReadStatus::Success, "bytes: an existing file is read");
    expect(bytes.size() == binary.size() &&
               std::equal(bytes.begin(), bytes.end(), binary.begin(),
                          [](uint8_t byte, char character) { return byte == static_cast<uint8_t>(character); }),
           "bytes: contents are exact, replacing what the output held");

    std::string text = "stale";
    expect(readFileText(root / "binary.bin", text) == FileReadStatus::Success && text == binary,
           "text: contents are exact");

    write(root / "empty.txt", "");
    expect(readFileText(root / "empty.txt", text) == FileReadStatus::Success && text.empty(),
           "text: an empty file reads as empty");

    bytes = {4, 5};
    expect(readFileBytes(root / "missing.bin", bytes) == FileReadStatus::OpenFailed && bytes.empty(),
           "bytes: a missing file reports OpenFailed and clears the output");

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " file contents test(s) failed\n";
        return 1;
    }
    std::cout << "File contents tests passed\n";
    return 0;
}

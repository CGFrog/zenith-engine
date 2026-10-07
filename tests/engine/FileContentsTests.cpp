// Whole-file reads through FileContents: binary content survives unchanged,
// empty files read as empty, and a missing file reports OpenFailed.

#include "engine/platform/FileContents.hpp"

#include "support/Doctest.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("FileContents");

TEST_CASE("FileContents: binary content survives unchanged") {
    const testsupport::TempDirectory root("file-contents-binary");
    // Embedded zeros and CR/LF pairs are not text-translated.
    const std::string binary("a\0b\r\nc\xff", 7);
    testsupport::writeText(root / "binary.bin", binary);
    std::vector<uint8_t> bytes{1, 2, 3};
    CHECK_MESSAGE(readFileBytes(root / "binary.bin", bytes) == FileReadStatus::Success,
                  "bytes: an existing file is read");
    CHECK_MESSAGE((bytes.size() == binary.size() &&
                   std::equal(bytes.begin(), bytes.end(), binary.begin(),
                              [](uint8_t byte, char character) { return byte == static_cast<uint8_t>(character); })),
                  "bytes: contents are exact, replacing what the output held");

    std::string text = "stale";
    CHECK_MESSAGE((readFileText(root / "binary.bin", text) == FileReadStatus::Success && text == binary),
                  "text: contents are exact");
}

TEST_CASE("FileContents: an empty file reads as empty") {
    const testsupport::TempDirectory root("file-contents-empty");
    testsupport::writeText(root / "empty.txt", "");
    std::string text = "stale";
    CHECK_MESSAGE((readFileText(root / "empty.txt", text) == FileReadStatus::Success && text.empty()),
                  "text: an empty file reads as empty");
}

TEST_CASE("FileContents: a missing file reports open failed and clears the output") {
    const testsupport::TempDirectory root("file-contents-missing");
    std::vector<uint8_t> bytes{4, 5};
    CHECK_MESSAGE((readFileBytes(root / "missing.bin", bytes) == FileReadStatus::OpenFailed && bytes.empty()),
                  "bytes: a missing file reports OpenFailed and clears the output");
}

TEST_SUITE_END();

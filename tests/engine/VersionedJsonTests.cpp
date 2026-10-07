// Versioned JSON user data: reading a missing, invalid, older, or newer file,
// and writing one with its version stamped, optionally keeping a backup.

#include "engine/platform/VersionedJson.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using testsupport::readText;
using testsupport::TempDirectory;
using testsupport::writeText;

namespace {

bool contains(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

const VersionedJsonFormat settings{.description = "test settings", .firstVersion = 2, .currentVersion = 4};

} // namespace

TEST_SUITE_BEGIN("VersionedJson");

TEST_CASE("VersionedJson: a missing file uses the defaults or is an error") {
    const TempDirectory root("versioned-json-missing");
    const VersionedJsonResult defaults = readVersionedJson(root / "missing.json", settings);
    CHECK_MESSAGE((defaults && !defaults.found && defaults.document.is_null()), "a missing file uses the defaults");

    VersionedJsonFormat required = settings;
    required.missing = MissingJsonFile::Error;
    const VersionedJsonResult missing = readVersionedJson(root / "missing.json", required);
    CHECK_MESSAGE((!missing && !missing.found && contains(missing.error, "test settings")),
                  ("a required file that is missing is an error naming the file's content: " + missing.error));
}

TEST_CASE("VersionedJson: invalid envelopes are rejected with the reason") {
    const TempDirectory root("versioned-json-invalid");
    const auto rejected = [&](const std::string& name, const std::string& text, const std::string& reason) {
        writeText(root / name, text);
        const VersionedJsonResult result = readVersionedJson(root / name, settings);
        CHECK_MESSAGE((!result && result.found && contains(result.error, "Invalid test settings") &&
                       contains(result.error, reason)),
                      (name + " is rejected for '" + reason + "': " + result.error));
    };
    rejected("broken.json", "{ not json", "not a JSON object");
    rejected("array.json", "[1, 2]", "not a JSON object");
    rejected("unversioned.json", R"({"value": 1})", "no integer \"version\"");
    rejected("text_version.json", R"({"version": "4"})", "no integer \"version\"");
    rejected("old.json", R"({"version": 1})", "unsupported version 1");
    rejected("new.json", R"({"version": 5})", "unsupported version 5");
}

TEST_CASE("VersionedJson: older and newer versions and the version key") {
    const TempDirectory root("versioned-json-versions");
    writeText(root / "outdated.json", R"({"version": 2, "value": 7})");
    const VersionedJsonResult outdated = readVersionedJson(root / "outdated.json", settings);
    CHECK_MESSAGE((outdated && outdated.found && outdated.version == 2 && outdated.document.at("value") == 7),
                  "an older supported version is returned for the caller to migrate");

    VersionedJsonFormat forward = settings;
    forward.newer = NewerJsonVersions::Read;
    writeText(root / "newer.json", R"({"version": 9})");
    const VersionedJsonResult newer = readVersionedJson(root / "newer.json", forward);
    CHECK_MESSAGE((newer && newer.version == 9), "NewerJsonVersions::Read accepts a newer version");

    VersionedJsonFormat formatKey = settings;
    formatKey.versionKey = "formatVersion";
    writeText(root / "format.json", R"({"formatVersion": 3})");
    const VersionedJsonResult keyed = readVersionedJson(root / "format.json", formatKey);
    CHECK_MESSAGE((keyed && keyed.version == 3), "the version key is configurable");
}

TEST_CASE("VersionedJson: writing stamps the version and can keep a backup") {
    const TempDirectory root("versioned-json-writing");
    const fs::path path = root / "nested" / "saved.json";
    const PlatformResult written = writeVersionedJson(path, settings, {{"value", 3}, {"version", 1}});
    CHECK_MESSAGE(static_cast<bool>(written), ("a document is written, creating its folder: " + written.error));
    CHECK_MESSAGE(readText(path) == "{\n  \"value\": 3,\n  \"version\": 4\n}\n",
                  "the current version is stamped over the document's and the text ends in a newline");
    const VersionedJsonResult read = readVersionedJson(path, settings);
    CHECK_MESSAGE((read && read.version == 4 && read.document.at("value") == 3), "a written document reads back");
    CHECK_MESSAGE(!fs::exists(path.string() + ".bak"), "no backup by default");

    VersionedJsonFormat backedUp = settings;
    backedUp.backup = UserDataBackup::Keep;
    CHECK_MESSAGE(static_cast<bool>(writeVersionedJson(path, backedUp, {{"value", 5}})), "a document is replaced");
    CHECK_MESSAGE((fs::exists(path.string() + ".bak") && contains(readText(path.string() + ".bak"), "\"value\": 3")),
                  "UserDataBackup::Keep keeps the replaced file");
    CHECK_MESSAGE(!fs::exists(path.string() + ".tmp"), "the temporary file is gone");

    CHECK_MESSAGE(!writeVersionedJson(root / "array.json", settings, nlohmann::json::array()),
                  "only an object can be written");
    CHECK_MESSAGE(
        static_cast<bool>(writeVersionedJson(root / "invalid_utf8.json", settings, {{"name", "bad \xff byte"}})),
        "invalid UTF-8 is replaced instead of throwing");
}

TEST_SUITE_END();

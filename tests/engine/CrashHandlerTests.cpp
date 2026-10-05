// Installation smoke check of the crash handler: a scope installs the
// process's handler (on Windows), creates its diagnostics folder, keeps a
// second scope inert while it is installed, and frees the slot when it ends.
// Writing a minidump needs a crashing process, which this test does not
// cause.

#include "engine/platform/CrashHandler.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <system_error>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

#ifdef _WIN32
constexpr bool platformHasHandler = true;
#else
constexpr bool platformHasHandler = false;
#endif

void testInstallation(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "diagnostics";
    {
        std::optional<CrashHandlerScope> handler(std::in_place, CrashHandlerOptions{.directory = directory});
        expect(handler->installed() == platformHasHandler, "a scope installs the handler where the platform has one");
        expect(std::filesystem::is_directory(directory), "installing creates the diagnostics folder");
        expect(handler->directory() == directory, "the scope reports where crashes are written");
        handler->setRecoveryAction([] {});
        handler->setRecoveryAction({});

        const CrashHandlerScope second({.directory = root / "second"});
        expect(!second.installed(), "a second scope is inert while the first is installed");

        handler.reset();
        const CrashHandlerScope third({.directory = root / "third"});
        expect(third.installed() == platformHasHandler, "a scope installs again once the first has ended");
    }
    const CrashHandlerScope withoutDirectory(CrashHandlerOptions{});
    expect(!withoutDirectory.installed(), "a scope without a directory installs nothing");
}
} // namespace

int main() {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "zenith_crash_handler_tests";
    std::filesystem::remove_all(root, error);
    testInstallation(root);
    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}

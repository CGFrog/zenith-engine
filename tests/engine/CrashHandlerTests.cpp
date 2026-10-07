// Installation smoke check of the crash handler: a scope installs the
// process's handler (on Windows), creates its diagnostics folder, keeps a
// second scope inert while it is installed, and frees the slot when it ends.
// Writing a minidump needs a crashing process, which this test does not
// cause.

#include "engine/platform/CrashHandler.hpp"

#include "support/Doctest.hpp"

#include <filesystem>
#include <optional>

namespace {

#ifdef _WIN32
constexpr bool platformHasHandler = true;
#else
constexpr bool platformHasHandler = false;
#endif

} // namespace

TEST_SUITE_BEGIN("CrashHandler");

TEST_CASE("CrashHandler: installation") {
    const testsupport::TempDirectory root("crash-handler-installation");
    const std::filesystem::path directory = root / "diagnostics";
    {
        std::optional<CrashHandlerScope> handler(std::in_place, CrashHandlerOptions{.directory = directory});
        CHECK_MESSAGE(handler->installed() == platformHasHandler,
                      "a scope installs the handler where the platform has one");
        CHECK_MESSAGE(std::filesystem::is_directory(directory), "installing creates the diagnostics folder");
        CHECK_MESSAGE(handler->directory() == directory, "the scope reports where crashes are written");
        handler->setRecoveryAction([] {});
        handler->setRecoveryAction({});

        const CrashHandlerScope second({.directory = root / "second"});
        CHECK_MESSAGE(!second.installed(), "a second scope is inert while the first is installed");

        handler.reset();
        const CrashHandlerScope third({.directory = root / "third"});
        CHECK_MESSAGE(third.installed() == platformHasHandler, "a scope installs again once the first has ended");
    }
    const CrashHandlerScope withoutDirectory(CrashHandlerOptions{});
    CHECK_MESSAGE(!withoutDirectory.installed(), "a scope without a directory installs nothing");
}

TEST_SUITE_END();

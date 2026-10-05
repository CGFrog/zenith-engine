#pragma once

#include <exception>
#include <optional>
#include <string>

// Runs call, a callback into game code (a system, a timer, a spawn callback),
// and returns what it threw: the what() of a std::exception, or "an unknown
// exception". nullopt when it returned normally. Hosts use it so a gameplay
// bug is reported and contained instead of unwinding through the engine.
// Structured exceptions (access violations) are not C++ exceptions and are
// left to the crash handler (engine/platform/CrashHandler.hpp).
template <typename Call> std::optional<std::string> callGuarded(Call&& call) {
    try {
        call();
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    } catch (...) {
        return std::string("an unknown exception");
    }
}

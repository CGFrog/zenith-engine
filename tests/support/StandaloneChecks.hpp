#pragma once

// StandaloneChecks: the checks of a test executable with its own main() (a GPU smoke, a benchmark), reported like
// doctest's.

#include <source_location>
#include <string_view>

namespace testsupport {

// Unit tests use doctest's CHECK; an executable that runs a frame loop or
// takes arguments (docs/TESTING.md, "Standalone tests") cannot, and counts its
// checks here instead. A failed check prints "<file>:<line>: FAILED: <what>"
// to stderr; exitCode() is the process's result.
class StandaloneChecks {
  public:
    // Returns condition, so a caller can stop when a check it depends on fails.
    bool check(bool condition, std::string_view what, std::source_location location = std::source_location::current());
    int failures() const;
    // 0 when every check passed, otherwise 1; prints a one-line summary.
    int exitCode(std::string_view name) const;

  private:
    int m_checks{0};
    int m_failures{0};
};

} // namespace testsupport

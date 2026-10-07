#include "support/StandaloneChecks.hpp"

#include <iostream>

namespace testsupport {

bool StandaloneChecks::check(bool condition, std::string_view what, std::source_location location) {
    ++m_checks;
    if (!condition) {
        ++m_failures;
        std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
    }
    return condition;
}

int StandaloneChecks::failures() const {
    return m_failures;
}

int StandaloneChecks::exitCode(std::string_view name) const {
    if (m_failures == 0) {
        std::cout << name << ": " << m_checks << " check(s) passed\n";
        return 0;
    }
    std::cerr << name << ": " << m_failures << " of " << m_checks << " check(s) failed\n";
    return 1;
}

} // namespace testsupport

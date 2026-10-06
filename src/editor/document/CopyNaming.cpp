#include "CopyNaming.hpp"

#include <algorithm>
#include <charconv>

namespace {
// Suffixes longer than this are not treated as counters, so they cannot
// overflow.
constexpr size_t maximumSuffixDigits = 9;

bool isDigit(char character) {
    return character >= '0' && character <= '9';
}
} // namespace

SplitCopyName splitCopyName(std::string_view name) {
    const size_t separator = name.rfind('_');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= name.size()) {
        return {.base = name};
    }
    const std::string_view digits = name.substr(separator + 1);
    if (digits.size() > maximumSuffixDigits || !std::ranges::all_of(digits, isDigit)) {
        return {.base = name};
    }
    uint64_t number = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), number);
    return {.base = name.substr(0, separator), .number = number};
}

void CopyNamer::add(std::string_view name) {
    m_used.emplace(name);
    const SplitCopyName split = splitCopyName(name);
    uint64_t& largest = m_largest[std::string(split.base)];
    largest = std::max(largest, split.number);
}

std::string CopyNamer::copyOf(std::string_view name) {
    if (!m_used.contains(std::string(name))) {
        add(name);
        return std::string(name);
    }
    const std::string base(splitCopyName(name).base);
    std::string copy = base + "_" + std::to_string(m_largest[base] + 1);
    add(copy);
    return copy;
}

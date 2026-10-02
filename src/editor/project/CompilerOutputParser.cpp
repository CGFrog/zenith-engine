#include "CompilerOutputParser.hpp"

#include <algorithm>
#include <charconv>
#include <regex>

namespace {
// Locations and severities always sit at the start of a line, so only this
// prefix is matched; the rest is appended to the message. Bounding the input
// keeps std::regex's backtracking cheap on very long template errors.
constexpr size_t matchedPrefixBytes = 1024;

constexpr auto regexFlags = std::regex::ECMAScript | std::regex::optimize;

std::string_view trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Removes a parallel MSBuild node prefix such as `12>`.
std::string_view stripNodePrefix(std::string_view line) {
    size_t digits = 0;
    while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') {
        ++digits;
    }
    if (digits > 0 && digits < line.size() && line[digits] == '>') {
        return line.substr(digits + 1);
    }
    return line;
}

// Removes MSBuild's trailing project reference: ` [C:\x\proj.vcxproj]`.
std::string stripProjectSuffix(std::string message) {
    static const std::regex suffix(R"(\s+\[[^\[\]]*\.(?:[A-Za-z]*proj|sln|targets|props)\]\s*$)", regexFlags);
    return std::regex_replace(message, suffix, "");
}

bool containsIgnoringCase(std::string_view text, std::string_view needle) {
    const auto lower = [](char character) {
        return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
    };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [&](char left, char right) { return lower(left) == lower(right); }) != text.end();
}

CompilerDiagnosticSeverity severityFromText(std::string_view text) {
    if (containsIgnoringCase(text, "error")) {
        return CompilerDiagnosticSeverity::Error;
    }
    if (containsIgnoringCase(text, "warning")) {
        return CompilerDiagnosticSeverity::Warning;
    }
    return CompilerDiagnosticSeverity::Note;
}

uint32_t parseNumber(const std::ssub_match& match) {
    uint32_t value = 0;
    if (match.matched) {
        const std::string text = match.str();
        std::from_chars(text.data(), text.data() + text.size(), value);
    }
    return value;
}

CompilerDiagnostic makeDiagnostic(std::string_view severity, std::optional<EditorSourceLocation> location,
                                  std::string code, std::string message) {
    return {.severity = severityFromText(severity),
            .location = std::move(location),
            .code = std::move(code),
            .message = stripProjectSuffix(std::string(trim(message)))};
}

std::optional<CompilerDiagnostic> parseLocated(const std::string& head, std::string_view rest) {
    // MSVC: file(line[,column]): severity [code]: message
    static const std::regex msvc(
        R"(^(.+?)\((\d+)(?:,(\d+))?(?:,\d+,\d+)?\)\s*:\s*(fatal error|error|warning|note)(?:\s+([A-Za-z]+\d+))?\s*:\s*(.*)$)",
        regexFlags | std::regex::icase);
    // GCC/Clang: file:line[:column]: severity: message
    static const std::regex gnu(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)", regexFlags);
    std::smatch match;
    if (std::regex_match(head, match, msvc)) {
        EditorSourceLocation location{std::filesystem::path(match[1].str()), parseNumber(match[2]),
                                      parseNumber(match[3])};
        return makeDiagnostic(match[4].str(), std::move(location), match[5].str(), match[6].str() + std::string(rest));
    }
    if (std::regex_match(head, match, gnu)) {
        EditorSourceLocation location{std::filesystem::path(match[1].str()), parseNumber(match[2]),
                                      parseNumber(match[3])};
        return makeDiagnostic(match[4].str(), std::move(location), {}, match[5].str() + std::string(rest));
    }
    return std::nullopt;
}

std::optional<CompilerDiagnostic> parseCMake(const std::string& head) {
    static const std::regex cmake(R"(^CMake (Error|Warning)(?: \(dev\))? at (.+):(\d+)(?: \(([^)]*)\))?:?\s*$)",
                                  regexFlags);
    std::smatch match;
    if (!std::regex_match(head, match, cmake)) {
        return std::nullopt;
    }
    EditorSourceLocation location{std::filesystem::path(match[2].str()), parseNumber(match[3]), 0};
    std::string message = "CMake " + match[1].str();
    if (match[4].matched) {
        message += " in " + match[4].str();
    }
    return makeDiagnostic(match[1].str(), std::move(location), {}, std::move(message));
}

std::optional<CompilerDiagnostic> parseToolLevel(const std::string& head, std::string_view rest,
                                                 std::string_view line) {
    // MSVC tools: `LINK : fatal error LNK1104: msg`, `a.obj : error LNK2019: msg`.
    static const std::regex msvcTool(
        R"(^(.+?)\s*:\s*((?:fatal |command line )?error|(?:command line )?warning)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)",
        regexFlags | std::regex::icase);
    // GNU tools: `collect2: error: msg`, `ld.lld: error: msg`.
    static const std::regex gnuTool(R"(^([^\s:][^:]*):\s*(fatal error|error|warning):\s*(.*)$)", regexFlags);
    std::smatch match;
    if (std::regex_match(head, match, msvcTool)) {
        return makeDiagnostic(match[2].str(), std::nullopt, match[3].str(),
                              match[1].str() + ": " + match[4].str() + std::string(rest));
    }
    if (std::regex_match(head, match, gnuTool)) {
        return makeDiagnostic(match[2].str(), std::nullopt, {},
                              match[1].str() + ": " + match[3].str() + std::string(rest));
    }
    if (line.find("undefined reference to") != std::string_view::npos) {
        return makeDiagnostic("error", std::nullopt, {}, std::string(line));
    }
    return std::nullopt;
}
} // namespace

std::optional<CompilerDiagnostic> parseCompilerOutputLine(std::string_view line) {
    line = trim(stripNodePrefix(trim(line)));
    // Cheap rejection keeps the regexes off ordinary progress output.
    if (!containsIgnoringCase(line, "error") && !containsIgnoringCase(line, "warning") &&
        !containsIgnoringCase(line, "note") && line.find("undefined reference") == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t headBytes = std::min(line.size(), matchedPrefixBytes);
    const std::string head(line.substr(0, headBytes));
    const std::string_view rest = line.substr(headBytes);
    if (std::optional<CompilerDiagnostic> located = parseLocated(head, rest)) {
        return located;
    }
    if (std::optional<CompilerDiagnostic> cmake = parseCMake(head)) {
        return cmake;
    }
    return parseToolLevel(head, rest, line);
}

std::optional<CompilerDiagnostic> CompilerOutputParser::addLine(std::string_view line) {
    std::optional<CompilerDiagnostic> diagnostic = parseCompilerOutputLine(line);
    if (!diagnostic) {
        return std::nullopt;
    }
    const EditorSourceLocation location = diagnostic->location.value_or(EditorSourceLocation{});
    Key key{diagnostic->severity, location.file, location.line, location.column, diagnostic->code, diagnostic->message};
    if (m_seen.insert(std::move(key)).second) {
        m_diagnostics.push_back(*diagnostic);
    }
    return diagnostic;
}

const std::vector<CompilerDiagnostic>& CompilerOutputParser::diagnostics() const {
    return m_diagnostics;
}

void CompilerOutputParser::clear() {
    m_diagnostics.clear();
    m_seen.clear();
}

std::vector<CompilerDiagnostic> parseCompilerOutput(std::span<const std::string> lines) {
    CompilerOutputParser parser;
    for (const std::string& line : lines) {
        parser.addLine(line);
    }
    return parser.diagnostics();
}

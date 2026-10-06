#pragma once

// content-manifest.json digests and package validation.

#include <filesystem>
#include <string>
#include <vector>

struct ContentValidationIssue {
    std::filesystem::path path;
    std::string message;
};

struct ContentValidationResult {
    bool success{false};
    std::vector<ContentValidationIssue> issues;
    explicit operator bool() const { return success; }
};

ContentValidationResult validateContentManifest(const std::filesystem::path& packageRoot,
                                                const std::filesystem::path& manifestPath = {});

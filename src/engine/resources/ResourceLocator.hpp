#pragma once

// Executable-relative asset and shader roots (assetRoot, shaderRoot, resourceRoot).

#include <filesystem>

// Initializes executable-relative resource discovery. Call once from an
// executable composition root before constructing Engine.
void initializeResourceLocator(const std::filesystem::path& executablePath);

// The resource root contains the packaged "assets" and "shaders" folders.
// Development builds use the same contract and may resolve the parent of a
// multi-config output directory.
std::filesystem::path resourceRoot();
std::filesystem::path assetRoot();
std::filesystem::path shaderRoot();

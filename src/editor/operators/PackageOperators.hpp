#pragma once

#include "editor/operators/OperatorRegistry.hpp"
#include "editor/project/PackageRunner.hpp"

#include <filesystem>

struct EditorToolSettings;

// Properties of file.build_package.
struct BuildPackageProperties {
    PackagePlatform platform{hostPackagePlatform()};
    PackageConfiguration configuration{PackageConfiguration::Release};
};

struct PackageOperatorServices {
    // Runs and keeps the package; the Build panel shows it.
    PackageRunner& runner;
    // build.directory (the engine checkout packaging configures) and
    // package.directory (the release tree inside it).
    const EditorToolSettings& tools;
    // The open .zenith-project; empty when the editor runs without one.
    std::filesystem::path projectFile;
};

// The PackageRequest Build and Package makes: the source directory is
// build.directory (the editor's working directory when empty), the release
// tree is package.directory inside it, and the project path is made
// absolute. Relative paths resolve against workingDirectory.
PackageRequest packageRequestFor(const EditorToolSettings& tools, const std::filesystem::path& projectFile,
                                 const BuildPackageProperties& properties,
                                 const std::filesystem::path& workingDirectory);

// Registers file.build_package, which checks the project and source tree,
// plans the configure, build, cook, and package commands (configuring only
// when the release tree needs it) and starts them on the runner. It needs no
// window, so it runs headless; the services must outlive the registry.
OperatorRegistrationResult registerPackageOperators(OperatorRegistry& registry, PackageOperatorServices services);

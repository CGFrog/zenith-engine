#include "PackageOperators.hpp"

#include "editor/operators/EditorOperatorFamilies.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "editor/settings/EditorSettings.hpp"

#include <system_error>
#include <utility>

namespace {
std::filesystem::path resolved(const std::filesystem::path& path, const std::filesystem::path& base) {
    return (path.is_absolute() ? path : base / path).lexically_normal();
}

Fields<BuildPackageProperties> buildPackageFields() {
    return std::move(Fields<BuildPackageProperties>()
                         .add("platform", &BuildPackageProperties::platform)
                         .options({{"windows", "Windows", static_cast<int64_t>(PackagePlatform::Windows)},
                                   {"linux", "Linux", static_cast<int64_t>(PackagePlatform::Linux)},
                                   {"macos", "macOS", static_cast<int64_t>(PackagePlatform::MacOS)}})
                         .add("configuration", &BuildPackageProperties::configuration)
                         .options({{"release", "Release", static_cast<int64_t>(PackageConfiguration::Release)},
                                   {"relwithdebinfo", "Release with Debug Info",
                                    static_cast<int64_t>(PackageConfiguration::RelWithDebInfo)}}));
}

OperatorResult buildPackage(const PackageOperatorServices& services, const BuildPackageProperties& properties) {
    if (services.runner.running()) {
        return OperatorResult::cancelled("A package is already being built");
    }
    std::error_code error;
    const std::filesystem::path workingDirectory = std::filesystem::current_path(error);
    PackageRequest request = packageRequestFor(services.tools, services.projectFile, properties, workingDirectory);
    if (!request.projectFile.empty() && !std::filesystem::is_regular_file(request.projectFile, error)) {
        return OperatorResult::failed("The project file '" + utf8PathText(request.projectFile) + "' does not exist.");
    }
    if (!std::filesystem::is_regular_file(request.sourceDirectory / "CMakeLists.txt", error)) {
        return OperatorResult::failed(
            "Packaging runs CMake on the engine checkout, but '" + utf8PathText(request.sourceDirectory) +
            "' has no CMakeLists.txt. Set the build directory in Edit > Preferences > Build.");
    }
    request.configure = packageCacheNeedsConfigure(request.buildDirectory, request.projectFile, request.configuration);
    PackagePlanResult planned = planPackage(request);
    if (!planned) {
        return OperatorResult::failed(planned.error);
    }
    if (const ChildProcessResult started = services.runner.start(std::move(planned.plan)); !started) {
        return OperatorResult::failed("Could not start packaging: " + started.error);
    }
    return OperatorResult::finished();
}
} // namespace

PackageRequest packageRequestFor(const EditorToolSettings& tools, const std::filesystem::path& projectFile,
                                 const BuildPackageProperties& properties,
                                 const std::filesystem::path& workingDirectory) {
    const std::filesystem::path source = tools.buildDirectory.empty()
                                             ? workingDirectory.lexically_normal()
                                             : resolved(pathFromUtf8Text(tools.buildDirectory), workingDirectory);
    return {.sourceDirectory = source,
            .buildDirectory = tools.packageDirectory.empty()
                                  ? std::filesystem::path{}
                                  : resolved(pathFromUtf8Text(tools.packageDirectory), source),
            .projectFile = projectFile.empty() ? projectFile : resolved(projectFile, workingDirectory),
            .platform = properties.platform,
            .configuration = properties.configuration};
}

OperatorRegistrationResult registerPackageOperators(OperatorRegistry& registry, PackageOperatorServices services) {
    return registry.add({.id = "file.build_package",
                         .label = "Build and Package",
                         .description =
                             "Build the open project in Release, cook its textures, and package it for this computer's "
                             "platform, then open the output folder",
                         .properties = OperatorProperties::make(BuildPackageProperties{}, buildPackageFields()),
                         .poll = [&runner = services.runner](const OperatorContext&) { return !runner.running(); },
                         .execute =
                             [services = std::move(services)](OperatorContext&, OperatorProperties& properties) {
                                 const auto* values = properties.get<BuildPackageProperties>();
                                 if (values == nullptr) {
                                     return OperatorResult::failed("Operator properties have the wrong type");
                                 }
                                 return buildPackage(services, *values);
                             }});
}

OperatorRegistrationResult registerPackageOperatorFamily(OperatorRegistry& registry,
                                                         const EditorOperatorServices& services) {
    return registerPackageOperators(
        registry, {.runner = services.packageRunner, .tools = services.tools, .projectFile = services.projectFile});
}

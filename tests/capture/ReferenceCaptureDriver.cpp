// Renders the views listed in a capture manifest with the built-in frame
// capture and compares them against checked-in reference PNGs, or refreshes
// those references with --update. docs/RENDERER_VALIDATION.md describes the
// manifest and the regeneration workflow.
//
// Exit codes: 0 when every view matches (or was updated), 1 on any failure,
// and 77 (CTest SKIP_RETURN_CODE) when no hardware Vulkan device exists.

#include "engine/diagnostics/ImageComparison.hpp"
#include "engine/diagnostics/RgbaImage.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <volk.h>

namespace {
constexpr int skipExitCode = 77;

struct CaptureView {
    std::string name;
    std::string covers;
    uint64_t frame{0};
    std::string camera;
    std::string motion;
    std::vector<std::pair<std::string, std::string>> environment;
    ImageComparisonTolerance tolerance;
};

struct CaptureManifest {
    std::filesystem::path referenceDirectory;
    std::filesystem::path project;
    std::string windowSize;
    std::vector<CaptureView> views;
};

struct DriverOptions {
    std::filesystem::path executable;
    std::filesystem::path manifest;
    std::filesystem::path outputDirectory;
    std::vector<std::string> selectedViews;
    uint32_t repeat{1};
    bool update{false};
};

void printUsage() {
    std::cerr << "Usage: zenith_reference_capture --executable <zenith> --manifest <captures.json> --output <dir>\n"
                 "                                [--update] [--view <name>]... [--repeat <runs>]\n";
}

bool parseArguments(int argc, char** argv, DriverOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        if (argument == "--update") {
            options.update = true;
        } else if (argument == "--executable" && hasValue) {
            options.executable = argv[++index];
        } else if (argument == "--manifest" && hasValue) {
            options.manifest = argv[++index];
        } else if (argument == "--output" && hasValue) {
            options.outputDirectory = argv[++index];
        } else if (argument == "--view" && hasValue) {
            options.selectedViews.emplace_back(argv[++index]);
        } else if (argument == "--repeat" && hasValue) {
            const std::string runs = argv[++index];
            const auto [end, error] = std::from_chars(runs.data(), runs.data() + runs.size(), options.repeat);
            if (error != std::errc{} || end != runs.data() + runs.size()) {
                std::cerr << "--repeat needs a run count, got '" << runs << "'\n";
                return false;
            }
        } else {
            std::cerr << "Unknown or incomplete argument '" << argument << "'\n";
            return false;
        }
    }
    if (options.executable.empty() || options.manifest.empty() || options.outputDirectory.empty() ||
        options.repeat == 0) {
        return false;
    }
    return true;
}

ImageComparisonTolerance readTolerance(const nlohmann::json& json, ImageComparisonTolerance tolerance) {
    tolerance.channelTolerance = json.value("channelTolerance", tolerance.channelTolerance);
    tolerance.maximumChannelDifference = json.value("maximumChannelDifference", tolerance.maximumChannelDifference);
    tolerance.maximumRootMeanSquareError =
        json.value("maximumRootMeanSquareError", tolerance.maximumRootMeanSquareError);
    tolerance.maximumDifferingPixelFraction =
        json.value("maximumDifferingPixelFraction", tolerance.maximumDifferingPixelFraction);
    return tolerance;
}

bool loadManifest(const std::filesystem::path& path, CaptureManifest& manifest) {
    std::ifstream stream(path);
    if (!stream) {
        std::cerr << "Cannot open capture manifest " << path << '\n';
        return false;
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(stream);
        manifest.referenceDirectory = std::filesystem::absolute(path).parent_path();
        manifest.project = (manifest.referenceDirectory / json.at("project").get<std::string>()).lexically_normal();
        manifest.windowSize = json.at("windowSize").get<std::string>();
        const ImageComparisonTolerance defaults = readTolerance(json.value("tolerance", nlohmann::json::object()), {});
        for (const nlohmann::json& entry : json.at("views")) {
            CaptureView view;
            view.name = entry.at("name").get<std::string>();
            view.covers = entry.value("covers", std::string{});
            view.frame = entry.at("frame").get<uint64_t>();
            view.camera = entry.at("camera").get<std::string>();
            view.motion = entry.value("motion", std::string{});
            const nlohmann::json environment = entry.value("environment", nlohmann::json::object());
            for (const auto& [name, value] : environment.items()) {
                view.environment.emplace_back(name, value.get<std::string>());
            }
            view.tolerance = readTolerance(entry.value("tolerance", nlohmann::json::object()), defaults);
            manifest.views.push_back(std::move(view));
        }
    } catch (const nlohmann::json::exception& error) {
        std::cerr << "Invalid capture manifest " << path << ": " << error.what() << '\n';
        return false;
    }
    return true;
}

// The references come from a hardware GPU; software rasterizers and machines
// without a Vulkan driver cannot reproduce them.
bool hardwareVulkanDeviceAvailable(std::string& reason) {
    if (volkInitialize() != VK_SUCCESS) {
        reason = "no Vulkan loader";
        return false;
    }
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pApplicationName = "zenith_reference_capture",
                                        .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        reason = "cannot create a Vulkan instance";
        return false;
    }
    volkLoadInstanceOnly(instance);
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    bool hardware = false;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        hardware = hardware || properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
    }
    vkDestroyInstance(instance, nullptr);
    reason = "no hardware Vulkan device";
    return hardware;
}

// Runs the engine for one view with a clean, deterministic capture
// environment and waits for it to exit.
bool runCapture(const DriverOptions& options, const CaptureManifest& manifest, const CaptureView& view,
                const std::filesystem::path& outputPath) {
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    SDL_Environment* environment = SDL_CreateEnvironment(true);
    if (environment == nullptr) {
        std::cerr << "Cannot copy the process environment: " << SDL_GetError() << '\n';
        return false;
    }
    for (const char* name : {"ZENITH_SMOKE_FRAMES", "ZENITH_BENCHMARK_FRAMES", "ZENITH_CAPTURE_CAMERA_MOTION",
                             "ZENITH_FORCE_HIGH_QUALITY", "ZENITH_FORCE_TAA", "ZENITH_FORCE_GPU_DRIVEN",
                             "ZENITH_FORCE_DYNAMIC_RESOLUTION", "ZENITH_DISABLE_DEPTH_PREPASS"}) {
        SDL_UnsetEnvironmentVariable(environment, name);
    }
    const std::string frame = std::to_string(view.frame);
    const std::string output = outputPath.string();
    SDL_SetEnvironmentVariable(environment, "ZENITH_CAPTURE_FRAME", frame.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "ZENITH_CAPTURE_PATH", output.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "ZENITH_CAPTURE_CAMERA", view.camera.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "ZENITH_WINDOW_SIZE", manifest.windowSize.c_str(), true);
    SDL_SetEnvironmentVariable(environment, "ZENITH_FRAME_DETERMINISTIC", "1", true);
    if (!view.motion.empty()) {
        SDL_SetEnvironmentVariable(environment, "ZENITH_CAPTURE_CAMERA_MOTION", view.motion.c_str(), true);
    }
    for (const auto& [name, value] : view.environment) {
        SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
    }

    const std::string executable = options.executable.string();
    const std::string project = manifest.project.string();
    const char* arguments[]{executable.c_str(), "--project", project.c_str(), nullptr};
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments);
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_DestroyEnvironment(environment);
    if (process == nullptr) {
        std::cerr << "Cannot start " << executable << ": " << SDL_GetError() << '\n';
        return false;
    }
    int exitCode = -1;
    SDL_WaitProcess(process, true, &exitCode);
    SDL_DestroyProcess(process);
    if (exitCode != 0) {
        std::cerr << "[" << view.name << "] engine exited with code " << exitCode << '\n';
        return false;
    }
    if (!std::filesystem::is_regular_file(outputPath)) {
        std::cerr << "[" << view.name << "] engine exited without writing " << outputPath << '\n';
        return false;
    }
    return true;
}

bool load(const std::filesystem::path& path, RgbaImage& image) {
    if (const ImageFileResult result = loadPng(path, image); !result) {
        std::cerr << result.error << '\n';
        return false;
    }
    return true;
}

// Compares a capture with the reference; on failure writes an amplified
// difference image next to the capture.
bool compareWithReference(const CaptureView& view, const std::filesystem::path& capturePath,
                          const std::filesystem::path& referencePath) {
    if (!std::filesystem::is_regular_file(referencePath)) {
        std::cerr << "[" << view.name << "] missing reference " << referencePath
                  << "; regenerate references with --update (docs/RENDERER_VALIDATION.md)\n";
        return false;
    }
    RgbaImage actual;
    RgbaImage reference;
    if (!load(capturePath, actual) || !load(referencePath, reference)) {
        return false;
    }
    const ImageComparison comparison = compareImages(actual, reference, view.tolerance);
    std::cout << "[" << view.name << "] " << (comparison.withinTolerance ? "match" : "MISMATCH") << ": "
              << comparison.summary() << '\n';
    if (!comparison.withinTolerance && comparison.dimensionsMatch) {
        std::filesystem::path diffPath = capturePath;
        diffPath.replace_extension(".diff.png");
        if (const ImageFileResult written = writePng(diffPath, differenceImage(actual, reference)); written) {
            std::cout << "[" << view.name << "] difference image: " << diffPath.string() << '\n';
        }
    }
    return comparison.withinTolerance;
}

// Captures the view `repeat` times, compares consecutive runs with each other
// (a stability check), and then compares or updates the reference.
bool processView(const DriverOptions& options, const CaptureManifest& manifest, const CaptureView& view) {
    std::cout << "[" << view.name << "] capturing frame " << view.frame << " (" << view.covers << ")\n";
    const std::filesystem::path capturePath = options.outputDirectory / (view.name + ".png");
    if (!runCapture(options, manifest, view, capturePath)) {
        return false;
    }
    bool stable = true;
    for (uint32_t run = 1; run < options.repeat; ++run) {
        const std::filesystem::path repeatPath =
            options.outputDirectory / (view.name + ".run" + std::to_string(run + 1) + ".png");
        if (!runCapture(options, manifest, view, repeatPath)) {
            return false;
        }
        RgbaImage first;
        RgbaImage repeated;
        if (!load(capturePath, first) || !load(repeatPath, repeated)) {
            return false;
        }
        const ImageComparison comparison = compareImages(repeated, first, view.tolerance);
        std::cout << "[" << view.name << "] run " << run + 1 << " vs run 1: " << comparison.summary() << '\n';
        stable = stable && comparison.withinTolerance;
    }
    if (!stable) {
        std::cerr << "[" << view.name << "] consecutive runs differ beyond the view's tolerance\n";
        return false;
    }
    const std::filesystem::path referencePath = manifest.referenceDirectory / (view.name + ".png");
    if (options.update) {
        std::error_code error;
        std::filesystem::copy_file(capturePath, referencePath, std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error) {
            std::cerr << "[" << view.name << "] cannot update " << referencePath << ": " << error.message() << '\n';
            return false;
        }
        std::cout << "[" << view.name << "] updated " << referencePath.string() << '\n';
        return true;
    }
    return compareWithReference(view, capturePath, referencePath);
}

bool selected(const DriverOptions& options, const CaptureView& view) {
    return options.selectedViews.empty() || std::find(options.selectedViews.begin(), options.selectedViews.end(),
                                                      view.name) != options.selectedViews.end();
}
} // namespace

int main(int argc, char** argv) {
    // Keep this driver's lines ordered with the engine output it interleaves.
    std::cout << std::unitbuf;
    DriverOptions options;
    if (!parseArguments(argc, argv, options)) {
        printUsage();
        return 1;
    }
    CaptureManifest manifest;
    if (!loadManifest(options.manifest, manifest)) {
        return 1;
    }
    std::string reason;
    if (!hardwareVulkanDeviceAvailable(reason)) {
        std::cout << "Skipping reference captures: " << reason << '\n';
        return skipExitCode;
    }
    std::error_code error;
    std::filesystem::create_directories(options.outputDirectory, error);
    if (error) {
        std::cerr << "Cannot create " << options.outputDirectory << ": " << error.message() << '\n';
        return 1;
    }
    int failures = 0;
    bool anySelected = false;
    for (const CaptureView& view : manifest.views) {
        if (!selected(options, view)) {
            continue;
        }
        anySelected = true;
        if (!processView(options, manifest, view)) {
            ++failures;
        }
    }
    if (!anySelected) {
        std::cerr << "No manifest view matches the requested --view names\n";
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " reference capture(s) failed\n";
        return 1;
    }
    return 0;
}

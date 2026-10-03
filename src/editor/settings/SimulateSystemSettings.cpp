#include "SimulateSystemSettings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;

constexpr int currentVersion = 1;

SimulateSystemSettingsResult parse(const std::string& text, SimulateSystemMask& mask) {
    const Json document = Json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {.error = "the file is not a JSON object"};
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != currentVersion) {
        return {.error = "unsupported version (expected " + std::to_string(currentVersion) + ")"};
    }
    SimulateSystemMask parsed;
    if (const auto systems = document.find("systems"); systems != document.end()) {
        if (!systems->is_object()) {
            return {.error = "systems must map system names to true or false"};
        }
        for (const auto& [name, enabled] : systems->items()) {
            if (name.empty() || !enabled.is_boolean()) {
                return {.error = "systems must map system names to true or false"};
            }
            parsed.set(name, enabled.get<bool>());
        }
    }
    mask = std::move(parsed);
    return {.success = true};
}
} // namespace

SimulateSystemSettingsResult loadSimulateSystemSettings(const std::filesystem::path& path, SimulateSystemMask& mask) {
    mask.clear();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return {.error = "Cannot inspect Simulate system settings " + path.string() + ": " + error.message()};
        }
        return {.success = true};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {.error = "Cannot read Simulate system settings " + path.string() + "; using defaults."};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (SimulateSystemSettingsResult result = parse(contents.str(), mask); !result) {
        return {.error = "Invalid Simulate system settings in " + path.string() + " (" + result.error +
                         "); using defaults."};
    }
    return {.success = true};
}

SimulateSystemSettingsResult saveSimulateSystemSettings(const std::filesystem::path& path,
                                                        const SimulateSystemMask& mask) {
    Json systems = Json::object();
    for (const auto& [name, enabled] : mask.choices()) {
        systems[name] = enabled;
    }
    const Json document{{"version", currentVersion}, {"systems", std::move(systems)}};
    std::error_code error;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return {.error = "Cannot create the Simulate system settings directory: " + error.message()};
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    // Replace invalid UTF-8 rather than throwing from dump().
    output << document.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!output) {
        return {.error = "Cannot write Simulate system settings " + path.string()};
    }
    return {.success = true};
}

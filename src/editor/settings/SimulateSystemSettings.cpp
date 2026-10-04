#include "SimulateSystemSettings.hpp"

#include "engine/platform/VersionedJson.hpp"

#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;

const VersionedJsonFormat simulateSystemSettingsFormat{.description = "Simulate system settings"};

SimulateSystemSettingsResult parse(const Json& document, SimulateSystemMask& mask) {
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
    const VersionedJsonResult read = readVersionedJson(path, simulateSystemSettingsFormat);
    if (!read) {
        return {.error = read.error + "; using defaults."};
    }
    if (!read.found) {
        return {.success = true};
    }
    if (SimulateSystemSettingsResult result = parse(read.document, mask); !result) {
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
    const Json document{{"systems", std::move(systems)}};
    if (const PlatformResult written = writeVersionedJson(path, simulateSystemSettingsFormat, document); !written) {
        return {.error = written.error};
    }
    return {.success = true};
}

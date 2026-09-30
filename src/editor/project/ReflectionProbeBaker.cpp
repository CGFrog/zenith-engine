#include "ReflectionProbeBaker.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

ReflectionProbeBakeResult ReflectionProbeBaker::bake(const TextureAsset& source,
                                                     const std::filesystem::path& destination,
                                                     ReflectionProbeBakeSettings settings) {
    if (source.width == 0 || source.height == 0 ||
        source.rgba.size() != static_cast<size_t>(source.width) * source.height * 4) {
        return {.error = "Reflection probe source is not a valid RGBA texture"};
    }
    if (settings.roughnessLevels == 0 || settings.roughnessLevels > 16) {
        return {.error = "Reflection probe roughness level count must be between 1 and 16"};
    }
    const PrefilteredEnvironment& filtered = m_cache.preprocess(source, settings.roughnessLevels);
    nlohmann::json specular = nlohmann::json::array();
    for (const glm::vec3& level : filtered.specularRoughnessLevels) {
        specular.push_back({level.x, level.y, level.z});
    }
    const nlohmann::json document = {
        {"version", 1},
        {"sourceHash", filtered.sourceHash},
        {"diffuseSky", {filtered.diffuseSky.x, filtered.diffuseSky.y, filtered.diffuseSky.z}},
        {"diffuseGround", {filtered.diffuseGround.x, filtered.diffuseGround.y, filtered.diffuseGround.z}},
        {"specularRoughnessLevels", std::move(specular)},
    };
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return {.error = "Could not create reflection-probe bake directory: " + error.message()};
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << document.dump(2) << '\n')) {
            return {.error = "Could not write reflection-probe bake: " + temporary.string()};
        }
    }
    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary);
        return {.error = "Could not replace reflection-probe bake: " + error.message()};
    }
    return {.success = true, .sourceHash = filtered.sourceHash};
}

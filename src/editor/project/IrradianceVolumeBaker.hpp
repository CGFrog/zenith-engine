#pragma once

// Bakes irradiance volume probes from scene geometry and lights.

#include "engine/assets/IrradianceVolumeAsset.hpp"
#include "engine/graphics/IrradianceVolume.hpp"
#include "engine/graphics/Lighting.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

class AssetManager;

// Editor-owned policy for baking irradiance volumes on the CPU. The runtime
// only loads the result (IrradianceVolumeData) and never decides when or how
// a volume is baked.
//
// Each probe casts 6 * directionsPerFace^2 rays through the texel centers of a
// cube map. A ray that escapes sees the sky; a ray that hits a surface sees its
// emission plus Lambertian reflection of direct sun and local light (with
// shadow rays) and, from the second bounce on, of the previous bounce's volume
// sampled with the runtime's visibility-aware interpolation. Radiance is
// projected to order-2 spherical harmonics of irradiance / pi, and hit
// distances are filtered into per-probe octahedral depth moments. A probe
// whose rays mostly hit single-sided back faces is inside geometry and is
// marked invalid. Results are deterministic: every probe is computed
// independently from the previous bounce, whatever the thread count.

struct IrradianceBakeTriangle {
    // World-space corners, counter-clockwise when seen from the front face.
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 c{0.0f};
    // Linear diffuse reflectance and emitted radiance.
    glm::vec3 albedo{0.8f};
    glm::vec3 emission{0.0f};
    bool doubleSided{false};
};

struct IrradianceBakeSun {
    // Unit direction from surfaces toward the light, in world space.
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    // Irradiance on a surface facing the light (color times intensity).
    glm::vec3 irradiance{0.0f};
    bool castsShadows{true};
};

// Everything the baker sees, in world space.
struct IrradianceBakeScene {
    std::vector<IrradianceBakeTriangle> triangles;
    std::optional<IrradianceBakeSun> sun;
    std::vector<LocalLight> localLights;
    // Sky radiance arriving from a world direction, including the
    // environment's diffuse intensity. Null means a black sky.
    std::function<glm::vec3(const glm::vec3&)> skyRadiance;
};

struct IrradianceVolumeBakeSettings {
    // Rays per probe are 6 * directionsPerFace^2.
    uint32_t directionsPerFace{8};
    // Light bounces; one means surfaces reflect direct light only.
    uint32_t bounces{3};
    // Zero uses the hardware concurrency.
    uint32_t workerThreads{0};
};

struct IrradianceVolumeBakeRequest {
    glm::vec3 minimum{-1.0f};
    glm::vec3 maximum{1.0f};
    glm::uvec3 probeCounts{2u};
    // Lookup biases for secondary bounces; normally the volume's own.
    irradiance_volume::ShadingParameters shading;
    IrradianceVolumeBakeSettings settings;
};

struct IrradianceVolumeBakeResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

// Collects mesh triangles (base-color and emissive factors, textures averaged
// when decoded as RGBA8, blended surfaces skipped), the primary directional
// and environment lights as the renderer resolves them, and every local light.
IrradianceBakeScene gatherIrradianceBakeScene(const Scene& scene, const AssetManager& assets);

IrradianceVolumeBakeResult bakeIrradianceVolume(const IrradianceBakeScene& scene,
                                                const IrradianceVolumeBakeRequest& request,
                                                IrradianceVolumeData& output);

// Bakes the volume on `entity` into `directory`, named by
// irradianceVolumeBakeFileName, and returns the file in `written`. Files are
// content addressed and never overwritten, so undoing a re-bake restores the
// previous file and duplicated volumes never share a changing file; superseded
// bakes stay on disk until removed by hand.
IrradianceVolumeBakeResult bakeIrradianceVolumeFile(const Scene& scene, const AssetManager& assets, Entity entity,
                                                    const std::filesystem::path& directory,
                                                    const IrradianceVolumeBakeSettings& settings,
                                                    IrradianceVolumeData& baked, std::filesystem::path& written);

// Bakes every irradiance volume in `scene` into irradianceVolumeBakeDirectory
// and points each component at its new data. Stops at the first failure.
IrradianceVolumeBakeResult bakeSceneIrradianceVolumes(Scene& scene, const AssetManager& assets,
                                                      const std::filesystem::path& assetRoot,
                                                      const std::optional<std::filesystem::path>& scenePath,
                                                      const IrradianceVolumeBakeSettings& settings,
                                                      uint32_t& volumesBaked);

// Where the editor stores a scene's bakes: <assetRoot>/baked/<scene file stem
// or "untitled">.
std::filesystem::path irradianceVolumeBakeDirectory(const std::filesystem::path& assetRoot,
                                                    const std::optional<std::filesystem::path>& scenePath);
// irradiance_volume_<entity id>_<64-bit FNV-1a of the encoded bytes>.zvol
std::string irradianceVolumeBakeFileName(SceneEntityId volume, std::span<const std::byte> encoded);

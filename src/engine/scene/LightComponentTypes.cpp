// Field keys are the scene document format, so renaming one breaks saved
// scenes unless a schema migration is added. Presentation hints mirror the
// ranges the inspector has always offered.

#include "LightComponentTypes.hpp"

#include "../assets/IrradianceVolumeAsset.hpp"
#include "ComponentTypeHelpers.hpp"
#include "ComponentTypes.hpp"
#include "SceneAssetReferences.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace {
using component_type_helpers::finiteNonNegative;
using component_type_helpers::finitePositive;
using component_type_helpers::unitInterval;

constexpr uint32_t maximumLocalShadowResolution = 16384;

std::string validateLocalShadowResolution(uint32_t resolution) {
    if (resolution > maximumLocalShadowResolution) {
        return "Local light shadow resolution must be a whole number of at most 16384 texels";
    }
    return {};
}

// Local-light shadow fields are optional so documents written before
// local-light shadows still load with shadows off.
template <typename Light> Fields<Light>& localLightShadowFields(Fields<Light>& fields) {
    return fields.add("casts_shadows", &Light::castsShadows)
        .optional()
        .label("Cast shadows")
        .add("shadow_resolution", &Light::shadowResolution)
        .optional()
        .drag(8.0f, 0.0, 4096.0)
        .format("%d texels")
        .tooltip("Requested shadow tile edge in texels; 0 uses the quality default.")
        .add("shadow_priority", &Light::shadowPriority)
        .optional()
        .drag(0.1f)
        .tooltip("Lights with a higher priority keep their shadow atlas space first.");
}

// Baked lighting is derived data: a reference that is well formed but whose
// file is gone loads as an unbaked volume (shaded by the environment) so the
// scene can still be opened and re-baked. Corrupt files still fail.
SceneDocumentResult loadIrradianceBake(IrradianceVolumeComponent& volume, const ComponentLoadContext& context) {
    if (volume.bakeAsset.empty()) {
        return {.success = true};
    }
    const std::filesystem::path reference(volume.bakeAsset);
    if (reference.is_absolute() || reference.empty() || *reference.begin() == "..") {
        return {.error = "Irradiance volume bake reference must be project-relative: " + volume.bakeAsset};
    }
    std::error_code error;
    if (!std::filesystem::exists(context.options.assetRoot / reference, error)) {
        return {.success = true};
    }
    std::filesystem::path path;
    if (SceneDocumentResult result = resolveProjectAssetPath(volume.bakeAsset, context.options, path); !result) {
        return result;
    }
    auto data = std::make_shared<IrradianceVolumeData>();
    if (IrradianceVolumeResult loaded = loadIrradianceVolume(path, *data); !loaded) {
        return {.error = loaded.error};
    }
    volume.bakedData = std::move(data);
    return {.success = true};
}
} // namespace

ComponentType directionalLightComponentType() {
    using C = DirectionalLightComponent;
    using S = DirectionalShadowSettings;
    return ComponentTypeBuilder<C>("directional_light", "Directional Light")
        .category("Lights")
        .fields(Fields<C>()
                    .add("direction", &C::direction)
                    .drag(0.01f, -1.0, 1.0)
                    .tooltip("Direction from a shaded surface toward the light, in world space.")
                    .add("color", &C::color)
                    .color()
                    .add("intensity", &C::intensity)
                    .drag(0.05f, 0.0, 10000.0)
                    .add("primary", &C::primary)
                    .structure("shadows", &C::shadows,
                               Fields<S>()
                                   .add("enabled", &S::enabled)
                                   .label("Cast shadows")
                                   .add("half_extent", &S::halfExtent)
                                   .drag(0.1f, 0.1, 10000.0)
                                   .add("focus_distance", &S::focusDistance)
                                   .drag(0.1f, 0.0, 10000.0)
                                   .add("depth_range", &S::depthRange)
                                   .drag(0.5f, 1.0, 10000.0)
                                   .add("constant_bias", &S::constantBias)
                                   .drag(0.01f, 0.0, 10.0)
                                   .add("slope_bias", &S::slopeBias)
                                   .drag(0.01f, 0.0, 10.0)
                                   .add("normal_bias", &S::normalBias)
                                   .drag(0.0005f, 0.0, 1.0)
                                   .format("%.4f")))
        .build();
}

ComponentType environmentLightComponentType() {
    using C = EnvironmentLightComponent;
    return ComponentTypeBuilder<C>("environment_light", "Environment Light")
        .category("Lights")
        .fields(Fields<C>()
                    .add("sky_color", &C::skyColor)
                    .color()
                    .add("ground_color", &C::groundColor)
                    .color()
                    .add("diffuse_intensity", &C::diffuseIntensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("specular_intensity", &C::specularIntensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("primary", &C::primary)
                    .add("environment_asset", &C::environmentAsset)
                    .optional()
                    .readOnly())
        .texture(&C::environmentAsset, &C::environmentMap)
        .build();
}

ComponentType volumetricFogComponentType() {
    using C = VolumetricFogComponent;
    return ComponentTypeBuilder<C>("volumetric_fog", "Volumetric Fog")
        .category("Environment")
        .fields(Fields<C>()
                    .add("density", &C::density)
                    .drag(0.001f, 0.0, 10.0)
                    .format("%.4f")
                    .add("height_falloff", &C::heightFalloff)
                    .drag(0.005f, 0.0, 10.0)
                    .format("%.3f")
                    .add("base_height", &C::baseHeight)
                    .drag(0.05f, -10000.0, 10000.0)
                    .add("albedo", &C::albedo)
                    .color()
                    .add("anisotropy", &C::anisotropy)
                    .slider(-0.95, 0.95)
                    .format("%.2f")
                    .add("noise_amount", &C::noiseAmount)
                    .optional()
                    .slider(0.0, 1.0)
                    .format("%.2f")
                    .add("noise_scale", &C::noiseScale)
                    .optional()
                    .drag(0.05f, 0.05, 1000.0)
                    .add("primary", &C::primary))
        .validate([](const C& fog) {
            // Infinite coefficients would turn the integrated fog into NaN.
            const bool valid = finiteNonNegative(fog.density) && finiteNonNegative(fog.heightFalloff) &&
                               std::abs(fog.anisotropy) < 1.0f && unitInterval(fog.noiseAmount) &&
                               finitePositive(fog.noiseScale) && finiteNonNegative(fog.albedo.r) &&
                               finiteNonNegative(fog.albedo.g) && finiteNonNegative(fog.albedo.b) &&
                               std::isfinite(fog.baseHeight);
            return valid ? std::string{} : std::string("Volumetric fog parameters are invalid");
        })
        .build();
}

ComponentType pointLightComponentType() {
    using C = PointLightComponent;
    Fields<C> fields;
    fields.add("color", &C::color)
        .color()
        .add("intensity", &C::intensity)
        .drag(0.05f, 0.0, 10000.0)
        .add("range", &C::range)
        .drag(0.05f, 0.01, 10000.0);
    return ComponentTypeBuilder<C>("point_light", "Point Light")
        .category("Lights")
        .fields(std::move(localLightShadowFields(fields)))
        .validate([](const C& light) {
            if (light.range <= 0.0f) {
                return std::string("Point light range must be positive");
            }
            return validateLocalShadowResolution(light.shadowResolution);
        })
        .build();
}

ComponentType spotLightComponentType() {
    using C = SpotLightComponent;
    Fields<C> fields;
    fields.add("direction", &C::direction)
        .drag(0.01f, -1.0, 1.0)
        .add("color", &C::color)
        .color()
        .add("intensity", &C::intensity)
        .drag(0.05f, 0.0, 10000.0)
        .add("range", &C::range)
        .drag(0.05f, 0.01, 10000.0)
        .add("inner_cone", &C::innerConeRadians)
        .angle()
        .slider(0.0, 89.0)
        .add("outer_cone", &C::outerConeRadians)
        .angle()
        .slider(0.0, 89.0);
    return ComponentTypeBuilder<C>("spot_light", "Spot Light")
        .category("Lights")
        .fields(std::move(localLightShadowFields(fields)))
        .validate([](const C& light) {
            if (light.range <= 0.0f || light.innerConeRadians < 0.0f ||
                light.outerConeRadians < light.innerConeRadians) {
                return std::string("Spot light range and cone angles are invalid");
            }
            return validateLocalShadowResolution(light.shadowResolution);
        })
        .sanitize([](C& light) { light.outerConeRadians = std::max(light.outerConeRadians, light.innerConeRadians); })
        .build();
}

ComponentType reflectionProbeComponentType() {
    using C = ReflectionProbeComponent;
    return ComponentTypeBuilder<C>("reflection_probe", "Reflection Probe")
        .category("Lights")
        .fields(Fields<C>()
                    .add("half_extents", &C::halfExtents)
                    .drag(0.01f, 0.01, 10000.0)
                    .add("blend_distance", &C::blendDistance)
                    .drag(0.01f, 0.0, 100.0)
                    .add("priority", &C::priority)
                    .drag(0.1f)
                    .add("environment_asset", &C::environmentAsset)
                    .optional()
                    .readOnly())
        .texture(&C::environmentAsset, &C::environmentMap)
        .validate([](const C& probe) {
            if (glm::any(glm::lessThanEqual(probe.halfExtents, glm::vec3(0.0f))) || probe.blendDistance < 0.0f) {
                return std::string("Reflection probe volume is invalid");
            }
            return std::string{};
        })
        .build();
}

ComponentType irradianceVolumeComponentType() {
    using C = IrradianceVolumeComponent;
    return ComponentTypeBuilder<C>("irradiance_volume", "Irradiance Volume")
        .category("Lights")
        .fields(Fields<C>()
                    .add("half_extents", &C::halfExtents)
                    .drag(0.01f, 0.01, 10000.0)
                    .add("probe_counts", &C::probeCounts)
                    .drag(0.05f, 1.0, static_cast<double>(maximumIrradianceProbesPerAxis))
                    .add("intensity", &C::intensity)
                    .drag(0.01f, 0.0, 100.0)
                    .add("blend_distance", &C::blendDistance)
                    .drag(0.01f, 0.0, 100.0)
                    .add("normal_bias", &C::normalBias)
                    .drag(0.005f, 0.0, 10.0)
                    .add("view_bias", &C::viewBias)
                    .drag(0.005f, 0.0, 10.0)
                    .add("bake_asset", &C::bakeAsset)
                    .optional()
                    .label("Bake")
                    .readOnly())
        .validate([](const C& volume) {
            const uint64_t probeCount =
                static_cast<uint64_t>(volume.probeCounts.x) * volume.probeCounts.y * volume.probeCounts.z;
            if (glm::any(glm::lessThanEqual(volume.halfExtents, glm::vec3(0.0f))) || probeCount == 0 ||
                glm::any(glm::greaterThan(volume.probeCounts, glm::uvec3(maximumIrradianceProbesPerAxis))) ||
                probeCount > maximumIrradianceProbes || volume.intensity < 0.0f || volume.blendDistance < 0.0f ||
                volume.normalBias < 0.0f || volume.viewBias < 0.0f) {
                return std::string("Irradiance volume extents, probe counts, intensity, or biases are invalid");
            }
            return std::string{};
        })
        .assetReference(&C::bakeAsset)
        .afterLoad(loadIrradianceBake)
        .build();
}

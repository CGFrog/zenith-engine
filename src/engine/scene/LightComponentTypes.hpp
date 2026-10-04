#pragma once

class ComponentType;

// Lights and lighting environment: directional, point and spot lights, the
// environment light, volumetric fog, reflection probes, and irradiance volumes.
// registerBuiltInComponentTypes registers them in the established order.
ComponentType directionalLightComponentType();
ComponentType environmentLightComponentType();
ComponentType volumetricFogComponentType();
ComponentType pointLightComponentType();
ComponentType spotLightComponentType();
ComponentType reflectionProbeComponentType();
ComponentType irradianceVolumeComponentType();

#pragma once

// How a navigation mesh is built, in metres and degrees.

#include <cstdint>
#include <string>

// How a navigation mesh is built, in metres and degrees. The agent values
// describe the walkers the mesh is for: where they fit, what they climb.
// Kept apart from NavigationWorld so a project's manifest (ProjectConfig) can
// hold them without the navigation and scene headers.
struct NavMeshBuildSettings {
    float cellSize{0.2f};
    float cellHeight{0.1f};
    float agentHeight{1.8f};
    float agentRadius{0.4f};
    float agentMaxClimb{0.35f};
    float agentMaxSlopeDegrees{45.0f};
    // Smallest region kept, and size below which regions merge, in cells.
    float regionMinimumArea{8.0f};
    float regionMergeArea{20.0f};
    float edgeMaxLength{12.0f};
    float edgeMaxError{1.3f};
    float detailSampleDistance{6.0f};
    float detailSampleMaxError{1.0f};
    // The physics layers whose static colliders are walkable geometry.
    uint32_t layerMask{~0u};

    bool operator==(const NavMeshBuildSettings&) const = default;
};

// Why settings cannot build a mesh, or empty: cell sizes, the agent's height
// and the edge error must be positive, the slope below 90 degrees, and every
// other value finite and not negative.
std::string validateNavMeshBuildSettings(const NavMeshBuildSettings& settings);

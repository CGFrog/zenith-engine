#pragma once

#include "../assets/Assets.hpp"
#include "../physics/PhysicsComponents.hpp"
#include "Collections.hpp"
#include "ParticleEmitter.hpp"
#include "SceneEntityId.hpp"
#include <compare>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using Entity = entt::entity;

struct SceneEntityIdComponent {
    SceneEntityId id;
};

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::mat4 local{1.0f};
    glm::mat4 world{1.0f};
};

struct SceneTransformUpdateStats {
    uint32_t visited{0};
    uint32_t recomputed{0};
};

struct SceneRevisions {
    uint64_t transform{1};
    uint64_t renderable{1};
    uint64_t light{1};
    uint64_t camera{1};
    uint64_t skin{1};
    uint64_t material{1};

    auto operator<=>(const SceneRevisions&) const = default;
};

enum class PhysicsLifecycleOperation : uint8_t {
    Upsert,
    Destroy,
};

struct PhysicsLifecycleEvent {
    Entity entity{entt::null};
    PhysicsLifecycleOperation operation{PhysicsLifecycleOperation::Upsert};
};

struct RelationshipComponent {
    Entity parent{entt::null};
    std::vector<Entity> children;
};

struct ModelComponent {
    ModelHandle model{};
};

struct ModelNodeComponent {
    ModelHandle model{};
    SceneEntityId instanceRoot{};
    uint32_t nodeIndex{0};
};

struct AnimationComponent {
    uint32_t clipIndex{0};
    float time{0.0f};
    float playbackSpeed{1.0f};
    bool looping{true};
    bool playing{true};
    std::optional<uint32_t> blendClipIndex;
    float blendTime{0.0f};
    float blendDuration{0.0f};
};

struct SkinComponent {
    ModelHandle model{};
    SceneEntityId instanceRoot{};
    uint32_t skinIndex{0};
    std::vector<glm::mat4> jointPalette;
};

struct MorphWeightsComponent {
    std::vector<float> weights;
};

struct MeshRendererComponent {
    MeshHandle mesh{};
};

struct CameraComponent {
    float verticalFovRadians{glm::radians(60.0f)};
    float nearPlane{0.1f};
    float farPlane{1000.0f};
    bool primary{true};
};

struct DirectionalShadowSettings {
    bool enabled{true};
    // World-space half width of the orthographic shadow volume.
    float halfExtent{14.0f};
    // Distance from the camera toward its view direction used to center the shadow volume.
    float focusDistance{6.0f};
    // World-space depth covered along the light direction.
    float depthRange{60.0f};
    // Raster depth-bias factors; normalBias is a world-space receiver offset.
    float constantBias{0.5f};
    float slopeBias{1.0f};
    float normalBias{0.01f};
};

struct DirectionalLightComponent {
    // Direction from a shaded surface toward the light, in world space.
    glm::vec3 direction{0.4f, 0.8f, 0.5f};
    glm::vec3 color{1.0f, 0.91f, 0.78f};
    float intensity{5.0f};
    bool primary{true};
    DirectionalShadowSettings shadows;
};

struct EnvironmentLightComponent {
    // Linear-light colors sampled as a hemisphere around the world Y axis.
    glm::vec3 skyColor{0.20f, 0.24f, 0.34f};
    glm::vec3 groundColor{0.08f, 0.065f, 0.05f};
    float diffuseIntensity{0.6f};
    float specularIntensity{0.35f};
    bool primary{true};
    // Optional project-relative HDR environment source. Import/preprocessing is
    // cached independently from scene ownership.
    std::string environmentAsset;
    TextureHandle environmentMap{};
};

// Participating medium for froxel volumetric fog, drawn when
// RenderSettings::volumetricFog is not Off. The primary component with the
// lowest entity id is used; its entity transform is ignored because the fog
// fills the whole view. Distances are world units (meters) and coefficients
// are per world unit.
struct VolumetricFogComponent {
    // Extinction coefficient at and below baseHeight; 0.02 removes about 2% of
    // the light per meter.
    float density{0.02f};
    // Above baseHeight the density decays as exp(-heightFalloff * (y - baseHeight)).
    float heightFalloff{0.1f};
    float baseHeight{0.0f};
    // Single-scattering albedo: scattering = albedo * extinction per channel.
    glm::vec3 albedo{0.9f};
    // Henyey-Greenstein asymmetry in (-1, 1); positive values scatter forward,
    // brightening the fog when looking toward a light.
    float anisotropy{0.4f};
    // Zero keeps the density smooth; one lets world-space noise vary it
    // between zero and twice the height-fog density.
    float noiseAmount{0.0f};
    // World size of one noise cell.
    float noiseScale{4.0f};
    bool primary{true};
};

// Local-light shadows are rendered into the renderer's shared atlas when the
// light casts shadows and LocalShadowQuality is not Off. shadowResolution is
// the requested tile edge in texels (per cube face for point lights); zero uses
// the quality default and the renderer clamps it to its quality maximum. Lights
// with a higher shadowPriority keep their atlas space first.
struct PointLightComponent {
    glm::vec3 color{1.0f};
    float intensity{10.0f};
    float range{8.0f};
    bool castsShadows{false};
    uint32_t shadowResolution{0};
    int32_t shadowPriority{0};
};

struct SpotLightComponent {
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{15.0f};
    float range{10.0f};
    float innerConeRadians{glm::radians(20.0f)};
    float outerConeRadians{glm::radians(30.0f)};
    // See PointLightComponent.
    bool castsShadows{false};
    uint32_t shadowResolution{0};
    int32_t shadowPriority{0};
};

struct ReflectionProbeComponent {
    glm::vec3 halfExtents{5.0f};
    float blendDistance{1.0f};
    int32_t priority{0};
    std::string environmentAsset;
    TextureHandle environmentMap{};
};

// An oriented box projector that paints onto opaque and alpha-masked surfaces
// inside it (see engine/graphics/Decals.hpp). The box is the entity's world
// transform applied to [-halfExtents, halfExtents], so the transform's scale
// also sizes it. The decal projects along the box's local -Y axis: an
// unrotated decal lands on floors, and texture u runs along local +X and v
// along local +Z. Colors are linear.
struct DecalComponent {
    glm::vec3 halfExtents{0.5f, 0.25f, 0.5f};
    // Optional project-relative textures. The albedo alpha masks every effect
    // of the decal; without an albedo texture the mask is one across the box.
    std::string albedoAsset;
    TextureHandle albedoTexture{};
    std::string normalAsset;
    TextureHandle normalTexture{};
    glm::vec3 tint{1.0f};
    // Scales the mask of every effect.
    float opacity{1.0f};
    // How strongly each surface property moves toward the decal's, in [0, 1].
    // Without a normal texture, normalStrength flattens the surface toward its
    // geometric normal (paint or water filling its bumps).
    float baseColorStrength{1.0f};
    float normalStrength{1.0f};
    float roughness{0.5f};
    float roughnessStrength{0.0f};
    float metallic{0.0f};
    float metallicStrength{0.0f};
    // Surfaces facing the projector (normal along local +Y) receive the full
    // decal up to angleFadeStart and none beyond angleFadeEnd, so walls next to
    // a floor decal stay clean. Radians, 0 <= start <= end <= pi.
    float angleFadeStartRadians{glm::radians(60.0f)};
    float angleFadeEndRadians{glm::radians(80.0f)};
    // Decals apply in ascending sort order (then stable scene id), later decals
    // painting over earlier ones.
    int32_t sortOrder{0};
};

struct IrradianceVolumeData;

// A baked grid of irradiance probes that replaces environment lighting for
// indirect diffuse inside its box (see engine/graphics/IrradianceVolume.hpp).
// The box is axis-aligned, centered on the entity's world position, with
// half extents in world units; rotation and scale are ignored, as for
// reflection probes. Probes sit at the centers of probeCounts cells.
struct IrradianceVolumeComponent {
    glm::vec3 halfExtents{4.0f, 2.0f, 4.0f};
    glm::uvec3 probeCounts{8u, 4u, 8u};
    // Multiplier on baked irradiance. The bake already includes the primary
    // environment light's diffuse intensity.
    float intensity{1.0f};
    // World-space width over which the volume fades to the environment at its
    // boundary.
    float blendDistance{0.5f};
    // World-space offsets along the surface normal and toward the viewer
    // applied before probe lookup, to avoid self-shadowing.
    float normalBias{0.1f};
    float viewBias{0.1f};
    // Optional project-relative baked data (.zvol). The loaded data is
    // immutable and shared; it records the bounds and counts it was baked
    // with, which may differ from the authored ones until the next bake.
    // bakedData stays null when the referenced file is missing.
    std::string bakeAsset;
    std::shared_ptr<const IrradianceVolumeData> bakedData;
};

// Components a scene document named but the loading registry did not know,
// for example types from a game module that is not loaded. Each entry is the
// document key and its JSON object exactly as read; saving writes them back
// unchanged so documents never lose data they cannot interpret.
struct PreservedComponentsComponent {
    std::vector<std::pair<std::string, std::string>> components;
};

struct SceneResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

enum class ReparentMode {
    PreserveLocalTransform,
    PreserveWorldTransform,
};

class Scene {
  public:
    Scene();
    Entity createEntity(std::string name = {});
    SceneResult createEntityWithId(SceneEntityId id, std::string name, Entity& entity);
    SceneEntityId id(Entity entity) const;
    Entity findEntity(SceneEntityId id) const;
    void setParent(Entity child, Entity parent);
    SceneResult reparent(Entity child, Entity parent, ReparentMode mode = ReparentMode::PreserveLocalTransform);
    SceneResult duplicateSubtree(Entity source, Entity parent, Entity& duplicate);
    SceneResult instantiateModel(Entity root, ModelHandle model, const AssetManager& assets);
    MeshRendererComponent& attachMesh(Entity entity, MeshHandle mesh);
    void destroyEntity(Entity entity);
    void clear();
    void replaceWith(Scene&& scene);
    void swap(Scene& other) noexcept;
    void updateWorldTransforms();
    void markTransformDirty(Entity entity);
    void markRenderableDirty(Entity entity);
    void markLightDirty(Entity entity);
    void markCameraDirty(Entity entity);
    void markSkinDirty(Entity entity);
    void markMaterialDirty(Entity entity);
    void markPhysicsDirty(Entity entity);
    const SceneTransformUpdateStats& lastTransformUpdateStats() const;
    const SceneRevisions& revisions() const;
    std::span<const Entity> changedTransforms() const;
    std::span<const PhysicsLifecycleEvent> physicsLifecycleEvents() const;
    void retirePhysicsLifecycleEvents();
    uint64_t renderPacketRevision() const;
    // Changes whenever the contents are replaced wholesale (clear, replaceWith,
    // swap), so caches keyed by stable entity ids can tell a reloaded scene
    // from edits to the same one.
    uint64_t contentGeneration() const;

    // Collection definitions (see Collections.hpp). Replacing them marks the
    // render and light state dirty, since collection flags and instance
    // offsets change what renders.
    const SceneCollections& collections() const;
    void setCollections(SceneCollections collections);

    entt::registry& registry();
    const entt::registry& registry() const;

  private:
    Entity cloneBranch(Entity source, Entity parent, bool appendCopySuffix);

    struct TransformCacheComponent {
        glm::mat4 local{1.0f};
        Entity parent{entt::null};
        bool valid{false};
        bool dirty{true};
    };

    bool updateBranch(Entity entity, const glm::mat4& parentWorld, bool parentChanged);

    entt::registry m_registry;
    uint64_t m_nextEntityId{1};
    SceneTransformUpdateStats m_lastTransformUpdateStats;
    SceneRevisions m_revisions;
    std::vector<Entity> m_changedTransforms;
    std::vector<Entity> m_dirtyTransformRoots;
    std::vector<Entity> m_transformAuditScratch;
    bool m_collectingTransformChanges{false};
    uint32_t m_transformAuditCountdown{0};
    std::vector<PhysicsLifecycleEvent> m_physicsLifecycleEvents;
    uint64_t m_renderPacketRevision{1};
    uint64_t m_contentGeneration{1};
    SceneCollections m_collections;
};

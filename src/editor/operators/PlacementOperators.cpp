#include "PlacementOperators.hpp"

#include "editor/application/EntityCommands.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/SceneFragment.hpp"
#include "editor/interaction/ModalTransform.hpp"
#include "editor/viewport/SnapQueries.hpp"
#include "engine/assets/Assets.hpp"
#include "engine/gameplay/Random.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace {
using ExecuteFunction = std::function<OperatorResult(OperatorContext&, OperatorProperties&)>;

template <typename Properties, typename Function> ExecuteFunction typed(Function run) {
    return [run = std::move(run)](OperatorContext& context, OperatorProperties& properties) {
        Properties* values = properties.get<Properties>();
        if (values == nullptr) {
            return OperatorResult::failed("Operator properties have the wrong type");
        }
        return run(context, *values);
    };
}

bool hasSelection(const OperatorContext& context) {
    return !context.selection.empty();
}

// A decimal stable id; nullopt when text is not one.
std::optional<SceneEntityId> parseEntityId(const std::string& text) {
    uint64_t id = 0;
    const char* end = text.data() + text.size();
    if (const auto parsed = std::from_chars(text.data(), end, id); parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return SceneEntityId{id};
}

glm::vec3 median(std::span<const glm::mat4> world) {
    return transformPivotPoint({.mode = TransformPivot::MedianPoint}, world);
}

// The world box around the meshes of entities' subtrees; empty when none
// has a loaded mesh. World transforms must be current.
BoundingBox subtreeBounds(const Scene& scene, const AssetManager& assets, std::span<const SceneEntityId> roots) {
    BoundingBox bounds;
    std::vector<Entity> pending;
    for (SceneEntityId id : roots) {
        if (const Entity entity = scene.findEntity(id); entity != entt::null) {
            pending.push_back(entity);
        }
    }
    const entt::registry& registry = scene.registry();
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        const auto& relationship = registry.get<RelationshipComponent>(entity);
        pending.insert(pending.end(), relationship.children.begin(), relationship.children.end());
        const auto* renderer = registry.try_get<MeshRendererComponent>(entity);
        const MeshAsset* mesh = renderer != nullptr ? assets.mesh(renderer->mesh) : nullptr;
        if (mesh == nullptr || mesh->localBox.empty()) {
            continue;
        }
        const glm::mat4& world = registry.get<TransformComponent>(entity).world;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) != 0 ? mesh->localBox.maximum.x : mesh->localBox.minimum.x,
                                  (corner & 2) != 0 ? mesh->localBox.maximum.y : mesh->localBox.minimum.y,
                                  (corner & 4) != 0 ? mesh->localBox.maximum.z : mesh->localBox.minimum.z);
            const glm::vec3 point(world * glm::vec4(local, 1.0f));
            bounds.minimum = glm::min(bounds.minimum, point);
            bounds.maximum = glm::max(bounds.maximum, point);
        }
    }
    return bounds;
}

// The selection's transform roots as a fragment, with the roots' ids (in
// fragment order) and world matrices.
struct CopiedSelection {
    std::string fragment;
    std::vector<SceneEntityId> roots;
    std::vector<glm::mat4> world;
};

std::optional<CopiedSelection> copySelection(const OperatorContext& context, std::string& error) {
    Scene& scene = context.editing.scene();
    scene.updateWorldTransforms();
    CopiedSelection copied;
    const std::vector<SceneEntityId> roots = context.selection.transformRoots(scene);
    if (const SceneDocumentResult result = SceneFragment::copy(
            scene, context.editing.assets(), context.editing.document().options(), roots, copied.fragment);
        !result) {
        error = result.error;
        return std::nullopt;
    }
    if (const SceneDocumentResult result = SceneFragment::roots(copied.fragment, copied.roots); !result) {
        error = result.error;
        return std::nullopt;
    }
    for (SceneEntityId id : copied.roots) {
        const Entity entity = scene.findEntity(id);
        copied.world.push_back(entity == entt::null ? glm::mat4(1.0f)
                                                    : scene.registry().get<TransformComponent>(entity).world);
    }
    return copied;
}

// Pastes every copy at once beside the originals and selects the copies.
OperatorResult pasteCopies(OperatorContext& context, const CopiedSelection& copied,
                           std::span<const std::vector<glm::mat4>> copies) {
    std::string replicated;
    if (const SceneDocumentResult result =
            SceneFragment::replicate(copied.fragment, context.editing.document().options(), copies, replicated);
        !result) {
        return OperatorResult::failed(result.error);
    }
    std::vector<SceneEntityId> pasted;
    if (const SceneDocumentResult result = EntityCommands::pasteBesideSources(
            context.editing, replicated, copied.roots, EntityCommands::PastedTransform::KeepWorld, pasted);
        !result) {
        return OperatorResult::failed(result.error);
    }
    context.selection.assign(std::move(pasted));
    return OperatorResult::finished();
}

Operator placeModelOperator() {
    return {.id = "object.place_model",
            .label = "Place Model",
            .description = "Add a model under a new object named after its file, and select it",
            .flags = OperatorFlags::Undo | OperatorFlags::Register | OperatorFlags::Internal,
            .properties = OperatorProperties::make(
                PlaceModelProperties{}, std::move(Fields<PlaceModelProperties>()
                                                      .add("asset", &PlaceModelProperties::asset)
                                                      .add("parent", &PlaceModelProperties::parent)
                                                      .add("location", &PlaceModelProperties::location)
                                                      .drag(0.05f)
                                                      .add("rotation", &PlaceModelProperties::rotation)
                                                      .drag(0.25f)
                                                      .format("%.1f deg")
                                                      .add("normal", &PlaceModelProperties::normal)
                                                      .drag(0.01f, -1.0, 1.0)
                                                      .add("align_to_normal", &PlaceModelProperties::alignToNormal))),
            .poll = [](const OperatorContext& context) { return context.editMode(); },
            .execute = typed<PlaceModelProperties>([](OperatorContext& context, PlaceModelProperties& values) {
                if (values.asset.empty()) {
                    return OperatorResult::cancelled("Choose a model to place");
                }
                Scene& scene = context.editing.scene();
                Entity parent = entt::null;
                if (!values.parent.empty()) {
                    const std::optional<SceneEntityId> parentId = parseEntityId(values.parent);
                    parent = parentId ? scene.findEntity(*parentId) : entt::null;
                    if (parent == entt::null) {
                        return OperatorResult::failed("'" + values.parent + "' is not an entity of the scene");
                    }
                }
                std::filesystem::path path = std::filesystem::path(values.asset);
                if (path.is_relative()) {
                    path = context.editing.document().options().assetRoot / path;
                }
                const AssetLoadResult<ModelHandle> loaded = context.editing.assets().loadModel(path);
                if (!loaded) {
                    return OperatorResult::failed("Could not place model: " + loaded.error);
                }
                const Entity root = scene.createEntity(path.stem().string());
                glm::mat4 world(placementRotation(values.rotation, values.normal, values.alignToNormal));
                world[3] = glm::vec4(values.location, 1.0f);
                if (parent != entt::null) {
                    scene.setParent(root, parent);
                    scene.updateWorldTransforms();
                    world = glm::inverse(scene.registry().get<TransformComponent>(parent).world) * world;
                }
                scene.registry().get<TransformComponent>(root).local = world;
                scene.markTransformDirty(root);
                if (const SceneResult result = scene.instantiateModel(root, loaded.handle, context.editing.assets());
                    !result) {
                    scene.destroyEntity(root);
                    return OperatorResult::failed("Could not place model: " + result.error);
                }
                scene.updateWorldTransforms();
                context.selection.selectOnly(scene.id(root));
                return OperatorResult::finished();
            }),
            // Models start at the 3D cursor, as the add operators do.
            .invokeDefaults =
                [](const OperatorContext& context, OperatorProperties& properties) {
                    if (PlaceModelProperties* values = properties.get<PlaceModelProperties>()) {
                        values->location = context.editing.document().editorData().cursorLocation;
                    }
                }};
}

Operator arrayOperator() {
    return {.id = "object.array",
            .label = "Array",
            .description = "Add copies of the selection in a row, each stepped from the last",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                ArrayProperties{}, std::move(Fields<ArrayProperties>()
                                                 .add("count", &ArrayProperties::count)
                                                 .drag(0.1f, 1.0, 1000.0)
                                                 .add("offset", &ArrayProperties::offset)
                                                 .drag(0.05f)
                                                 .add("rotation_deg", &ArrayProperties::rotationDegrees)
                                                 .label("Rotation")
                                                 .drag(0.25f)
                                                 .add("scale_step", &ArrayProperties::scaleStep)
                                                 .drag(0.01f, -0.99, 10.0)
                                                 .add("relative_to_bounds", &ArrayProperties::relativeToBounds))),
            .poll = hasSelection,
            .execute = typed<ArrayProperties>([](OperatorContext& context, ArrayProperties& values) {
                if (values.count < 1) {
                    return OperatorResult::cancelled("An array needs at least one copy");
                }
                std::string error;
                const std::optional<CopiedSelection> copied = copySelection(context, error);
                if (!copied) {
                    return OperatorResult::failed("Array failed: " + error);
                }
                glm::vec3 offset = values.offset;
                if (values.relativeToBounds) {
                    const BoundingBox bounds =
                        subtreeBounds(context.editing.scene(), context.editing.assets(), copied->roots);
                    // Without a mesh the selection counts as a metre across.
                    const glm::vec3 size = bounds.empty() ? glm::vec3(1.0f) : bounds.maximum - bounds.minimum;
                    offset *= size;
                }
                return pasteCopies(
                    context, *copied,
                    arrayCopyTransforms(copied->world, values.count, offset, values.rotationDegrees, values.scaleStep));
            })};
}

// Where one scattered copy goes: a random point of the disc, dropped onto
// the surface below it.
struct ScatterSample {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float yawRadians{0.0f};
    float scale{1.0f};
};

std::vector<std::vector<glm::mat4>> scatterCopyTransforms(const OperatorContext& context, const CopiedSelection& copied,
                                                          const ScatterProperties& values) {
    const Scene& scene = context.editing.scene();
    const glm::vec3 center = median(copied.world);
    const std::vector<Entity> excluded = snapExclusions(scene, copied.roots);
    const float rayHeight = std::max(values.radius, 1.0f);
    RandomStream random(values.seed);
    std::vector<std::vector<glm::mat4>> copies;
    for (int32_t copy = 0; copy < values.count; ++copy) {
        // Every draw happens whatever the options, so toggling one keeps
        // the copies where they were.
        const float distance = values.radius * std::sqrt(random.unit());
        const float angle = random.range(0.0f, 2.0f * std::numbers::pi_v<float>);
        ScatterSample sample;
        sample.yawRadians = random.range(0.0f, 2.0f * std::numbers::pi_v<float>);
        sample.scale = std::max(1.0f + random.range(-values.scaleJitter, values.scaleJitter), 0.01f);
        sample.position = center + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * distance;
        const LensRay down{.origin = sample.position + glm::vec3(0.0f, rayHeight, 0.0f),
                           .direction = {0.0f, -1.0f, 0.0f}};
        if (const std::optional<SurfaceHit> hit =
                surfaceHitAlongRay(scene, context.editing.assets(), down, nullptr, excluded)) {
            sample.position = hit->position;
            sample.normal = hit->normal;
        }
        glm::mat3 rotation =
            values.alignToSurface ? rotationBetween({0.0f, 1.0f, 0.0f}, sample.normal) : glm::mat3(1.0f);
        if (values.randomYaw) {
            rotation = rotation * glm::mat3(glm::rotate(glm::mat4(1.0f), sample.yawRadians, {0.0f, 1.0f, 0.0f}));
        }
        copies.push_back(applyPivotedTransform(
            copied.world, TransformPivot::MedianPoint, center, glm::mat3(1.0f),
            {.translation = sample.position - center, .rotation = rotation, .scale = glm::vec3(sample.scale)}));
    }
    return copies;
}

Operator scatterOperator() {
    return {.id = "object.scatter",
            .label = "Scatter",
            .description = "Add copies of the selection on the surfaces around it, at random",
            .flags = OperatorFlags::Undo | OperatorFlags::Register,
            .properties = OperatorProperties::make(
                ScatterProperties{}, std::move(Fields<ScatterProperties>()
                                                   .add("count", &ScatterProperties::count)
                                                   .drag(0.1f, 1.0, 1000.0)
                                                   .add("radius", &ScatterProperties::radius)
                                                   .drag(0.05f, 0.0, 10000.0)
                                                   .add("seed", &ScatterProperties::seed)
                                                   .add("align_to_surface", &ScatterProperties::alignToSurface)
                                                   .add("random_yaw", &ScatterProperties::randomYaw)
                                                   .add("scale_jitter", &ScatterProperties::scaleJitter)
                                                   .drag(0.01f, 0.0, 0.99))),
            .poll = hasSelection,
            .execute = typed<ScatterProperties>([](OperatorContext& context, ScatterProperties& values) {
                if (values.count < 1) {
                    return OperatorResult::cancelled("Scatter needs at least one copy");
                }
                std::string error;
                const std::optional<CopiedSelection> copied = copySelection(context, error);
                if (!copied) {
                    return OperatorResult::failed("Scatter failed: " + error);
                }
                return pasteCopies(context, *copied, scatterCopyTransforms(context, *copied, values));
            })};
}
} // namespace

glm::mat3 rotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 axis = glm::cross(from, to);
    const float sine = glm::length(axis);
    const float cosine = glm::dot(from, to);
    if (sine < 1e-6f) {
        if (cosine > 0.0f) {
            return glm::mat3(1.0f);
        }
        // Opposite: half a turn about any axis perpendicular to from.
        const glm::vec3 helper = std::abs(from.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        return glm::mat3_cast(glm::angleAxis(std::numbers::pi_v<float>, glm::normalize(glm::cross(from, helper))));
    }
    return glm::mat3_cast(glm::angleAxis(std::atan2(sine, cosine), axis / sine));
}

glm::mat3 placementRotation(const glm::vec3& rotationDegrees, const glm::vec3& normal, bool alignToNormal) {
    const glm::mat3 euler = glm::mat3_cast(glm::quat(glm::radians(rotationDegrees)));
    if (!alignToNormal || glm::length(normal) < 1e-6f) {
        return euler;
    }
    return rotationBetween({0.0f, 1.0f, 0.0f}, glm::normalize(normal)) * euler;
}

std::vector<std::vector<glm::mat4>> arrayCopyTransforms(std::span<const glm::mat4> start, int32_t count,
                                                        const glm::vec3& offset, const glm::vec3& rotationDegrees,
                                                        float scaleStep) {
    const glm::mat3 turn = glm::mat3_cast(glm::quat(glm::radians(rotationDegrees)));
    const PivotedTransform step{.rotation = turn, .scale = glm::vec3(std::max(1.0f + scaleStep, 0.01f))};
    std::vector<std::vector<glm::mat4>> copies;
    std::vector<glm::mat4> previous(start.begin(), start.end());
    glm::vec3 stepOffset = offset;
    for (int32_t copy = 0; copy < count; ++copy) {
        PivotedTransform change = step;
        change.translation = stepOffset;
        previous =
            applyPivotedTransform(previous, TransformPivot::MedianPoint, median(previous), glm::mat3(1.0f), change);
        copies.push_back(previous);
        stepOffset = turn * stepOffset;
    }
    return copies;
}

OperatorRegistrationResult registerPlacementOperators(OperatorRegistry& registry) {
    for (Operator& op : std::vector<Operator>{placeModelOperator(), arrayOperator(), scatterOperator()}) {
        if (OperatorRegistrationResult result = registry.add(std::move(op)); !result) {
            return result;
        }
    }
    return {.success = true};
}

OperatorRegistrationResult registerPlacementOperatorFamily(OperatorRegistry& registry, const EditorOperatorServices&) {
    return registerPlacementOperators(registry);
}

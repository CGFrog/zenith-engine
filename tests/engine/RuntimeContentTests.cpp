// Runtime content: meshes, materials and textures created in code
// (AssetManager::createMesh and friends, MeshBuilder), their validation,
// generations and collection, MaterialOverrideComponent (its effect on a
// material, scene leases, and its saved form), triangle-mesh colliders of
// runtime meshes sharing cached Jolt shapes, and navigation baked from raw
// runtime geometry.

#include "engine/assets/MeshBuilder.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/graphics/MaterialRecord.hpp"
#include "engine/module/GameServices.hpp"
#include "engine/navigation/NavigationWorld.hpp"
#include "engine/physics/PhysicsWorld.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool near(float first, float second, float tolerance = 1.0e-4f) {
    return std::abs(first - second) <= tolerance;
}

MaterialHandle createGreyMaterial(AssetManager& assets) {
    MaterialAsset material;
    material.baseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    material.metallic = 0.0f;
    material.roughness = 0.8f;
    const AssetLoadResult<MaterialHandle> created = assets.createMaterial(material, "grey");
    expect(static_cast<bool>(created), "a runtime material is created: " + created.error);
    return created.handle;
}

MeshAsset buildBox(MaterialHandle material, const glm::vec3& halfExtents = glm::vec3(1.0f)) {
    MeshBuilder builder;
    builder.beginPrimitive(material).box(glm::vec3(0.0f), halfExtents);
    MeshBuildResult built = builder.build();
    expect(static_cast<bool>(built), "a box mesh builds: " + built.error);
    return std::move(built.mesh);
}

void testMeshBuilder() {
    const MaterialHandle material{0, 1};
    MeshBuilder builder;
    builder.beginPrimitive(material);
    const uint32_t a = builder.vertex({0.0f, 0.0f, 0.0f}, {}, {0.0f, 0.0f});
    const uint32_t b = builder.vertex({1.0f, 0.0f, 0.0f}, {}, {1.0f, 0.0f});
    const uint32_t c = builder.vertex({0.0f, 0.0f, -1.0f}, {}, {0.0f, 1.0f});
    builder.triangle(a, b, c);
    const MeshBuildResult triangle = builder.build({.optimize = false});
    expect(static_cast<bool>(triangle) && triangle.mesh.primitives.size() == 1 &&
               triangle.mesh.primitives[0].indexCount == 3,
           "a single triangle builds into one primitive");
    expect(triangle && std::ranges::all_of(triangle.mesh.vertices,
                                           [](const Vertex& vertex) { return near(vertex.normal.y, 1.0f); }),
           "normals are generated from a counter-clockwise triangle seen from above");
    expect(triangle && near(triangle.mesh.bounds.center.x, 0.5f) && triangle.mesh.bounds.radius > 0.7f,
           "bounds enclose the vertices");

    const MeshBuildResult box = MeshBuilder().beginPrimitive(material).box({1.0f, 2.0f, 3.0f}, glm::vec3(0.5f)).build();
    expect(box && box.mesh.vertices.size() == 24 && box.mesh.indices.size() >= 36,
           "a box is six rectangles of four vertices");
    bool outward = true;
    for (size_t index = 0; box && index + 2 < box.mesh.primitives[0].indexCount; index += 3) {
        const glm::vec3 p0 = box.mesh.vertices[box.mesh.indices[index]].position;
        const glm::vec3 p1 = box.mesh.vertices[box.mesh.indices[index + 1]].position;
        const glm::vec3 p2 = box.mesh.vertices[box.mesh.indices[index + 2]].position;
        const glm::vec3 faceCenter = (p0 + p1 + p2) / 3.0f;
        outward = outward && glm::dot(glm::cross(p1 - p0, p2 - p0), faceCenter - glm::vec3(1.0f, 2.0f, 3.0f)) > 0.0f;
    }
    expect(outward, "every box triangle winds counter-clockwise seen from outside");
    expect(box && box.mesh.localBox.minimum == glm::vec3(0.5f, 1.5f, 2.5f) &&
               box.mesh.localBox.maximum == glm::vec3(1.5f, 2.5f, 3.5f),
           "the local box is computed");

    MeshBuilder broken;
    broken.beginPrimitive(material).vertex(glm::vec3(0.0f));
    broken.triangle(0, 1, 2);
    const MeshBuildResult missing = broken.build();
    expect(!missing && missing.error.find("addresses no vertex") != std::string::npos,
           "a triangle past the vertices is reported");
    MeshBuilder early;
    early.vertex(glm::vec3(0.0f));
    expect(!early.build() && early.build().error.find("beginPrimitive") != std::string::npos,
           "a vertex before any primitive is reported");
    MeshBuilder empty;
    empty.beginPrimitive(material);
    expect(!empty.build(), "a mesh without vertices does not build");
    MeshBuilder unbalanced;
    const std::vector<glm::vec3> positions{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const std::vector<glm::vec3> normals{{0, 1, 0}};
    unbalanced.beginPrimitive(material).append(positions, normals, {}, std::vector<uint32_t>{0, 2, 1});
    expect(!unbalanced.build(), "appended normals must match the positions");
    MeshBuilder appended;
    appended.beginPrimitive(material).append(positions, {}, {}, std::vector<uint32_t>{0, 2, 1});
    const MeshBuildResult fromArrays = appended.build({.optimize = false});
    expect(fromArrays && fromArrays.mesh.indices == std::vector<uint32_t>{0, 2, 1},
           "positions and indices append as one primitive");
}

void testCreateAndResolve() {
    AssetManager assets;
    TextureAsset texture{.width = 2, .height = 2, .rgba = std::vector<uint8_t>(16, 255)};
    const uint64_t before = assets.revision();
    const AssetLoadResult<TextureHandle> createdTexture = assets.createTexture(texture, "white");
    expect(createdTexture && assets.texture(createdTexture.handle) != nullptr && assets.revision() != before,
           "a runtime texture is created and changes the asset revision");
    expect(!assets.textureSource(createdTexture.handle), "a runtime texture has no source");

    MaterialAsset material;
    material.baseColorTexture = createdTexture.handle;
    const AssetLoadResult<MaterialHandle> createdMaterial = assets.createMaterial(material, "textured");
    expect(static_cast<bool>(createdMaterial), "a material referencing a runtime texture is created");

    const AssetLoadResult<MeshHandle> createdMesh =
        assets.createMesh(buildBox(createdMaterial.handle, glm::vec3(2.0f)), "crate");
    const MeshAsset* mesh = assets.mesh(createdMesh.handle);
    expect(createdMesh && mesh != nullptr, "a runtime mesh is created");
    expect(!assets.meshSource(createdMesh.handle), "a runtime mesh has no source path");
    expect(mesh != nullptr && near(mesh->bounds.radius, std::sqrt(12.0f)) && mesh->localBox.maximum == glm::vec3(2.0f),
           "createMesh computes bounds and the local box");

    const std::vector<AssetDiagnostic> diagnostics = assets.diagnostics();
    const auto described = [&](AssetKind kind, const std::string& source) {
        return std::ranges::any_of(diagnostics, [&](const AssetDiagnostic& diagnostic) {
            return diagnostic.kind == kind && diagnostic.source == source;
        });
    };
    expect(described(AssetKind::Mesh, "runtime:crate") && described(AssetKind::Material, "runtime:textured") &&
               described(AssetKind::Texture, "runtime:white"),
           "diagnostics name runtime assets by their debug names");

    const AssetLoadResult<MeshHandle> second = assets.createMesh(buildBox(createdMaterial.handle), "crate");
    expect(second && second.handle != createdMesh.handle, "every createMesh call is a new asset, never a cache hit");
}

void testValidation() {
    AssetManager assets;
    const MaterialHandle material = createGreyMaterial(assets);
    const uint64_t revision = assets.revision();
    const uint64_t bytes = assets.cpuBytes();

    MeshAsset outOfRange = buildBox(material);
    outOfRange.indices[4] = 999;
    AssetLoadResult<MeshHandle> mesh = assets.createMesh(outOfRange, "bad index");
    expect(!mesh && mesh.error.find("bad index") != std::string::npos &&
               mesh.error.find("addresses no vertex") != std::string::npos,
           "an index past the vertices is rejected with a description: " + mesh.error);

    MeshAsset deadMaterial = buildBox(material);
    deadMaterial.primitives[0].material = MaterialHandle{42, 7};
    mesh = assets.createMesh(deadMaterial, "dead material");
    expect(!mesh && mesh.error.find("material") != std::string::npos, "a primitive needs a live material");

    MeshAsset partialTriangle = buildBox(material);
    partialTriangle.primitives[0].indexCount = 4;
    mesh = assets.createMesh(partialTriangle, "partial");
    expect(!mesh && mesh.error.find("multiple of three") != std::string::npos,
           "primitive ranges must be whole triangles");

    MeshAsset pastEnd = buildBox(material);
    pastEnd.primitives[0].firstIndex = static_cast<uint32_t>(pastEnd.indices.size());
    mesh = assets.createMesh(pastEnd, "past end");
    expect(!mesh && mesh.error.find("reach past") != std::string::npos, "primitive ranges stay inside the indices");

    MeshAsset notFinite = buildBox(material);
    notFinite.vertices[3].position.x = std::nanf("");
    expect(!assets.createMesh(notFinite, "nan"), "non-finite vertices are rejected");
    expect(!assets.createMesh(MeshAsset{}, "empty"), "an empty mesh is rejected");

    MaterialAsset deadTexture;
    deadTexture.normalTexture = TextureHandle{3, 9};
    const AssetLoadResult<MaterialHandle> badMaterial = assets.createMaterial(deadTexture, "dead texture");
    expect(!badMaterial && badMaterial.error.find("texture slot 1") != std::string::npos,
           "a material's texture slots must be live: " + badMaterial.error);
    MaterialAsset infinite;
    infinite.roughness = std::numeric_limits<float>::infinity();
    expect(!assets.createMaterial(infinite, "infinite"), "material factors must be finite");

    TextureAsset shortPixels{.width = 4, .height = 4, .rgba = std::vector<uint8_t>(15)};
    const AssetLoadResult<TextureHandle> badTexture = assets.createTexture(shortPixels, "short");
    expect(!badTexture && badTexture.error.find("width * height * 4") != std::string::npos,
           "RGBA8 pixels must match the size: " + badTexture.error);
    expect(!assets.createTexture(TextureAsset{}, "empty"), "a texture needs a size");
    TextureAsset badBlocks{.width = 4, .height = 4, .format = TextureFormat::Bc7};
    expect(!assets.createTexture(badBlocks, "blocks"), "a block-compressed texture needs its mip chain");

    expect(assets.revision() == revision && assets.cpuBytes() == bytes, "failed creations add nothing");
}

void testGenerationsAndCollection() {
    AssetManager assets;
    const MaterialHandle material = createGreyMaterial(assets);
    const AssetLease<MaterialHandle> materialLease = assets.lease(material);
    const MeshHandle first = assets.createMesh(buildBox(material), "first").handle;
    AssetLease<MeshHandle> lease = assets.lease(first);
    expect(static_cast<bool>(lease), "a runtime mesh can be leased");

    AssetCollectionResult collected = assets.collectUnused(true);
    expect(assets.mesh(first) != nullptr && assets.material(material) != nullptr,
           "leased runtime assets survive collection");

    lease = {};
    collected = assets.collectUnused(true);
    expect(collected.assetsEvicted == 1 && assets.mesh(first) == nullptr,
           "an unleased runtime mesh is collected and its handle goes stale");

    const MeshHandle replacement = assets.createMesh(buildBox(material, glm::vec3(3.0f)), "replacement").handle;
    expect(replacement.index == first.index && replacement.generation != first.generation,
           "a recycled slot gets a new generation");
    expect(assets.mesh(first) == nullptr && assets.mesh(replacement) != nullptr &&
               assets.mesh(replacement)->localBox.maximum == glm::vec3(3.0f),
           "the stale handle never aliases the new mesh");
    const std::vector<AssetDiagnostic> diagnostics = assets.diagnostics();
    expect(std::ranges::any_of(diagnostics,
                               [&](const AssetDiagnostic& diagnostic) {
                                   return diagnostic.kind == AssetKind::Mesh && diagnostic.index == replacement.index &&
                                          diagnostic.source == "runtime:replacement";
                               }),
           "a recycled slot carries the new debug name");
}

void testMaterialOverride() {
    MaterialAsset base;
    base.baseColor = {0.5f, 0.4f, 0.2f, 0.8f};
    base.emissive = {0.1f, 0.0f, 0.0f};
    base.emissiveStrength = 2.0f;
    base.roughness = 0.6f;
    base.metallic = 0.5f;
    base.alphaMode = MaterialAlphaMode::Mask;
    base.baseColorTexture = TextureHandle{1, 1};

    MaterialOverrideComponent identity;
    expect(!materialOverrideChangesFactors(identity), "a default override changes nothing");
    identity.material = MaterialHandle{4, 1};
    expect(!materialOverrideChangesFactors(identity), "a replacement material alone needs no override record");

    const MaterialOverrideComponent tint{.baseColorFactor = {2.0f, 0.5f, 1.0f, 0.5f},
                                         .emissive = {0.0f, 1.0f, 0.0f},
                                         .emissiveStrength = 3.0f,
                                         .roughnessFactor = 2.0f,
                                         .metallicFactor = 0.5f};
    expect(materialOverrideChangesFactors(tint), "a tint changes factors");
    const MaterialAsset tinted = applyMaterialOverride(base, tint);
    expect(near(tinted.baseColor.r, 1.0f) && near(tinted.baseColor.g, 0.2f) && near(tinted.baseColor.b, 0.2f) &&
               near(tinted.baseColor.a, 0.4f),
           "the base color factor multiplies");
    expect(near(tinted.emissive.r, 0.2f) && near(tinted.emissive.g, 3.0f) && near(tinted.emissiveStrength, 1.0f),
           "the override's emission adds to the material's");
    expect(near(tinted.roughness, 1.0f) && near(tinted.metallic, 0.25f), "roughness and metallic multiply, clamped");
    expect(tinted.alphaMode == MaterialAlphaMode::Mask && tinted.baseColorTexture == base.baseColorTexture,
           "alpha mode and textures stay the material's");
}

void testOverrideLeasesAndSaving() {
    AssetManager assets;
    const MaterialHandle sharedMaterial = createGreyMaterial(assets);
    const AssetLoadResult<MaterialHandle> red =
        assets.createMaterial(MaterialAsset{.baseColor = {1.0f, 0.0f, 0.0f, 1.0f}}, "red");
    const MeshHandle mesh = assets.createMesh(buildBox(sharedMaterial), "box").handle;

    Scene scene;
    const Entity entity = scene.createEntity("Tinted");
    scene.attachMesh(entity, mesh);
    scene.registry().emplace<MaterialOverrideComponent>(
        entity, MaterialOverrideComponent{.baseColorFactor = {0.2f, 0.4f, 0.6f, 1.0f},
                                          .emissive = {0.5f, 0.0f, 0.0f},
                                          .emissiveStrength = 4.0f,
                                          .roughnessFactor = 0.5f,
                                          .metallicFactor = 0.0f,
                                          .material = red.handle});
    {
        const AssetLeaseSet leases = assets.leaseSceneAssets(scene);
        expect(leases.materials.size() == 1, "a scene lease holds an override's replacement material");
        assets.collectUnused(true);
        expect(assets.material(red.handle) != nullptr && assets.mesh(mesh) != nullptr,
               "assets a leased scene uses survive collection");
    }

    // Saving a runtime mesh is refused with a description; the factors of an
    // override save on their own.
    std::string document;
    const SceneDocumentResult refused = SceneDocument::serialize(scene, assets, {}, document);
    expect(!refused && refused.error.find("runtime") != std::string::npos,
           "a scene holding a runtime mesh cannot be saved: " + refused.error);

    scene.registry().remove<MeshRendererComponent>(entity);
    const SceneDocumentResult saved = SceneDocument::serialize(scene, assets, {}, document);
    expect(saved && document.find("material_override") != std::string::npos,
           "an override's factors save: " + saved.error);
    Scene restored;
    const SceneDocumentResult loaded = SceneDocument::deserialize(document, assets, {}, restored);
    expect(static_cast<bool>(loaded), "the saved override loads: " + loaded.error);
    const Entity reloaded = restored.findEntity(scene.id(entity));
    const MaterialOverrideComponent* values =
        reloaded != entt::null ? restored.registry().try_get<MaterialOverrideComponent>(reloaded) : nullptr;
    expect(values != nullptr && values->baseColorFactor == glm::vec4(0.2f, 0.4f, 0.6f, 1.0f) &&
               values->emissive == glm::vec3(0.5f, 0.0f, 0.0f) && values->emissiveStrength == 4.0f &&
               values->roughnessFactor == 0.5f && values->metallicFactor == 0.0f,
           "every factor round-trips");
    expect(values != nullptr && !values->material, "the runtime replacement material is not saved");
}

Entity addStaticMesh(Scene& scene, MeshHandle mesh, const glm::mat4& transform) {
    const Entity entity = scene.createEntity("Wall");
    scene.registry().get<TransformComponent>(entity).local = transform;
    scene.markTransformDirty(entity);
    scene.registry().emplace<ColliderComponent>(entity, TriangleMeshCollider{mesh});
    scene.registry().emplace<RigidBodyComponent>(entity);
    return entity;
}

void testShapeCache() {
    AssetManager assets;
    const MaterialHandle material = createGreyMaterial(assets);
    AssetLease<MeshHandle> lease;
    const MeshHandle wall = assets.createMesh(buildBox(material, {2.0f, 1.0f, 0.25f}), "wall").handle;
    lease = assets.lease(wall);

    Scene scene;
    std::vector<Entity> walls;
    for (int index = 0; index < 4; ++index) {
        // Rotated copies decompose to scales that differ only by rounding.
        walls.push_back(addStaticMesh(scene, wall,
                                      glm::translate(glm::mat4(1.0f), glm::vec3(index * 6.0f, 0.0f, 0.0f)) *
                                          glm::rotate(glm::mat4(1.0f), glm::radians(37.0f * index), {0, 1, 0})));
    }
    const Entity scaled = addStaticMesh(scene, wall,
                                        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f)) *
                                            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)));
    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    const PhysicsResult stepped = physics.step(scene, assets, physics.config().fixedTimeStep);
    expect(stepped && physics.stats().bodies == 5 && physics.stats().skippedBodies == 0,
           "static colliders of a runtime mesh build bodies");
    expect(physics.cachedMeshShapes() == 2, "identical mesh colliders share one shape, another scale has its own (" +
                                                std::to_string(physics.cachedMeshShapes()) + ")");
    PhysicsRayHit hit;
    expect(physics.raycast({0.0f, 10.0f, 10.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, hit) && hit.entity == scaled &&
               near(hit.position.y, 2.0f, 1.0e-3f),
           "a ray hits the scaled runtime mesh's top face");
    expect(physics.raycast({18.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, hit) && hit.entity == walls[3],
           "a ray hits a shared-shape copy");

    // A replacement mesh: the bodies rebuild on its new shape, and the old
    // mesh's entry goes once it is collected.
    const MeshHandle taller = assets.createMesh(buildBox(material, {2.0f, 3.0f, 0.25f}), "taller").handle;
    const AssetLease<MeshHandle> tallerLease = assets.lease(taller);
    for (const Entity entity : walls) {
        scene.registry().get<ColliderComponent>(entity).shape = TriangleMeshCollider{taller};
        scene.markPhysicsDirty(entity);
    }
    scene.destroyEntity(scaled);
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)), "physics steps");
    expect(physics.cachedMeshShapes() == 3, "the replacement mesh adds its shape");
    lease = {};
    assets.collectUnused(true);
    expect(assets.mesh(wall) == nullptr, "the replaced mesh is collected");
    expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)), "physics steps again");
    expect(physics.cachedMeshShapes() == 1,
           "entries of a collected mesh are dropped (" + std::to_string(physics.cachedMeshShapes()) + ")");
    expect(physics.raycast({18.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f, hit) && hit.entity == walls[3] &&
               near(hit.position.y, 3.0f, 1.0e-3f),
           "bodies collide with the replacement mesh");
    physics.shutdown();
    expect(physics.cachedMeshShapes() == 0, "shutdown releases the cached shapes");
}

// A convex hull collider follows its mesh and, unlike a triangle mesh, may sit
// on a dynamic body: a 2 x 1 x 2 m block falls onto a floor and rests on it.
void testConvexHullBody() {
    AssetManager assets;
    const MaterialHandle material = createGreyMaterial(assets);
    const MeshHandle block = assets.createMesh(buildBox(material, {1.0f, 0.5f, 1.0f}), "block").handle;
    const AssetLease<MeshHandle> lease = assets.lease(block);

    Scene scene;
    const Entity floor = scene.createEntity("Floor");
    scene.registry().get<TransformComponent>(floor).local = glm::translate(glm::mat4(1.0f), {0.0f, -0.5f, 0.0f});
    scene.markTransformDirty(floor);
    scene.registry().emplace<ColliderComponent>(floor, BoxCollider{{10.0f, 0.5f, 10.0f}});
    scene.registry().emplace<RigidBodyComponent>(floor);
    const Entity falling = scene.createEntity("Block");
    scene.registry().get<TransformComponent>(falling).local = glm::translate(glm::mat4(1.0f), {0.0f, 3.0f, 0.0f});
    scene.markTransformDirty(falling);
    scene.registry().emplace<ColliderComponent>(falling, ConvexHullCollider{block});
    scene.registry().emplace<RigidBodyComponent>(
        falling, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic, .layer = PhysicsLayer::Dynamic});

    PhysicsWorld physics;
    expect(static_cast<bool>(physics.init()), "physics initializes");
    expect(static_cast<bool>(physics.refreshBody(falling, scene, assets)), "a dynamic convex hull body builds");
    for (int step = 0; step < 240; ++step) {
        expect(static_cast<bool>(physics.step(scene, assets, physics.config().fixedTimeStep)), "physics steps");
    }
    const float height = scene.registry().get<TransformComponent>(falling).local[3].y;
    expect(near(height, 0.5f, 0.05f), "the hull comes to rest on the floor at its half height");
    PhysicsRayHit hit;
    expect(physics.raycast({0.4f, 5.0f, 0.4f}, {0.0f, -1.0f, 0.0f}, 10.0f, hit) && hit.entity == falling &&
               near(hit.position.y, height + 0.5f, 0.05f),
           "a ray hits the hull's top face");

    scene.registry().get<ColliderComponent>(falling).shape = ConvexHullCollider{};
    expect(!physics.refreshBody(falling, scene, assets), "a hull without a mesh is reported, not built");
    physics.shutdown();
}

void testNavigationFromRuntimeGeometry() {
    AssetManager assets;
    const MaterialHandle material = createGreyMaterial(assets);
    // A 10 m floor tile at y = 0, laid twice side by side.
    MeshBuilder builder;
    builder.beginPrimitive(material).rectangle({-5.0f, 0.0f, 5.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -10.0f});
    const MeshHandle tile = assets.createMesh(builder.build().mesh, "floor tile").handle;
    const MeshAsset* mesh = assets.mesh(tile);
    expect(mesh != nullptr, "the floor tile is created");
    if (mesh == nullptr) {
        return;
    }
    NavigationGeometry geometry;
    appendMeshGeometry(geometry, *mesh, glm::mat4(1.0f));
    appendMeshGeometry(geometry, *mesh, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)));
    expect(geometry.vertices.size() == 8 && geometry.indices.size() == 12, "each tile appends its triangles");
    const glm::vec3 a = geometry.vertices[geometry.indices[0]];
    const glm::vec3 b = geometry.vertices[geometry.indices[1]];
    const glm::vec3 c = geometry.vertices[geometry.indices[2]];
    expect(glm::cross(b - a, c - a).y > 0.0f, "an upward floor winds counter-clockwise from above");
    NavigationGeometry mirrored;
    appendMeshGeometry(mirrored, *mesh, glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
    const glm::vec3 ma = mirrored.vertices[mirrored.indices[0]];
    const glm::vec3 mb = mirrored.vertices[mirrored.indices[1]];
    const glm::vec3 mc = mirrored.vertices[mirrored.indices[2]];
    expect(glm::cross(mb - ma, mc - ma).y > 0.0f, "a mirroring transform keeps the floor facing up");

    DebugDraw debugDraw;
    Logger logger;
    GameServices services(assets, {}, debugDraw, &logger);
    const NavigationResult baked = services.outside().bakeNavigation(geometry);
    expect(static_cast<bool>(baked), "navigation bakes from raw geometry: " + baked.error);
    Scene scene;
    std::vector<glm::vec3> path;
    expect(services.outside().navigation(scene).findPath({-4.0f, 0.0f, 0.0f}, {14.0f, 0.0f, 0.0f}, path) ==
               NavPath::Complete,
           "a path crosses both runtime tiles");
    expect(!services.outside().bakeNavigation(NavigationGeometry{}), "empty geometry fails to bake");
    expect(!services.outside().navigation(scene).baked(), "a failed bake leaves no mesh, without a lazy rebake");
}
} // namespace

int main() {
    testMeshBuilder();
    testCreateAndResolve();
    testValidation();
    testGenerationsAndCollection();
    testMaterialOverride();
    testOverrideLeasesAndSaving();
    testShapeCache();
    testConvexHullBody();
    testNavigationFromRuntimeGeometry();
    if (failures == 0) {
        std::cout << "Runtime content tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

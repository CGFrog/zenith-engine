#pragma once

// Component types: ComponentType, ComponentTypeBuilder, ComponentTypeRegistry, and registerBuiltInComponentTypes.

#include "../reflection/Reflection.hpp"
#include "SceneDocument.hpp"

#include <nlohmann/json_fwd.hpp>

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Everything a component codec may use while writing a scene document.
struct ComponentSaveContext {
    const AssetManager& assets;
    const SceneDocumentOptions& options;
};

// Everything a component codec may use while reading a scene document.
struct ComponentLoadContext {
    AssetManager& assets;
    const SceneDocumentOptions& options;
};

template <typename Component> class ComponentTypeBuilder;

// A builder's asset reference field: its byte offset within the component and
// the asset kind it gets when its descriptor names none.
struct ComponentAssetReferenceOffset {
    std::ptrdiff_t offset{0};
    std::string kind;
};

// The reflected string field that addresses member within object, or null.
// Builders use it to find the field behind a member pointer.
const FieldDescriptor* findStringFieldAt(std::span<const FieldDescriptor> fields, void* object, const void* member);

// Runtime description of one ECS component type: its stable document key, how
// it is saved and loaded, how it is created and removed, and the reflected
// fields the editor inspects. Built with ComponentTypeBuilder and held by a
// ComponentTypeRegistry.
//
// Most types are described by reflected fields alone and serialize
// generically. Types whose data does not map to fields (asset handles,
// variants, dynamic arrays) supply a custom codec instead.
//
// Each type has a schema version. Versions above 1 are written to the
// component's document object as "$version"; older documents are upgraded by
// the registered migrations before they are read.
//
// Asset reference fields (texture paths and other `assetReference` fields)
// hold a project-relative path at runtime and are written to documents as
// reference objects (see SceneAssetReferences.hpp). They are converted back to
// path strings after the component migrations run, so a migration sees a
// field either as a reference object or as a legacy path string. Each such
// field carries FieldPresentation::assetKind, so editors know which files it
// takes.
class ComponentType {
  public:
    const std::string& key() const;
    const std::string& displayName() const;
    const std::string& category() const;
    // Internal types are created by the engine (for example by model
    // instantiation) rather than added by users, so editors hide them.
    bool internal() const;
    uint32_t schemaVersion() const;
    std::span<const FieldDescriptor> fields() const;

    bool has(const Scene& scene, Entity entity) const;
    // The entity's component, or null when it has none.
    void* find(Scene& scene, Entity entity) const;
    const void* find(const Scene& scene, Entity entity) const;
    // Adds a default-constructed component; the entity must not have one.
    void addDefault(Scene& scene, Entity entity) const;
    void remove(Scene& scene, Entity entity) const;

    // Copies a component value, and assigns one back, so callers can undo an
    // in-place edit without knowing the type.
    std::any copy(const void* component) const;
    void assign(void* component, const std::any& value) const;
    // A default-constructed component value, assignable with assign().
    std::any makeDefault() const;
    // Assigns the default value to the entity's component in place, keeping
    // the component (and its EnTT storage slot). Returns false, changing
    // nothing, when the entity has no such component.
    bool resetToDefault(Scene& scene, Entity entity) const;
    // Restores invariants an edit may have broken, such as a spot light's
    // outer cone narrower than its inner cone.
    void sanitize(void* component) const;
    // Empty when the component is valid, otherwise why it is not. Documents
    // containing an invalid component fail to load with this message.
    std::string validate(const void* component) const;
    // Whether the asset a loaded path field names (a texture() or material()
    // binding) is loaded; nullopt when the field is not such a path.
    std::optional<bool> assetLoaded(const void* component, std::string_view pathField) const;
    // Points the component's runtime handles (texture() and material()
    // bindings) at the assets its path fields name now, after an edit changed
    // a path in place: a path that no longer names the loaded asset is loaded
    // (or, when empty, its handle cleared). A handle with no source file (a
    // runtime asset code assigned) is kept while its path stays empty.
    // Without this, saving would write the stale handle's path over the
    // edited one. On failure the handle is cleared and the error returned.
    SceneDocumentResult reloadAssets(void* component, AssetManager& assets, const SceneDocumentOptions& options) const;
    // Points each texture() binding at replace(its handle), for a texture
    // reloaded or imported again; replace returns the handle unchanged to
    // keep it. Path fields are left alone. Returns whether a handle changed.
    bool replaceTextures(void* component, const std::function<TextureHandle(TextureHandle)>& replace) const;

    SceneDocumentResult save(const Scene& scene, Entity entity, const ComponentSaveContext& context,
                             nlohmann::json& output) const;
    // Reads the document object and adds the component to entity, replacing
    // any it already has. Nothing is added when reading fails.
    SceneDocumentResult load(const nlohmann::json& input, Scene& scene, Entity entity,
                             const ComponentLoadContext& context) const;

  private:
    template <typename Component> friend class ComponentTypeBuilder;
    friend class ComponentTypeRegistry;
    // Records the reflected string fields at the given byte offsets within
    // probe as asset references; returns why one is not such a field.
    friend std::string addAssetReferenceFields(ComponentType& type, void* probe,
                                               std::span<const ComponentAssetReferenceOffset> offsets);

    struct TextureBinding {
        std::string pathField;
        std::function<std::string&(void*)> path;
        std::function<TextureHandle&(void*)> handle;
    };

    struct MaterialBinding {
        std::string pathField;
        std::function<std::string&(void*)> path;
        std::function<MaterialHandle&(void*)> handle;
    };

    struct Migration {
        uint32_t fromVersion{1};
        std::function<SceneDocumentResult(nlohmann::json&)> apply;
    };

    ComponentType() = default;

    std::string m_key;
    std::string m_displayName;
    std::string m_category;
    bool m_internal{false};
    uint32_t m_schemaVersion{1};
    std::vector<FieldDescriptor> m_fields;
    std::vector<TextureBinding> m_textures;
    std::vector<MaterialBinding> m_materials;
    // Keys of reflected string fields saved as asset references, including
    // every texture path.
    std::vector<std::string> m_assetReferenceFields;
    std::vector<Migration> m_migrations;
    // Set by the builder when the description is inconsistent; the registry
    // refuses such types.
    std::string m_definitionError;

    std::function<void*(entt::registry&, Entity)> m_find;
    std::function<void(entt::registry&, Entity)> m_addDefault;
    std::function<void(entt::registry&, Entity)> m_remove;
    std::function<void(entt::registry&, Entity, std::any&&)> m_emplace;
    std::function<std::any()> m_makeDefault;
    std::function<void*(std::any&)> m_valueAddress;
    std::function<std::any(const void*)> m_copy;
    std::function<void(void*, const std::any&)> m_assign;
    std::function<std::string(const void*)> m_validate;
    std::function<void(void*)> m_sanitize;
    std::function<SceneDocumentResult(void*, const ComponentLoadContext&)> m_afterLoad;
    std::function<SceneDocumentResult(const void*, const ComponentSaveContext&, nlohmann::json&)> m_customSave;
    std::function<SceneDocumentResult(const nlohmann::json&, void*, const ComponentLoadContext&)> m_customLoad;
};

// Describes Component as a ComponentType:
//
//   ComponentTypeBuilder<PointLightComponent>("point_light", "Point Light")
//       .category("Lights")
//       .fields(Fields<PointLightComponent>().add("range", &PointLightComponent::range))
//       .validate([](const PointLightComponent& light) { return light.range > 0 ? "" : "range must be positive"; })
//       .build();
template <typename Component> class ComponentTypeBuilder {
  public:
    using Saver = std::function<SceneDocumentResult(const Component&, const ComponentSaveContext&, nlohmann::json&)>;
    using Loader = std::function<SceneDocumentResult(const nlohmann::json&, Component&, const ComponentLoadContext&)>;

    ComponentTypeBuilder(std::string key, std::string displayName) {
        m_type.m_key = std::move(key);
        m_type.m_displayName = std::move(displayName);
        m_type.m_find = [](entt::registry& registry, Entity entity) -> void* {
            return registry.try_get<Component>(entity);
        };
        m_type.m_addDefault = [](entt::registry& registry, Entity entity) { registry.emplace<Component>(entity); };
        m_type.m_remove = [](entt::registry& registry, Entity entity) { registry.remove<Component>(entity); };
        m_type.m_emplace = [](entt::registry& registry, Entity entity, std::any&& value) {
            registry.emplace_or_replace<Component>(entity, std::move(*std::any_cast<Component>(&value)));
        };
        m_type.m_makeDefault = [] { return std::any(Component{}); };
        m_type.m_valueAddress = [](std::any& value) -> void* { return std::any_cast<Component>(&value); };
        m_type.m_copy = [](const void* component) { return std::any(*static_cast<const Component*>(component)); };
        m_type.m_assign = [](void* component, const std::any& value) {
            *static_cast<Component*>(component) = *std::any_cast<Component>(&value);
        };
    }

    ComponentTypeBuilder& category(std::string name) {
        m_type.m_category = std::move(name);
        return *this;
    }

    ComponentTypeBuilder& internal() {
        m_type.m_internal = true;
        return *this;
    }

    ComponentTypeBuilder& fields(Fields<Component> fields) {
        m_type.m_fields = std::move(fields).build();
        return *this;
    }

    // Loads the texture named by a reflected path field into handle after
    // reading, and saves the loaded texture's path in place of the authored
    // one. The path member must be one of the reflected fields; its asset
    // kind defaults to "texture".
    ComponentTypeBuilder& texture(std::string Component::* path, TextureHandle Component::* handle) {
        m_pendingTextures.emplace_back(path, handle);
        return addAssetReference(path, "texture");
    }

    // Loads the material file (.zmat) named by a reflected path field into
    // handle after reading (AssetManager::loadMaterial). The authored path is
    // saved. Its asset kind defaults to "material".
    ComponentTypeBuilder& material(std::string Component::* path, MaterialHandle Component::* handle) {
        m_pendingMaterials.emplace_back(path, handle);
        return addAssetReference(path, "material");
    }

    // Saves a reflected path field as an asset reference (with the asset's
    // GUID when the document has an asset database) and resolves it by GUID
    // on load. Nothing is loaded; use texture() for texture paths. Its asset
    // kind defaults to "asset" (any file); name one with Fields::asset.
    ComponentTypeBuilder& assetReference(std::string Component::* path) {
        return addAssetReference(path, "asset");
    }

    ComponentTypeBuilder& validate(std::function<std::string(const Component&)> validator) {
        m_type.m_validate = [validator = std::move(validator)](const void* component) {
            return validator(*static_cast<const Component*>(component));
        };
        return *this;
    }

    ComponentTypeBuilder& sanitize(std::function<void(Component&)> sanitizer) {
        m_type.m_sanitize = [sanitizer = std::move(sanitizer)](void* component) {
            sanitizer(*static_cast<Component*>(component));
        };
        return *this;
    }

    // Runs after the component is read and validated, for derived data such
    // as baked lighting.
    ComponentTypeBuilder& afterLoad(std::function<SceneDocumentResult(Component&, const ComponentLoadContext&)> hook) {
        m_type.m_afterLoad = [hook = std::move(hook)](void* component, const ComponentLoadContext& context) {
            return hook(*static_cast<Component*>(component), context);
        };
        return *this;
    }

    // Replaces field-driven serialization. Validation, texture bindings, and
    // afterLoad still run.
    ComponentTypeBuilder& codec(Saver save, Loader load) {
        m_type.m_customSave = [save = std::move(save)](const void* component, const ComponentSaveContext& context,
                                                       nlohmann::json& output) {
            return save(*static_cast<const Component*>(component), context, output);
        };
        m_type.m_customLoad = [load = std::move(load)](const nlohmann::json& input, void* component,
                                                       const ComponentLoadContext& context) {
            return load(input, *static_cast<Component*>(component), context);
        };
        return *this;
    }

    ComponentTypeBuilder& schemaVersion(uint32_t version) {
        m_type.m_schemaVersion = version;
        return *this;
    }

    // Upgrades a document object written at fromVersion to fromVersion + 1.
    ComponentTypeBuilder& migration(uint32_t fromVersion, std::function<SceneDocumentResult(nlohmann::json&)> apply) {
        m_type.m_migrations.push_back({fromVersion, std::move(apply)});
        return *this;
    }

    // Consumes the builder; it must not be used afterwards.
    ComponentType build() {
        // A path member is identified by the field that addresses it.
        Component probe{};
        if (std::string error = addAssetReferenceFields(m_type, &probe, m_pendingAssetReferences); !error.empty()) {
            m_type.m_definitionError = std::move(error);
        }
        for (const auto& binding : m_pendingMaterials) {
            std::string Component::* const path = binding.first;
            MaterialHandle Component::* const handle = binding.second;
            const FieldDescriptor* pathField = findStringFieldAt(m_type.m_fields, &probe, &(probe.*path));
            if (pathField == nullptr) {
                m_type.m_definitionError = "A material path of '" + m_type.m_key + "' is not a reflected string field";
                continue;
            }
            m_type.m_materials.push_back(
                {pathField->key,
                 [path](void* component) -> std::string& { return static_cast<Component*>(component)->*path; },
                 [handle](void* component) -> MaterialHandle& { return static_cast<Component*>(component)->*handle; }});
        }
        for (const auto& binding : m_pendingTextures) {
            std::string Component::* const path = binding.first;
            TextureHandle Component::* const handle = binding.second;
            const FieldDescriptor* pathField = findStringFieldAt(m_type.m_fields, &probe, &(probe.*path));
            if (pathField == nullptr) {
                m_type.m_definitionError = "A texture path of '" + m_type.m_key + "' is not a reflected string field";
                continue;
            }
            m_type.m_textures.push_back(
                {pathField->key,
                 [path](void* component) -> std::string& { return static_cast<Component*>(component)->*path; },
                 [handle](void* component) -> TextureHandle& { return static_cast<Component*>(component)->*handle; }});
        }
        return std::move(m_type);
    }

  private:
    ComponentTypeBuilder& addAssetReference(std::string Component::* path, std::string kind) {
        // Kept as an offset so every component type shares one container type.
        const Component probe{};
        m_pendingAssetReferences.push_back(
            {reinterpret_cast<const char*>(&(probe.*path)) - reinterpret_cast<const char*>(&probe), std::move(kind)});
        return *this;
    }

    ComponentType m_type;
    std::vector<std::pair<std::string Component::*, TextureHandle Component::*>> m_pendingTextures;
    std::vector<std::pair<std::string Component::*, MaterialHandle Component::*>> m_pendingMaterials;
    std::vector<ComponentAssetReferenceOffset> m_pendingAssetReferences;
};

// The component types a scene document understands, in registration order.
// Build it fully before use: add() may move earlier entries, so pointers from
// find() are valid only while no type is being added.
class ComponentTypeRegistry {
  public:
    // Rejects inconsistent descriptions, duplicate keys, and schema versions
    // without a migration from every earlier version.
    SceneDocumentResult add(ComponentType type);
    const ComponentType* find(std::string_view key) const;
    std::span<const ComponentType> types() const;

  private:
    std::vector<ComponentType> m_types;
};

// Registers the engine's own components (lights, cameras, particles, physics,
// model instances, ...). Games start from these and add their own.
SceneDocumentResult registerBuiltInComponentTypes(ComponentTypeRegistry& registry);

// An immutable registry holding only the built-in types, created on first use.
std::shared_ptr<const ComponentTypeRegistry> builtInComponentTypes();

// The registry a document uses: options.componentTypes, or the built-ins.
const ComponentTypeRegistry& componentTypesFor(const SceneDocumentOptions& options);

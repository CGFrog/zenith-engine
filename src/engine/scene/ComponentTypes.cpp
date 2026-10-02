#include "ComponentTypes.hpp"

#include "../reflection/FieldJson.hpp"
#include "SceneAssetReferences.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace {
using Json = nlohmann::json;

constexpr const char* versionKey = "$version";
} // namespace

const std::string& ComponentType::key() const {
    return m_key;
}

const std::string& ComponentType::displayName() const {
    return m_displayName;
}

const std::string& ComponentType::category() const {
    return m_category;
}

bool ComponentType::internal() const {
    return m_internal;
}

uint32_t ComponentType::schemaVersion() const {
    return m_schemaVersion;
}

std::span<const FieldDescriptor> ComponentType::fields() const {
    return m_fields;
}

bool ComponentType::has(const Scene& scene, Entity entity) const {
    return find(scene, entity) != nullptr;
}

void* ComponentType::find(Scene& scene, Entity entity) const {
    if (!scene.registry().valid(entity)) {
        return nullptr;
    }
    return m_find(scene.registry(), entity);
}

const void* ComponentType::find(const Scene& scene, Entity entity) const {
    // try_get has no side effects; the registry is only non-const for storage lookup.
    return find(const_cast<Scene&>(scene), entity);
}

void ComponentType::addDefault(Scene& scene, Entity entity) const {
    m_addDefault(scene.registry(), entity);
}

void ComponentType::remove(Scene& scene, Entity entity) const {
    m_remove(scene.registry(), entity);
}

std::any ComponentType::copy(const void* component) const {
    return m_copy(component);
}

void ComponentType::assign(void* component, const std::any& value) const {
    m_assign(component, value);
}

void ComponentType::sanitize(void* component) const {
    if (m_sanitize) {
        m_sanitize(component);
    }
}

std::string ComponentType::validate(const void* component) const {
    return m_validate ? m_validate(component) : std::string{};
}

std::optional<bool> ComponentType::textureLoaded(const void* component, std::string_view pathField) const {
    for (const TextureBinding& binding : m_textures) {
        if (binding.pathField == pathField) {
            return static_cast<bool>(binding.handle(const_cast<void*>(component)));
        }
    }
    return std::nullopt;
}

SceneDocumentResult ComponentType::save(const Scene& scene, Entity entity, const ComponentSaveContext& context,
                                        Json& output) const {
    const void* component = find(scene, entity);
    if (component == nullptr) {
        return {.error = "Entity has no " + m_displayName + " component"};
    }
    if (m_customSave) {
        if (SceneDocumentResult result = m_customSave(component, context, output); !result) {
            return result;
        }
    } else {
        writeReflectedFields(component, m_fields, output);
    }
    for (const TextureBinding& binding : m_textures) {
        void* mutableComponent = const_cast<void*>(component);
        std::string reference;
        if (SceneDocumentResult result =
                projectTextureReference(context.assets, binding.handle(mutableComponent),
                                        binding.path(mutableComponent), context.options, reference);
            !result) {
            return result;
        }
        output[binding.pathField] = std::move(reference);
    }
    if (m_schemaVersion > 1) {
        output[versionKey] = m_schemaVersion;
    }
    return {.success = true};
}

SceneDocumentResult ComponentType::load(const Json& input, Scene& scene, Entity entity,
                                        const ComponentLoadContext& context) const {
    if (!input.is_object()) {
        return {.error = m_displayName + " must be an object"};
    }
    Json document = input;
    uint32_t version = 1;
    if (const auto found = document.find(versionKey); found != document.end()) {
        if (!found->is_number_unsigned() || found->get<uint64_t>() == 0) {
            return {.error = m_displayName + " schema version must be a positive whole number"};
        }
        version = found->get<uint32_t>();
        document.erase(versionKey);
    }
    if (version > m_schemaVersion) {
        return {.error = m_displayName + " was saved with schema version " + std::to_string(version) +
                         ", newer than the supported version " + std::to_string(m_schemaVersion)};
    }
    for (; version < m_schemaVersion; ++version) {
        const auto migration = std::ranges::find(m_migrations, version, &Migration::fromVersion);
        if (SceneDocumentResult result = migration->apply(document); !result) {
            return {.error = m_displayName + " migration from version " + std::to_string(version) +
                             " failed: " + result.error};
        }
    }

    std::any value = m_makeDefault();
    void* component = m_valueAddress(value);
    if (m_customLoad) {
        // Custom codecs read with nlohmann's checked accessors, which throw on
        // missing keys or wrong types.
        try {
            if (SceneDocumentResult result = m_customLoad(document, component, context); !result) {
                return result;
            }
        } catch (const nlohmann::json::exception& error) {
            return {.error = m_displayName + ": " + error.what()};
        }
    } else if (std::string error = readReflectedFields(document, component, m_fields); !error.empty()) {
        return {.error = m_displayName + ": " + error};
    }
    if (std::string error = validate(component); !error.empty()) {
        return {.error = std::move(error)};
    }
    for (const TextureBinding& binding : m_textures) {
        if (SceneDocumentResult result =
                loadProjectTexture(binding.path(component), context.assets, context.options, binding.handle(component));
            !result) {
            return result;
        }
    }
    if (m_afterLoad) {
        if (SceneDocumentResult result = m_afterLoad(component, context); !result) {
            return result;
        }
    }
    m_emplace(scene.registry(), entity, std::move(value));
    return {.success = true};
}

SceneDocumentResult ComponentTypeRegistry::add(ComponentType type) {
    const std::string name =
        type.m_key.empty() ? std::string("A component type") : "Component type '" + type.m_key + "'";
    if (type.m_key.empty() || type.m_key.front() == '$') {
        return {.error = name + " needs a key that does not start with '$'"};
    }
    if (find(type.m_key) != nullptr) {
        return {.error = name + " is already registered"};
    }
    if (!type.m_definitionError.empty()) {
        return {.error = type.m_definitionError};
    }
    if (std::string error = validateFieldDescriptors(type.m_fields); !error.empty()) {
        return {.error = name + ": " + error};
    }
    if (type.m_schemaVersion == 0) {
        return {.error = name + " has schema version 0; versions start at 1"};
    }
    for (uint32_t version = 1; version < type.m_schemaVersion; ++version) {
        if (std::ranges::count(type.m_migrations, version, &ComponentType::Migration::fromVersion) != 1) {
            return {.error = name + " needs exactly one migration from schema version " + std::to_string(version)};
        }
    }
    m_types.push_back(std::move(type));
    return {.success = true};
}

const ComponentType* ComponentTypeRegistry::find(std::string_view key) const {
    const auto found = std::ranges::find(m_types, key, &ComponentType::key);
    return found == m_types.end() ? nullptr : &*found;
}

std::span<const ComponentType> ComponentTypeRegistry::types() const {
    return m_types;
}

std::shared_ptr<const ComponentTypeRegistry> builtInComponentTypes() {
    static const std::shared_ptr<const ComponentTypeRegistry> registry = [] {
        auto created = std::make_shared<ComponentTypeRegistry>();
        // The built-in descriptions are fixed at compile time and covered by
        // tests, so a failure here is a programming error.
        if (const SceneDocumentResult result = registerBuiltInComponentTypes(*created); !result) {
            throw std::logic_error("Invalid built-in component types: " + result.error);
        }
        return std::shared_ptr<const ComponentTypeRegistry>(std::move(created));
    }();
    return registry;
}

const ComponentTypeRegistry& componentTypesFor(const SceneDocumentOptions& options) {
    if (options.componentTypes) {
        return *options.componentTypes;
    }
    return *builtInComponentTypes();
}

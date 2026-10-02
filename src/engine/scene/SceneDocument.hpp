#pragma once

#include "Scene.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

class AssetManager;
class ComponentTypeRegistry;

struct SceneDocumentOptions {
    std::filesystem::path assetRoot;
    // Component types the document reads and writes. Null uses the built-in
    // engine types; games that add their own pass a registry extending them.
    std::shared_ptr<const ComponentTypeRegistry> componentTypes;
};

struct SceneDocumentResult {
    bool success{false};
    std::string error;

    explicit operator bool() const {
        return success;
    }
};

class ParsedSceneDocument {
  public:
    ParsedSceneDocument() = default;

    explicit operator bool() const {
        return static_cast<bool>(m_impl);
    }

  private:
    friend class SceneDocument;
    struct Impl;
    std::shared_ptr<const Impl> m_impl;
};

class SceneDocument {
  public:
    static constexpr uint32_t currentVersion = 1;

    static SceneDocumentResult serialize(const Scene& scene, const AssetManager& assets,
                                         const SceneDocumentOptions& options, std::string& output);
    static SceneDocumentResult deserialize(std::string_view input, AssetManager& assets,
                                           const SceneDocumentOptions& options, Scene& destination);
    static SceneDocumentResult parse(std::string_view input, ParsedSceneDocument& output);
    static SceneDocumentResult commit(const ParsedSceneDocument& document, AssetManager& assets,
                                      const SceneDocumentOptions& options, Scene& destination);
    static SceneDocumentResult save(const std::filesystem::path& path, const Scene& scene, const AssetManager& assets,
                                    const SceneDocumentOptions& options);
    static SceneDocumentResult load(const std::filesystem::path& path, AssetManager& assets,
                                    const SceneDocumentOptions& options, Scene& destination);
};

#pragma once

// GPU copies of ThumbnailCache images, so ImGui can draw them.

#include "editor/project/ThumbnailCache.hpp"
#include "engine/graphics/Rendering.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

// GPU copies of ThumbnailCache images, so ImGui can draw them. At most
// capacity images are alive; asking for another destroys the one drawn least
// recently. An image whose thumbnail was decoded again (the file changed) is
// updated in place.
//
// The renderer owns the images (RenderRuntime::createUiImage), so the owner
// calls release with the runtime before the cache goes away; a cache that is
// destroyed without it leaves its images to the renderer until shutdown.
// Everything here runs on the UI thread.
class ThumbnailTextures {
  public:
    static constexpr size_t defaultCapacity = 128;

    explicit ThumbnailTextures(ThumbnailCache& thumbnails, size_t capacity = defaultCapacity);

    // The UI texture showing image's thumbnail, requesting the decode first.
    // Empty until the thumbnail is decoded and uploaded (the frame after it
    // is first asked for at the earliest), and for images that cannot be
    // decoded.
    UiTextureHandle texture(RenderRuntime& rendering, const std::filesystem::path& image);
    // Destroys every image.
    void release(RenderRuntime& rendering);
    size_t size() const;

  private:
    struct Entry {
        UiImageId image;
        uint64_t serial{0};
        uint64_t lastUse{0};
    };

    void evictLeastRecentlyUsed(RenderRuntime& rendering);

    ThumbnailCache& m_thumbnails;
    size_t m_capacity;
    std::unordered_map<std::string, Entry> m_entries;
    uint64_t m_useCounter{0};
};

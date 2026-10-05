#include "ThumbnailTextures.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace {
std::string key(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}
} // namespace

ThumbnailTextures::ThumbnailTextures(ThumbnailCache& thumbnails, size_t capacity)
    : m_thumbnails(thumbnails)
    , m_capacity(std::max<size_t>(capacity, 1)) {}

UiTextureHandle ThumbnailTextures::texture(RenderRuntime& rendering, const std::filesystem::path& image) {
    m_thumbnails.request(image);
    const std::string imageKey = key(image);
    const auto existing = m_entries.find(imageKey);
    const std::optional<ThumbnailStatus> status = m_thumbnails.status(image);
    const bool ready = status && status->state == ThumbnailState::Ready;
    if (existing != m_entries.end()) {
        Entry& entry = existing->second;
        entry.lastUse = ++m_useCounter;
        // A newer decode replaces the pixels; until then the old ones show.
        if (ready && status->serial != entry.serial) {
            if (std::optional<CachedThumbnail> decoded = m_thumbnails.find(image);
                decoded && decoded->state == ThumbnailState::Ready) {
                rendering.updateUiImage(entry.image, {.width = decoded->width,
                                                      .height = decoded->height,
                                                      .rgba = std::move(decoded->rgba)});
                entry.serial = decoded->serial;
            }
        }
        return rendering.uiImageTexture(entry.image);
    }
    if (!ready) {
        return {};
    }
    std::optional<CachedThumbnail> decoded = m_thumbnails.find(image);
    if (!decoded || decoded->state != ThumbnailState::Ready) {
        return {};
    }
    if (m_entries.size() >= m_capacity) {
        evictLeastRecentlyUsed(rendering);
    }
    const uint64_t serial = decoded->serial;
    const UiImageId created =
        rendering.createUiImage({.width = decoded->width, .height = decoded->height, .rgba = std::move(decoded->rgba)});
    if (!created) {
        return {};
    }
    m_entries.emplace(imageKey, Entry{.image = created, .serial = serial, .lastUse = ++m_useCounter});
    // Published once the renderer uploads it, a frame later.
    return rendering.uiImageTexture(created);
}

void ThumbnailTextures::release(RenderRuntime& rendering) {
    for (const auto& [path, entry] : m_entries) {
        rendering.destroyUiImage(entry.image);
    }
    m_entries.clear();
}

size_t ThumbnailTextures::size() const {
    return m_entries.size();
}

void ThumbnailTextures::evictLeastRecentlyUsed(RenderRuntime& rendering) {
    const auto oldest = std::ranges::min_element(
        m_entries, {}, [](const std::pair<const std::string, Entry>& entry) { return entry.second.lastUse; });
    if (oldest == m_entries.end()) {
        return;
    }
    rendering.destroyUiImage(oldest->second.image);
    m_entries.erase(oldest);
}

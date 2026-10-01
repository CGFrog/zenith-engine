#include "ThumbnailCache.hpp"

#include "engine/assets/ImageDecoder.hpp"

#include <algorithm>

namespace {
constexpr uint32_t maximumThumbnailExtent = 96;

std::string key(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

CachedThumbnail downsample(TextureAsset source) {
    const float scale = std::min(1.0f, static_cast<float>(maximumThumbnailExtent) /
                                           static_cast<float>(std::max(source.width, source.height)));
    const uint32_t width = std::max(1u, static_cast<uint32_t>(source.width * scale));
    const uint32_t height = std::max(1u, static_cast<uint32_t>(source.height * scale));
    CachedThumbnail thumbnail{.state = ThumbnailState::Ready, .width = width, .height = height};
    thumbnail.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t sourceY = std::min(source.height - 1, y * source.height / height);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t sourceX = std::min(source.width - 1, x * source.width / width);
            const size_t sourceOffset = (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
            const size_t destinationOffset = (static_cast<size_t>(y) * width + x) * 4;
            std::copy_n(source.rgba.data() + sourceOffset, 4, thumbnail.rgba.data() + destinationOffset);
        }
    }
    return thumbnail;
}
} // namespace

ThumbnailCache::ThumbnailCache()
    : m_worker([this](std::stop_token stopToken) { work(stopToken); }) {}

ThumbnailCache::~ThumbnailCache() {
    m_worker.request_stop();
    m_wake.notify_all();
}

void ThumbnailCache::request(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    if (m_entries.contains(key(path))) {
        return;
    }
    m_entries.emplace(key(path), CachedThumbnail{});
    m_queue.push_back(path);
    m_wake.notify_one();
}

std::optional<CachedThumbnail> ThumbnailCache::find(const std::filesystem::path& path) const {
    std::lock_guard lock(m_mutex);
    const auto found = m_entries.find(key(path));
    return found == m_entries.end() ? std::nullopt : std::optional<CachedThumbnail>{found->second};
}

void ThumbnailCache::invalidate(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    m_entries.erase(key(path));
}

void ThumbnailCache::work(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        std::filesystem::path path;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, stopToken, [this] { return !m_queue.empty(); });
            if (stopToken.stop_requested()) {
                return;
            }
            path = std::move(m_queue.front());
            m_queue.pop_front();
        }

        ImageDecodeResult decoded = decodeImageFile(path);
        CachedThumbnail thumbnail;
        if (decoded && decoded.texture.blockCompressed()) {
            // Thumbnails sample RGBA8 texels; block-compressed KTX2 has none on the CPU.
            thumbnail.state = ThumbnailState::Failed;
            thumbnail.error = "Block-compressed textures have no thumbnail preview";
        } else if (decoded) {
            thumbnail = downsample(std::move(decoded.texture));
        } else {
            thumbnail.state = ThumbnailState::Failed;
            thumbnail.error = std::move(decoded.error);
        }
        std::lock_guard lock(m_mutex);
        if (const auto found = m_entries.find(key(path)); found != m_entries.end()) {
            found->second = std::move(thumbnail);
        }
    }
}

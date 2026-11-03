#include "ThumbnailCache.hpp"

#include "engine/assets/ImageDecoder.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr uint32_t maximumThumbnailExtent = 96;

std::string key(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

// One display texel. High-dynamic-range sources are compressed with Reinhard
// and gamma-encoded so radiance above one still previews.
void copyPreviewTexel(const TextureAsset& source, size_t sourceOffset, uint8_t* destination) {
    if (source.rgbaHalf.empty()) {
        std::copy_n(source.rgba.data() + sourceOffset, 4, destination);
        return;
    }
    for (size_t channel = 0; channel < 3; ++channel) {
        const float radiance = std::max(glm::unpackHalf1x16(source.rgbaHalf[sourceOffset + channel]), 0.0f);
        const float display = std::pow(radiance / (1.0f + radiance), 1.0f / 2.2f);
        destination[channel] = static_cast<uint8_t>(std::lround(std::clamp(display, 0.0f, 1.0f) * 255.0f));
    }
    destination[3] = 255;
}

CachedThumbnail downsample(TextureAsset source) {
    // Integer scaling, so the longer side is exactly the extent (a float
    // scale truncated some sources one texel short).
    const uint32_t longest = std::max(source.width, source.height);
    const uint32_t extent = std::min(longest, maximumThumbnailExtent);
    const auto fit = [longest, extent](uint32_t side) {
        return std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(side) * extent / longest));
    };
    const uint32_t width = fit(source.width);
    const uint32_t height = fit(source.height);
    CachedThumbnail thumbnail{.state = ThumbnailState::Ready, .width = width, .height = height};
    thumbnail.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t sourceY = std::min(source.height - 1, y * source.height / height);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t sourceX = std::min(source.width - 1, x * source.width / width);
            const size_t sourceOffset = (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
            const size_t destinationOffset = (static_cast<size_t>(y) * width + x) * 4;
            copyPreviewTexel(source, sourceOffset, thumbnail.rgba.data() + destinationOffset);
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

std::optional<ThumbnailStatus> ThumbnailCache::status(const std::filesystem::path& path) const {
    std::lock_guard lock(m_mutex);
    const auto found = m_entries.find(key(path));
    if (found == m_entries.end()) {
        return std::nullopt;
    }
    return ThumbnailStatus{.state = found->second.state, .serial = found->second.serial};
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
        thumbnail.serial = ++m_nextSerial;
        std::lock_guard lock(m_mutex);
        if (const auto found = m_entries.find(key(path)); found != m_entries.end()) {
            found->second = std::move(thumbnail);
        }
    }
}

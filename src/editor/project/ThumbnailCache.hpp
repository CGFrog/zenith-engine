#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class ThumbnailState {
    Queued,
    Ready,
    Failed,
};

struct CachedThumbnail {
    ThumbnailState state{ThumbnailState::Queued};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;
    std::string error;
};

// Decodes and downsamples source images away from the render/UI thread. GPU
// upload remains renderer-owned; this cache contains only immutable CPU data.
class ThumbnailCache {
  public:
    ThumbnailCache();
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    void request(const std::filesystem::path& path);
    std::optional<CachedThumbnail> find(const std::filesystem::path& path) const;
    void invalidate(const std::filesystem::path& path);

  private:
    void work(std::stop_token stopToken);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::filesystem::path> m_queue;
    std::unordered_map<std::string, CachedThumbnail> m_entries;
    std::jthread m_worker;
};

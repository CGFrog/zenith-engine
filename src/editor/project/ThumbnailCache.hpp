#pragma once

// Asset Browser thumbnails, decoded and downsampled off the UI thread.

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
    // Distinguishes decodes: a later decode of the same path (after
    // invalidate) has a larger serial. Zero while queued.
    uint64_t serial{0};
};

// A thumbnail's state without its pixels, cheap to ask for every frame.
struct ThumbnailStatus {
    ThumbnailState state{ThumbnailState::Queued};
    uint64_t serial{0};
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
    // nullopt when path was never requested (or was invalidated since).
    std::optional<ThumbnailStatus> status(const std::filesystem::path& path) const;
    void invalidate(const std::filesystem::path& path);

  private:
    void work(std::stop_token stopToken);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<std::filesystem::path> m_queue;
    std::unordered_map<std::string, CachedThumbnail> m_entries;
    // Worker thread only.
    uint64_t m_nextSerial{0};
    std::jthread m_worker;
};

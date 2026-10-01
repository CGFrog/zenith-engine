#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

enum class ResidencyState : uint8_t {
    Requested,
    Decoding,
    UploadPending,
    Resident,
    EvictionPending,
    Retired,
};

enum class StreamSegmentKind : uint8_t {
    TextureMipRange,
    MeshChunk,
};

struct StreamAssetHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    uint32_t generation{0};

    explicit operator bool() const {
        return index != std::numeric_limits<uint32_t>::max();
    }

    auto operator<=>(const StreamAssetHandle&) const = default;
};

struct StreamSegmentDescription {
    StreamSegmentKind kind{StreamSegmentKind::TextureMipRange};
    uint32_t first{0};
    uint32_t count{1};
    uint64_t decodedBytes{0};
    uint64_t uploadBytes{0};
    bool required{false};
};

struct StreamPriority {
    float projectedVisibility{0.0f};
    float distance{std::numeric_limits<float>::max()};
    float gameplayImportance{0.0f};
    uint64_t deadlineFrame{std::numeric_limits<uint64_t>::max()};
};

struct StreamAssetRequest {
    std::string key;
    uint64_t cpuBytes{0};
    StreamPriority priority;
    std::vector<StreamSegmentDescription> segments;
};

struct StreamingBudgets {
    uint64_t decodeBytesPerFrame{16ull * 1024ull * 1024ull};
    uint64_t uploadBytesPerFrame{16ull * 1024ull * 1024ull};
    uint32_t decodeJobsPerFrame{4};
    uint32_t uploadJobsPerFrame{8};
    uint64_t cpuResidentBytes{512ull * 1024ull * 1024ull};
    uint64_t gpuResidentBytes{1024ull * 1024ull * 1024ull};
    uint32_t unusedFrameThreshold{120};
};

struct StreamWorkItem {
    StreamAssetHandle asset;
    uint32_t segment{std::numeric_limits<uint32_t>::max()};
    uint64_t bytes{0};
};

struct AssetResidencySnapshot {
    StreamAssetHandle handle;
    std::string key;
    ResidencyState cpuState{ResidencyState::Requested};
    ResidencyState gpuState{ResidencyState::Requested};
    uint32_t residentSegments{0};
    uint32_t totalSegments{0};
    uint64_t cpuBytes{0};
    uint64_t gpuBytes{0};
    uint64_t lastUsedFrame{0};
};

struct StreamingStats {
    uint64_t frame{0};
    uint64_t decodedBytes{0};
    uint64_t uploadedBytes{0};
    uint64_t decodeBacklogBytes{0};
    uint64_t uploadBacklogBytes{0};
    uint64_t cpuResidentBytes{0};
    uint64_t gpuResidentBytes{0};
    uint64_t peakCpuResidentBytes{0};
    uint64_t peakGpuResidentBytes{0};
    uint64_t temporaryCacheBytes{0};
    uint64_t temporaryCacheBytesDiscarded{0};
    uint64_t distantMipBytesDropped{0};
    uint32_t decodeJobs{0};
    uint32_t uploadJobs{0};
    uint32_t residencyMisses{0};
    uint32_t cancelledCompletions{0};
    uint32_t evictedAssets{0};
    uint32_t hardBudgetFailures{0};
};

class DerivedAssetResidencyManager {
  public:
    explicit DerivedAssetResidencyManager(StreamingBudgets budgets = {});
    ~DerivedAssetResidencyManager();

    StreamAssetHandle request(StreamAssetRequest request);
    bool updatePriority(StreamAssetHandle asset, StreamPriority priority);
    bool cancel(StreamAssetHandle asset, uint64_t retireAfterSerial = 0);
    void touch(StreamAssetHandle asset, StreamPriority priority);

    void beginFrame(uint64_t frame);
    std::vector<StreamWorkItem> takeDecodeWork();
    bool completeDecode(StreamAssetHandle asset, uint64_t decodedBytes);
    std::vector<StreamWorkItem> takeUploadWork();
    bool completeUpload(StreamAssetHandle asset, uint32_t segment, uint64_t uploadedBytes);

    void setTemporaryCacheBytes(uint64_t bytes);
    bool enforceMemoryBudgets(uint64_t retireAfterSerial);
    void retire(uint64_t completedSerial);
    void noteResidencyMiss(StreamAssetHandle asset);

    std::optional<AssetResidencySnapshot> snapshot(StreamAssetHandle asset) const;
    std::vector<AssetResidencySnapshot> snapshots() const;
    const StreamingStats& stats() const;
    const StreamingBudgets& budgets() const;

  private:
    struct Segment;
    struct Asset;

    Asset* find(StreamAssetHandle asset);
    const Asset* find(StreamAssetHandle asset) const;
    double priorityScore(const Asset& asset) const;
    void refreshBacklog();
    void markForEviction(Asset& asset, uint64_t retireAfterSerial);

    StreamingBudgets m_budgets;
    std::vector<Asset> m_assets;
    StreamingStats m_stats;
    uint64_t m_decodeBytesRemaining{0};
    uint64_t m_uploadBytesRemaining{0};
    uint32_t m_decodeJobsRemaining{0};
    uint32_t m_uploadJobsRemaining{0};
};

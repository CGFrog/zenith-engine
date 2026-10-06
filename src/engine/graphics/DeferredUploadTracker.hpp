#pragma once

// Which renderer-owned GPU copy of an asset could not be created this frame.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Which renderer-owned GPU copy of an asset could not be created this frame.
enum class DeferredUploadKind : uint8_t {
    Mesh,
    Texture,
};

// An asset that has stayed deferred past the configured bound.
struct DeferredUploadWarning {
    DeferredUploadKind kind{DeferredUploadKind::Mesh};
    // The asset's generational handle key: (generation << 32) | index.
    uint64_t assetKey{0};
    // Consecutive frames, including this one, the asset was deferred.
    uint32_t deferredFrames{0};
};

// One closed frame of deferral bookkeeping.
struct DeferredUploadFrameSummary {
    // Distinct assets deferred during the frame, per kind.
    uint32_t deferredMeshes{0};
    uint32_t deferredTextures{0};
    // Assets that crossed the warning bound during this frame. Each deferral
    // episode of an asset appears here at most once. Valid until the next
    // endFrame() call.
    std::span<const DeferredUploadWarning> overdue;
};

// Human-readable description of an overdue upload, suitable for a log line.
std::string describeDeferredUpload(const DeferredUploadWarning& warning);

// Tracks how many consecutive frames each asset's GPU upload has been deferred
// (for example because the staging ring was full), so a retry that never
// succeeds is reported instead of silently drawing nothing or a fallback.
//
// Frames are delimited by endFrame(). An asset reported deferred any number of
// times within a frame ages by one frame. An asset that is not reported during
// a frame is forgotten at endFrame(): its upload landed, it was released, or
// nothing requested it. Memory is therefore bounded by the number of distinct
// assets deferred in the current and previous frame. CPU-only and independent
// of the graphics API.
class DeferredUploadTracker {
  public:
    struct Configuration {
        // Warn once when an asset stays deferred for more than this many
        // consecutive frames. Zero disables warnings (counting continues).
        uint32_t warningFrames{120};
    };

    DeferredUploadTracker() = default;
    explicit DeferredUploadTracker(Configuration configuration);

    void setConfiguration(Configuration configuration) {
        m_configuration = configuration;
    }

    const Configuration& configuration() const {
        return m_configuration;
    }

    // Records that the asset's upload was attempted and deferred this frame.
    void markDeferred(DeferredUploadKind kind, uint64_t assetKey);
    // Records that the asset's upload landed; a later deferral starts a new
    // episode with a fresh age and may warn again.
    void markUploaded(DeferredUploadKind kind, uint64_t assetKey);
    // Forgets the asset immediately, for example when it was released.
    void release(DeferredUploadKind kind, uint64_t assetKey);

    // Closes the current frame: ages every asset deferred during it, forgets
    // the others, and returns the frame's counts and newly overdue assets.
    DeferredUploadFrameSummary endFrame();

    // True when the asset was reported deferred during the open frame.
    bool deferredThisFrame(DeferredUploadKind kind, uint64_t assetKey) const;
    // Assets currently remembered across all kinds.
    size_t trackedAssetCount() const;
    // Consecutive deferred frames recorded for the asset; zero when untracked.
    uint32_t deferredFrames(DeferredUploadKind kind, uint64_t assetKey) const;

  private:
    struct Entry {
        // Closed frames the asset has been deferred in a row.
        uint32_t deferredFrames{0};
        bool deferredThisFrame{false};
        bool warned{false};
    };

    using EntryMap = std::unordered_map<uint64_t, Entry>;

    EntryMap& entries(DeferredUploadKind kind);
    const EntryMap& entries(DeferredUploadKind kind) const;

    Configuration m_configuration;
    std::array<EntryMap, 2> m_entries;
    std::vector<DeferredUploadWarning> m_overdue;
};

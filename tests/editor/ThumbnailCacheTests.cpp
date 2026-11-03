// The Asset Browser's ThumbnailCache: how decoded images are downsampled
// (sizes, sampled texels, HDR tone mapping), request/find/status/invalidate
// bookkeeping, failed decodes, and stopping the worker with requests pending.
// The worker is waited for by polling its results against a generous deadline.

#include "editor/project/ThumbnailCache.hpp"
#include "engine/diagnostics/RgbaImage.hpp"

#include "support/Doctest.hpp"
#include "support/Files.hpp"
#include "support/TempDirectory.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_BEGIN("ThumbnailCache");

namespace {

constexpr auto workerDeadline = std::chrono::seconds(30);

// An image whose texel (x, y) is (x, y, 7, 255), so a sampled texel says
// where it came from (coordinates wrap past 255).
RgbaImage coordinateImage(uint32_t width, uint32_t height) {
    RgbaImage image{.width = width, .height = height};
    image.pixels.reserve(static_cast<size_t>(width) * height * RgbaImage::channelCount);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            image.pixels.insert(image.pixels.end(),
                                {static_cast<uint8_t>(x), static_cast<uint8_t>(y), uint8_t{7}, uint8_t{255}});
        }
    }
    return image;
}

std::filesystem::path writeCoordinatePng(const testsupport::TempDirectory& folder, const char* name, uint32_t width,
                                         uint32_t height) {
    const std::filesystem::path path = folder / name;
    const ImageFileResult written = writePng(path, coordinateImage(width, height));
    REQUIRE_MESSAGE(static_cast<bool>(written), written.error);
    return path;
}

// The thumbnail once the worker has decoded path (Ready or Failed); what the
// cache holds at the deadline otherwise.
std::optional<CachedThumbnail> waitForDecode(const ThumbnailCache& cache, const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + workerDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<ThumbnailStatus> status = cache.status(path);
        if (!status || status->state != ThumbnailState::Queued) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cache.find(path);
}

std::optional<CachedThumbnail> decode(const std::filesystem::path& path) {
    ThumbnailCache cache;
    cache.request(path);
    return waitForDecode(cache, path);
}

// The RGBA texel at (x, y).
std::array<uint8_t, 4> texel(const CachedThumbnail& thumbnail, uint32_t x, uint32_t y) {
    const size_t offset = (static_cast<size_t>(y) * thumbnail.width + x) * 4;
    return {thumbnail.rgba[offset], thumbnail.rgba[offset + 1], thumbnail.rgba[offset + 2], thumbnail.rgba[offset + 3]};
}

// A flat (not run-length encoded) Radiance HDR file, one RGBE texel per
// pixel in a single row.
std::vector<uint8_t> radianceFile(const std::vector<std::array<uint8_t, 4>>& rgbe) {
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X " + std::to_string(rgbe.size()) + "\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    for (const std::array<uint8_t, 4>& texel : rgbe) {
        bytes.insert(bytes.end(), texel.begin(), texel.end());
    }
    return bytes;
}

} // namespace

TEST_CASE("ThumbnailCache: a large image is downsampled to fit 96 texels keeping its aspect") {
    const testsupport::TempDirectory folder("thumbnail-downsample");
    const std::optional<CachedThumbnail> thumbnail = decode(writeCoordinatePng(folder, "wide.png", 192, 96));
    REQUIRE(thumbnail.has_value());
    REQUIRE(thumbnail->state == ThumbnailState::Ready);
    CHECK(thumbnail->width == 96);
    CHECK(thumbnail->height == 48);
    REQUIRE(thumbnail->rgba.size() == size_t{96} * 48 * 4);
    CHECK(thumbnail->error.empty());
    // Each texel is the source texel at twice its coordinates.
    for (const auto& [x, y] : std::array<std::pair<uint32_t, uint32_t>, 4>{{{0, 0}, {1, 0}, {10, 7}, {95, 47}}}) {
        CAPTURE(x);
        CAPTURE(y);
        CHECK(texel(*thumbnail, x, y) ==
              std::array<uint8_t, 4>{static_cast<uint8_t>(2 * x), static_cast<uint8_t>(2 * y), 7, 255});
    }
}

TEST_CASE("ThumbnailCache: the longer side is exactly 96 texels whatever the source size") {
    struct Size {
        uint32_t width;
        uint32_t height;
        uint32_t thumbnailWidth;
        uint32_t thumbnailHeight;
    };

    // 148 and 191 lose a texel when the scale is a rounded float.
    const std::array<Size, 5> sizes{{
        {148, 74, 96, 48},
        {191, 10, 96, 5},
        {10, 300, 3, 96},
        {1, 500, 1, 96},
        {1000, 1000, 96, 96},
    }};
    const testsupport::TempDirectory folder("thumbnail-sizes");
    for (const Size& size : sizes) {
        CAPTURE(size.width);
        CAPTURE(size.height);
        const std::string name = std::to_string(size.width) + "x" + std::to_string(size.height) + ".png";
        const std::optional<CachedThumbnail> thumbnail =
            decode(writeCoordinatePng(folder, name.c_str(), size.width, size.height));
        REQUIRE(thumbnail.has_value());
        REQUIRE(thumbnail->state == ThumbnailState::Ready);
        CHECK(thumbnail->width == size.thumbnailWidth);
        CHECK(thumbnail->height == size.thumbnailHeight);
        CHECK(thumbnail->rgba.size() == size_t{thumbnail->width} * thumbnail->height * 4);
    }
}

TEST_CASE("ThumbnailCache: an image within 96 texels keeps its size and texels") {
    const testsupport::TempDirectory folder("thumbnail-small");
    const RgbaImage source = coordinateImage(5, 3);
    const std::filesystem::path path = folder / "small.png";
    REQUIRE(static_cast<bool>(writePng(path, source)));
    const std::optional<CachedThumbnail> thumbnail = decode(path);
    REQUIRE(thumbnail.has_value());
    REQUIRE(thumbnail->state == ThumbnailState::Ready);
    CHECK(thumbnail->width == 5);
    CHECK(thumbnail->height == 3);
    CHECK_MESSAGE(thumbnail->rgba == source.pixels, "a small image should not be resampled");
}

TEST_CASE("ThumbnailCache: high dynamic range texels are tone mapped for display") {
    const testsupport::TempDirectory folder("thumbnail-hdr");
    const std::filesystem::path path = folder / "sky.hdr";
    // Radiance 1, 3, and 0: mantissa * 2^(exponent - 136).
    testsupport::writeBytes(path, radianceFile({{128, 128, 128, 129}, {192, 192, 192, 130}, {0, 0, 0, 0}}));
    const std::optional<CachedThumbnail> thumbnail = decode(path);
    REQUIRE(thumbnail.has_value());
    REQUIRE_MESSAGE(thumbnail->state == ThumbnailState::Ready, thumbnail->error);
    REQUIRE(thumbnail->width == 3);
    REQUIRE(thumbnail->height == 1);
    // Reinhard (r / (1 + r)), then gamma 2.2: 1 -> 0.5 -> 186, 3 -> 0.75 -> 224.
    CHECK(texel(*thumbnail, 0, 0) == std::array<uint8_t, 4>{186, 186, 186, 255});
    CHECK(texel(*thumbnail, 1, 0) == std::array<uint8_t, 4>{224, 224, 224, 255});
    CHECK(texel(*thumbnail, 2, 0) == std::array<uint8_t, 4>{0, 0, 0, 255});
}

TEST_CASE("ThumbnailCache: a file that cannot be decoded fails with a reason") {
    const testsupport::TempDirectory folder("thumbnail-failed");
    const std::filesystem::path garbage = folder / "garbage.png";
    testsupport::writeText(garbage, "not an image");
    const std::filesystem::path missing = folder / "missing.png";
    for (const std::filesystem::path& path : {garbage, missing}) {
        CAPTURE(path.filename().string());
        const std::optional<CachedThumbnail> thumbnail = decode(path);
        REQUIRE(thumbnail.has_value());
        CHECK(thumbnail->state == ThumbnailState::Failed);
        CHECK_FALSE(thumbnail->error.empty());
        CHECK(thumbnail->rgba.empty());
        CHECK_MESSAGE(thumbnail->serial != 0, "a failed decode is still a decode");
    }
}

TEST_CASE("ThumbnailCache: requests are found by normalized path until invalidated") {
    const testsupport::TempDirectory folder("thumbnail-bookkeeping");
    const std::filesystem::path path = writeCoordinatePng(folder, "plate.png", 8, 8);
    const std::filesystem::path samePath = folder.path() / "sub" / ".." / "." / "plate.png";
    ThumbnailCache cache;
    CHECK_FALSE(cache.find(path).has_value());
    CHECK_FALSE(cache.status(path).has_value());

    cache.request(path);
    const std::optional<ThumbnailStatus> queued = cache.status(path);
    REQUIRE_MESSAGE(queued.has_value(), "a request should be known at once");
    CHECK((queued->state == ThumbnailState::Queued || queued->state == ThumbnailState::Ready));

    const std::optional<CachedThumbnail> first = waitForDecode(cache, path);
    REQUIRE(first.has_value());
    REQUIRE(first->state == ThumbnailState::Ready);
    CHECK(first->serial != 0);
    const std::optional<ThumbnailStatus> status = cache.status(samePath);
    REQUIRE_MESSAGE(status.has_value(), "another spelling of the path should find the same entry");
    CHECK(status->state == ThumbnailState::Ready);
    CHECK(status->serial == first->serial);

    // Requesting a known path again decodes nothing: a later request for
    // another path is served (one worker, in order) without a new serial here.
    cache.request(samePath);
    const std::filesystem::path other = writeCoordinatePng(folder, "other.png", 4, 4);
    cache.request(other);
    REQUIRE(waitForDecode(cache, other).has_value());
    CHECK_MESSAGE(cache.status(path)->serial == first->serial, "a repeated request should not decode again");

    cache.invalidate(samePath);
    CHECK_FALSE(cache.find(path).has_value());
    CHECK_FALSE(cache.status(path).has_value());
    CHECK_MESSAGE(cache.status(other).has_value(), "invalidating should leave other entries");

    cache.request(path);
    const std::optional<CachedThumbnail> second = waitForDecode(cache, path);
    REQUIRE(second.has_value());
    CHECK(second->state == ThumbnailState::Ready);
    CHECK_MESSAGE(second->serial > first->serial, "a decode after invalidating should have a larger serial");
}

TEST_CASE("ThumbnailCache: invalidating a pending request drops its result") {
    const testsupport::TempDirectory folder("thumbnail-invalidate-pending");
    const std::filesystem::path dropped = writeCoordinatePng(folder, "dropped.png", 64, 64);
    const std::filesystem::path kept = writeCoordinatePng(folder, "kept.png", 4, 4);
    ThumbnailCache cache;
    cache.request(dropped);
    cache.invalidate(dropped);
    cache.request(kept);
    // The worker takes requests in order, so dropped was decoded (or skipped)
    // before kept is ready.
    const std::optional<CachedThumbnail> keptThumbnail = waitForDecode(cache, kept);
    REQUIRE(keptThumbnail.has_value());
    CHECK(keptThumbnail->state == ThumbnailState::Ready);
    CHECK_MESSAGE(!cache.status(dropped).has_value(), "a finished decode must not bring back an invalidated entry");
}

TEST_CASE("ThumbnailCache: destruction stops the worker with requests pending") {
    const testsupport::TempDirectory folder("thumbnail-shutdown");
    std::vector<std::filesystem::path> paths;
    for (int index = 0; index < 4; ++index) {
        paths.push_back(writeCoordinatePng(folder, ("large" + std::to_string(index) + ".png").c_str(), 512, 512));
    }
    // The cache lives on its own thread so that a destructor that never
    // returns fails this check instead of hanging the test until CTest's timeout.
    auto destroyed = std::make_shared<std::promise<void>>();
    std::future<void> done = destroyed->get_future();
    std::thread([destroyed, paths] {
        {
            ThumbnailCache cache;
            for (int round = 0; round < 16; ++round) {
                for (const std::filesystem::path& path : paths) {
                    cache.request(path);
                    cache.invalidate(path);
                }
            }
            for (const std::filesystem::path& path : paths) {
                cache.request(path);
            }
        }
        {
            // An idle worker stops too.
            const ThumbnailCache idle;
        }
        destroyed->set_value();
    }).detach();
    CHECK_MESSAGE(done.wait_for(workerDeadline) == std::future_status::ready,
                  "destroying a busy or an idle cache should stop its worker");
}

TEST_SUITE_END();

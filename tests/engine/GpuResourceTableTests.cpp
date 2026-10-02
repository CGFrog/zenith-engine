#include "engine/graphics/resources/BindlessTextureTable.hpp"
#include "engine/graphics/resources/GeometryArena.hpp"
#include "engine/graphics/resources/MipChainBlit.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// The table only stores handles, so distinct fake values are enough on the CPU.
VkImageView fakeView(uint64_t value) {
    return reinterpret_cast<VkImageView>(static_cast<uintptr_t>(value));
}

void testBindlessTable() {
    const VkImageView fallback = fakeView(1);
    BindlessTextureTable table(4, 2, fallback);
    expect(table.takePendingWrites(0).size() == 4, "a new frame set must write every slot once");
    expect(table.takePendingWrites(0).empty(), "written slots stay clean for that frame");

    const auto first = table.acquire(fakeView(10));
    const auto second = table.acquire(fakeView(11));
    expect(first && second && *first == 0 && *second == 1, "slots are handed out from the low end");
    expect(table.view(*second) == fakeView(11), "a slot reports the view it was given");
    expect(table.takePendingWrites(0) == std::vector<uint32_t>({0, 1}), "frame 0 rewrites newly assigned slots");
    expect(table.takePendingWrites(1).size() == 4, "frame 1 still owes its initial writes");
    expect(table.takePendingWrites(1).empty(), "each frame tracks its own pending writes");

    table.release(*first);
    expect(table.view(*first) == fallback, "a released slot falls back to the default view");
    expect(table.takePendingWrites(0) == std::vector<uint32_t>({0}) &&
               table.takePendingWrites(1) == std::vector<uint32_t>({0}),
           "releasing a slot makes every frame set rewrite it");
    expect(table.acquire(fakeView(12)) == first, "released slots are reused");

    expect(table.acquire(fakeView(13)).has_value() && table.acquire(fakeView(14)).has_value(),
           "the table fills to capacity");
    expect(!table.acquire(fakeView(15)).has_value(), "a full table reports exhaustion instead of overwriting");
    expect(table.liveCount() == 4, "live slots are counted");
    expect(!table.acquire(VK_NULL_HANDLE).has_value(), "null views are rejected");
}

void testGeometryArena() {
    GeometryArena arena({.vertices = 100, .indices = 300});
    const auto first = arena.allocate(60, 120, GeometryLayout::Static);
    const auto second = arena.allocate(40, 180, GeometryLayout::Static);
    expect(first && second && first->page == 0 && second->page == 0, "meshes share a page while it has room");
    const bool disjoint =
        first->vertexOffset + 60 <= second->vertexOffset || second->vertexOffset + 40 <= first->vertexOffset;
    expect(disjoint, "vertex ranges in one page never overlap");

    const auto third = arena.allocate(10, 10, GeometryLayout::Static);
    expect(third && third->page == 1 && arena.pageCount() == 2, "a full page opens another page");

    const auto oversized = arena.allocate(500, 900, GeometryLayout::Static);
    expect(oversized && arena.pageCapacity(oversized->page).vertices >= 500 &&
               arena.pageCapacity(oversized->page).indices >= 900,
           "oversized meshes receive a page large enough to hold them");

    arena.free(*first);
    const auto reused = arena.allocate(50, 100, GeometryLayout::Static);
    expect(reused && reused->page == 0, "freed ranges are reused before new pages open");
    expect(!arena.allocate(0, 3, GeometryLayout::Static).has_value(), "empty meshes are rejected");
}

void testGeometryArenaLayouts() {
    GeometryArena arena({.vertices = 100, .indices = 300});
    const auto staticMesh = arena.allocate(10, 30, GeometryLayout::Static);
    const auto skinnedMesh = arena.allocate(10, 30, GeometryLayout::Skinned);
    expect(staticMesh && skinnedMesh && staticMesh->page != skinnedMesh->page,
           "skinned meshes never share a page with static meshes");
    expect(arena.pageLayout(staticMesh->page) == GeometryLayout::Static &&
               arena.pageLayout(skinnedMesh->page) == GeometryLayout::Skinned,
           "pages report the vertex layout they were opened for");
    const auto secondSkinned = arena.allocate(10, 30, GeometryLayout::Skinned);
    expect(secondSkinned && secondSkinned->page == skinnedMesh->page, "meshes of one layout share its page");
    expect(arena.pageCount() == 2, "no page opens while a page of the right layout has room");
}

void testMipChainBlitPlan() {
    expect(fullMipChainLength(256, 64) == 9 && fullMipChainLength(1, 1) == 1 && fullMipChainLength(5, 3) == 3 &&
               fullMipChainLength(0, 0) == 0,
           "a full chain halves the largest dimension down to one texel");

    const std::vector<MipBlitStep> steps = planMipChainBlits(256, 64, 9);
    expect(steps.size() == 8, "every level after the base is filled by one blit");
    bool chained = true;
    for (uint32_t index = 0; index < steps.size(); ++index) {
        chained = chained && steps[index].sourceMip == index;
        if (index > 0) {
            chained = chained && steps[index].sourceWidth == steps[index - 1].destinationWidth &&
                      steps[index].sourceHeight == steps[index - 1].destinationHeight;
        }
    }
    expect(chained, "each blit reads the level the previous blit wrote");
    expect(steps.front().sourceWidth == 256 && steps.front().sourceHeight == 64 &&
               steps.front().destinationWidth == 128 && steps.front().destinationHeight == 32,
           "the first blit halves the base level");
    expect(steps.back().destinationWidth == 1 && steps.back().destinationHeight == 1, "a full chain ends at one texel");
    expect(steps[6].sourceHeight == 1 && steps[6].destinationHeight == 1,
           "a dimension that reached one texel stays at one");

    const std::vector<MipBlitStep> odd = planMipChainBlits(5, 3, 3);
    expect(odd.size() == 2 && odd[0].destinationWidth == 2 && odd[0].destinationHeight == 1 &&
               odd[1].destinationWidth == 1,
           "odd extents round down");
    expect(planMipChainBlits(64, 64, 3).size() == 2, "a partial chain stops at the requested level count");
    expect(planMipChainBlits(64, 64, 1).empty() && planMipChainBlits(64, 64, 0).empty(),
           "a single level needs no blits");
    expect(planMipChainBlits(4, 4, 10).size() == 2, "requests past the full chain are clamped");
}
} // namespace

int main() {
    testMipChainBlitPlan();
    testBindlessTable();
    testGeometryArena();
    testGeometryArenaLayouts();
    return failures == 0 ? 0 : 1;
}

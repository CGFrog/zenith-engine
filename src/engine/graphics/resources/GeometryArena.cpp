#include "GeometryArena.hpp"

#include <algorithm>

GeometryArena::GeometryArena(GeometryPageCapacity defaultPage)
    : m_defaultPage(defaultPage) {}

GeometryArena::~GeometryArena() {
    for (Page& page : m_pages) {
        // Destroying a block with live allocations is invalid in VMA.
        vmaClearVirtualBlock(page.vertices);
        vmaClearVirtualBlock(page.indices);
        vmaDestroyVirtualBlock(page.vertices);
        vmaDestroyVirtualBlock(page.indices);
    }
}

std::optional<GeometryAllocation> GeometryArena::allocate(uint32_t vertexCount, uint32_t indexCount,
                                                          GeometryLayout layout) {
    if (vertexCount == 0 || indexCount == 0) {
        return std::nullopt;
    }
    for (uint32_t page = 0; page < m_pages.size(); ++page) {
        if (m_pages[page].layout != layout) {
            continue;
        }
        if (auto placed = allocateIn(page, vertexCount, indexCount)) {
            return placed;
        }
    }
    const GeometryPageCapacity capacity{std::max(m_defaultPage.vertices, vertexCount),
                                        std::max(m_defaultPage.indices, indexCount)};
    if (!addPage(capacity, layout)) {
        return std::nullopt;
    }
    return allocateIn(static_cast<uint32_t>(m_pages.size() - 1), vertexCount, indexCount);
}

void GeometryArena::free(const GeometryAllocation& allocation) {
    if (allocation.page >= m_pages.size()) {
        return;
    }
    Page& page = m_pages[allocation.page];
    if (allocation.vertexRange) {
        vmaVirtualFree(page.vertices, allocation.vertexRange);
    }
    if (allocation.indexRange) {
        vmaVirtualFree(page.indices, allocation.indexRange);
    }
}

GeometryPageCapacity GeometryArena::pageCapacity(uint32_t page) const {
    return page < m_pages.size() ? m_pages[page].capacity : GeometryPageCapacity{};
}

GeometryLayout GeometryArena::pageLayout(uint32_t page) const {
    return page < m_pages.size() ? m_pages[page].layout : GeometryLayout::Static;
}

std::optional<GeometryAllocation> GeometryArena::allocateIn(uint32_t pageIndex, uint32_t vertexCount,
                                                            uint32_t indexCount) {
    Page& page = m_pages[pageIndex];
    // Virtual blocks are sized in elements, so offsets are element indices.
    VmaVirtualAllocationCreateInfo vertexRequest{.size = vertexCount};
    VmaVirtualAllocationCreateInfo indexRequest{.size = indexCount};
    GeometryAllocation result{.page = pageIndex};
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize indexOffset = 0;
    if (vmaVirtualAllocate(page.vertices, &vertexRequest, &result.vertexRange, &vertexOffset) != VK_SUCCESS) {
        return std::nullopt;
    }
    if (vmaVirtualAllocate(page.indices, &indexRequest, &result.indexRange, &indexOffset) != VK_SUCCESS) {
        vmaVirtualFree(page.vertices, result.vertexRange);
        return std::nullopt;
    }
    result.vertexOffset = static_cast<int32_t>(vertexOffset);
    result.firstIndex = static_cast<uint32_t>(indexOffset);
    return result;
}

bool GeometryArena::addPage(GeometryPageCapacity capacity, GeometryLayout layout) {
    Page page{.capacity = capacity, .layout = layout};
    VmaVirtualBlockCreateInfo vertexInfo{.size = capacity.vertices};
    VmaVirtualBlockCreateInfo indexInfo{.size = capacity.indices};
    if (vmaCreateVirtualBlock(&vertexInfo, &page.vertices) != VK_SUCCESS) {
        return false;
    }
    if (vmaCreateVirtualBlock(&indexInfo, &page.indices) != VK_SUCCESS) {
        vmaDestroyVirtualBlock(page.vertices);
        return false;
    }
    m_pages.push_back(page);
    return true;
}

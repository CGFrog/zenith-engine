#pragma once
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <vector>

// Location of one mesh inside the shared geometry buffers. Offsets are in
// elements (vertices, 32-bit indices), matching vkCmdDrawIndexed parameters.
struct GeometryAllocation {
    uint32_t page{0};
    int32_t vertexOffset{0};
    uint32_t firstIndex{0};
    VmaVirtualAllocation vertexRange{VK_NULL_HANDLE};
    VmaVirtualAllocation indexRange{VK_NULL_HANDLE};
};

// Vertex streams a page holds. Every stream in a page is indexed by the same
// vertex offset, so skinned meshes, which need the extra skin stream, live in
// their own pages and static pages never pay for skin storage.
enum class GeometryLayout : uint8_t {
    Static,
    Skinned,
};

struct GeometryPageCapacity {
    uint32_t vertices{0};
    uint32_t indices{0};
};

// Suballocates meshes into a small number of large vertex/index pages so draws
// share buffer bindings and indirect commands can address any mesh by offset.
// This class owns only the bookkeeping; the renderer creates the GPU stream and
// index buffers per page with the capacity and layout reported for it.
class GeometryArena {
  public:
    explicit GeometryArena(GeometryPageCapacity defaultPage);
    ~GeometryArena();
    GeometryArena(const GeometryArena&) = delete;
    GeometryArena& operator=(const GeometryArena&) = delete;

    // Places the mesh in the first page of the given layout with room, opening a
    // new page (sized to fit oversized meshes) when none has space.
    std::optional<GeometryAllocation> allocate(uint32_t vertexCount, uint32_t indexCount, GeometryLayout layout);
    void free(const GeometryAllocation& allocation);

    uint32_t pageCount() const {
        return static_cast<uint32_t>(m_pages.size());
    }

    GeometryPageCapacity pageCapacity(uint32_t page) const;
    GeometryLayout pageLayout(uint32_t page) const;

  private:
    struct Page {
        GeometryPageCapacity capacity;
        GeometryLayout layout{GeometryLayout::Static};
        VmaVirtualBlock vertices{VK_NULL_HANDLE};
        VmaVirtualBlock indices{VK_NULL_HANDLE};
    };

    std::optional<GeometryAllocation> allocateIn(uint32_t page, uint32_t vertexCount, uint32_t indexCount);
    bool addPage(GeometryPageCapacity capacity, GeometryLayout layout);

    GeometryPageCapacity m_defaultPage;
    std::vector<Page> m_pages;
};

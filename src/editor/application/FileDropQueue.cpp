#include "FileDropQueue.hpp"

#include <iterator>
#include <utility>

void FileDropQueue::add(std::span<const DroppedFile> drops) {
    m_drops.insert(m_drops.end(), drops.begin(), drops.end());
}

bool FileDropQueue::empty() const {
    return m_drops.empty();
}

std::span<const DroppedFile> FileDropQueue::pending() const {
    return m_drops;
}

std::vector<std::filesystem::path> FileDropQueue::claimWithin(glm::vec2 minimum, glm::vec2 maximum) {
    std::vector<std::filesystem::path> claimed;
    std::erase_if(m_drops, [&](const DroppedFile& drop) {
        const bool inside = drop.position.x >= minimum.x && drop.position.y >= minimum.y &&
                            drop.position.x < maximum.x && drop.position.y < maximum.y;
        if (inside) {
            claimed.push_back(drop.path);
        }
        return inside;
    });
    return claimed;
}

std::vector<DroppedFile> FileDropQueue::takeAll() {
    return std::exchange(m_drops, {});
}

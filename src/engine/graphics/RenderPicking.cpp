#include "RenderPicking.hpp"

#include <algorithm>
#include <utility>

std::optional<PickRect> clampPickRect(const PickRect& rect, uint32_t viewWidth, uint32_t viewHeight) {
    const int64_t left = std::max<int64_t>(rect.x, 0);
    const int64_t top = std::max<int64_t>(rect.y, 0);
    const int64_t right = std::min<int64_t>(static_cast<int64_t>(rect.x) + rect.width, viewWidth);
    const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(rect.y) + rect.height, viewHeight);
    if (right <= left || bottom <= top) {
        return std::nullopt;
    }
    return PickRect{.x = static_cast<int32_t>(left),
                    .y = static_cast<int32_t>(top),
                    .width = static_cast<uint32_t>(right - left),
                    .height = static_cast<uint32_t>(bottom - top)};
}

glm::vec3 reconstructWorldPosition(glm::vec2 viewPixel, float depth, glm::uvec2 viewSize,
                                   const glm::mat4& inverseViewProjection) {
    // Vulkan clip space: x and y run -1..1 from the top-left corner, matching
    // the flipped projection the renderer draws with.
    const glm::vec2 ndc = viewPixel / glm::vec2(viewSize) * 2.0f - 1.0f;
    const glm::vec4 world = inverseViewProjection * glm::vec4(ndc, depth, 1.0f);
    return glm::vec3(world) / world.w;
}

std::optional<glm::vec3> pickWorldPosition(const PickResult& result, uint32_t viewX, uint32_t viewY) {
    const PickRect& region = result.region;
    const int64_t localX = static_cast<int64_t>(viewX) - region.x;
    const int64_t localY = static_cast<int64_t>(viewY) - region.y;
    if (localX < 0 || localY < 0 || localX >= region.width || localY >= region.height) {
        return std::nullopt;
    }
    const size_t index = static_cast<size_t>(localY) * region.width + static_cast<size_t>(localX);
    if (index >= result.ids.size() || index >= result.depth.size() || result.ids[index] == 0) {
        return std::nullopt;
    }
    return reconstructWorldPosition(glm::vec2(viewX, viewY) + 0.5f, result.depth[index],
                                    {result.viewWidth, result.viewHeight}, result.inverseViewProjection);
}

PickTicket PickRequestBook::request(RenderViewId view, const PickRect& rect) {
    if (!view || rect.width == 0 || rect.height == 0) {
        return {};
    }
    if (m_entries.size() >= maximumOutstanding) {
        const auto finished =
            std::ranges::find_if(m_entries, [](const Entry& entry) { return entry.state == State::Finished; });
        if (finished == m_entries.end()) {
            return {};
        }
        m_entries.erase(finished);
    }
    const PickTicket ticket{m_nextTicket++};
    m_entries.push_back({.ticket = ticket, .view = view, .rect = rect});
    return ticket;
}

PickStatus PickRequestBook::status(PickTicket ticket) const {
    const Entry* entry = find(ticket);
    if (entry == nullptr) {
        return PickStatus::Unknown;
    }
    return entry->state == State::Finished ? entry->result.status : PickStatus::Pending;
}

PickResult PickRequestBook::take(PickTicket ticket) {
    Entry* entry = find(ticket);
    if (entry == nullptr) {
        return {};
    }
    if (entry->state != State::Finished) {
        return {.status = PickStatus::Pending, .view = entry->view};
    }
    PickResult result = std::move(entry->result);
    std::erase_if(m_entries, [ticket](const Entry& candidate) { return candidate.ticket == ticket; });
    return result;
}

bool PickRequestBook::cancel(PickTicket ticket) {
    Entry* entry = find(ticket);
    if (entry == nullptr || entry->state == State::Finished) {
        return false;
    }
    finish(*entry, PickStatus::Cancelled, {});
    return true;
}

void PickRequestBook::cancelView(RenderViewId view) {
    for (Entry& entry : m_entries) {
        if (entry.view == view && entry.state != State::Finished) {
            finish(entry, PickStatus::Cancelled, {});
        }
    }
}

std::vector<PickRequestBook::Recording> PickRequestBook::startRecording(RenderViewId view) {
    std::vector<Recording> recordings;
    for (Entry& entry : m_entries) {
        if (entry.view == view && entry.state == State::Queued) {
            entry.state = State::Recording;
            recordings.push_back({.ticket = entry.ticket, .rect = entry.rect});
        }
    }
    return recordings;
}

bool PickRequestBook::awaitingResult(PickTicket ticket) const {
    const Entry* entry = find(ticket);
    return entry != nullptr && entry->state == State::Recording;
}

void PickRequestBook::complete(PickTicket ticket, PickResult result) {
    Entry* entry = find(ticket);
    if (entry == nullptr || entry->state != State::Recording) {
        return;
    }
    entry->result = std::move(result);
    entry->result.view = entry->view;
    entry->result.status = PickStatus::Ready;
    entry->result.error.clear();
    entry->state = State::Finished;
}

void PickRequestBook::fail(PickTicket ticket, std::string error) {
    Entry* entry = find(ticket);
    if (entry == nullptr || entry->state == State::Finished) {
        return;
    }
    finish(*entry, PickStatus::Failed, std::move(error));
}

PickRequestBook::Entry* PickRequestBook::find(PickTicket ticket) {
    const auto found = std::ranges::find_if(m_entries, [ticket](const Entry& entry) { return entry.ticket == ticket; });
    return found == m_entries.end() ? nullptr : &*found;
}

const PickRequestBook::Entry* PickRequestBook::find(PickTicket ticket) const {
    const auto found = std::ranges::find_if(m_entries, [ticket](const Entry& entry) { return entry.ticket == ticket; });
    return found == m_entries.end() ? nullptr : &*found;
}

void PickRequestBook::finish(Entry& entry, PickStatus status, std::string error) {
    entry.result = {.status = status, .error = std::move(error), .view = entry.view};
    entry.state = State::Finished;
}

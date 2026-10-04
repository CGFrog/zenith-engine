#include "EventQueue.hpp"

#include <cassert>

void EventQueue::advanceFrame() {
    for (auto& [key, channel] : m_channels) {
        channel->advance();
    }
}

void EventQueue::clear() {
    for (auto& [key, channel] : m_channels) {
        channel->clear();
    }
}

bool EventQueue::sameType(const ChannelBase& channel, const char* typeName) {
    const bool same = channel.typeName == typeName;
    assert(same && "Two event types share one eventKey");
    return same;
}

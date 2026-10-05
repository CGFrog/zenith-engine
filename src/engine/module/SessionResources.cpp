#include "SessionResources.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>

SessionResources::~SessionResources() {
    clear();
}

void SessionResources::clear() {
    // A destructor may look up another resource, so each value leaves the
    // store before it is destroyed.
    while (!m_entries.empty()) {
        std::unique_ptr<EntryBase> newest = std::move(m_entries.back());
        m_entries.pop_back();
        newest.reset();
    }
}

size_t SessionResources::size() const {
    return m_entries.size();
}

SessionResources::EntryBase* SessionResources::findEntry(std::string_view key) {
    const auto found =
        std::ranges::find_if(m_entries, [key](const std::unique_ptr<EntryBase>& entry) { return entry->key == key; });
    return found == m_entries.end() ? nullptr : found->get();
}

bool SessionResources::erase(std::string_view key) {
    const auto found =
        std::ranges::find_if(m_entries, [key](const std::unique_ptr<EntryBase>& entry) { return entry->key == key; });
    if (found == m_entries.end()) {
        return false;
    }
    std::unique_ptr<EntryBase> removed = std::move(*found);
    m_entries.erase(found);
    removed.reset();
    return true;
}

bool SessionResources::sameType(const EntryBase& entry, const char* typeName) {
    const bool same = entry.typeName == typeName;
    assert(same && "Two session resource types share one resourceKey");
    return same;
}

void SessionResources::requireSameType(const EntryBase& entry, const char* typeName) {
    if (!sameType(entry, typeName)) {
        throw std::logic_error("Session resource key '" + entry.key + "' belongs to another type");
    }
}

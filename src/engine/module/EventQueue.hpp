#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

// A type a system can publish through an EventQueue: a copyable value with a
// key unique among event types, for example
//
//   struct DoorOpened {
//       static constexpr std::string_view eventKey = "my_game.door_opened";
//       SceneEntityId door;
//   };
//
// Engine events use the "zenith." prefix (see GameEvents.hpp).
template <typename Event>
concept GameEvent = std::is_copy_constructible_v<Event> && requires {
    { Event::eventKey } -> std::convertible_to<std::string_view>;
};

// Typed messages between systems, double-buffered per rendered frame: what is
// published during frame N is read by every phase of frame N + 1 (in
// publication order), then dropped. A FixedUpdate system in a frame that runs
// no fixed step therefore misses that frame's events.
//
// Channels are found by Event::eventKey rather than by a C++ type id, since
// the engine and each game module have their own copy of a template's
// statics. Reusing a key for another type is a programming error: in debug
// builds it asserts, otherwise the mismatched publish or read is ignored.
//
// Lifetime: a channel's code (its destructor) comes from the binary that first
// used its event type, so a queue that a game module used must be destroyed
// before the module unloads (see GameServices).
class EventQueue {
  public:
    template <GameEvent Event> void publish(Event event) {
        if (Channel<Event>* channel = channelFor<Event>()) {
            channel->next.push_back(std::move(event));
        }
    }

    // The events of type Event published during the previous frame.
    template <GameEvent Event> std::span<const Event> read() const {
        const Channel<Event>* channel = find<Event>();
        return channel == nullptr ? std::span<const Event>{} : std::span<const Event>(channel->current);
    }

    // Starts a frame: last frame's events become readable and the ones before
    // them are dropped.
    void advanceFrame();
    // Drops every event, readable or pending.
    void clear();

  private:
    struct ChannelBase {
        explicit ChannelBase(const char* typeName)
            : typeName(typeName) {}

        virtual ~ChannelBase() = default;
        virtual void advance() = 0;
        virtual void clear() = 0;

        // typeid names compare equal across binaries built by one compiler,
        // unlike the type_info objects themselves.
        std::string typeName;
    };

    template <typename Event> struct Channel final : ChannelBase {
        Channel()
            : ChannelBase(typeid(Event).name()) {}

        void advance() override {
            current.swap(next);
            next.clear();
        }

        void clear() override {
            current.clear();
            next.clear();
        }

        std::vector<Event> current;
        std::vector<Event> next;
    };

    struct KeyHash {
        using is_transparent = void;

        size_t operator()(std::string_view key) const {
            return std::hash<std::string_view>{}(key);
        }
    };

    template <typename Event> Channel<Event>* channelFor() {
        const std::string_view key = Event::eventKey;
        auto found = m_channels.find(key);
        if (found == m_channels.end()) {
            found = m_channels.emplace(std::string(key), std::make_unique<Channel<Event>>()).first;
        }
        return checked<Event>(found->second.get());
    }

    template <typename Event> const Channel<Event>* find() const {
        const auto found = m_channels.find(std::string_view(Event::eventKey));
        return found == m_channels.end() ? nullptr : checked<Event>(found->second.get());
    }

    template <typename Event> static Channel<Event>* checked(ChannelBase* channel) {
        if (!sameType(*channel, typeid(Event).name())) {
            return nullptr;
        }
        return static_cast<Channel<Event>*>(channel);
    }

    template <typename Event> static const Channel<Event>* checked(const ChannelBase* channel) {
        return checked<Event>(const_cast<ChannelBase*>(channel));
    }

    // Asserts in debug builds when two types share a key.
    static bool sameType(const ChannelBase& channel, const char* typeName);

    std::unordered_map<std::string, std::unique_ptr<ChannelBase>, KeyHash, std::equal_to<>> m_channels;
};

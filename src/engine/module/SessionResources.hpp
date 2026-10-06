#pragma once

// Per-session values, one per type keyed by resourceKey (GameServices::resource<T>()).

#include <concepts>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// A type a gameplay session keeps one of in its SessionResources: an object
// type with a key unique among resource types, for example
//
//   struct RunState {
//       static constexpr std::string_view resourceKey = "my_game.run_state";
//       uint32_t floor{1};
//   };
//
// Engine resources would use the "zenith." prefix.
template <typename Resource>
concept SessionResource = std::is_object_v<Resource> && !std::is_const_v<Resource> && requires {
    { Resource::resourceKey } -> std::convertible_to<std::string_view>;
};

// The gameplay session's own data, one value per type: definition tables, the
// current run's state, caches. A module's static variables are lost when the
// module reloads and a scene's registry context goes with the scene; these
// live exactly as long as the session's GameServices.
//
// Values are found by Resource::resourceKey rather than by a C++ type id,
// since the engine and each game module have their own copy of a template's
// statics. Reusing a key for another type is a programming error: in debug
// builds it asserts; otherwise find returns null and get and emplace throw
// std::logic_error.
//
// Lifetime: a value's destructor is code of the binary that created it, so
// the store must be cleared before that module unloads. GameServices owns
// one and destroys it (newest value first) before its other members, which
// both hosts do before unloading a module (GameplayRunner::stop,
// GameModuleHost::stopGameplay).
class SessionResources {
  public:
    SessionResources() = default;
    SessionResources(const SessionResources&) = delete;
    SessionResources& operator=(const SessionResources&) = delete;
    ~SessionResources();

    // The Resource, default-constructed the first time it is asked for.
    template <SessionResource Resource>
        requires std::default_initializable<Resource>
    Resource& get() {
        if (Resource* existing = find<Resource>()) {
            return *existing;
        }
        return emplace<Resource>();
    }

    // Constructs the Resource from arguments, replacing (destroying) any
    // previous value of it.
    template <SessionResource Resource, typename... Arguments> Resource& emplace(Arguments&&... arguments) {
        auto entry = std::make_unique<Entry<Resource>>(std::forward<Arguments>(arguments)...);
        Resource& value = entry->value;
        if (EntryBase* existing = findEntry(Resource::resourceKey)) {
            requireSameType(*existing, typeid(Resource).name());
        }
        erase(Resource::resourceKey);
        m_entries.push_back(std::move(entry));
        return value;
    }

    // Null when the session has none (or the key belongs to another type).
    template <SessionResource Resource> Resource* find() {
        EntryBase* entry = findEntry(Resource::resourceKey);
        if (entry == nullptr || !sameType(*entry, typeid(Resource).name())) {
            return nullptr;
        }
        return &static_cast<Entry<Resource>*>(entry)->value;
    }

    template <SessionResource Resource> const Resource* find() const {
        return const_cast<SessionResources*>(this)->find<Resource>();
    }

    // Destroys the Resource; false when there was none.
    template <SessionResource Resource> bool erase() {
        return find<Resource>() != nullptr && erase(Resource::resourceKey);
    }

    // Destroys every value, newest first.
    void clear();
    size_t size() const;

  private:
    struct EntryBase {
        EntryBase(std::string_view key, const char* typeName)
            : key(key)
            , typeName(typeName) {}

        virtual ~EntryBase() = default;

        std::string key;
        // typeid names compare equal across binaries built by one compiler,
        // unlike the type_info objects themselves.
        std::string typeName;
    };

    template <typename Resource> struct Entry final : EntryBase {
        template <typename... Arguments>
        explicit Entry(Arguments&&... arguments)
            : EntryBase(Resource::resourceKey, typeid(Resource).name())
            , value(std::forward<Arguments>(arguments)...) {}

        Resource value;
    };

    EntryBase* findEntry(std::string_view key);
    bool erase(std::string_view key);
    // Asserts in debug builds when two types share a key.
    static bool sameType(const EntryBase& entry, const char* typeName);
    // Throws std::logic_error naming the key when the types differ.
    static void requireSameType(const EntryBase& entry, const char* typeName);

    // In creation order, so clear destroys the newest first.
    std::vector<std::unique_ptr<EntryBase>> m_entries;
};

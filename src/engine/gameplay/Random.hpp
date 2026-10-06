#pragma once

// Deterministic random numbers (PCG32) identical on every platform, and named streams derived from one seed.

#include "engine/reflection/Reflection.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small, fast random number generator whose results are the same on every
// platform, compiler, and build type: PCG32 (XSH RR, 64-bit state, selectable
// sequence; O'Neill, pcg-random.org). Every helper is defined here rather
// than through <random>'s distributions, whose results differ between
// standard library implementations, so a seed always replays the same game.
// The integer helpers are exact everywhere; the float helpers also need IEEE
// single precision without fused multiply-add contraction (MSVC's default
// /fp:precise; -ffp-contract=off on GCC and Clang).
//
// Not for security: the output is predictable from a few values.
class RandomStream {
  public:
    // The generator's whole state; restoring it continues the sequence
    // exactly where it was read.
    struct State {
        uint64_t state{0};
        // Selects the sequence; always odd.
        uint64_t increment{1};

        bool operator==(const State&) const = default;
    };

    // Seeded as PCG32's reference pcg32_srandom(initialState, sequence):
    // streams with different sequences are distinct.
    explicit RandomStream(uint64_t initialState = 0, uint64_t sequence = 0);

    State state() const;
    // Continues from a state read with state(); an even increment is made
    // odd.
    void setState(State state);

    // The next 32 random bits.
    uint32_t nextUInt32();
    // Uniform in [0, bound), without modulo bias; 0 when bound is 0.
    uint32_t below(uint32_t bound);
    // Uniform in [minimum, maximum], both included; the bounds may come in
    // either order.
    int32_t range(int32_t minimum, int32_t maximum);
    // Uniform in [0, 1), in steps of 2^-24.
    float unit();
    // Uniform in [minimum, maximum).
    float range(float minimum, float maximum);
    // True with the given probability; clamped to [0, 1].
    bool chance(float probability);
    // An index drawn in proportion to weights. Negative, NaN, and infinite
    // weights count as zero; nullopt when no weight is positive.
    std::optional<size_t> weightedIndex(std::span<const float> weights);

    // Shuffles items uniformly (Fisher-Yates).
    template <typename T> void shuffle(std::span<T> items) {
        for (size_t index = items.size(); index > 1; --index) {
            const auto other = static_cast<size_t>(below(static_cast<uint32_t>(index)));
            std::swap(items[index - 1], items[other]);
        }
    }

    template <typename T> void shuffle(std::vector<T>& items) {
        shuffle(std::span<T>(items));
    }

  private:
    State m_state;
};

// One named stream's place in its sequence, for saving.
struct RandomStreamSnapshot {
    std::string name;
    uint64_t state{0};
    uint64_t increment{1};
};

// Every stream a session used and the seed they derive from: what a save
// keeps to resume a run's randomness exactly (RandomStreams::snapshot).
struct RandomStreamsSnapshot {
    uint64_t seed{0};
    // Sorted by name.
    std::vector<RandomStreamSnapshot> streams;
};

// The snapshot's reflected fields, so a game's saved run can hold it:
// Fields<Run>().structure("random", &Run::random, randomStreamsSnapshotFields()).
Fields<RandomStreamsSnapshot> randomStreamsSnapshotFields();

// Named random streams for one gameplay session, all derived from one seed.
// Each name ("layout", "loot", "combat", ...) gets its own stream, so drawing
// from one never changes another: a floor generates the same layout however
// many loot rolls came before it. A stream starts the first time its name is
// asked for, from the seed and the name alone.
class RandomStreams {
  public:
    explicit RandomStreams(uint64_t seed = 0);

    uint64_t seed() const;
    // Starts over from seed: every stream restarts.
    void reseed(uint64_t seed);
    // The stream for name; valid until reseed or restore.
    RandomStream& stream(std::string_view name);

    // The seed and the streams used so far, sorted by name, for saving.
    RandomStreamsSnapshot snapshot() const;
    // Reseeds from the snapshot, then continues each stream it lists from its
    // saved state; streams it does not list start fresh from the seed.
    void restore(const RandomStreamsSnapshot& snapshot);

  private:
    uint64_t m_seed{0};
    std::map<std::string, RandomStream, std::less<>> m_streams;
};

// A seed from the operating system's entropy source, for a new run whose
// seed the player did not choose. Record it to replay the run.
uint64_t entropySeed();

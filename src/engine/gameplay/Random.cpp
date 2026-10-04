#include "Random.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace {
constexpr uint64_t pcgMultiplier = 6364136223846793005ULL;

// SplitMix64's finalizer (Steele, Lea, Flood): spreads nearby inputs (seeds
// 1, 2, 3; similar names) over the whole range.
uint64_t mix(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

// 64-bit FNV-1a of a stream's name.
uint64_t nameHash(std::string_view name) {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (const char character : name) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

RandomStream streamFor(uint64_t seed, std::string_view name) {
    const uint64_t hash = nameHash(name);
    return RandomStream(mix(seed ^ mix(hash)), hash);
}
} // namespace

RandomStream::RandomStream(uint64_t initialState, uint64_t sequence) {
    m_state.state = 0;
    m_state.increment = (sequence << 1U) | 1U;
    nextUInt32();
    m_state.state += initialState;
    nextUInt32();
}

RandomStream::State RandomStream::state() const {
    return m_state;
}

void RandomStream::setState(State state) {
    state.increment |= 1U;
    m_state = state;
}

uint32_t RandomStream::nextUInt32() {
    const uint64_t previous = m_state.state;
    m_state.state = previous * pcgMultiplier + m_state.increment;
    const auto shifted = static_cast<uint32_t>(((previous >> 18U) ^ previous) >> 27U);
    const auto rotation = static_cast<uint32_t>(previous >> 59U);
    return (shifted >> rotation) | (shifted << ((0U - rotation) & 31U));
}

uint32_t RandomStream::below(uint32_t bound) {
    if (bound == 0) {
        return 0;
    }
    // Values below threshold would make the low results more likely.
    const uint32_t threshold = (0U - bound) % bound;
    while (true) {
        const uint32_t value = nextUInt32();
        if (value >= threshold) {
            return value % bound;
        }
    }
}

int32_t RandomStream::range(int32_t minimum, int32_t maximum) {
    if (minimum > maximum) {
        std::swap(minimum, maximum);
    }
    const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(maximum) - minimum) + 1U;
    const uint32_t offset = span > UINT32_MAX ? nextUInt32() : below(static_cast<uint32_t>(span));
    return static_cast<int32_t>(static_cast<int64_t>(minimum) + offset);
}

float RandomStream::unit() {
    return static_cast<float>(nextUInt32() >> 8U) * 0x1.0p-24f;
}

float RandomStream::range(float minimum, float maximum) {
    return minimum + (maximum - minimum) * unit();
}

bool RandomStream::chance(float probability) {
    // Draws even for 0 and 1, so the stream advances the same way whatever
    // the probability.
    const float value = unit();
    return value < std::clamp(probability, 0.0f, 1.0f);
}

std::optional<size_t> RandomStream::weightedIndex(std::span<const float> weights) {
    const auto usable = [](float weight) { return std::isfinite(weight) && weight > 0.0f ? weight : 0.0f; };
    float total = 0.0f;
    for (const float weight : weights) {
        total += usable(weight);
    }
    if (!(total > 0.0f) || !std::isfinite(total)) {
        return std::nullopt;
    }
    const float target = unit() * total;
    float sum = 0.0f;
    std::optional<size_t> last;
    for (size_t index = 0; index < weights.size(); ++index) {
        const float weight = usable(weights[index]);
        if (weight <= 0.0f) {
            continue;
        }
        last = index;
        sum += weight;
        if (target < sum) {
            return index;
        }
    }
    // Rounding can leave target at the total; it belongs to the last bucket.
    return last;
}

RandomStreams::RandomStreams(uint64_t seed)
    : m_seed(seed) {}

uint64_t RandomStreams::seed() const {
    return m_seed;
}

void RandomStreams::reseed(uint64_t seed) {
    m_seed = seed;
    m_streams.clear();
}

RandomStream& RandomStreams::stream(std::string_view name) {
    if (auto found = m_streams.find(name); found != m_streams.end()) {
        return found->second;
    }
    return m_streams.emplace(std::string(name), streamFor(m_seed, name)).first->second;
}

std::vector<std::pair<std::string, RandomStream::State>> RandomStreams::states() const {
    std::vector<std::pair<std::string, RandomStream::State>> states;
    states.reserve(m_streams.size());
    for (const auto& [name, stream] : m_streams) {
        states.emplace_back(name, stream.state());
    }
    return states;
}

void RandomStreams::restore(uint64_t seed, std::span<const std::pair<std::string, RandomStream::State>> states) {
    reseed(seed);
    for (const auto& [name, state] : states) {
        stream(name).setState(state);
    }
}

uint64_t entropySeed() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32U) | device();
}

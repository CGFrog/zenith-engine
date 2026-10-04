// CPU-only coverage of the gameplay random streams: PCG32's reference
// sequence, the helpers' ranges and determinism, named streams that do not
// disturb each other, saving and restoring their state, and GameServices.

#include "engine/assets/Assets.hpp"
#include "engine/gameplay/Random.hpp"
#include "engine/graphics/DebugDraw.hpp"
#include "engine/module/GameServices.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::vector<uint32_t> draw(RandomStream& stream, size_t count) {
    std::vector<uint32_t> values(count);
    for (uint32_t& value : values) {
        value = stream.nextUInt32();
    }
    return values;
}

void testReferenceSequence() {
    // pcg32-demo's pcg32_srandom(42u, 54u).
    RandomStream stream(42U, 54U);
    expect(draw(stream, 6) ==
               std::vector<uint32_t>{0xa15c02b7U, 0x7b47f409U, 0xba1d3330U, 0x83d2f293U, 0xbfa4784bU, 0xcbed606eU},
           "the generator reproduces PCG32's reference sequence");
}

void testHelpers() {
    RandomStream stream(7U, 3U);
    bool belowInRange = true;
    bool rangeInRange = true;
    bool unitInRange = true;
    std::array<int, 6> faces{};
    for (int index = 0; index < 6000; ++index) {
        belowInRange = belowInRange && stream.below(10U) < 10U;
        const int32_t die = stream.range(6, 1);
        rangeInRange = rangeInRange && die >= 1 && die <= 6;
        if (die >= 1 && die <= 6) {
            ++faces[static_cast<size_t>(die - 1)];
        }
        const float value = stream.unit();
        unitInRange = unitInRange && value >= 0.0f && value < 1.0f;
    }
    expect(belowInRange, "below stays under its bound");
    expect(rangeInRange, "range includes both bounds, given in either order");
    expect(std::ranges::all_of(faces, [](int count) { return count > 850 && count < 1150; }),
           "range is roughly uniform");
    expect(unitInRange, "unit stays in [0, 1)");
    expect(stream.below(0U) == 0U, "below 0 is 0");
    const int32_t wide = stream.range(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
    expect(wide >= std::numeric_limits<int32_t>::min(), "the full int range is allowed");

    const float scaled = stream.range(-2.0f, 2.0f);
    expect(scaled >= -2.0f && scaled < 2.0f, "a float range stays within its bounds");
    expect(!stream.chance(0.0f) && stream.chance(1.0f) && !stream.chance(-1.0f) && stream.chance(2.0f),
           "chance clamps its probability");

    const std::array<float, 4> weights{0.0f, 3.0f, -1.0f, 1.0f};
    std::array<int, 4> picks{};
    for (int index = 0; index < 4000; ++index) {
        if (const std::optional<size_t> picked = stream.weightedIndex(weights)) {
            ++picks[*picked];
        }
    }
    expect(picks[0] == 0 && picks[2] == 0, "zero and negative weights are never picked");
    expect(picks[1] > 2700 && picks[1] < 3300 && picks[1] + picks[3] == 4000, "picks follow the weights");
    const std::array<float, 2> none{0.0f, std::numeric_limits<float>::quiet_NaN()};
    expect(!stream.weightedIndex(none) && !stream.weightedIndex({}), "no positive weight picks nothing");

    std::vector<int> items(20);
    std::iota(items.begin(), items.end(), 0);
    std::vector<int> shuffled = items;
    stream.shuffle(shuffled);
    std::vector<int> sorted = shuffled;
    std::ranges::sort(sorted);
    expect(sorted == items && shuffled != items, "shuffle permutes the items");
}

void testDeterminism() {
    RandomStream first(99U, 1U);
    RandomStream second(99U, 1U);
    expect(draw(first, 100) == draw(second, 100), "the same seed gives the same sequence");

    RandomStream saved(5U, 2U);
    draw(saved, 10);
    const RandomStream::State state = saved.state();
    const std::vector<uint32_t> expected = draw(saved, 10);
    RandomStream restored;
    restored.setState(state);
    expect(draw(restored, 10) == expected, "a restored state continues the sequence");
}

void testNamedStreams() {
    RandomStreams streams(1U);
    // Golden values: a change here changes every saved seed's game.
    expect(draw(streams.stream("layout"), 3) == std::vector<uint32_t>{0xf732644bU, 0xeeb61c11U, 0x166875feU},
           "a named stream derives from the seed and its name as before");

    RandomStreams quiet(1U);
    RandomStreams busy(1U);
    draw(busy.stream("loot"), 50);
    expect(draw(quiet.stream("layout"), 20) == draw(busy.stream("layout"), 20),
           "drawing from one stream never changes another");
    expect(draw(RandomStreams(1U).stream("loot"), 3) == std::vector<uint32_t>{0x14324f99U, 0x95aafe4dU, 0x5ea9c9a3U},
           "differently named streams differ");
    expect(draw(RandomStreams(2U).stream("layout"), 3) != draw(RandomStreams(1U).stream("layout"), 3),
           "another seed gives another stream");

    RandomStreams session(1234U);
    draw(session.stream("loot"), 7);
    draw(session.stream("combat"), 3);
    const auto states = session.states();
    expect(states.size() == 2 && states[0].first == "combat" && states[1].first == "loot",
           "states lists the used streams by name");
    const std::vector<uint32_t> nextLoot = draw(session.stream("loot"), 5);
    const std::vector<uint32_t> firstLayout = draw(session.stream("layout"), 5);

    RandomStreams resumed;
    resumed.restore(1234U, states);
    expect(resumed.seed() == 1234U && draw(resumed.stream("loot"), 5) == nextLoot,
           "a restored stream continues where it was saved");
    expect(draw(resumed.stream("layout"), 5) == firstLayout, "a stream not saved starts fresh from the seed");

    session.reseed(1234U);
    expect(session.states().empty(), "reseeding restarts every stream");
}

void testGameServices() {
    Logger logger;
    AssetManager assets{&logger};
    DebugDraw debugDraw;
    GameServices services(assets, {}, debugDraw, &logger);
    services.randomStreams().reseed(1U);
    expect(draw(services.random("layout"), 3) == std::vector<uint32_t>{0xf732644bU, 0xeeb61c11U, 0x166875feU},
           "the session's streams come from its seed");
}
} // namespace

int main() {
    testReferenceSequence();
    testHelpers();
    testDeterminism();
    testNamedStreams();
    testGameServices();
    if (failures == 0) {
        std::cout << "Random tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

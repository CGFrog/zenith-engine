#include "engine/diagnostics/OverflowEpisode.hpp"
#include "engine/graphics/RenderItemBudget.hpp"

#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Objects are admitted whole, in build order, and once one does not fit every
// later object is dropped, so the drawn set is a stable prefix.
void testRenderItemBudget() {
    RenderItemBudget budget(10);
    expect(budget.admit(4) && budget.admit(6), "objects that fit the capacity exactly are admitted");
    expect(!budget.admit(1) && budget.dropped() == 1, "an object past the capacity is dropped and counted");
    expect(!budget.admit(0) && budget.admitted() == 10, "nothing is admitted once the budget is full");

    RenderItemBudget prefix(10);
    expect(prefix.admit(8), "the first object is admitted");
    expect(!prefix.admit(3), "an object larger than the remaining room is dropped whole");
    expect(!prefix.admit(1) && prefix.admitted() == 8 && prefix.dropped() == 4,
           "a later object that would fit is dropped too, keeping the drawn set a prefix");

    RenderItemBudget oversized(4);
    expect(!oversized.admit(5) && oversized.admitted() == 0 && oversized.dropped() == 5,
           "an object larger than the whole capacity is dropped");
}

void testOverflowEpisode() {
    OverflowEpisode episode;
    expect(!episode.update(false) && !episode.active(), "a limit within bounds starts no episode");
    expect(episode.update(true) && episode.active(), "the first overflow starts an episode");
    expect(!episode.update(true) && !episode.update(true), "a sustained overflow is reported once");
    expect(!episode.update(false) && !episode.active(), "an update within bounds ends the episode");
    expect(episode.update(true), "a later overflow starts a new episode");
}
} // namespace

int main() {
    testRenderItemBudget();
    testOverflowEpisode();
    if (failures == 0) {
        std::cout << "Runtime limit tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

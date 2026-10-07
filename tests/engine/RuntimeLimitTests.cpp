#include "engine/diagnostics/OverflowEpisode.hpp"
#include "engine/graphics/RenderItemBudget.hpp"

#include "support/Doctest.hpp"

#include <iostream>

TEST_SUITE_BEGIN("RuntimeLimit");

namespace {

// Objects are admitted whole, in build order, and once one does not fit every
// later object is dropped, so the drawn set is a stable prefix.
TEST_CASE("RuntimeLimit: render item budget") {
    RenderItemBudget budget(10);
    CHECK_MESSAGE((budget.admit(4) && budget.admit(6)), "objects that fit the capacity exactly are admitted");
    CHECK_MESSAGE((!budget.admit(1) && budget.dropped() == 1), "an object past the capacity is dropped and counted");
    CHECK_MESSAGE((!budget.admit(0) && budget.admitted() == 10), "nothing is admitted once the budget is full");

    RenderItemBudget prefix(10);
    CHECK_MESSAGE(prefix.admit(8), "the first object is admitted");
    CHECK_MESSAGE(!prefix.admit(3), "an object larger than the remaining room is dropped whole");
    CHECK_MESSAGE((!prefix.admit(1) && prefix.admitted() == 8 && prefix.dropped() == 4),
                  "a later object that would fit is dropped too, keeping the drawn set a prefix");

    RenderItemBudget oversized(4);
    CHECK_MESSAGE((!oversized.admit(5) && oversized.admitted() == 0 && oversized.dropped() == 5),
                  "an object larger than the whole capacity is dropped");
}

TEST_CASE("RuntimeLimit: overflow episode") {
    OverflowEpisode episode;
    CHECK_MESSAGE((!episode.update(false) && !episode.active()), "a limit within bounds starts no episode");
    CHECK_MESSAGE((episode.update(true) && episode.active()), "the first overflow starts an episode");
    CHECK_MESSAGE((!episode.update(true) && !episode.update(true)), "a sustained overflow is reported once");
    CHECK_MESSAGE((!episode.update(false) && !episode.active()), "an update within bounds ends the episode");
    CHECK_MESSAGE(episode.update(true), "a later overflow starts a new episode");
}
} // namespace

TEST_SUITE_END();

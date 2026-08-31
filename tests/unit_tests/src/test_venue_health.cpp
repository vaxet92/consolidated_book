#include <gtest/gtest.h>

#include "md_core/venue_health.h"

using namespace market_data;

namespace {

// One second, in nanoseconds. A stand-in threshold for these tests only -
// the real per-venue values are config, and are to be set from measured
// inter-arrival gaps rather than picked here.
constexpr int64_t kThresholdNs = 1'000'000'000;

// steady_clock's epoch is arbitrary and machine-local, so a realistic "now"
// is some large number with no meaning of its own. Using a large base rather
// than starting at 0 keeps the tests honest: nothing here may depend on the
// timestamps being small or on the epoch being any particular point.
constexpr int64_t kNow = 987'654'321'000'000;

}  // namespace

// --- never seen -------------------------------------------------------------

TEST(VenueHealthTest, NeverSeenIsNoData) {
    EXPECT_EQ(ClassifyVenue(/*last=*/0, kNow, kThresholdNs), VenueHealth::kNoData);
}

// The reason last_mono_ns == 0 is checked BEFORE the subtraction. A venue
// that never spoke has no age; computing one gives the process uptime, which
// is both meaningless and always large enough to look stale. A venue whose
// subscription was silently rejected would then be reported as a dead
// exchange - the wrong problem to go looking for.
TEST(VenueHealthTest, NeverSeenStaysNoDataEvenAfterLongUptime) {
    const int64_t one_day_ns = 86'400ll * 1'000'000'000ll;
    EXPECT_EQ(ClassifyVenue(/*last=*/0, one_day_ns, kThresholdNs), VenueHealth::kNoData);
}

// --- normal operation -------------------------------------------------------

TEST(VenueHealthTest, FreshIsLive) {
    const int64_t last = kNow - 1'000'000;  // 1ms ago
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, ZeroAgeIsLive) {
    EXPECT_EQ(ClassifyVenue(kNow, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, WellPastThresholdIsStale) {
    const int64_t last = kNow - 10 * kThresholdNs;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kStale);
}

// --- the boundary -----------------------------------------------------------
//
// These two pin down the `>` vs `>=` choice. Either is defensible; what is
// not defensible is nobody knowing which one the code does. If the
// comparison is ever changed, exactly one of these two fails, which is the
// point of writing both.

TEST(VenueHealthTest, ExactlyAtThresholdIsLive) {
    const int64_t last = kNow - kThresholdNs;  // age == threshold
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, OneNanosecondPastThresholdIsStale) {
    const int64_t last = kNow - kThresholdNs - 1;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kStale);
}

// --- clock races ------------------------------------------------------------

// The provider stamps recv_mono_ns on its own thread; the publisher reads
// `now` on another. Their steady_clock readings can cross, giving an age
// that is slightly negative. That must read as live - a message from the
// immediate future is not stale - and it must not wrap into a huge positive
// age, which is why these are int64_t and not an unsigned type.
TEST(VenueHealthTest, NegativeAgeIsLive) {
    const int64_t last = kNow + 5'000;  // stamped 5us "after" now
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, LargeNegativeAgeIsStillLive) {
    const int64_t last = kNow + 10 * kThresholdNs;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

// --- admission --------------------------------------------------------------

TEST(VenueHealthTest, OnlyLiveIsAdmissible) {
    EXPECT_TRUE(IsAdmissible(VenueHealth::kLive));
    EXPECT_FALSE(IsAdmissible(VenueHealth::kStale));
    // kNoData must not sneak into the merge on the grounds that its book is
    // empty anyway. Relying on "empty book contributes nothing" would make
    // admission depend on a second, unrelated invariant holding.
    EXPECT_FALSE(IsAdmissible(VenueHealth::kNoData));
}

// --- compile time -----------------------------------------------------------
//
// Not a runtime assertion: these fail the BUILD if the function stops being
// constexpr. It is declared constexpr so it can inline inside the merge
// loop, and losing that silently would be easy to miss.

static_assert(ClassifyVenue(0, kNow, kThresholdNs) == VenueHealth::kNoData);
static_assert(ClassifyVenue(kNow - kThresholdNs, kNow, kThresholdNs) == VenueHealth::kLive);
static_assert(ClassifyVenue(kNow - kThresholdNs - 1, kNow, kThresholdNs) == VenueHealth::kStale);
static_assert(IsAdmissible(VenueHealth::kLive));
static_assert(!IsAdmissible(VenueHealth::kStale));

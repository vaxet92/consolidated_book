#include <gtest/gtest.h>

#include "md_core/consolidated_book.h"

using namespace market_data;
using namespace market_data::consolidated;

namespace {

void SetBook(VenueBookArray& books, VenueId venue, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{};
    update.venue = venue;
    update.instrument = InstrumentId::BTCUSDT;
    update.seq = 1;
    update.is_snapshot = true;
    update.bids = std::move(bids);
    update.asks = std::move(asks);

    books[static_cast<size_t>(venue)] = std::make_unique<VenueBook>(venue, InstrumentId::BTCUSDT);
    books[static_cast<size_t>(venue)]->ApplyUpdate(update);
}

// Notional is unsigned __int128, which gtest cannot print on failure - there
// is no operator<< for it. Test values are small, so compare as uint64.
uint64_t CumNotional(const MergedLevel& level) {
    return static_cast<uint64_t>(level.cum_notional);
}

}  // namespace

// ------------------------------------------------------------- MergeBooks ---

TEST(ConsolidatedBookTest, EmptyVenuesProduceEmptyBook) {
    VenueBookArray books{};
    Book merged;

    MergeBooks(books, merged);

    EXPECT_TRUE(merged.bids.empty());
    EXPECT_TRUE(merged.asks.empty());
}

TEST(ConsolidatedBookTest, SingleVenueIsCopiedWithPrefixSums) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {99, 3}}, {{101, 2}});
    Book merged;

    MergeBooks(books, merged);

    ASSERT_EQ(merged.bids.size(), 2u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[0].cum_qty, 5u);
    EXPECT_EQ(CumNotional(merged.bids[0]), 100u * 5u);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[1].cum_qty, 8u);                     // 5 + 3
    EXPECT_EQ(CumNotional(merged.bids[1]), 500u + 99u * 3u);   // 500 + 297

    ASSERT_EQ(merged.asks.size(), 1u);
    EXPECT_EQ(merged.asks[0].price, 101u);
    EXPECT_EQ(merged.asks[0].cum_qty, 2u);
}

// Bids must come out descending and asks ascending, regardless of which
// venue contributed which level.
TEST(ConsolidatedBookTest, DisjointPricesInterleaveInSortedOrder) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 1}, {98, 1}}, {{105, 1}, {107, 1}});
    SetBook(books, VenueId::OKX, {{99, 1}, {97, 1}}, {{106, 1}, {108, 1}});
    Book merged;

    MergeBooks(books, merged);

    ASSERT_EQ(merged.bids.size(), 4u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[2].price, 98u);
    EXPECT_EQ(merged.bids[3].price, 97u);

    ASSERT_EQ(merged.asks.size(), 4u);
    EXPECT_EQ(merged.asks[0].price, 105u);
    EXPECT_EQ(merged.asks[1].price, 106u);
    EXPECT_EQ(merged.asks[2].price, 107u);
    EXPECT_EQ(merged.asks[3].price, 108u);
}

// The case the whole attribution design exists for (§5.3): two venues at the
// same price collapse into ONE level carrying both.
TEST(ConsolidatedBookTest, SamePriceAcrossVenuesMergesWithAttribution) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{100, 2}}, {});
    Book merged;

    MergeBooks(books, merged);

    ASSERT_EQ(merged.bids.size(), 1u) << "one price, one level";
    const MergedLevel& level = merged.bids[0];
    EXPECT_EQ(level.price, 100u);
    EXPECT_EQ(level.cum_qty, 10u);  // 5 + 3 + 2
    ASSERT_EQ(level.venue_count, 3);

    // Attribution is written in venue-index order by the merge.
    EXPECT_EQ(level.venues[0].venue, VenueId::BINANCE);
    EXPECT_EQ(level.venues[0].qty, 5u);
    EXPECT_EQ(level.venues[1].venue, VenueId::BYBIT);
    EXPECT_EQ(level.venues[1].qty, 2u);
    EXPECT_EQ(level.venues[2].venue, VenueId::OKX);
    EXPECT_EQ(level.venues[2].qty, 3u);
}

TEST(ConsolidatedBookTest, PartiallyOverlappingPricesMergeCorrectly) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {98, 2}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}, {99, 4}}, {});
    Book merged;

    MergeBooks(books, merged);

    ASSERT_EQ(merged.bids.size(), 3u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[0].cum_qty, 8u);  // both venues
    EXPECT_EQ(merged.bids[0].venue_count, 2);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[1].cum_qty, 12u);  // 8 + 4, OKX only
    EXPECT_EQ(merged.bids[1].venue_count, 1);
    EXPECT_EQ(merged.bids[2].price, 98u);
    EXPECT_EQ(merged.bids[2].cum_qty, 14u);  // 12 + 2, Binance only
}

TEST(ConsolidatedBookTest, MaxDepthTruncatesBothSides) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 1}, {99, 1}, {98, 1}, {97, 1}},
            {{101, 1}, {102, 1}, {103, 1}, {104, 1}});
    Book merged;

    MergeBooks(books, merged, /*max_depth=*/2);

    EXPECT_EQ(merged.bids.size(), 2u);
    EXPECT_EQ(merged.asks.size(), 2u);
    EXPECT_EQ(merged.bids[1].price, 99u) << "keeps the TOP levels, not arbitrary ones";
    EXPECT_EQ(merged.asks[1].price, 102u);
}

// LevelQty recovers the per-level quantity that MergedLevel deliberately
// does not store.
TEST(ConsolidatedBookTest, LevelQtyRecoversPerLevelQuantity) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {99, 3}, {98, 7}}, {});
    Book merged;

    MergeBooks(books, merged);

    ASSERT_EQ(merged.bids.size(), 3u);
    EXPECT_EQ(LevelQty(merged.bids, 0), 5u) << "index 0 is the base case";
    EXPECT_EQ(LevelQty(merged.bids, 1), 3u);
    EXPECT_EQ(LevelQty(merged.bids, 2), 7u);
}

// The Book is reused across publishes (Clear keeps capacity) - a second merge
// must not leave levels from the first.
TEST(ConsolidatedBookTest, ReusedBookHasNoStaleLevels) {
    Book merged;

    VenueBookArray first{};
    SetBook(first, VenueId::BINANCE, {{100, 1}, {99, 1}, {98, 1}}, {{101, 1}});
    MergeBooks(first, merged);
    ASSERT_EQ(merged.bids.size(), 3u);

    VenueBookArray second{};
    SetBook(second, VenueId::OKX, {{200, 9}}, {});
    MergeBooks(second, merged);

    ASSERT_EQ(merged.bids.size(), 1u) << "levels from the first merge must be gone";
    EXPECT_EQ(merged.bids[0].price, 200u);
    EXPECT_EQ(merged.bids[0].cum_qty, 9u) << "prefix sums restart, they do not continue";
    EXPECT_TRUE(merged.asks.empty());
}

// ---------------------------------------------------------- volume bands ---

namespace {

// Band math takes prices/quantities at production scale (x 1e8), and
// FillToNotional scales its target the same way. Raw single-digit fixtures
// make the target unreachable, so every band would report insufficient_depth
// - the merge tests above can stay raw only because they never touch
// notional.
constexpr PriceTicks Px(uint64_t whole) {
    return whole * kScaleFactor;
}
constexpr QtyUnits Qty(uint64_t whole) {
    return whole * kScaleFactor;
}

// Asks: 100@5, 101@10, 102@20. Cumulative notional: 500, 1510, 3550 USDT.
Book MakeBandBook() {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {}, {{Px(100), Qty(5)}, {Px(101), Qty(10)}, {Px(102), Qty(20)}});
    Book merged;
    MergeBooks(books, merged);
    return merged;
}

}  // namespace

// Target lands exactly on a level boundary: no partial fill.
TEST(ConsolidatedBookTest, FillToNotionalExactLevelBoundary) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 500 * kScaleFactor);

    EXPECT_EQ(fill.filled_qty, Qty(5));
    EXPECT_EQ(fill.filled_notional, 500u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(100));
    EXPECT_EQ(fill.level_count, 1u);
    EXPECT_EQ(fill.vwap, Px(100));  // 500 / 5
    EXPECT_FALSE(fill.insufficient_depth);
}

// Target falls inside level 1: 500 filled at price 100, then 505 more of the
// 1010 available at price 101 -> 5 extra units.
TEST(ConsolidatedBookTest, FillToNotionalSplitsFinalLevelProportionally) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 1005 * kScaleFactor);

    EXPECT_EQ(fill.filled_qty, Qty(10));  // 5 @100 + 5 @101
    EXPECT_EQ(fill.filled_notional, 1005u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(101));
    EXPECT_EQ(fill.level_count, 2u);
    // 1005 / 10 = 100.5 exactly. At production scale the fractional half
    // survives - at raw scale it would truncate to 100 and hide whether the
    // proportional split worked at all.
    EXPECT_EQ(fill.vwap, 1005u * kScaleFactor / 10);
    EXPECT_FALSE(fill.insufficient_depth);
}

// Asking for more than the book holds is a legitimate answer, not an error.
TEST(ConsolidatedBookTest, FillToNotionalExhaustedBookFlagsInsufficientDepth) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 10'000 * kScaleFactor);

    EXPECT_TRUE(fill.insufficient_depth);
    EXPECT_EQ(fill.filled_qty, Qty(35));  // everything: 5 + 10 + 20
    EXPECT_EQ(fill.filled_notional, 3550u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(102));
    EXPECT_EQ(fill.level_count, 3u);
}

TEST(ConsolidatedBookTest, FillToNotionalOnEmptySideFlagsInsufficientDepth) {
    Book book;
    auto fill = FillToNotional(book.bids, 1000 * kScaleFactor);

    EXPECT_TRUE(fill.insufficient_depth);
    EXPECT_EQ(fill.filled_qty, 0u);
}

// The multi-band walk must produce exactly what repeated single calls do -
// that equivalence is the whole justification for the single-pass version.
TEST(ConsolidatedBookTest, MultiBandMatchesRepeatedSingleBandCalls) {
    Book book = MakeBandBook();
    const std::vector<uint64_t> targets{500 * kScaleFactor, 1005 * kScaleFactor, 3550 * kScaleFactor,
                                        10'000 * kScaleFactor};

    std::vector<NotionalFill> multi;
    FillToNotionalBands(book.asks, targets, multi);

    ASSERT_EQ(multi.size(), targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        auto single = FillToNotional(book.asks, targets[i]);
        EXPECT_EQ(multi[i].filled_qty, single.filled_qty) << "band " << i;
        EXPECT_EQ(multi[i].filled_notional, single.filled_notional) << "band " << i;
        EXPECT_EQ(multi[i].worst_price, single.worst_price) << "band " << i;
        EXPECT_EQ(multi[i].level_count, single.level_count) << "band " << i;
        EXPECT_EQ(multi[i].vwap, single.vwap) << "band " << i;
        EXPECT_EQ(multi[i].insufficient_depth, single.insufficient_depth) << "band " << i;
    }
}

// ----------------------------------------------------------- price bands ---

// Asks from 100: a 100bps band reaches 101, a 200bps band reaches 102.
TEST(ConsolidatedBookTest, FillToBpsAskSideWalksUp) {
    Book book = MakeBandBook();

    auto fill = FillToBps(book.asks, 100, /*is_bid=*/false);

    EXPECT_EQ(fill.limit_price, Px(101));  // 100 * 10100 / 10000
    EXPECT_EQ(fill.level_count, 2u);       // 100 and 101 are inside, 102 is not
    EXPECT_EQ(fill.cum_qty, Qty(15));      // 5 + 10
    EXPECT_EQ(fill.cum_notional, 1510u * kScaleFactor);
    EXPECT_EQ(fill.vwap, 10066666666u);  // 1510 / 15 = 100.666..., truncated at 1e8 scale
}

TEST(ConsolidatedBookTest, FillToBpsBidSideWalksDown) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{Px(100), Qty(5)}, {Px(99), Qty(10)}, {Px(98), Qty(20)}}, {});
    Book merged;
    MergeBooks(books, merged);

    auto fill = FillToBps(merged.bids, 100, /*is_bid=*/true);

    EXPECT_EQ(fill.limit_price, Px(99));  // 100 * 9900 / 10000
    EXPECT_EQ(fill.level_count, 2u);      // 100 and 99 inside, 98 is not
    EXPECT_EQ(fill.cum_qty, Qty(15));
}

// A band narrower than the first gap contains only the top level.
TEST(ConsolidatedBookTest, FillToBpsNarrowBandTakesOnlyTopLevel) {
    Book book = MakeBandBook();

    auto fill = FillToBps(book.asks, 10, /*is_bid=*/false);

    EXPECT_EQ(fill.limit_price, 10010000000u);  // 100 * 10010 / 10000 = 100.1
    EXPECT_EQ(fill.level_count, 1u);            // only 100 is inside; 101 is past the limit
    EXPECT_EQ(fill.cum_qty, Qty(5));
}

// A band wide enough to cover the whole book runs out before reaching its
// limit price. Without the flag, that lower bound is indistinguishable from
// "this is all the liquidity within the band" - the exact confusion a
// 1000bps band on live data produces, where the walk stops at the depth
// budget nowhere near 10% from the top.
TEST(ConsolidatedBookTest, FillToBpsFlagsExhaustedBook) {
    Book book = MakeBandBook();  // asks 100, 101, 102

    auto wide = FillToBps(book.asks, 1000, /*is_bid=*/false);  // limit 110 - past every level
    EXPECT_TRUE(wide.insufficient_depth) << "walked the whole book without reaching the limit";
    EXPECT_EQ(wide.level_count, 3u);

    auto narrow = FillToBps(book.asks, 100, /*is_bid=*/false);  // limit 101 - stops inside the book
    EXPECT_FALSE(narrow.insufficient_depth) << "found the boundary, so the total is complete";
    EXPECT_EQ(narrow.level_count, 2u);
}

TEST(ConsolidatedBookTest, FillToBpsOnEmptySideReturnsNothing) {
    Book book;
    auto fill = FillToBps(book.bids, 100, /*is_bid=*/true);

    EXPECT_EQ(fill.level_count, 0u);
    EXPECT_EQ(fill.cum_qty, 0u);
}

TEST(ConsolidatedBookTest, MultiBpsBandMatchesRepeatedSingleCalls) {
    Book book = MakeBandBook();
    const std::vector<uint32_t> bands{10, 100, 200, 1000};

    std::vector<BpsFill> multi;
    FillToBpsBands(book.asks, bands, /*is_bid=*/false, multi);

    ASSERT_EQ(multi.size(), bands.size());
    for (size_t i = 0; i < bands.size(); ++i) {
        auto single = FillToBps(book.asks, bands[i], /*is_bid=*/false);
        EXPECT_EQ(multi[i].limit_price, single.limit_price) << "band " << i;
        EXPECT_EQ(multi[i].cum_qty, single.cum_qty) << "band " << i;
        EXPECT_EQ(multi[i].cum_notional, single.cum_notional) << "band " << i;
        EXPECT_EQ(multi[i].level_count, single.level_count) << "band " << i;
        EXPECT_EQ(multi[i].vwap, single.vwap) << "band " << i;
    }
}

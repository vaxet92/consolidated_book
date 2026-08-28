#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "md_core/consolidated_bbo.h"

using namespace market_data;

namespace {

BookUpdate MakeSnapshot(std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{};
    update.seq = 1;
    update.is_snapshot = true;
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

void SetBook(VenueBookArray& books, VenueId venue, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    books[static_cast<size_t>(venue)] = std::make_unique<VenueBook>(venue, InstrumentId::BTCUSDT);
    books[static_cast<size_t>(venue)]->ApplyUpdate(MakeSnapshot(std::move(bids), std::move(asks)));
}

BboQuote MakeQuote(VenueId venue, PriceTicks bid_px, QtyUnits bid_qty, PriceTicks ask_px, QtyUnits ask_qty) {
    BboQuote quote;
    quote.venue = venue;
    quote.instrument = InstrumentId::BTCUSDT;
    quote.bid_price = bid_px;
    quote.bid_qty = bid_qty;
    quote.ask_price = ask_px;
    quote.ask_qty = ask_qty;
    return quote;
}

// Applies a quote the way Core does: store into the array FIRST (the rescan
// path re-reads it), then fold it in.
void Apply(VenueQuoteArray& quotes, consolidated::BBO& bbo, const BboQuote& quote) {
    quotes[static_cast<size_t>(quote.venue)] = quote;
    consolidated::UpdateBBOWithQuote(bbo, quote, quotes);
}

// The incremental path appends venues in arrival order; the full scan
// appends in venue-index order. The sets must match, the order need not -
// so compare sorted copies. Comparing only .size() would miss a
// wrong-venue or swapped-qty bug.
void ExpectSameVenues(const std::vector<consolidated::VenueQuote>& a,
                      const std::vector<consolidated::VenueQuote>& b) {
    auto sorted_a = a;
    auto sorted_b = b;
    auto by_venue = [](const consolidated::VenueQuote& l, const consolidated::VenueQuote& r) {
        return l.venue < r.venue;
    };
    std::sort(sorted_a.begin(), sorted_a.end(), by_venue);
    std::sort(sorted_b.begin(), sorted_b.end(), by_venue);

    ASSERT_EQ(sorted_a.size(), sorted_b.size());
    for (size_t i = 0; i < sorted_a.size(); ++i) {
        EXPECT_EQ(sorted_a[i].venue, sorted_b[i].venue);
        EXPECT_EQ(sorted_a[i].qty, sorted_b[i].qty);
    }
}

}  // namespace

TEST(ConsolidatedBboTest, EmptyBooksHaveNoConsolidatedBBO) {
    VenueBookArray books{};  // all null - no venue configured

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.total_qty, 0u);
    EXPECT_TRUE(bbo.best_bid.venues.empty());
    EXPECT_EQ(bbo.best_ask.total_qty, 0u);
    EXPECT_TRUE(bbo.best_ask.venues.empty());
    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, SingleVenueContributesBBO) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 5u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::BINANCE);
    EXPECT_EQ(bbo.best_bid.venues[0].qty, 5u);

    EXPECT_EQ(bbo.best_ask.price, 101u);
}

TEST(ConsolidatedBboTest, HighestBidWinsAcrossVenues) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{102, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{99, 1}}, {});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 102u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::OKX);
}

TEST(ConsolidatedBboTest, TiedBidsAtBestPriceAreSummedWithAttribution) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{95, 1}}, {});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 8u);  // 5 + 3
    ASSERT_EQ(bbo.best_bid.venues.size(), 2u);
}

TEST(ConsolidatedBboTest, LowestAskWinsAcrossVenues) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {}, {{105, 2}});
    SetBook(books, VenueId::OKX, {}, {{103, 4}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_ask.price, 103u);
    EXPECT_EQ(bbo.best_ask.total_qty, 4u);
}

TEST(ConsolidatedBboTest, NonCrossedBookIsNotFlagged) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, CrossedBookSetsFlag) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{105, 5}}, {});  // bid above the other venue's ask
    SetBook(books, VenueId::OKX, {}, {{100, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_TRUE(bbo.crossed);
}

// The case that can't be solved from the consolidated BBO alone: the only
// venue at the best level steps down, so the new best must come from the
// per-venue array (the rescan path).
TEST(ConsolidatedBboTest, IncrementalSoleBestVenueSteppingDownRescans) {
    VenueQuoteArray quotes{};
    consolidated::BBO bbo;

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 100, 5, 110, 5));
    Apply(quotes, bbo, MakeQuote(VenueId::OKX, 99, 3, 111, 3));
    Apply(quotes, bbo, MakeQuote(VenueId::BYBIT, 98, 1, 112, 1));
    ASSERT_EQ(bbo.best_bid.price, 100u);  // Binance alone at the top

    // Binance drops below both others - answer must be OKX at 99, not 95.
    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 95, 5, 110, 5));

    EXPECT_EQ(bbo.best_bid.price, 99u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::OKX);
}

// A tied level losing one venue must keep the other - no rescan, no phantom
// level, and total_qty must drop by exactly the leaver's old qty.
TEST(ConsolidatedBboTest, IncrementalTiedLevelLosingOneVenueKeepsTheOther) {
    VenueQuoteArray quotes{};
    consolidated::BBO bbo;

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 100, 5, 110, 5));
    Apply(quotes, bbo, MakeQuote(VenueId::OKX, 100, 3, 111, 3));
    ASSERT_EQ(bbo.best_bid.total_qty, 8u);  // 5 + 3, tied at 100

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 90, 5, 110, 5));

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);  // 8 - Binance's old 5
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::OKX);
}

// UpdateBBOWithQuote maintains persistent state, so a bug accumulates
// silently instead of failing loudly. Drive a random stream through it and
// assert after EVERY update that it exactly equals a full rescan - the same
// oracle role std::map plays for VenueBook (DESIGN_1 §5.1).
TEST(ConsolidatedBboTest, IncrementalMatchesFullScanOverRandomStream) {
    std::mt19937 rng(12345);  // fixed seed - reproducible failures

    // Deliberately narrow price range: with wide prices two venues would
    // essentially never collide, so the tie and rescan branches - the ones
    // most likely to be wrong - would never execute. Bid and ask draw from
    // the SAME range, so crossed books occur constantly too.
    std::uniform_int_distribution<uint64_t> price_dist(100, 104);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 9);
    std::uniform_int_distribution<size_t> venue_dist(0, static_cast<size_t>(VenueId::COUNT) - 1);

    VenueQuoteArray quotes{};
    consolidated::BBO incremental;

    for (int i = 0; i < 20000; ++i) {
        auto quote = MakeQuote(static_cast<VenueId>(venue_dist(rng)), price_dist(rng), qty_dist(rng), price_dist(rng),
                               qty_dist(rng));
        Apply(quotes, incremental, quote);

        auto expected = consolidated::ComputeBBOFromQuotes(quotes);

        ASSERT_EQ(incremental.best_bid.price, expected.best_bid.price) << "bid price diverged at update " << i;
        ASSERT_EQ(incremental.best_bid.total_qty, expected.best_bid.total_qty) << "bid qty diverged at update " << i;
        ASSERT_EQ(incremental.best_ask.price, expected.best_ask.price) << "ask price diverged at update " << i;
        ASSERT_EQ(incremental.best_ask.total_qty, expected.best_ask.total_qty) << "ask qty diverged at update " << i;
        ASSERT_EQ(incremental.crossed, expected.crossed) << "crossed diverged at update " << i;
        ExpectSameVenues(incremental.best_bid.venues, expected.best_bid.venues);
        ExpectSameVenues(incremental.best_ask.venues, expected.best_ask.venues);
    }
}

TEST(ConsolidatedBboTest, UnconfiguredVenueSlotIsSkippedNotCrashed) {
    VenueBookArray books{};  // every slot null except one
    SetBook(books, VenueId::BYBIT, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::BYBIT);
}

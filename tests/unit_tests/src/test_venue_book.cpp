#include <gtest/gtest.h>

#include <map>
#include <random>

#include "venue_book.h"

using namespace market_data;

namespace {

BookUpdate MakeUpdate(uint64_t seq, bool is_snapshot, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{VenueId::BINANCE, InstrumentId::BTCUSDT, bids.size(), is_snapshot, seq};
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

// A single venue's own book must NEVER be crossed. Cross-venue crossing is
// expected and normal (§6.3); within one venue it means we corrupted the
// book - a stale level that should have been removed, or a bad comparator.
void ExpectNotCrossed(const VenueBook& book) {
    auto bid = book.BestBid();
    auto ask = book.BestAsk();
    if (bid && ask) {
        EXPECT_LT(bid->first, ask->first)
            << "single-venue book is crossed: bid " << bid->first << " >= ask " << ask->first;
    }
}

// Compares a whole side, in iteration order - so this also verifies the
// comparators (bids descending, asks ascending), not just membership.
// Two functions rather than one template because the sides have different
// comparator types, so a single signature can't take both.
void ExpectBids(const VenueBook& book, const std::vector<PriceLevel>& expected) {
    std::vector<PriceLevel> actual;
    for (const auto& [price, qty] : book.bids()) {
        actual.push_back({price, qty});
    }
    ASSERT_EQ(actual.size(), expected.size()) << "bid level count";
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].price, expected[i].price) << "bid price at index " << i;
        EXPECT_EQ(actual[i].qty, expected[i].qty) << "bid qty at index " << i;
    }
}

void ExpectAsks(const VenueBook& book, const std::vector<PriceLevel>& expected) {
    std::vector<PriceLevel> actual;
    for (const auto& [price, qty] : book.asks()) {
        actual.push_back({price, qty});
    }
    ASSERT_EQ(actual.size(), expected.size()) << "ask level count";
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].price, expected[i].price) << "ask price at index " << i;
        EXPECT_EQ(actual[i].qty, expected[i].qty) << "ask qty at index " << i;
    }
}

}  // namespace

TEST(VenueBookTest, EmptyBookHasNoBestBidOrAsk) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    EXPECT_FALSE(book.BestBid().has_value());
    EXPECT_FALSE(book.BestAsk().has_value());
}

TEST(VenueBookTest, ApplyDeltaInsertsLevelAsBestOfBook) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, /*is_snapshot=*/false, {{100, 5}}, {{101, 7}}));

    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(book.BestBid()->first, 100u);
    EXPECT_EQ(book.BestBid()->second, 5u);

    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(book.BestAsk()->first, 101u);
    EXPECT_EQ(book.BestAsk()->second, 7u);
}

TEST(VenueBookTest, ApplyDeltaAtExistingPriceOverwritesQty) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {{100, 5}}, {}));
    book.ApplyUpdate(MakeUpdate(2, false, {{100, 9}}, {}));

    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(book.BestBid()->second, 9u);
}

TEST(VenueBookTest, ZeroQtyDeltaRemovesLevel) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {{100, 5}}, {}));
    book.ApplyUpdate(MakeUpdate(2, false, {{100, 0}}, {}));

    EXPECT_FALSE(book.BestBid().has_value());
}

TEST(VenueBookTest, SnapshotReplacesEntireBook) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    // Old state: a bid at 100 that the new snapshot will not mention.
    book.ApplyUpdate(MakeUpdate(1, false, {{100, 5}}, {}));

    book.ApplyUpdate(MakeUpdate(2, /*is_snapshot=*/true, {{200, 3}}, {{201, 4}}));

    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(book.BestBid()->first, 200u);  // old level at 100 is gone
    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(book.BestAsk()->first, 201u);
}

TEST(VenueBookTest, BestBidIsHighestPriceAmongMultipleLevels) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {{100, 1}, {102, 1}, {101, 1}}, {}));

    ASSERT_TRUE(book.BestBid().has_value());
    EXPECT_EQ(book.BestBid()->first, 102u);
}

TEST(VenueBookTest, BestAskIsLowestPriceAmongMultipleLevels) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {}, {{105, 1}, {103, 1}, {104, 1}}));

    ASSERT_TRUE(book.BestAsk().has_value());
    EXPECT_EQ(book.BestAsk()->first, 103u);
}

// Snapshot, then a realistic run of deltas: update a qty, remove a level,
// insert a new best, insert deeper, then a mid-stream snapshot. The exact
// book is asserted after every step, and the not-crossed invariant holds
// throughout.
TEST(VenueBookTest, SnapshotThenDeltasProducesExactBook) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(100, /*is_snapshot=*/true, {{1000, 10}, {999, 20}, {998, 30}},
                                {{1001, 15}, {1002, 25}, {1003, 35}}));
    ExpectBids(book, {{1000, 10}, {999, 20}, {998, 30}});
    ExpectAsks(book, {{1001, 15}, {1002, 25}, {1003, 35}});
    ExpectNotCrossed(book);

    // Update a bid qty in place; remove the best ask (qty 0).
    book.ApplyUpdate(MakeUpdate(101, false, {{1000, 12}}, {{1001, 0}}));
    ExpectBids(book, {{1000, 12}, {999, 20}, {998, 30}});
    ExpectAsks(book, {{1002, 25}, {1003, 35}});
    ExpectNotCrossed(book);

    // Insert a new best bid, still below the best ask.
    book.ApplyUpdate(MakeUpdate(102, false, {{1001, 5}}, {}));
    ExpectBids(book, {{1001, 5}, {1000, 12}, {999, 20}, {998, 30}});
    ExpectNotCrossed(book);

    // Remove the deepest bid; add a deeper ask.
    book.ApplyUpdate(MakeUpdate(103, false, {{998, 0}}, {{1004, 40}}));
    ExpectBids(book, {{1001, 5}, {1000, 12}, {999, 20}});
    ExpectAsks(book, {{1002, 25}, {1003, 35}, {1004, 40}});
    ExpectNotCrossed(book);

    // A mid-stream snapshot replaces EVERYTHING - levels not mentioned are
    // gone, including ones that were never explicitly removed.
    book.ApplyUpdate(MakeUpdate(104, true, {{2000, 1}}, {{2001, 2}}));
    ExpectBids(book, {{2000, 1}});
    ExpectAsks(book, {{2001, 2}});
    ExpectNotCrossed(book);
}

// Drives many random deltas through VenueBook and against a trivially-correct
// reference model written inline. Both must agree after every update. The
// reference is deliberately dumb - that is what makes it a useful oracle for
// ApplySide's erase/overwrite logic (DESIGN_1 §5.1's pattern).
TEST(VenueBookTest, RandomDeltasMatchReferenceModelAndNeverCross) {
    std::mt19937 rng(4242);  // fixed seed - reproducible failures
    std::uniform_int_distribution<uint64_t> bid_px(900, 999);
    std::uniform_int_distribution<uint64_t> ask_px(1000, 1099);
    std::uniform_int_distribution<uint64_t> qty(0, 9);  // 0 means remove

    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);
    std::map<PriceTicks, QtyUnits> ref_bids;
    std::map<PriceTicks, QtyUnits> ref_asks;

    auto apply_ref = [](std::map<PriceTicks, QtyUnits>& side, const std::vector<PriceLevel>& levels) {
        for (const auto& level : levels) {
            if (level.qty == 0) {
                side.erase(level.price);
            } else {
                side[level.price] = level.qty;
            }
        }
    };

    for (int i = 0; i < 5000; ++i) {
        std::vector<PriceLevel> bids{{bid_px(rng), qty(rng)}};
        std::vector<PriceLevel> asks{{ask_px(rng), qty(rng)}};

        book.ApplyUpdate(MakeUpdate(static_cast<uint64_t>(i + 1), false, bids, asks));
        apply_ref(ref_bids, bids);
        apply_ref(ref_asks, asks);

        ASSERT_EQ(book.bids().size(), ref_bids.size()) << "bid count diverged at update " << i;
        ASSERT_EQ(book.asks().size(), ref_asks.size()) << "ask count diverged at update " << i;
        for (const auto& [price, q] : ref_bids) {
            auto it = book.bids().find(price);
            ASSERT_NE(it, book.bids().end()) << "missing bid " << price << " at update " << i;
            EXPECT_EQ(it->second, q) << "bid qty at " << price << ", update " << i;
        }
        for (const auto& [price, q] : ref_asks) {
            auto it = book.asks().find(price);
            ASSERT_NE(it, book.asks().end()) << "missing ask " << price << " at update " << i;
            EXPECT_EQ(it->second, q) << "ask qty at " << price << ", update " << i;
        }
        // Bid and ask ranges never overlap by construction, so a cross here
        // means the book is corrupt, not that the market moved.
        ExpectNotCrossed(book);
    }
}

TEST(VenueBookTest, LastSeqTracksMostRecentlyAppliedUpdate) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {{100, 1}}, {}));
    book.ApplyUpdate(MakeUpdate(2, false, {{100, 2}}, {}));

    EXPECT_EQ(book.last_seq(), 2u);
}

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

// ------------------------------------------------ multi-level deltas --------
//
// ApplySide chains an insertion hint from one level to the next, so its
// behaviour only differs from the naive version when a SINGLE delta carries
// several levels. Every test above uses one level per side, which means none
// of them exercised the hint chain at all.

// The normal case: many levels, sorted in each side's own order (bids
// descending, asks ascending) - exactly what the venues send, and the case the
// hint is meant to make cheap.
TEST(VenueBookTest, SortedMultiLevelDeltaAppliesEveryLevel) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    for (uint64_t i = 0; i < 50; ++i) {
        bids.push_back({1000 - i, i + 1});  // descending
        asks.push_back({2000 + i, i + 1});  // ascending
    }

    book.ApplyUpdate(MakeUpdate(1, false, bids, asks));

    ASSERT_EQ(book.bids().size(), 50u);
    ASSERT_EQ(book.asks().size(), 50u);
    for (uint64_t i = 0; i < 50; ++i) {
        EXPECT_EQ(book.bids().at(1000 - i), i + 1) << "bid " << i;
        EXPECT_EQ(book.asks().at(2000 + i), i + 1) << "ask " << i;
    }
}

// KEY: the hint is a PERFORMANCE suggestion, never a correctness dependency.
// A wrong hint makes std::map fall back to a normal search and nothing else.
//
// This is the property that made hint-chaining the right choice over walking
// the book with an iterator: that variant IS correctness-dependent on sorted
// input, and silently drops updates when the assumption breaks - the worst
// failure mode available. This test would fail loudly under that design.
TEST(VenueBookTest, UnsortedMultiLevelDeltaIsStillApplyCorrectly) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    // Deliberately scrambled - neither ascending nor descending.
    const std::vector<PriceLevel> bids = {{950, 5}, {999, 1}, {970, 3}, {990, 2}, {960, 4}};
    const std::vector<PriceLevel> asks = {{1050, 5}, {1001, 1}, {1030, 3}, {1010, 2}, {1040, 4}};

    book.ApplyUpdate(MakeUpdate(1, false, bids, asks));

    ASSERT_EQ(book.bids().size(), 5u);
    ASSERT_EQ(book.asks().size(), 5u);
    for (const auto& level : bids) {
        EXPECT_EQ(book.bids().at(level.price), level.qty) << "bid " << level.price;
    }
    for (const auto& level : asks) {
        EXPECT_EQ(book.asks().at(level.price), level.qty) << "ask " << level.price;
    }
    // Ordering must still be correct despite the scrambled input.
    EXPECT_EQ(book.BestBid()->first, 999u);
    EXPECT_EQ(book.BestAsk()->first, 1001u);
}

// The dangling-hint hazard, in one delta.
//
// erase(key) would invalidate the chained hint whenever it pointed at the
// erased element, and passing a dangling iterator as a hint is undefined
// behaviour. ApplySide uses erase(iterator), which returns the FOLLOWING
// element - valid, and already the right hint for the next price.
//
// Interleaving removals with insertions in a single delta is what forces that
// path; a delta of pure inserts or pure erases would never reach it.
TEST(VenueBookTest, ErasesInterleavedWithInsertsInOneDelta) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    std::vector<PriceLevel> seed_bids;
    std::vector<PriceLevel> seed_asks;
    for (uint64_t i = 0; i < 20; ++i) {
        seed_bids.push_back({1000 - i, 10});
        seed_asks.push_back({2000 + i, 10});
    }
    book.ApplyUpdate(MakeUpdate(1, false, seed_bids, seed_asks));
    ASSERT_EQ(book.bids().size(), 20u);

    // Every other level removed, the rest rewritten - in sorted order, so the
    // hint stays "correct" right across the erases.
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    for (uint64_t i = 0; i < 20; ++i) {
        const QtyUnits qty = (i % 2 == 0) ? 0 : 77;
        bids.push_back({1000 - i, qty});
        asks.push_back({2000 + i, qty});
    }
    book.ApplyUpdate(MakeUpdate(2, false, bids, asks));

    EXPECT_EQ(book.bids().size(), 10u);
    EXPECT_EQ(book.asks().size(), 10u);
    for (uint64_t i = 0; i < 20; ++i) {
        if (i % 2 == 0) {
            EXPECT_EQ(book.bids().count(1000 - i), 0u) << "bid " << i << " should be gone";
        } else {
            EXPECT_EQ(book.bids().at(1000 - i), 77u) << "bid " << i;
        }
    }
    // The best bid was removed, so the next one down must take over.
    EXPECT_EQ(book.BestBid()->first, 999u);
}

// Deleting a price the book does not hold must be a no-op, not an insertion of
// a zero-quantity level and not a corrupted hint for what follows.
TEST(VenueBookTest, EraseOfAbsentPriceDoesNotDisturbTheRest) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    const std::vector<PriceLevel> bids = {{999, 0}, {998, 5}, {997, 0}, {996, 7}};
    book.ApplyUpdate(MakeUpdate(1, false, bids, {}));

    EXPECT_EQ(book.bids().size(), 2u);
    EXPECT_EQ(book.bids().at(998), 5u);
    EXPECT_EQ(book.bids().at(996), 7u);
    EXPECT_EQ(book.BestBid()->first, 998u);
}

// The oracle test above, but with MULTI-level deltas - which is what actually
// exercises the hint chain. Same trivially-correct reference model; the two
// must agree after every update.
TEST(VenueBookTest, RandomMultiLevelDeltasMatchReferenceModel) {
    std::mt19937 rng(9001);
    std::uniform_int_distribution<uint64_t> bid_px(900, 999);
    std::uniform_int_distribution<uint64_t> ask_px(1000, 1099);
    std::uniform_int_distribution<uint64_t> qty(0, 9);  // 0 removes
    std::uniform_int_distribution<int> level_count(1, 25);
    std::uniform_int_distribution<int> sorted_coin(0, 1);

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

    for (int i = 0; i < 2000; ++i) {
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> asks;
        const int count = level_count(rng);
        for (int k = 0; k < count; ++k) {
            bids.push_back({bid_px(rng), qty(rng)});
            asks.push_back({ask_px(rng), qty(rng)});
        }

        // Half the updates arrive sorted (the venues' real behaviour, and the
        // case the hint exploits), half scrambled - so the run covers both the
        // fast path and the fallback.
        if (sorted_coin(rng) == 1) {
            std::sort(bids.begin(), bids.end(),
                      [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
            std::sort(asks.begin(), asks.end(),
                      [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });
        }

        book.ApplyUpdate(MakeUpdate(static_cast<uint64_t>(i + 1), false, bids, asks));
        apply_ref(ref_bids, bids);
        apply_ref(ref_asks, asks);

        ASSERT_EQ(book.bids().size(), ref_bids.size()) << "bid count diverged at update " << i;
        ASSERT_EQ(book.asks().size(), ref_asks.size()) << "ask count diverged at update " << i;
        for (const auto& [price, q] : ref_bids) {
            auto it = book.bids().find(price);
            ASSERT_NE(it, book.bids().end()) << "missing bid " << price << " at update " << i;
            ASSERT_EQ(it->second, q) << "bid qty at " << price << ", update " << i;
        }
        for (const auto& [price, q] : ref_asks) {
            auto it = book.asks().find(price);
            ASSERT_NE(it, book.asks().end()) << "missing ask " << price << " at update " << i;
            ASSERT_EQ(it->second, q) << "ask qty at " << price << ", update " << i;
        }
    }
}

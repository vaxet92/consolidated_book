#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "flat_order_book.h"
#include "map_order_book.h"

using namespace market_data;

namespace {

const InstrumentKey kSpotBtc = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

BookUpdate MakeUpdate(uint64_t seq, bool is_snapshot, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{VenueId::BINANCE, kSpotBtc, bids.size(), is_snapshot, seq};
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

// Materialises a side in ITERATION order. Works for both books because both
// yield something destructurable into (price, qty): std::map yields a pair,
// FlatOrderBook yields a PriceLevel.
template <typename Range>
std::vector<PriceLevel> Levels(Range&& side) {
    std::vector<PriceLevel> out;
    for (const auto& [price, qty] : side) {
        out.push_back({price, qty});
    }
    return out;
}

// The oracle comparison (CLAUDE.md §6): the std::map book is the reference and
// the flat book must agree with it exactly.
//
// KEY: this compares SEQUENCES, not sets, so it verifies ordering as well as
// contents. Both books present best-first, but by completely different
// mechanisms - MapOrderBook from its std::greater/std::less comparators,
// FlatOrderBook by reversing a vector deliberately stored worst-first. Two
// unrelated mechanisms arriving at the same order is exactly what an oracle is
// for; comparing membership alone would pass even with the sides backwards.
void ExpectSameBook(const MapOrderBook& oracle, const FlatOrderBook& flat, int update_index) {
    const std::vector<PriceLevel> oracle_bids = Levels(oracle.bids());
    const std::vector<PriceLevel> flat_bids = Levels(flat.bids());
    ASSERT_EQ(flat_bids.size(), oracle_bids.size()) << "bid level count, update " << update_index;
    for (size_t i = 0; i < oracle_bids.size(); ++i) {
        ASSERT_EQ(flat_bids[i].price, oracle_bids[i].price) << "bid price at " << i << ", update " << update_index;
        ASSERT_EQ(flat_bids[i].qty, oracle_bids[i].qty) << "bid qty at " << i << ", update " << update_index;
    }

    const std::vector<PriceLevel> oracle_asks = Levels(oracle.asks());
    const std::vector<PriceLevel> flat_asks = Levels(flat.asks());
    ASSERT_EQ(flat_asks.size(), oracle_asks.size()) << "ask level count, update " << update_index;
    for (size_t i = 0; i < oracle_asks.size(); ++i) {
        ASSERT_EQ(flat_asks[i].price, oracle_asks[i].price) << "ask price at " << i << ", update " << update_index;
        ASSERT_EQ(flat_asks[i].qty, oracle_asks[i].qty) << "ask qty at " << i << ", update " << update_index;
    }

    // BestBid/BestAsk read back() on the flat book and begin() on the map, so
    // they are a separate code path from the iteration above, not a restatement
    // of it. A reversed layout with a correct iterator would still fail here.
    ASSERT_EQ(flat.BestBid().has_value(), oracle.BestBid().has_value()) << "update " << update_index;
    if (oracle.BestBid()) {
        EXPECT_EQ(flat.BestBid()->first, oracle.BestBid()->first) << "best bid price, update " << update_index;
        EXPECT_EQ(flat.BestBid()->second, oracle.BestBid()->second) << "best bid qty, update " << update_index;
    }
    ASSERT_EQ(flat.BestAsk().has_value(), oracle.BestAsk().has_value()) << "update " << update_index;
    if (oracle.BestAsk()) {
        EXPECT_EQ(flat.BestAsk()->first, oracle.BestAsk()->first) << "best ask price, update " << update_index;
        EXPECT_EQ(flat.BestAsk()->second, oracle.BestAsk()->second) << "best ask qty, update " << update_index;
    }
}

}  // namespace

TEST(FlatOrderBookTest, EmptyBookHasNoBestBidOrAsk) {
    FlatOrderBook book(VenueId::BINANCE, kSpotBtc);

    EXPECT_FALSE(book.BestBid().has_value());
    EXPECT_FALSE(book.BestAsk().has_value());
    EXPECT_EQ(book.total_bytes_moved(), 0u);
}

// The main correctness check for ApplySide's merge pass.
//
// KEY: the deltas here are MULTI-LEVEL and their ordering VARIES, and both
// properties are load-bearing. The existing MapOrderBookTest oracle sends
// single-level deltas, which cannot exercise a merge at all - with one level
// there is nothing to merge and no order to get wrong. And ApplySide takes
// three different branches depending on the delta's order (storage order,
// reverse-storage order, neither), so a generator that only ever emitted sorted
// input would leave the branch that PREVENTS SILENT BOOK CORRUPTION completely
// untested.
TEST(FlatOrderBookTest, RandomMultiLevelDeltasMatchTheMapOracle) {
    std::mt19937 rng(4242);  // fixed seed - reproducible failures
    std::uniform_int_distribution<uint64_t> bid_px(900, 999);
    std::uniform_int_distribution<uint64_t> ask_px(1000, 1099);
    std::uniform_int_distribution<uint64_t> qty(0, 9);  // 0 means remove
    std::uniform_int_distribution<int> level_count(1, 8);
    std::uniform_int_distribution<int> ordering(0, 2);

    MapOrderBook oracle(VenueId::BINANCE, kSpotBtc);
    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);

    // Distinct prices only: a delta naming the same price twice has no defined
    // meaning, and no venue sends one. Generating them would test our tolerance
    // of a malformed message rather than our handling of a real one.
    auto make_side = [&](auto& price_dist, bool best_is_high, int order) {
        // Drawn ONCE. Inside the loop condition it would be re-drawn every
        // iteration, so the delta size would be whatever the last draw happened
        // to be rather than a chosen count.
        const size_t wanted = static_cast<size_t>(level_count(rng));
        std::vector<uint64_t> prices;
        while (prices.size() < wanted) {
            const uint64_t price = price_dist(rng);
            if (std::find(prices.begin(), prices.end(), price) == prices.end()) {
                prices.push_back(price);
            }
        }

        // order 0: BEST FIRST - what every venue actually sends, and the
        //          reverse of how FlatOrderBook stores it (the common branch).
        // order 1: WORST FIRST - already in storage order.
        // order 2: shuffled - the branch that must sort before merging.
        if (order == 0) {
            std::sort(prices.begin(), prices.end(),
                      [&](uint64_t a, uint64_t b) { return best_is_high ? a > b : a < b; });
        } else if (order == 1) {
            std::sort(prices.begin(), prices.end(),
                      [&](uint64_t a, uint64_t b) { return best_is_high ? a < b : a > b; });
        } else {
            std::shuffle(prices.begin(), prices.end(), rng);
        }

        std::vector<PriceLevel> levels;
        levels.reserve(prices.size());
        for (uint64_t price : prices) {
            levels.push_back({price, qty(rng)});
        }
        return levels;
    };

    for (int i = 0; i < 5000; ++i) {
        const int order = ordering(rng);
        const BookUpdate update =
            MakeUpdate(static_cast<uint64_t>(i + 1), false, make_side(bid_px, /*best_is_high=*/true, order),
                       make_side(ask_px, /*best_is_high=*/false, order));

        oracle.ApplyUpdate(update);
        flat.ApplyUpdate(update);

        ASSERT_NO_FATAL_FAILURE(ExpectSameBook(oracle, flat, i));

        // Bid and ask ranges cannot overlap by construction, so a cross here
        // means the book is corrupt, not that the market moved.
        if (auto bid = flat.BestBid(); bid && flat.BestAsk()) {
            ASSERT_LT(bid->first, flat.BestAsk()->first) << "flat book crossed at update " << i;
        }
    }
}

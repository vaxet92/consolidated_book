// Latency micro-benchmark for the order book hot path: per-venue apply, the
// k-way merge, and the BBO.
//
// Not a correctness test - the unit tests own that. This exists to produce
// the BEFORE numbers that CLAUDE.md section 7 requires before any of the
// optimisations in DESIGN_1 section 14.2 (flat-vector book, incremental
// merge) can be justified.
//
// KEY: a nanosecond figure here is NOT a decision on its own. It only becomes
// one when multiplied by the real message rate. At 60 updates/sec a 50us
// merge is 0.3% of a core and there is nothing to optimise; at 50k/sec the
// same number saturates it. The rate is measured separately - do not draw a
// conclusion from this file alone.
//
// The two comparisons this is built to produce:
//
//   merge_3venue - merge_1venue    what the k-way SELECTION costs
//   iterate_only / merge_3venue    what fraction is just walking red-black
//                                  trees, i.e. how much a flat vector could win
//
// Build guarded by BUILD_BENCHMARKS (default OFF).
//
// Usage:  bench_md_core [iterations]   (default 2000)

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "latency_benchmark.h"
#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "md_core/venue_book.h"

using namespace market_data;
using namespace market_data::consolidated;

namespace {

// --- fixture scale ---------------------------------------------------------
//
// Production scale, not toy numbers. Band accumulation runs in __int128
// precisely because price x qty at this scale overflows uint64, so a fixture
// with single-digit prices would exercise a different code path than the one
// we are trying to measure.

constexpr PriceTicks kMidPrice = 50'000 * kScaleFactor;  // 50k USDT
constexpr PriceTicks kTick = kScaleFactor / 100;         // 0.01 USDT

// What the aggregator ACTUALLY subscribes to at the default --depth=500,
// after SelectDepthTier rounds each venue to a tier it publishes:
//
//   Binance  500   exact tier
//   Bybit   1000   500 rounds UP to the next tier {1, 50, 200, 1000}
//   OKX      400   500 exceeds its deepest tier, so it is capped and warned
//
// Confirmed against a live run: "orderbook.1000.BTCUSDT" on the wire.
constexpr size_t kBinanceLevels = 1000;
constexpr size_t kBybitLevels = 1000;
constexpr size_t kOkxLevels = 400;

// KEY: this is NOT 1900 output levels. Every venue sits on the same tick grid
// anchored at the same mid, so they OVERLAP - the merged depth is the DEEPEST
// venue's depth, not the sum. Here that is Bybit's 1000, with three venues
// contributing to levels 1-400, two to 401-500, and one beyond that.
//
// That is the realistic shape and it also exercises the tie path heavily,
// which is the expensive one: a second scan across all venues per output
// level. A real book has gaps in its price grid, so contiguous ticks are
// slightly pessimistic on tie frequency - the right direction for a
// before-number.
constexpr size_t kRealisticMergedDepth = kBybitLevels;

// For the selection-isolation runs below, every venue gets the SAME depth, so
// the output level count is constant no matter how many venues take part and
// the only variable is how many venues the merge must choose between.
constexpr size_t kIsolationDepth = 1000;

// KEY: venues must OVERLAP in price. They quote the same asset, so near the
// top of book they land on the same ticks constantly, and the merge's
// tie-handling path - a second scan across all venues per output level - runs
// on almost every level. Disjoint fixtures would skip that path entirely and
// report a merge that is cheaper than the real one.
//
// Every venue is anchored at the same mid and walks the same tick grid, so
// overlap is total where their depths overlap. That is the pessimistic end of
// realistic, which is the right end for a before-number.

BookUpdate MakeSnapshot(VenueId venue, size_t levels) {
    BookUpdate update{};
    update.venue = venue;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = 1;
    update.is_snapshot = true;
    update.bids.reserve(levels);
    update.asks.reserve(levels);

    // Deterministic quantities - a fixed seed, so two runs are comparable.
    // Varying them matters: identical quantities would let the compiler and
    // the branch predictor do better here than they can in production.
    std::mt19937_64 rng(static_cast<uint64_t>(venue) * 7919 + 42);
    std::uniform_int_distribution<uint64_t> qty_dist(kScaleFactor / 100, 5 * kScaleFactor);

    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(i + 1) * kTick;
        update.bids.push_back({kMidPrice - offset, qty_dist(rng)});
        update.asks.push_back({kMidPrice + offset, qty_dist(rng)});
    }
    return update;
}

std::unique_ptr<VenueBook> MakeBook(VenueId venue, size_t levels) {
    auto book = std::make_unique<VenueBook>(venue, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    book->ApplyUpdate(MakeSnapshot(venue, levels));
    return book;
}

// Runtime-settable, because the right values are an empirical question and
// the defaults above are read off the code rather than off a live book. Run
// with different depths rather than arguing about the constants:
//
//   bench_md_core 2000 --binance=20 --bybit=1000 --okx=400
//
// This matters more than it looks. The whole point of the exercise is to find
// out whether std::map is the bottleneck, and that answer depends heavily on
// how many levels are in the book - a 20-level map fits in cache and a
// 1000-level one does not.
struct Fixture {
    size_t binance = kBinanceLevels;
    size_t bybit = kBybitLevels;
    size_t okx = kOkxLevels;
    size_t isolation = kIsolationDepth;
};

// Gives `venue_count` venues the SAME depth, so every variant produces the
// same number of OUTPUT levels and the only thing that changes is how many
// venues the merge compares per level.
//
// KEY: holding the OUTPUT constant is the correct control, not the total
// input. Because the venues share one tick grid they overlap, so three venues
// of 333 levels produce 333 output levels while one venue of 1000 produces
// 1000 - splitting a fixed total would change the output size and the
// comparison would measure that instead of the selection cost.
void FillIsolation(VenueBookArray& books, size_t venue_count, size_t depth) {
    for (auto& book : books) {
        book.reset();
    }
    static constexpr VenueId kOrder[] = {VenueId::BINANCE, VenueId::OKX, VenueId::BYBIT};
    for (size_t i = 0; i < venue_count; ++i) {
        books[static_cast<size_t>(kOrder[i])] = MakeBook(kOrder[i], depth);
    }
}

// The realistic shape: each venue at the depth it is actually subscribed to.
// Merged output is the DEEPEST venue's depth, not the sum, because they
// overlap - with the defaults that is Bybit's 1000, where levels 1-400 have
// three contributors, 401-500 have two, and the rest have one.
void FillRealistic(VenueBookArray& books, const Fixture& fixture) {
    for (auto& book : books) {
        book.reset();
    }
    books[static_cast<size_t>(VenueId::BINANCE)] = MakeBook(VenueId::BINANCE, fixture.binance);
    books[static_cast<size_t>(VenueId::OKX)] = MakeBook(VenueId::OKX, fixture.okx);
    books[static_cast<size_t>(VenueId::BYBIT)] = MakeBook(VenueId::BYBIT, fixture.bybit);
}

// Minimal --key=value parsing. Not worth a config library in a benchmark.
bool ParseSizeArg(const char* arg, const char* key, size_t& out) {
    const size_t key_len = std::strlen(key);
    if (std::strncmp(arg, key, key_len) != 0) {
        return false;
    }
    const long value = std::atol(arg + key_len);
    if (value > 0) {
        out = static_cast<size_t>(value);
    }
    return true;
}

// --- delta fixtures --------------------------------------------------------

// Rewrites quantities at prices that ALREADY exist. std::map does a lookup
// and an assignment: no node allocation, no rebalancing.
BookUpdate MakeQtyUpdate(size_t levels) {
    BookUpdate update{};
    update.venue = VenueId::BINANCE;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = 2;
    update.is_snapshot = false;
    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(i + 1) * kTick;
        update.bids.push_back({kMidPrice - offset, 3 * kScaleFactor});
        update.asks.push_back({kMidPrice + offset, 3 * kScaleFactor});
    }
    return update;
}

// KEY: the one that matters. Removes levels (qty == 0 -> erase) and adds new
// prices that were not in the book (-> insert, which allocates a node and
// rebalances the tree). std::map's real cost is node allocation, and a
// benchmark that only rewrote existing quantities would miss it entirely and
// make the map look better than it is.
//
// Erase and insert are paired in ONE call so the book returns to its starting
// size and the measurement is repeatable across iterations.
struct ChurnDeltas {
    BookUpdate remove;
    BookUpdate add;
};

ChurnDeltas MakeChurn(size_t levels) {
    ChurnDeltas churn;
    for (BookUpdate* update : {&churn.remove, &churn.add}) {
        update->venue = VenueId::BINANCE;
        update->instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
        update->seq = 3;
        update->is_snapshot = false;
    }
    for (size_t i = 0; i < levels; ++i) {
        // Deep in the book, away from the top, so the churn does not also
        // change the best price on every call - that would measure a
        // different thing.
        const PriceTicks offset = static_cast<PriceTicks>(500 + i) * kTick;
        churn.remove.bids.push_back({kMidPrice - offset, 0});
        churn.remove.asks.push_back({kMidPrice + offset, 0});
        churn.add.bids.push_back({kMidPrice - offset, 2 * kScaleFactor});
        churn.add.asks.push_back({kMidPrice + offset, 2 * kScaleFactor});
    }
    return churn;
}

// --- quote fixtures --------------------------------------------------------

BboQuote MakeQuote(VenueId venue, PriceTicks bid, PriceTicks ask) {
    BboQuote quote;
    quote.venue = venue;
    quote.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    quote.bid_price = bid;
    quote.bid_qty = kScaleFactor;
    quote.ask_price = ask;
    quote.ask_qty = kScaleFactor;
    return quote;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 2000;
    Fixture fixture;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (ParseSizeArg(arg, "--binance=", fixture.binance)) continue;
        if (ParseSizeArg(arg, "--bybit=", fixture.bybit)) continue;
        if (ParseSizeArg(arg, "--okx=", fixture.okx)) continue;
        if (ParseSizeArg(arg, "--isolation=", fixture.isolation)) continue;
        const long v = std::atol(arg);
        if (v > 0) iterations = static_cast<std::size_t>(v);
    }
    const std::size_t warmup = std::min<std::size_t>(1000, iterations);

    // Overlapping venues means merged depth is the DEEPEST venue, not the sum.
    const size_t merged_depth = std::max({fixture.binance, fixture.bybit, fixture.okx});

    std::printf("md_core order book latency  (iterations=%zu, warmup=%zu)\n", iterations, warmup);
    std::printf("fixture: binance=%zu bybit=%zu okx=%zu  ->  %zu merged levels (venues share one tick grid)\n",
                fixture.binance, fixture.bybit, fixture.okx, merged_depth);
    std::printf("prices x1e8 around %llu, tick %llu\n\n", static_cast<unsigned long long>(kMidPrice),
                static_cast<unsigned long long>(kTick));

    LatencyBenchmark bench(iterations, warmup);

    // ---- 1. per-venue apply ------------------------------------------------
    std::printf("VenueBook::ApplyUpdate\n");
    {
        auto book = MakeBook(VenueId::BINANCE, kBinanceLevels);

        const BookUpdate qty_small = MakeQtyUpdate(5);
        const BookUpdate qty_deep = MakeQtyUpdate(50);
        bench.Measure("qty_update_5", [&] {
            book->ApplyUpdate(qty_small);
            return book->last_seq();
        });
        bench.Measure("qty_update_50", [&] {
            book->ApplyUpdate(qty_deep);
            return book->last_seq();
        });

        // Erase then re-insert, so the book size is unchanged per call. The
        // reported cost is for BOTH halves together - 20 erases plus 20
        // inserts - not for one of them.
        const ChurnDeltas churn = MakeChurn(20);
        bench.Measure("churn_20x2", [&] {
            book->ApplyUpdate(churn.remove);
            book->ApplyUpdate(churn.add);
            return book->bids().size();
        });
    }

    // ---- 2. the merge, isolating the selection cost -------------------------
    //
    // Same total level count in all three, so the ONLY variable is how many
    // venues the merge has to choose between per output level.
    std::printf("\nMergeBooks - venue count at constant %zu OUTPUT levels\n", fixture.isolation);
    {
        Book merged;
        VenueBookArray books{};

        FillIsolation(books, 1, fixture.isolation);
        bench.Measure("merge_1venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });

        FillIsolation(books, 2, fixture.isolation);
        bench.Measure("merge_2venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });

        FillIsolation(books, 3, fixture.isolation);
        bench.Measure("merge_3venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });
    }

    // ---- 3. the realistic shape, and how cost scales with output depth ------
    std::printf("\nMergeBooks - realistic depths (Binance %zu / OKX %zu / Bybit %zu), by max_depth\n", fixture.binance,
                fixture.okx, fixture.bybit);
    {
        Book merged;
        VenueBookArray books{};
        FillRealistic(books, fixture);

        bench.Measure("merge_depth_50", [&] {
            MergeBooks(books, kVenueCount, merged, 50);
            return merged.bids.size();
        });
        bench.Measure("merge_depth_400", [&] {
            MergeBooks(books, kVenueCount, merged, 400);
            return merged.bids.size();
        });
        bench.Measure("merge_full", [&] {
            MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth);
            return merged.bids.size();
        });
    }

    // ---- 4. tree traversal alone -------------------------------------------
    //
    // KEY: the decisive measurement. Walks every level of all three books with
    // no merge logic, no tie handling and no prefix sums - just ++it and an
    // add. Whatever fraction this is of merge_full is the part a flat vector
    // could attack, because iterating a sorted vector is a linear scan over
    // contiguous memory while iterating a std::map is a pointer chase through
    // red-black tree nodes.
    //
    // If this dominates, the flat-vector book is the answer and incremental
    // merging (section 14.2 step 16b) is solving the wrong problem.
    std::printf("\nTree traversal alone (no merge logic)\n");
    {
        VenueBookArray books{};
        FillRealistic(books, fixture);

        bench.Measure("iterate_only", [&] {
            uint64_t sum = 0;
            for (const auto& book : books) {
                if (!book) continue;
                for (const auto& [price, qty] : book->bids()) {
                    sum += price + qty;
                }
                for (const auto& [price, qty] : book->asks()) {
                    sum += price + qty;
                }
            }
            return sum;
        });
    }

    // ---- 5. the BBO, expected to be negligible ------------------------------
    //
    // Included to CONFIRM that rather than assume it. The quote stream can be
    // faster than the depth stream, so "it is only three venues" is not by
    // itself an argument that it does not matter.
    std::printf("\nBBO\n");
    {
        VenueQuoteArray quotes{};
        quotes[static_cast<size_t>(VenueId::BINANCE)] =
            MakeQuote(VenueId::BINANCE, kMidPrice - kTick, kMidPrice + kTick);
        quotes[static_cast<size_t>(VenueId::OKX)] = MakeQuote(VenueId::OKX, kMidPrice - 2 * kTick, kMidPrice + kTick);
        quotes[static_cast<size_t>(VenueId::BYBIT)] =
            MakeQuote(VenueId::BYBIT, kMidPrice - kTick, kMidPrice + 2 * kTick);

        BBO bbo = ComputeBBOFromQuotes(quotes);
        bbo.best_bid.venues.reserve(kVenueCount);
        bbo.best_ask.venues.reserve(kVenueCount);

        // Alternates the incoming price so the update does not settle into one
        // branch - a quote that never changes the best level would only ever
        // exercise UpdateEqualPrice.
        uint64_t tick = 0;
        bench.Measure("bbo_incremental", [&] {
            const PriceTicks bid = kMidPrice - kTick - static_cast<PriceTicks>(tick++ % 3) * kTick;
            BboQuote quote = MakeQuote(VenueId::BINANCE, bid, kMidPrice + kTick);
            // slot == VenueId here: the fixture fills the quote array by index
            // rather than going through Core::RegisterVenue, so it picks that
            // layout deliberately (DESIGN.md §17.6).
            constexpr VenueSlot kBinanceSlot = static_cast<VenueSlot>(static_cast<size_t>(VenueId::BINANCE));
            quotes[VenueSlotIndex(kBinanceSlot)] = quote;
            UpdateBBOWithQuote(bbo, quote, kBinanceSlot, quotes);
            return bbo.best_bid.price;
        });

        bench.Measure("bbo_fullscan", [&] {
            BBO scanned = ComputeBBOFromQuotes(quotes);
            return scanned.best_bid.price + scanned.best_ask.price;
        });
    }

    std::printf(
        "\nNOTE: this loop keeps the tree nodes HOT in cache. In production there are\n"
        "milliseconds between merges and those nodes may be evicted, so these numbers\n"
        "UNDERSTATE std::map's disadvantage - the real gap is at least this wide.\n"
        "Multiply by the measured message rate before drawing any conclusion.\n");

    return 0;
}

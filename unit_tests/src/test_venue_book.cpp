#include <gtest/gtest.h>

#include "venue_book.h"

namespace {

BookUpdate MakeUpdate(uint64_t seq, bool is_snapshot, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{};
    update.venue = VenueId::BINANCE;
    update.instrument = InstrumentId::BTCUSDT;
    update.seq = seq;
    update.recv_ts_ns = 0;
    update.exch_ts_ns = 0;
    update.is_snapshot = is_snapshot;
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
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

TEST(VenueBookTest, LastSeqTracksMostRecentlyAppliedUpdate) {
    VenueBook book(VenueId::BINANCE, InstrumentId::BTCUSDT);

    book.ApplyUpdate(MakeUpdate(1, false, {{100, 1}}, {}));
    book.ApplyUpdate(MakeUpdate(2, false, {{100, 2}}, {}));

    EXPECT_EQ(book.last_seq(), 2u);
}

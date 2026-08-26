#include <gtest/gtest.h>

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

}  // namespace

TEST(ConsolidatedBboTest, EmptyBooksHaveNoConsolidatedBBO) {
    VenueBookArray books{};  // all null - no venue configured

    auto bbo = ComputeConsolidatedBBO(books);

    EXPECT_EQ(bbo.best_bid.total_qty, 0u);
    EXPECT_TRUE(bbo.best_bid.venues.empty());
    EXPECT_EQ(bbo.best_ask.total_qty, 0u);
    EXPECT_TRUE(bbo.best_ask.venues.empty());
    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, SingleVenueContributesBBO) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = ComputeConsolidatedBBO(books);

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

    auto bbo = ComputeConsolidatedBBO(books);

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

    auto bbo = ComputeConsolidatedBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 8u);  // 5 + 3
    ASSERT_EQ(bbo.best_bid.venues.size(), 2u);
}

TEST(ConsolidatedBboTest, LowestAskWinsAcrossVenues) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {}, {{105, 2}});
    SetBook(books, VenueId::OKX, {}, {{103, 4}});

    auto bbo = ComputeConsolidatedBBO(books);

    EXPECT_EQ(bbo.best_ask.price, 103u);
    EXPECT_EQ(bbo.best_ask.total_qty, 4u);
}

TEST(ConsolidatedBboTest, NonCrossedBookIsNotFlagged) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = ComputeConsolidatedBBO(books);

    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, CrossedBookSetsFlag) {
    VenueBookArray books{};
    SetBook(books, VenueId::BINANCE, {{105, 5}}, {});  // bid above the other venue's ask
    SetBook(books, VenueId::OKX, {}, {{100, 7}});

    auto bbo = ComputeConsolidatedBBO(books);

    EXPECT_TRUE(bbo.crossed);
}

TEST(ConsolidatedBboTest, UnconfiguredVenueSlotIsSkippedNotCrashed) {
    VenueBookArray books{};  // every slot null except one
    SetBook(books, VenueId::BYBIT, {{100, 5}}, {{101, 7}});

    auto bbo = ComputeConsolidatedBBO(books);

    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].venue, VenueId::BYBIT);
}

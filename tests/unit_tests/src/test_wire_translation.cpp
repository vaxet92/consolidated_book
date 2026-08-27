#include <gtest/gtest.h>

#include "aggregator/wire_translation.h"

using namespace market_data;

TEST(WireTranslationTest, VenueIdMapsToCorrectWireVenue) {
    EXPECT_EQ(ToWire(VenueId::BINANCE), wire::BINANCE);
    EXPECT_EQ(ToWire(VenueId::BYBIT), wire::BYBIT);
    EXPECT_EQ(ToWire(VenueId::OKX), wire::OKX);
}

TEST(WireTranslationTest, CountVenueMapsToUnspecified) {
    // Not a real venue - should never actually be passed in practice, but
    // must not silently pick a real venue if it ever is.
    EXPECT_EQ(ToWire(VenueId::COUNT), wire::VENUE_UNSPECIFIED);
}

TEST(WireTranslationTest, PriceLevelFieldsCarryOverExactly) {
    consolidated::ConsolidatedPriceLevel level;
    level.price = 7831010000000ull;
    level.total_qty = 9531387ull;
    level.venues.push_back({VenueId::BINANCE, 9531387ull});

    auto wire_level = ToWire(level);

    EXPECT_EQ(wire_level.price(), 7831010000000LL);
    EXPECT_EQ(wire_level.total_qty(), 9531387LL);
    ASSERT_EQ(wire_level.venues_size(), 1);
    EXPECT_EQ(wire_level.venues(0).venue(), wire::BINANCE);
    EXPECT_EQ(wire_level.venues(0).qty(), 9531387LL);
}

TEST(WireTranslationTest, BboCarriesCrossedFlagAndBothSides) {
    consolidated::BBO bbo;
    bbo.best_bid.price = 100;
    bbo.best_bid.total_qty = 5;
    bbo.best_ask.price = 99;
    bbo.best_ask.total_qty = 3;
    bbo.crossed = true;

    auto wire_bbo = ToWire(bbo);

    EXPECT_EQ(wire_bbo.best_bid().price(), 100LL);
    EXPECT_EQ(wire_bbo.best_ask().price(), 99LL);
    EXPECT_TRUE(wire_bbo.crossed());
}

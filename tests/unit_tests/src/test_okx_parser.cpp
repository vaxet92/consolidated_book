#include <gtest/gtest.h>

#include "md_provider/okx/okx_parser.h"

using namespace market_data;

namespace {

// Real OKX "books" channel message shape:
// {"arg","action","data":[{"asks","bids","ts","checksum","seqId"}]}
constexpr const char* kSnapshotMessage = R"({
    "arg": {"channel": "books", "instId": "BTC-USDT"},
    "action": "snapshot",
    "data": [
        {
            "asks": [["8476.98", "415", "0", "13"]],
            "bids": [["8476.97", "256", "0", "12"]],
            "ts": "1597026383085",
            "checksum": -855196043,
            "seqId": 123456
        }
    ]
})";

constexpr const char* kUpdateMessage = R"({
    "arg": {"channel": "books", "instId": "BTC-USDT"},
    "action": "update",
    "data": [
        {
            "asks": [],
            "bids": [["8476.97", "0", "0", "0"]],
            "ts": "1597026383200",
            "checksum": 123456789,
            "seqId": 123457
        }
    ]
})";

constexpr const char* kSubscribeAck = R"({"event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT"}})";

}  // namespace

TEST(OkxParserTest, ParsesSnapshotMessage) {
    auto update = ParseOkxBooksMessage(kSnapshotMessage, VenueId::OKX, InstrumentId::BTCUSDT);

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::OKX);
    EXPECT_EQ(update->instrument, InstrumentId::BTCUSDT);
    EXPECT_TRUE(update->is_snapshot);
    EXPECT_EQ(update->seq, 123456u);
    EXPECT_EQ(update->exch_ts_ns, 1597026383085LL * 1'000'000);

    // 4-element levels [price, qty, deprecated, numOrders] - only first two matter.
    ASSERT_EQ(update->asks.size(), 1u);
    EXPECT_EQ(update->asks[0].price, 847698000000ull);  // 8476.98 * 1e8
    EXPECT_EQ(update->asks[0].qty, 41500000000ull);      // 415 * 1e8

    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 847697000000ull);  // 8476.97 * 1e8
    EXPECT_EQ(update->bids[0].qty, 25600000000ull);      // 256 * 1e8
}

TEST(OkxParserTest, ParsesUpdateMessageAsNonSnapshot) {
    auto update = ParseOkxBooksMessage(kUpdateMessage, VenueId::OKX, InstrumentId::BTCUSDT);

    ASSERT_TRUE(update.has_value());
    EXPECT_FALSE(update->is_snapshot);
    EXPECT_EQ(update->seq, 123457u);
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].qty, 0u);  // qty "0" means "remove this level"
}

TEST(OkxParserTest, IgnoresNonBooksMessages) {
    auto update = ParseOkxBooksMessage(kSubscribeAck, VenueId::OKX, InstrumentId::BTCUSDT);
    EXPECT_FALSE(update.has_value());
}

TEST(OkxParserTest, MalformedJsonReturnsNulloptNotACrash) {
    auto update = ParseOkxBooksMessage("{not valid json", VenueId::OKX, InstrumentId::BTCUSDT);
    EXPECT_FALSE(update.has_value());
}

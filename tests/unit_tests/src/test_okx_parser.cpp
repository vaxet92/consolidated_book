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

// Real bbo-tbt shape: no "action" and no "checksum", unlike the books channel.
constexpr const char* kBboMessage = R"({
    "arg": {"channel": "bbo-tbt", "instId": "BTC-USDT"},
    "data": [
        {
            "asks": [["8506.96", "100", "0", "2"]],
            "bids": [["8446.00", "95", "0", "3"]],
            "ts": "1597026383085",
            "seqId": 363996337
        }
    ]
})";

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
    EXPECT_EQ(update->asks[0].qty, 41500000000ull);     // 415 * 1e8

    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 847697000000ull);  // 8476.97 * 1e8
    EXPECT_EQ(update->bids[0].qty, 25600000000ull);     // 256 * 1e8
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

TEST(OkxParserTest, ParsesBboTbtMessage) {
    auto update = ParseOkxBboMessage(kBboMessage, VenueId::OKX, InstrumentId::BTCUSDT);

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::OKX);
    EXPECT_TRUE(update->is_snapshot);  // bbo-tbt is always a full replacement
    EXPECT_EQ(update->seq, 363996337u);
    EXPECT_EQ(update->exch_ts_ns, 1597026383085LL * 1'000'000);

    ASSERT_EQ(update->asks.size(), 1u);
    EXPECT_EQ(update->asks[0].price, 850696000000ull);  // 8506.96 * 1e8
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 844600000000ull);  // 8446.00 * 1e8
}

TEST(OkxParserTest, BboIgnoresSubscribeAck) {
    auto update = ParseOkxBboMessage(kSubscribeAck, VenueId::OKX, InstrumentId::BTCUSDT);
    EXPECT_FALSE(update.has_value());
}

// The regression this parser split exists for: a shared parser probing for
// an absent `action` field left simdjson's lazy iterator at a broken depth,
// and the next lookup asserted instead of returning an error.
TEST(OkxParserTest, BboMalformedJsonReturnsNulloptNotACrash) {
    auto update = ParseOkxBboMessage("{not valid json", VenueId::OKX, InstrumentId::BTCUSDT);
    EXPECT_FALSE(update.has_value());
}

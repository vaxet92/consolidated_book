#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "aggregator/conflated_channel.h"

using namespace market_data;

TEST(ConflatedChannelTest, PushThenWaitAndTakeReturnsValue) {
    ConflatedChannel<int> channel;
    channel.Push(5);

    auto value = channel.WaitAndTake();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 5);
}

TEST(ConflatedChannelTest, WaitAndTakeBlocksUntilPush) {
    ConflatedChannel<int> channel;
    std::optional<int> received;

    std::thread consumer([&] { received = channel.WaitAndTake(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the consumer start waiting
    channel.Push(42);
    consumer.join();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 42);
}

TEST(ConflatedChannelTest, OverwriteSemanticsOnlyLatestValueSurvives) {
    ConflatedChannel<int> channel;
    channel.Push(1);
    channel.Push(2);  // overwrites 1 - it was never read

    auto value = channel.WaitAndTake();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 2);
}

TEST(ConflatedChannelTest, CloseUnblocksWaitingConsumerWithNullopt) {
    ConflatedChannel<int> channel;
    std::optional<int> received = 999;  // sentinel, should be overwritten to nullopt

    std::thread consumer([&] { received = channel.WaitAndTake(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.Close();
    consumer.join();

    EXPECT_FALSE(received.has_value());
}

TEST(ConflatedChannelTest, ClosePreservesAlreadyPendingValue) {
    ConflatedChannel<int> channel;
    channel.Push(7);
    channel.Close();  // a value was already waiting - Close() doesn't discard it

    auto value = channel.WaitAndTake();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 7);
}

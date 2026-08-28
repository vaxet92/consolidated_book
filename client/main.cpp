#include "aggregator.grpc.pb.h"
#include "logger/logger.h"

#include <grpcpp/grpcpp.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace market_data;

namespace {

constexpr const char* kServerAddress = "localhost:50051";
constexpr int kMaxBackoffSeconds = 8;

// Pure integer formatting - no floating point, even at display time.
// 7831010000000 at scale=8 becomes "78310.10000000".
std::string FormatScaled(int64_t value, uint32_t scale) {
    int64_t divisor = 1;
    for (uint32_t i = 0; i < scale; ++i) {
        divisor *= 10;
    }
    int64_t integer_part = value / divisor;
    int64_t frac_part = value % divisor;
    if (frac_part < 0) {
        frac_part = -frac_part;
    }
    return fmt::format("{}.{:0{}}", integer_part, frac_part, scale);
}

// Per-venue attribution (DESIGN_1 §5.3) - e.g. "BINANCE:0.00531,OKX:0.00200"
std::string FormatVenues(const wire::ConsolidatedPriceLevel& level, uint32_t qty_scale) {
    std::string out;
    for (int i = 0; i < level.venues_size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += fmt::format("{}:{}", wire::Venue_Name(level.venues(i).venue()),
                           FormatScaled(level.venues(i).qty(), qty_scale));
    }
    return out;
}

}  // namespace

int main() {
    int backoff_seconds = 1;

    while (true) {
        Logger::Log(LogLevel::kInfo, "[client_bbo] connecting to {}", kServerAddress);

        auto channel = grpc::CreateChannel(kServerAddress, grpc::InsecureChannelCredentials());
        auto stub = wire::Aggregator::NewStub(channel);

        wire::SubscribeRequest request;
        request.set_symbol("BTCUSDT");
        request.add_feeds(wire::BBO);

        // A ClientContext is single-use - it must be recreated per attempt.
        grpc::ClientContext context;
        std::unique_ptr<grpc::ClientReader<wire::Update>> reader(stub->Subscribe(&context, request));

        wire::Update update;
        std::optional<uint64_t> last_seq;

        while (reader->Read(&update)) {
            backoff_seconds = 1;  // healthy stream - reset the backoff

            if (last_seq && update.seq() != *last_seq + 1) {
                if (update.seq() > *last_seq) {
                    // Client-side gap detection (§9.3): a skipped seq means the
                    // server's depth-1 conflation (§7.4) overwrote a pending
                    // update before we read it. Expected under load, not an
                    // error - but the contract says say so, out loud, on stderr.
                    fmt::print(stderr, "[gap] expected seq {}, got {} ({} update(s) conflated away)\n", *last_seq + 1,
                               update.seq(), update.seq() - *last_seq - 1);
                } else {
                    // Not conflation - seq went backwards. Means the server
                    // restarted (its counter resets to 0) or published out of
                    // order. Subtracting here would underflow on uint64_t.
                    fmt::print(stderr, "[seq-reset] seq went backwards: {} -> {}\n", *last_seq, update.seq());
                }
            }
            last_seq = update.seq();

            if (update.payload_case() != wire::Update::kBbo) {
                continue;  // we only subscribed to BBO
            }
            const auto& bbo = update.bbo();

            // if (!bbo.crossed()) {
            fmt::print("seq={} {} bid {} : {} [{}] | ask {} : {} [{}]{}\n", update.seq(), update.symbol(),

                       FormatScaled(bbo.best_bid().price(), update.price_scale()),
                       FormatScaled(bbo.best_bid().total_qty(), update.qty_scale()),
                       FormatVenues(bbo.best_bid(), update.qty_scale()),
                       FormatScaled(bbo.best_ask().price(), update.price_scale()),
                       FormatScaled(bbo.best_ask().total_qty(), update.qty_scale()),
                       FormatVenues(bbo.best_ask(), update.qty_scale()), bbo.crossed() ? " CROSSED" : "");
            // }
        }

        grpc::Status status = reader->Finish();
        if (!status.ok()) {
            fmt::print(stderr, "[client_bbo] stream ended: {}\n", status.error_message());
        }

        // A rejected subscription won't succeed on retry - don't spin on it.
        if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
            status.error_code() == grpc::StatusCode::INVALID_ARGUMENT) {
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
        backoff_seconds = std::min(backoff_seconds * 2, kMaxBackoffSeconds);
    }
}

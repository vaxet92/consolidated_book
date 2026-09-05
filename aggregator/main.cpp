#include "aggregator_service.h"
#include "config/config.h"
#include "latency_recorder.h"
#include "md_core/md_core.h"
#include "md_provider/binance/binance_provider.h"
#include "md_provider/bybit/bybit_provider.h"
#include "md_provider/okx/okx_provider.h"
#include "logger/logger.h"
#include "types/venue.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

using namespace market_data;

namespace {

// Resolves the requested depth to a tier this venue actually publishes, and
// says so when it cannot reach what was asked for - OKX tops out at 400
// without VIP4, so a request above that is silently under-delivered unless
// it is reported.
uint32_t ResolveDepth(VenueId venue, uint32_t desired) {
    uint32_t tier = 0;
    switch (venue) {
        case VenueId::BINANCE:
            tier = SelectDepthTier(kBinanceDepthTiers, desired);
            break;
        case VenueId::BYBIT:
            tier = SelectDepthTier(kBybitDepthTiers, desired);
            break;
        case VenueId::OKX:
            tier = SelectDepthTier(kOkxDepthTiers, desired);
            break;
        case VenueId::COUNT:
            tier = desired;
            break;
    }

    if (tier < desired) {
        Logger::Log(LogLevel::kWarning, "[{}] requested depth {} exceeds this venue's deepest tier - using {}",
                    VenueConverter::ToVenueString(venue), desired, tier);
    } else if (tier > desired) {
        Logger::Log(LogLevel::kInfo, "[{}] depth {} rounded up to venue tier {}", VenueConverter::ToVenueString(venue),
                    desired, tier);
    }
    return tier;
}

}  // namespace

int main(int argc, char* argv[]) {
    ServerConfig server_config = ServerConfig::ParseFromArgs(argc, argv);
    if (!server_config.Validate()) {
        return 2;
    }

    // connections is worth logging: at N it opens N x venues x 2 sockets, so
    // the number needs to be visible when a venue starts refusing us.
    Logger::Log(LogLevel::kInfo, "[Aggregator] starting (depth={}, connections={}, grpc_port={})", server_config.depth,
                server_config.connections, server_config.grpc_port);

    AggregatorServiceImpl service;

    // Core has no knowledge of gRPC - these lambdas are the only thing that
    // connects the two, matching the same seam-style callback used
    // everywhere else in this project (Provider::CallBack, Core::BboCallback).
    //
    // BBO has no per-client parameterization, so Core computes it fully and
    // the service just relays it. The merged Book DOES have per-client
    // parameterization (§8.4's bands) - Core hands over the shared snapshot
    // and decides nothing about bands; PublishVolumeBands/PublishPriceBands
    // (not built yet) are where per-subscriber band math will happen.
    // BEFORE-number for DESIGN_1 §14.2 step 12 (per-venue SPSC queues).
    // Measures provider-stamp to book-published, which is the only span that
    // means the same thing under both the current mutex handoff and the queues
    // that will replace it. See latency_recorder.h for why.
    //
    // Safe unguarded: PublishBook now runs on the single consolidator thread,
    // so this recorder is only ever touched by one thread. It used to be safe
    // for a different reason - Core::apply_mutex_ serialised the provider
    // threads - and that mutex is gone (§7.2).
    LatencyRecorder publish_latency("book_publish", /*report_every=*/1000, /*warmup=*/200);

    Core core(
        [&service](InstrumentId instrument, const consolidated::BBO& bbo) { service.PublishBbo(instrument, bbo); },
        [&service, &publish_latency](InstrumentId instrument, std::shared_ptr<const consolidated::Book> book) {
            publish_latency.Record(book->source_mono_ns, book->venue_levels);
            service.PublishBook(instrument, std::move(book));
        });

    // --venues= selects which exchanges to run. ParseFromArgs already fills
    // in all three when the flag is absent, so "empty" here can only mean the
    // operator named venues we do not recognise - worth refusing rather than
    // silently starting an aggregator with no data source.
    auto venue_enabled = [&server_config](std::string_view name) {
        return std::find(server_config.venues.begin(), server_config.venues.end(), name) != server_config.venues.end();
    };

    std::vector<VenueId> enabled_venues;
    if (venue_enabled("binance")) enabled_venues.push_back(VenueId::BINANCE);
    if (venue_enabled("bybit")) enabled_venues.push_back(VenueId::BYBIT);
    if (venue_enabled("okx")) enabled_venues.push_back(VenueId::OKX);

    if (enabled_venues.empty()) {
        Logger::Log(LogLevel::kError, "[Aggregator] no recognised venue in --venues (expected binance, bybit, okx),");
        return 2;
    }

    // Venue registration - the in-process stand-in for the kHello handshake
    // (DESIGN.md §17.4). Core registers no venues from config: a venue exists
    // because a provider exists, so the wiring layer that creates the
    // providers is what tells Core about them.
    //
    // KEY: this must run BEFORE Init(). AddInstrument creates a VenueBook for
    // each venue active AT THAT MOMENT, so registering afterwards would leave
    // BTCUSDT with an all-null book array and every update rejected as
    // "unconfigured venue" - silently, with the process otherwise healthy.
    //
    // Slot per venue, kept because every provider's callbacks bind to their
    // own slot below. Indexed by VenueId purely as a local lookup while
    // wiring - Core itself never indexes by VenueId again.
    std::array<std::optional<VenueSlot>, kVenueCount> slot_for_venue{};
    for (VenueId venue : enabled_venues) {
        const std::string venue_name = VenueConverter::ToVenueString(venue);
        const std::optional<VenueSlot> slot = core.RegisterVenue(venue_name);
        if (!slot.has_value()) {
            Logger::Log(LogLevel::kError, "[Aggregator] failed to register venue {} - refusing to start", venue_name);
            return 2;
        }
        slot_for_venue[static_cast<size_t>(venue)] = *slot;
    }

    // md_core attributes every level by SLOT and knows no venue names
    // (DESIGN.md §17.6). This table is what turns a slot back into a venue on
    // the wire, and it is built once here - after every registration above,
    // before the server starts - rather than per level.
    //
    // KEY: forgetting this call does not crash and does not drop data. Every
    // level would publish VENUE_UNSPECIFIED, which is a quiet loss of
    // attribution rather than a loud failure. The default is deliberately
    // UNSPECIFIED rather than a real venue, so the failure mode is "no
    // attribution" and never "wrong exchange".
    service.SetVenueWireTable(
        MakeVenueWireTable([&core](VenueSlot slot) { return core.VenueName(slot); }));

    CoreConfig config = {
        .venues = enabled_venues,
        .default_instruments = {InstrumentId::BTCUSDT},
    };
    core.Init(config);

    // Splits ApplyUpdate's cost into lock wait / book apply / merge, so the
    // live latency can be attributed rather than guessed at. Core is handed
    // the clock rather than reading one, which is what keeps md_core free of
    // I/O and of any clock at all.
    TimingBreakdown timing_breakdown(/*report_every=*/1000, /*warmup=*/200);
    core.SetInstrumentation(&LatencyRecorder::NowMonotonicNs,
                            [&timing_breakdown](const Core::ApplyTimings& timings) {
                                timing_breakdown.Record(timings.lock_wait_ns, timings.book_apply_ns, timings.merge_ns,
                                                        timings.merged_depth, timings.delta_levels);
                            });

    // Each provider gets callbacks bound to ITS OWN slot, resolved once above
    // when the venue registered and fixed for the life of the connection
    // (§17.4 - accept is registration).
    //
    // KEY: the slot is captured, not looked up per message. Core no longer
    // translates VenueId -> slot on the hot path, which removes both an array
    // read per message and the race that translation would have had against
    // RegisterVenue once the mutex is gone.
    //
    // KEY: these ENQUEUE, they do not apply. The work happens later, on the
    // consolidator thread. A false return means the queue stayed full for the
    // whole bounded window - the venue's diff chain is broken and only the
    // provider can fix it, by resyncing. Ignoring it would leave Core applying
    // deltas to a book that has silently lost one.
    auto make_update_sink = [&core](VenueSlot slot, VenueId venue) {
        return [&core, slot, venue](BookUpdate&& update) {
            if (!core.EnqueueUpdate(slot, std::move(update))) {
                Logger::Log(LogLevel::kError, "[{}] core queue full - depth update dropped, resync required",
                            VenueConverter::ToVenueString(venue));
                // TODO: call provider->RequestResync() here. The provider owns
                // resync (§4.2/§9) but is not reachable from this lambda yet -
                // wiring it needs the provider to exist before its callbacks,
                // which is the next step, not this one.
            }
        };
    };
    auto make_quote_sink = [&core](VenueSlot slot) {
        // No return value: a dropped quote is safe. A quote is a complete
        // top-of-book snapshot, so the next one supersedes it whole.
        return [&core, slot](const BboQuote& quote) { core.EnqueueQuote(slot, quote); };
    };
    auto make_health_sink = [&core](VenueSlot slot, VenueId venue) {
        return [&core, slot, venue](const VenueHealthEvent& event) {
            if (!core.EnqueueHealth(slot, event)) {
                Logger::Log(LogLevel::kError, "[{}] core queue full - health event dropped, resync required",
                            VenueConverter::ToVenueString(venue));
            }
        };
    };

    std::vector<std::unique_ptr<Provider>> providers;
    // Parallel to `providers`, so the health callback below can bind each
    // provider to the slot it registered under.
    std::vector<VenueSlot> provider_slots;

    if (venue_enabled("binance")) {
        ProviderConfig binance_config = {
            .venue_id = VenueId::BINANCE,
            .instrument = InstrumentId::BTCUSDT,
            .host = std::string(kBinanceHost),
            .port = std::string(kBinancePort),
            .depth = ResolveDepth(VenueId::BINANCE, server_config.depth),
            // Same count for every venue. Per-venue tuning may eventually be
            // needed - OKX rate-limits connection ATTEMPTS more tightly than the
            // others - but no venue's limits have been verified, so three
            // unverified numbers would be guessing where one is defensible.
            .connections = server_config.connections,
            // Binance publishes no keepalive on either stream, so these are
            // placeholders, not derived values - see types/venue.h.
            .depth_backstop_ns = staleness::kBinanceDepthBackstopNs,
            .bbo_backstop_ns = staleness::kBinanceBboBackstopNs,
        };
        const VenueSlot slot = *slot_for_venue[static_cast<size_t>(VenueId::BINANCE)];
        providers.push_back(std::make_unique<BinanceProvider>(binance_config, make_update_sink(slot, VenueId::BINANCE),
                                                  make_quote_sink(slot)));
        provider_slots.push_back(slot);
    }

    if (venue_enabled("bybit")) {
        ProviderConfig bybit_config = {
            .venue_id = VenueId::BYBIT,
            .instrument = InstrumentId::BTCUSDT,
            .host = std::string(kBybitHost),
            .port = std::string(kBybitPort),
            .depth = ResolveDepth(VenueId::BYBIT, server_config.depth),
            .connections = server_config.connections,
            .depth_backstop_ns = staleness::kBybitDepthBackstopNs,
            // The only tight, derived backstop we have: Bybit republishes L1
            // with the same `u` every 3s of no change.
            .bbo_backstop_ns = staleness::kBybitBboBackstopNs,
        };
        const VenueSlot slot = *slot_for_venue[static_cast<size_t>(VenueId::BYBIT)];
        providers.push_back(std::make_unique<BybitProvider>(bybit_config, make_update_sink(slot, VenueId::BYBIT),
                                                  make_quote_sink(slot)));
        provider_slots.push_back(slot);
    }

    if (venue_enabled("okx")) {
        ProviderConfig okx_config = {
            .venue_id = VenueId::OKX,
            .instrument = InstrumentId::BTCUSDT,
            .host = std::string(kOkxHost),
            .port = std::string(kOkxPort),
            .depth = ResolveDepth(VenueId::OKX, server_config.depth),
            .connections = server_config.connections,
            // Derived from OKX's ~60s seqId == prevSeqId keepalive.
            .depth_backstop_ns = staleness::kOkxDepthBackstopNs,
            .bbo_backstop_ns = staleness::kOkxBboBackstopNs,
        };
        const VenueSlot slot = *slot_for_venue[static_cast<size_t>(VenueId::OKX)];
        providers.push_back(std::make_unique<OKXProvider>(okx_config, make_update_sink(slot, VenueId::OKX),
                                                  make_quote_sink(slot)));
        provider_slots.push_back(slot);
    }

    // The health seam (DESIGN_1 §6.5). Each provider decides the verdict for
    // its own two streams and pushes it here; Core stores it and the next
    // merge honours it. Set before Start(), because the watchdog arms as soon
    // as the provider's io_context runs.
    //
    // Same shape as on_update/on_quote above: main.cpp is the only place that
    // knows about both sides, which is what keeps Core free of any knowledge
    // of providers, sockets or threads.
    for (size_t i = 0; i < providers.size(); ++i) {
        providers[i]->SetHealthCallback(make_health_sink(provider_slots[i], enabled_venues[i]));
    }

    // The consolidator thread starts HERE: after every venue is registered and
    // every provider is wired, before any provider opens a socket.
    //
    // KEY: registration must be complete first. RegisterVenue mutates state the
    // consolidator reads with no lock, which is safe only while no consolidator
    // is running (see Core::Start).
    core.Start();

    for (auto& provider : providers) {
        provider->Start();
    }

    std::string server_address = fmt::format("0.0.0.0:{}", server_config.grpc_port);
    grpc::ServerBuilder builder;
    // Insecure credentials: authentication/TLS on the gRPC hop is explicitly
    // out of scope (DESIGN_1 §1.2), not an oversight.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    Logger::Log(LogLevel::kInfo, "[Aggregator] gRPC server listening on {}", server_address);

    // TODO: no signal handling yet - the process only stops on an external
    // kill, which skips provider->Stop() below. Graceful shutdown (SIGINT/
    // SIGTERM -> server->Shutdown() + provider->Stop() for each) is
    // hardening work (DESIGN_1 §14 step 9), not done yet.
    server->Wait();

    for (auto& provider : providers) {
        provider->Stop();
    }

    // After the providers, so nothing is still enqueueing when the
    // consolidator drains for the last time.
    core.Stop();

    return 0;
}

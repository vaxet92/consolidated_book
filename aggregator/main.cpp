#include "aggregator_service.h"
#include "md_core/md_core.h"
#include "md_provider/binance/binance_provider.h"
#include "md_provider/bybit/bybit_provider.h"
#include "md_provider/okx/okx_provider.h"
#include "logger/logger.h"
#include "types/venue.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <vector>

using namespace market_data;

int main() {
    Logger::Log(LogLevel::kInfo, "[Aggregator] starting");

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
    Core core(
        [&service](InstrumentId instrument, const consolidated::BBO& bbo) { service.PublishBbo(instrument, bbo); },
        [&service](InstrumentId instrument, std::shared_ptr<const consolidated::Book> book) {
            service.PublishBook(instrument, std::move(book));
        });

    CoreConfig config = {
        .venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX},
        .default_instruments = {InstrumentId::BTCUSDT},
    };
    core.Init(config);

    auto on_update = [&core](const BookUpdate& update) { core.ApplyUpdate(update); };
    auto on_quote = [&core](const BboQuote& quote) { core.ApplyQuote(quote); };

    std::vector<std::unique_ptr<Provider>> providers;

    ProviderConfig binance_config = {
        .venue_id = VenueId::BINANCE,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kBinanceHost),
        .port = std::string(kBinancePort),
    };
    providers.push_back(std::make_unique<BinanceProvider>(binance_config, on_update, on_quote));

    ProviderConfig bybit_config = {
        .venue_id = VenueId::BYBIT,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kBybitHost),
        .port = std::string(kBybitPort),
    };
    providers.push_back(std::make_unique<BybitProvider>(bybit_config, on_update, on_quote));

    ProviderConfig okx_config = {
        .venue_id = VenueId::OKX,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kOkxHost),
        .port = std::string(kOkxPort),
    };
    providers.push_back(std::make_unique<OKXProvider>(okx_config, on_update, on_quote));

    for (auto& provider : providers) {
        provider->Start();
    }

    std::string server_address = "0.0.0.0:50051";
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

    return 0;
}

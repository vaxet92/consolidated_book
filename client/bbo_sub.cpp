#include "client_common.h"

using namespace market_data;

// Fixed-subscription test binary: BBO only, no flags. client_app is the
// production client - this exists to exercise one feed in isolation.
int main() {
    ClientConfig config;
    config.want_bbo = true;

    return RunSubscription(config, "bbo_sub", config.ToRequest(), PrintBbo);
}

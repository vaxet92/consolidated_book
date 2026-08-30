#include "client_common.h"

using namespace market_data;

// Fixed-subscription test binary: volume bands only, server defaults, no
// flags. client_app is the production client - this exists to exercise one
// feed in isolation.
int main() {
    ClientConfig config;
    config.want_volume_bands = true;  // empty notional_bands = server defaults

    return RunSubscription(config, "volume_band_sub", config.ToRequest(), PrintVolumeBands);
}

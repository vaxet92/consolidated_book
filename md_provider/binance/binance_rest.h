#pragma once

#include <optional>
#include <string>

namespace market_data {

// One-shot synchronous HTTPS GET. Returns the response body, or
// std::nullopt on any failure (DNS, TLS, non-200 status, malformed
// response). Never throws.
//
// Blocking BY DESIGN, but it MUST NOT be called from a Provider's
// io_context thread: that thread drives the WebSocket reads, and blocking
// it for an HTTP round-trip stalls exactly the event buffering the snapshot
// sync depends on. Call it from a separate thread and hand the result back
// with net::post(ioc, ...).
std::optional<std::string> HttpsGet(const std::string& host, const std::string& port, const std::string& target);

}  // namespace market_data

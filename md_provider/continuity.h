#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "md_core/types.h"

namespace market_data {

// What a venue's sequencing rules say to do with an incoming depth message.
enum class ContinuityAction {
    kApply,   // in-sequence delta - apply it
    kReset,   // snapshot - clear the book, then apply
    kIgnore,  // keep-alive / no-change - nothing to do, NOT a gap
    kGap,     // sequence broken: the book is wrong, resync (§4.2)
};

// Bybit orderbook.50 - `u` increments by exactly 1 per delta.
//   snapshot        -> kReset
//   u == last_u     -> kIgnore
//   u == last_u + 1 -> kApply
//   otherwise       -> kGap
// Updates last_u on kReset/kApply. Bybit's u == 1 service-restart case is
// normalised into is_snapshot by the parser, so it needs no rule here.
ContinuityAction CheckBybitContinuity(const BookUpdate& update, uint64_t& last_u);

// OKX books - chained by prevSeqId == previous seqId. seqIds are NOT
// contiguous, so Bybit's "+1" rule would be wrong here.
//   snapshot (prevSeqId == -1) -> kReset
//   prevSeqId != last_seq      -> kGap
//   seqId == prevSeqId         -> kIgnore  (~60s keep-alive, empty asks/bids)
//   otherwise                  -> kApply   (incl. seqId < prevSeqId, which is
//                                 a documented maintenance reset, not a gap)
// Updates last_seq on kReset/kApply.
ContinuityAction CheckOkxContinuity(const BookUpdate& update, uint64_t& last_seq);

// Binance depthUpdate, once live - U == last_u + 1.
// Binance never sends a snapshot on the stream, so kReset/kIgnore never
// occur here; the book is seeded from REST instead (see below).
// Updates last_u on kApply.
ContinuityAction CheckBinanceContinuity(const BookUpdate& update, uint64_t& last_u);

// Binance snapshot reconciliation (§4.2). Given the REST snapshot's
// lastUpdateId and the events buffered while it was in flight, returns the
// index of the first event to apply, or nullopt if the snapshot is too old
// to join onto the buffer (caller refetches a newer one).
//
// Events with u <= lastUpdateId are already contained in the snapshot; the
// first survivor must satisfy U <= lastUpdateId + 1 <= u.
std::optional<size_t> ReconcileBinanceSnapshot(uint64_t last_update_id, const std::vector<BookUpdate>& pending);

}  // namespace market_data

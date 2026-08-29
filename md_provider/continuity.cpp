#include "continuity.h"

namespace market_data {

ContinuityAction CheckBybitContinuity(const BookUpdate& update, uint64_t& last_u) {
    // Includes Bybit's u == 1 service-restart case - the parser normalises
    // that into is_snapshot, so nothing venue-specific is needed here.
    if (update.is_snapshot) {
        last_u = update.seq;
        return ContinuityAction::kReset;
    }
    if (update.seq == last_u) {
        // Republished with no change. Documented for level 1 on linear/
        // inverse perps; shouldn't occur on spot orderbook.50, but it is
        // explicitly not a gap.
        return ContinuityAction::kIgnore;
    }
    if (update.seq == last_u + 1) {
        last_u = update.seq;
        return ContinuityAction::kApply;
    }
    return ContinuityAction::kGap;
}

ContinuityAction CheckOkxContinuity(const BookUpdate& update, uint64_t& last_seq) {
    if (update.is_snapshot) {
        last_seq = update.seq;  // snapshot carries prevSeqId == -1
        return ContinuityAction::kReset;
    }
    if (update.prev_seq < 0 || static_cast<uint64_t>(update.prev_seq) != last_seq) {
        return ContinuityAction::kGap;
    }
    if (update.seq == static_cast<uint64_t>(update.prev_seq)) {
        // ~60s keep-alive: empty asks/bids, seqId == prevSeqId. Chain is
        // intact, nothing to apply.
        return ContinuityAction::kIgnore;
    }
    // Normal update. seqId may be SMALLER than prevSeqId after an OKX
    // maintenance sequence reset - documented as normal, and the chain still
    // holds, so it must NOT be treated as a gap.
    last_seq = update.seq;
    return ContinuityAction::kApply;
}

ContinuityAction CheckBinanceContinuity(const BookUpdate& update, uint64_t& last_u) {
    if (static_cast<uint64_t>(update.prev_seq) != last_u + 1) {
        return ContinuityAction::kGap;
    }
    last_u = update.seq;
    return ContinuityAction::kApply;
}

std::optional<size_t> ReconcileBinanceSnapshot(uint64_t last_update_id, const std::vector<BookUpdate>& pending) {
    // Everything with u <= lastUpdateId is already inside the snapshot.
    size_t first = 0;
    while (first < pending.size() && pending[first].seq <= last_update_id) {
        ++first;
    }

    // No events left to apply: the snapshot is simply newer than everything
    // buffered. Valid - apply the snapshot alone.
    if (first == pending.size()) {
        return first;
    }

    // The first survivor must straddle the snapshot: U <= lastUpdateId + 1.
    // If it starts after that, an update was missed between the snapshot and
    // the buffer, and this snapshot cannot be joined onto it.
    if (static_cast<uint64_t>(pending[first].prev_seq) > last_update_id + 1) {
        return std::nullopt;
    }
    return first;
}

}  // namespace market_data

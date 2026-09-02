# Consolidated Order Book Aggregator — Design Document

**Assignment:** Aggregate BTCUSDT market data from 3 CEXs into one consolidated book; expose gRPC streaming endpoints; three client services publishing BBO, notional volume bands, and bps price bands to stdout. C++, Dockerized, docker-compose, README.

**Decisions frozen for this document:** Binance + OKX + Bybit, **spot** BTCUSDT. C++20, Boost.Beast/Asio + OpenSSL for WSS, gRPC + Protobuf, CMake + vcpkg, simdjson, GoogleTest, a small `fmt`-based `Logger` (spdlog rejected — see §2). Optimization scope: *measured and moderate* — flat structures, seqlock, conflated fan-out, benchmarked; the tick-indexed ladder is designed here and documented as future work rather than built.

---

## 1. Scope

### 1.1 In scope

| Area | Deliverable |
|---|---|
| Market data ingest | 3 venue adapters, snapshot+delta sync, gap detection, resync, reconnect |
| Consolidated book | Per-venue books + lazy k-way merge with per-venue attribution |
| Staleness policy | Watchdog, drift detection, venue admission/exclusion, status published to clients |
| gRPC API | Single extensible `Subscribe` streaming RPC, conflated fan-out |
| Derived views | Consolidated BBO; VWAP-to-notional bands; cumulative-depth-within-bps bands |
| Clients | 3 client binaries (BBO / volume bands / price bands) → stdout |
| Packaging | Multi-stage Dockerfile per service, docker-compose for all 4 |
| Docs | README: build, run, technical decisions, known limitations |
| Tests | Unit + property + integration, driven by a deterministic replay provider |

### 1.2 Explicitly out of scope (state in README)

Multi-symbol (designed for, not exercised) · order entry / execution · persistence of market data beyond replay capture files · authentication/TLS on the gRPC hop · cross-venue fee, withdrawal, or inventory modelling · kernel bypass, busy-poll, CPU pinning, hugepages.

### 1.3 The one instrument-level trap

BTCUSDT **spot** and BTCUSDT **perpetual** are different instruments with different prices. Mixing them into one book is a correctness bug, not a rounding artifact. All three adapters use spot. This is asserted in config validation, not just documented.

---

## 2. Component architecture

```
                      Aggregator process
  ┌──────────────────────────────────────────────────────────────┐
  │                                                              │
  │  ┌────────────┐   ┌────────────┐   ┌────────────┐            │
  │  │ Binance    │   │ OKX        │   │ Bybit      │  thread    │
  │  │ adapter    │   │ adapter    │   │ adapter    │  per venue │
  │  │ WSS+parse  │   │ WSS+parse  │   │ WSS+parse  │            │
  │  └─────┬──────┘   └─────┬──────┘   └─────┬──────┘            │
  │  ┌─────▼──────┐   ┌─────▼──────┐   ┌─────▼──────┐            │
  │  │ SPSC queue │   │ SPSC queue │   │ SPSC queue │            │
  │  └─────┬──────┘   └─────┬──────┘   └─────┬──────┘            │
  │        └────────────────┼────────────────┘                   │
  │                   ┌─────▼─────────────────┐                  │
  │                   │  Consolidator         │  single thread   │
  │                   │  OWNS all 3 VenueBooks│  coalesced       │
  │                   │  apply → merge →      │  wakeup          │
  │                   │  derive → Snapshot    │  (no locks)      │
  │                   └─────┬─────────────────┘                  │
  │                         │ shared_ptr<const Snapshot>         │
  │                   ┌─────▼─────────────────┐                  │
  │                   │  gRPC async server    │  N completion-   │
  │                   │  per-session conflate │  queue threads   │
  │                   └─────┬─────────────────┘                  │
  └─────────────────────────┼────────────────────────────────────┘
                            │ server-streaming
        ┌───────────────────┼───────────────────┐
   ┌────▼─────┐       ┌─────▼─────┐       ┌─────▼──────┐
   │ client   │       │ client    │       │ client     │
   │ bbo      │       │ vol-bands │       │ px-bands   │
   └──────────┘       └───────────┘       └────────────┘
```

**Libraries / targets**

- `md_core` — domain types, `VenueBook`, consolidator, band math. No I/O, no networking. This is where the tests live.
- `logger` — a small header-only wrapper over `fmt` (`Logger::Log(LogLevel, format_string, args...)`). Not `md_providers` or `md_core`-specific; anything can depend on it.
- `md_providers` — `IMarketDataProvider` + Binance/OKX/Bybit/Replay implementations.
- `md_proto` — generated protobuf/gRPC stubs.
- `aggregator` — wiring + gRPC server.
- `client_common` + three thin client binaries.

`md_core` having zero I/O dependencies is what makes the whole thing testable; it's the single most important structural decision in the codebase.

**Logging: `fmt`-based `Logger`, not `spdlog`.** `spdlog` was the original choice, but for a single-process, single-symbol aggregator with no log rotation, no async sinks, and no multi-sink fan-out requirement, it's a large dependency for what boils down to "format a line and write it to stdout." A ~40-line header wrapping `fmt::print` gives the same call-site ergonomics (`Logger::Log(LogLevel::kInfo, "seq={}", seq)`) without pulling in a logging framework's full surface area (loggers-by-name, sinks, async queues, rotation policies) that this project never uses. If a real need for structured/async logging shows up later (e.g. logging becoming a bottleneck on the hot path), `spdlog` is the documented fallback — this is a rejected-then-reconsidered alternative, not a closed door.

---

## 3. Domain model

Prices and quantities are **scaled `int64`**, never floating point.

```
PriceTicks   = int64   // price × 1e8, integral in the canonical grid
QtyUnits     = int64   // base quantity × 1e8
Notional     = int128  // ticks × units needs the width; or int64 on a rescaled grid
VenueId      = uint8   // dense index, array-friendly
```

Rationale: doubles accumulate error across VWAP sums, compare badly for level identity, and read as a red flag in review. `int128` for notional avoids overflow at 50M USDT with 1e8×1e8 scaling — alternatively rescale before multiplying; whichever is chosen must be commented with the overflow bound.

**Canonical price grid.** Venue tick sizes differ (Binance spot 0.01, OKX 0.1, Bybit 0.01). The canonical grid is the *minimum* tick across configured venues, so every venue price maps exactly onto it with no rounding. If a future venue has an incommensurable tick, the adapter must reject at startup rather than silently round. No rounding = no bias to document.

**Normalized update** produced by every adapter:

```
BookUpdate {
  VenueId  venue
  uint64   seq            // venue-native monotonic sequence
  uint64   recv_ts_ns     // CLOCK_MONOTONIC, ours
  uint64   exch_ts_ns     // venue's, for drift estimation only
  bool     is_snapshot    // full replace vs delta
  span<LevelDelta> bids, asks   // qty == 0 means remove
}
```

`LevelDelta { PriceTicks price; QtyUnits qty; }` — absolute qty at price, matching all three venues' semantics. Adapters own all venue-specific weirdness; nothing above this line knows a venue's name.

---

## 4. Market data provider layer

### 4.1 Interface

```
class MDProvider {
  virtual void start(BookUpdateSink&) = 0;
  virtual void stop() = 0;
  virtual VenueStatus status() const = 0;
};
```

The sink is called on the provider's own thread. Providers never allocate per update after warm-up: parse buffers, delta vectors, and read buffers are reused.

Parsing is factored into a `Parser` base class that owns a reused `simdjson::ondemand::parser` and a growable input buffer; `BinanceParser` / `OkxParser` / `BybitParser` derive from it. One instance per parsing thread — the parser is not thread-safe. A venue's depth and fast-BBO streams run on the same thread and share one parser; Binance's detached REST-snapshot fetch uses its own.

### 4.2 Sync state machine (identical shape for all venues)

```
Disconnected → Connecting → Syncing → Live
                    ▲          │        │
                    │          │        ├─→ Gap    ─→ Resync ─┐
                    └──────────┴────────┴─→ Stale ────────────┘
                             (backoff + jitter)
```

- **Syncing** — subscribe to the delta stream first, buffer, *then* fetch the REST snapshot, then discard buffered events older than the snapshot and apply the rest. Doing it in the other order loses updates.
- **Live** — continuity is asserted on every message. Any violation → `Gap`.
- **Gap** — the book is *wrong*. Strictly worse than stale: it must be excluded immediately and resynced. Exponential backoff with jitter, plus hysteresis so a flapping feed doesn't resync-storm.
- **Stale** — the book may still be correct but is not current (§6).

### 4.3 Per-venue specifics

| | Binance | OKX | Bybit |
|---|---|---|---|
| Depth stream | `btcusdt@depth@100ms` | `books` | `orderbook.50.BTCUSDT` |
| Snapshot | REST `/api/v3/depth` | in-channel snapshot | in-channel snapshot |
| Continuity | `U <= lastUpdateId+1 <= u`, then `pu` chaining | seq/prevSeq | `u` monotonic, `type=snapshot` resets |
| Integrity check | — | ~~CRC32 over top-25~~ **none (deprecated)** | — |
| Fast BBO | `btcusdt@bookTicker` | `bbo-tbt` | `orderbook.1` |
| Heartbeat | server ping / 3m | `ping` text / 30s | `{"op":"ping"}` / 20s |

**OKX's CRC32 is gone — this section is corrected.** This document originally treated OKX's checksum as free end-to-end validation that our book construction matches the venue's. OKX has since deprecated it: the `checksum` field is still present in `books`, `books-l2-tbt` and `books50-l2-tbt` pushes, but its **value is fixed to 0** and it must not be used for integrity verification. OKX directs users to `seqId`/`prevSeqId` instead, which is what §4.2's sequencing implements.

Consequence worth stating plainly: we are down from three defences against a silently wrong book to two — the fast-BBO oracle (§4.4) and the `std::map` test oracle (§5.1). Neither is a per-message check against the venue's own view of the book, which the checksum was, and no other venue offers one either. That makes the fast-BBO oracle the only *live* cross-check available and correspondingly more valuable than when this document was written.

### 4.4 Combining fast-BBO and depth streams

Both are available per venue, and the temptation is to splice fast-BBO ticks into the depth book. **Do not** — the two streams are not mutually sequenced and splicing corrupts the book. Two legitimate uses:

1. Route fast-BBO into the BBO publishing path only, flagged in the API as a distinct source with lower latency than the depth-derived book.
2. Use it as a **correctness oracle**: if the depth-derived BBO disagrees with the fast-BBO stream for more than ~200ms, our book is wrong → force resync. Cheap, and a strong live invariant.

Decision for v1: implement (2) — the invariant — and subscribe fast-BBO for it. Implement (1) only if BBO latency measurement shows the 100ms depth throttle dominating.

### 4.5 Replay provider

Reads a capture file (newline-delimited raw venue frames + our receive timestamps) and re-emits through the identical adapter parse path, honoring or compressing inter-frame gaps. This is not a convenience — it is what makes book logic, sequencing, staleness, and band math deterministically testable, and it lets docker-compose demo with no network or exchange geo-restrictions. A small capture tool writes these files.

---

## 5. Book structures and the consolidation strategy

### 5.1 Per-venue book

**Chosen: flat sorted vector per side.** Bids descending, asks ascending — implemented once, templated on comparator. Rationale over `std::map`: contiguous memory, no per-node allocation, and for the few-hundred-level working set a `memmove` on insert beats red-black tree rebalancing comfortably. Updates cluster near the top of book, where the memmove is shortest.

`std::map` is built first as the reference implementation and kept permanently as a test oracle — every property test runs both and asserts equality. It costs nothing and it's the cheapest correctness insurance available.

**Designed, not built: tick-indexed ladder.** `qty[(price − base) / tick]` with a maintained best-index and a two-level bitmap for next-non-empty-level lookup. O(1) update, O(1) BBO. Needs a price window around mid with rebasing on drift and an out-of-window fallback path. This is the correct endgame for a single instrument and is documented in the README as the next optimization, with the flat-vector benchmark as its baseline.

### 5.2 Consolidation: hybrid, not eager

Two candidate designs:

- **Eager** — every venue delta writes into one shared consolidated structure. Cost is paid per *update*; requires storing per-venue qty at every price to subtract correctly; every update touches shared state.
- **Lazy** — three independent books, k-way merged at publish time. Cost is paid per *publish*.

**Chosen: hybrid.**

- **Consolidated BBO is eager.** Each venue's best bid/ask is cached; consolidated BBO is `max`/`min` over three values — a handful of comparisons per update. The BBO path stays low-latency.
- **Deep merge is lazy.** At each publish tick, merge the top-N of each venue book. Updates arrive far faster than the publish rate, so paying O(N) per publish instead of per update is a large win. Excluding a degraded venue becomes "skip it in the merge" — free.

**Depth budget N.** N must cover the deepest question asked: the 50M+ notional band and the 1000bps price band, whichever reaches further. On BTCUSDT spot, 1000bps (10%) is far deeper than typical book coverage, so bands *will* legitimately exhaust available depth. This is not an error — it's reported as an `insufficient_depth` flag with the partial fill, and it must be visible in the API rather than silently truncated. N is configured with headroom (target: full available depth from each venue's stream, ~1000 levels max).

### 5.3 Consolidated level

```
ConsolidatedLevel {
  PriceTicks price
  QtyUnits   total_qty
  QtyUnits   qty_by_venue[MAX_VENUES]   // attribution, not optional
}
```

Per-venue attribution is carried through to the wire. A consolidated book without attribution is much less useful to any real consumer, and it's what makes the staleness story legible.

---

## 6. Latency and staleness — the core policy

### 6.1 Framing

An order book is **state**, not an event stream. There is no merged timeline of Tokyo events and New York events to reconstruct. Each venue's book is a replicated state machine driven by its own sequenced feed; the consolidated book is the sum of the latest known states. Latency therefore does not require *alignment* — it requires a *staleness policy*.

What latency actually costs us: a venue 100ms behind contributes liquidity that may no longer exist. Consuming that book produces adverse selection. The job is to detect it and say so.

### 6.2 Why exclusion, not just reporting

The merge takes `max(bid)` and `min(ask)`. A frozen venue never moves. So when the market falls it **always** looks like the best bid, and when it rises it **always** looks like the best ask.

Staleness is therefore not noise that averages out — **the merge actively selects for it**. One frozen venue out of three corrupts the output nearly every time the market moves, not one third of the time. Worked example, from `ConsolidatedBookTest.FrozenVenueNoLongerWinsTheBestBid`:

```
BINANCE frozen   bid 50000   ask 50010     (prices from before the move)
BYBIT   live     bid 49900   ask 49910
OKX     live     bid 49899   ask 49911

admitting the frozen venue:
  best bid 50000 (BINANCE), best ask 49910 (BYBIT)  ->  CROSSED
  a phantom 90-tick arbitrage that nobody can trade
```

Reporting the venue as stale while still merging it is not enough. It must be excluded.

### 6.2a The two clocks

Staleness and drift need **different clocks**, and one field cannot serve both:

| field | clock | used for |
|---|---|---|
| `recv_ts_ns` | wall (`system_clock`) | drift against `exch_ts_ns`, wire, logs |
| `recv_mono_ns` | monotonic (`steady_clock`) | staleness only |

Staleness is `now_mono − last_mono`. Both readings come from the same never-jumping clock, so the clock's arbitrary epoch cancels exactly. A wall clock would break this in both directions: an NTP step **backwards** blinds the watchdog, and a step **forwards** marks every venue stale at once and publishes an empty book that reads in the logs like a total exchange outage.

`recv_ts_ns − exch_ts_ns` must **never** be used as a staleness measure. Our clock and the venue's are not synchronized, so that difference is staleness **plus** an unknown clock offset **plus** network delay — three unknowns, one equation. It is fit for drift estimation only.

### 6.2b The signals, in order of certainty

1. **Connection state.** Every socket for a venue's stream is down → `kDisconnected`, excluded. This is the only verdict made with certainty.

   The reverse does **not** hold. A connection that is up does not mean data is flowing: TCP stays `ESTABLISHED` while an exchange's publisher thread wedges or a middlebox holds the socket open. **Connection state can condemn a venue, never clear one.** That asymmetry is why the timer-based verdicts still exist alongside it.

2. **Venue keepalives.** Two of three venues prove liveness by *republishing an id we have already seen*, which makes silence past a documented interval into evidence rather than a guess. See §6.2c.

3. **Cross-venue corroboration.** Silence is ambiguous: a dead feed and a quiet market look identical to a timer. The disambiguation is that **a quiet market is a market-wide property, while a dead feed is per-venue**:

   ```
   all three venues silent        -> the market is quiet   -> nobody is stale
   two venues active, one silent  -> that one is broken    -> stale
   ```

   This is the only sub-minute signal available for Binance, which publishes no keepalive at all.

4. **Absolute backstop.** A generous per-venue timeout that catches the zombie-connection case signal 1 cannot see. Demoted to a backstop precisely *because* signals 2 and 3 detect a broken feed faster; it no longer has to be tuned tightly.

5. **One-way-delay drift detector.** An EWMA of `(recv_ts − exch_ts)` with its running minimum. The absolute value is meaningless (clock offset), but *deviation above the running minimum* is queueing delay — an early warning that a feed is falling behind. **Diagnostics only: it must never gate admission**, for the three-unknowns reason in §6.2a.

6. **Publish freshness; do not hide it.** Every message carries a `venue_status` block: state, `age_ms`, `last_seq`, `drift_ms`, and whether the venue contributed. Without it the client cannot tell "the bid dropped because the market moved" from "the bid dropped because we excluded Binance" — the same number, completely different meaning.

7. **Admission rule.** A venue contributes iff its verdict is `kLive`. This is an **allow-list, not a deny-list**: written as `== kLive` rather than `!= kStale`, so any state added later defaults to *excluded*. Asserted by `VenueHealthTest.AdmissionIsAnAllowListNotADenyList`.

   Hysteresis on re-admission (healthy for M consecutive ticks) so a marginal feed cannot flicker in and out. **Not built yet.**

### 6.2c Per-venue thresholds are derived, not invented

| venue | stream | keepalive behaviour | backstop |
|---|---|---|---|
| Bybit | L1 (our BBO) | republishes with the **same `u`** after **3s** of no change | derived from 3s |
| OKX | `books` (our depth) | sends `seqId == prevSeqId`, empty sides, after **~60s** | derived from ~60s |
| Bybit | depth (L50) | none documented | measure |
| Binance | depth + `@bookTicker` | **none** — sends nothing when nothing changes | measure |
| OKX | `bbo-tbt` | unverified | measure |

"Bybit L1 must speak every 3 seconds because Bybit documents that it does" is defensible. "250ms felt right" is not — and a 250ms threshold would in fact mark Bybit L1 stale on every quiet interval, since the venue itself only promises 3s.

OKX's ~60s bounds the worst case but is far too slow to be a detector on its own, which is why signals 1 and 3 carry the load there.

**The liveness stamp must sit in front of the duplicate filter.** Both keepalives carry an id already seen, so `SeqDedup` correctly drops them as book updates — and would silently destroy the liveness signal with them. `NoteDepthActivity()`/`NoteBboActivity()` are therefore called immediately after a successful parse and **before** `AcceptDepth`/`AcceptBbo`. The `kIgnore` branches in `CheckBybitContinuity`/`CheckOkxContinuity` that document these keepalives sit behind the filter and are unreachable for them.

A **duplicate from a redundant connection** updates the same stamp, and that is correct: for liveness, a duplicate and a keepalive are the same event — the venue put bytes on the wire.

**Protocol frames never count.** Ping, pong and subscribe acks are rejected by the parser before the stamp. A heartbeat proves the socket is open, not that the data is flowing, and a connected-but-silent venue is exactly the failure being hunted.

### 6.2d Depth and BBO staleness are tracked separately

The depth and fast-BBO streams are **separate sockets**. A venue can be stale on one and live on the other, so each carries its own stamp and its own verdict.

Consequence, which must be documented for clients: the published BBO and the published Book can legitimately disagree about which venues are included. That is correct, not a bug — but it looks like one to anyone who has not been told.

### 6.3 Crossed consolidated books

With three venues, venue A's bid **will** sometimes exceed venue B's ask. This is genuine cross-venue arbitrage plus propagation delay, not a bug.

**Decision: publish as-is, with a `crossed` flag and the crossing magnitude in bps.** It is the true consolidated state, and hiding it would destroy exactly the signal an aggregation consumer wants. The alternative — uncrossing by matching the overlap off — is documented in the README as a rejected option with reasoning.

Band math must therefore tolerate a crossed book: walking bids downward from a best bid that sits above the best ask is well-defined, and the VWAP results are still correct statements about available liquidity.

### 6.4 Rejected: delay equalization

Buffering every feed by D ms so slow-venue updates land in correct order. Used in market data recording and some fairness-regulated venues. **Rejected here**: it adds D latency to all three venues to compensate for one, which is the wrong trade for a consumer that wants current state. Recorded in the README — knowing what not to build is part of the design.

### 6.5 Where the verdict is made: the provider decides, and tells Core

**Chosen: the Provider computes the verdict for its own two streams and pushes it to Core as an in-band notification.**

Four reasons:

1. **It closes the total-outage hole for free.** If Core checked health when an update arrived, then all three venues going silent would mean nothing calls Core, nothing is checked, and the client keeps the last stale book forever — the case that matters most. The Provider already runs an `io_context` on its own thread, so a `steady_timer` there fires whether or not data arrives. No extra thread, no extra component.
2. **Nothing on the hot path.** Core reads a stored verdict instead of calling out per merge.
3. **The Provider owns the facts.** Sockets are its business; connection state does not need to travel to be understood.
4. **Staleness is an edge, not a level.** A venue goes stale rarely, so pushing on *change* matches the shape of the event.

**The notification must be in-band, on the same channel as the updates.** A side channel — health in an atomic that Core reads directly, updates through a queue — produces an inconsistent view of a single venue:

```
Core is draining Bybit updates produced 50ms ago
side channel says "Bybit is stale RIGHT NOW"
  -> Core excludes Bybit from the merge
  -> while still applying Bybit book updates from the queue
```

A real-time signal mixed with a delayed stream. In-band instead:

```
drain: update, update, [BYBIT STALE], update, ...
```

Core learns of the staleness at exactly the right point in that venue's timeline — late, but *consistently* late, together with the data it belongs to. Correct by construction rather than by timing.

Per-venue SPSC (§7.2) gives exactly this ordering and no more: Bybit's updates and Bybit's health event are ordered because they share a queue; Bybit's event and *Binance's* update have no defined order, and need none, because the venues are independent. Until the queues land, `Emit()` calls Core synchronously from the provider thread, which already provides the same per-venue ordering — the queue preserves it rather than creating it.

**The division of labour:**

| who | knows | decides |
|---|---|---|
| Provider | its own sockets, its own silence | `kDisconnected`, `kNoData`, and `kStale` past its own backstop |
| Core | all three venues at once | cross-venue: silent *relative to peers* (§6.2b signal 3) |

Core takes the pushed verdict as a starting point and may only make it **worse**, never better.

### 6.6 Rejected alternatives

**Rejected: Core pulls status from the providers.** Core would hold a callback returning each venue's facts and classify them itself. It works, but it cannot see a total outage (nothing calls Core when nothing arrives), it puts an indirect call on the merge path, and it forces cross-thread reads of provider state that the push design does not need. §6.5 supersedes it.

**Rejected: attaching status to each update.** The venue whose health you need is the one that is **not** calling you.

**Rejected: distinguishing keepalives from duplicates by counting copies.** With N redundant connections, count copies of an id; the (N+1)-th copy is a keepalive rather than a duplicate. It works while all N connections are healthy — and **misclassifies the moment one dies**, because the round never completes and the next keepalive is counted as the missing duplicate. The rule depends on connection health, but its purpose is to help detect that connection health is bad. That circularity is in the shape of the rule and cannot be tuned away.

It is also unnecessary: for liveness, a duplicate and a keepalive are the same event. Per-connection liveness, which is what the counting was reaching for, is already available from `SeqDedup::LastMask()` with no timers and no round-counting.

**Rejected: treating the fast-BBO stream's incremental BBO as filterable.** `UpdateBBOWithQuote` maintains persistent state, so a stale venue's price is *already folded into* the running BBO; skipping its future quotes does not remove it, and the venue sends nothing more to displace it. Excluding a venue from the BBO therefore requires a forced full `ComputeBBOFromQuotes` rescan on the live→stale transition. Filtering a stateless recomputation is easy; filtering an incremental one requires detecting the *transition* and rebuilding.

The merged Book has no equivalent problem: `MergeBooks` is a full rebuild every pass, so a policy change needs no repair step. This is the hidden payoff of the eager full-merge design.

---

## 7. Threading, synchronization, and the publish path

### 7.1 The parallelism axis is the venue

One thread per venue. Each does WSS receive → TLS decrypt → JSON parse → produce a normalized `BookUpdate` → push to an SPSC queue. Parsing is roughly 80% of the ingest CPU, and this parallelizes it across cores with no coordination.

**Rejected: a worker pool parsing one venue's stream.** It breaks message ordering and forces a resequencer — real complexity bought for negative benefit. The venue is the correct sharding axis.

### 7.2 Ownership: one thread owns all books

**Chosen: the consolidator thread owns all three `VenueBook`s.** Venue threads only parse; they never touch a book. Deltas cross into the consolidator through per-venue SPSC ring buffers (one producer, one consumer — the simplest lock-free structure there is).

Consequence: **there is no lock anywhere on the book path.** No mutex, no seqlock, no atomics on book state. The book logic is single-threaded, so it is deterministic, trivially testable with fake input, and TSan-clean by construction rather than by care.

> **STATUS: this describes the target design, not the current code.** The SPSC queues are **not built**. Today the provider threads call `Core::ApplyUpdate` directly through a `std::function`, and `Core::apply_mutex_` guards `venue_books_` and `venue_quotes_` against concurrent calls. That mutex is correct but costs contention this design would not have, and it is the one lock the paragraph above says does not exist. Closing this gap is §14.2 step 12; the mutex is commented as interim at its declaration.

**Rejected for v1: per-venue book ownership with a seqlock for the reader.** Under that design each venue thread applies its own deltas and the consolidator reads across a seqlock. It parallelizes the delta apply — but the apply is the *cheapest* step, and parsing was already parallel in both designs:

- Delta apply costs roughly 1–3 µs (about 5 binary searches plus small memmoves in a few-hundred-level vector).
- The consolidator does this for three venues, so one core saturates near **150k updates/sec**.
- The public throttled channels deliver about **60 updates/sec** combined.
- End-to-end latency is dominated by Binance's own 100ms grouping — five orders of magnitude larger than the ~200ns queue hop this would remove.

Optimizing a 200ns hop behind a 100ms throttled source is not an optimization. Against that, a seqlock is easy to write and hard to write *correctly*; a memory-ordering mistake yields a silently wrong book at a low rate, which is the worst failure mode available in a two-week project.

**Condition to revisit:** consolidator CPU above ~50% of one core (roughly 50k updates/sec), which requires tick-by-tick channels rather than the public throttled ones. The benchmark that would detect this is built; the optimization is not.

For the record on the discarded third option: `shared_mutex` is the worst of the three. Under contention it degrades to a futex syscall, and readers stall the writer — backwards, since the writer is on the latency path and the reader is a periodic publisher.

### 7.3 Consolidator thread

Woken by a **coalesced wakeup**: any venue push sets a dirty flag and signals. The consolidator drains, works, and loops. This is self-clocking — on a single update in a quiet market it publishes immediately, and during a burst the updates arriving while it works collapse into the next pass. No fixed timer to tune, and no artificial delay when idle. An optional rate floor exists per subscription (`throttle_ms`).

Per pass:

1. Drain all SPSC queues; apply deltas to the three books.
2. K-way merge the top-N of each admitted venue.
3. **Single pass** over the merged book computes BBO, all volume bands, and all price bands on both sides — the bands are cumulative and monotonic in depth, so one walk fills every band simultaneously. This is why the derived views are cheap.
4. Build one immutable `Snapshot`, publish via `shared_ptr<const Snapshot>`.

Fan-out to N subscribers is then N refcount bumps and zero book copying. The merge and band math happen **once**, not once per subscriber.

### 7.3b Rejected: a message transport between internal components

ZeroMQ (or any socket transport) inside the aggregator process would serialize a struct, copy it through a socket buffer, and deserialize it — for work an in-memory ring buffer does by moving a pointer.

Splitting each venue adapter into its own process and container *is* a legitimate architecture: one adapter crashing would not take down the others, and each could be restarted independently. It costs serialization on every update, extra latency, and more failure modes. For a single-symbol three-venue aggregator, one process is the right trade. Recorded because the isolation argument is real and wins at larger scale.

### 7.4 Backpressure: per-session conflation

**The #1 systems risk is a slow gRPC client back-pressuring the book.** Each session holds a depth-1 pending slot with overwrite semantics: if a new snapshot arrives while the previous write is still in flight, the pending one is replaced, not queued. Nothing upstream ever blocks.

This must be documented as intentional: **this is a state-publishing API, not an event log.** Dropping intermediate states is correct by design, and the `seq` field lets clients observe that it happened.

### 7.5 Allocation discipline

Hot paths allocate nothing after warm-up: a reused simdjson parser and input buffer in the `Parser` base class, `BookUpdate` level vectors reserved to the venue depth tier at construction, pre-sized merge scratch, snapshot objects recycled through a free list. Protobuf message reuse via `Arena` on the publish path. An allocation counter in debug builds asserts this in tests.

---

## 8. Derived views — definitions and math

Definitions must be pinned down explicitly; both have real ambiguity and reviewers will check that it was noticed.

### 8.1 Best Bid-Offer

Consolidated best bid = highest bid price across admitted venues; qty = summed qty at exactly that price across venues that quote it, with per-venue attribution. Symmetric for ask. Spread and mid included. `crossed` flag when best bid ≥ best ask.

### 8.2 Volume bands — 1M / 5M / 10M / 25M / 50M+ notional

**Interpretation: VWAP to fill N USDT of notional by sweeping the consolidated book.** Walk from the top accumulating `price × qty` until the band's notional is reached, splitting the final level proportionally.

Per band, per side, report: `vwap`, `worst_price` (last level touched), `filled_notional`, `filled_qty`, and `insufficient_depth` when the book is exhausted before the band is reached. Notional is in USDT (the quote currency). The `50M+` band is the terminal band — reported as VWAP over all available depth with the flag set when it can't be filled, which on BTCUSDT spot it frequently won't be.

The alternative reading — "the price level at which cumulative notional crosses N" — is a strictly weaker statement than the VWAP and is not what an execution consumer wants. Noted in README.

### 8.3 Price bands — BBO + 50 / 100 / 200 / 500 / 1000 bps

**Interpretation: cumulative liquidity available within X bps of the consolidated BBO**, measured from the BBO as the assignment literally states (bids: from best bid down to `best_bid × (1 − bps/10000)`; asks: up from best ask).

Per band, per side, report: `cum_qty`, `cum_notional`, `vwap`, `level_count`, `limit_price`.

Ambiguity noted in README: measuring from the **mid** is the more common industry convention and behaves better when the spread is wide or the book is crossed. The literal wording ("BBO+") wins for v1; the reference point is a config flag so both are available.

### 8.4 Parameterization

The assignment's band values are **defaults, not constants.** Bands arrive as a repeated field in the subscription request. Hardcoding 1M/5M/10M is the obvious extensibility miss on a rubric that names "API/protocol design and extensibility" explicitly.

---

## 9. gRPC API design

### 9.1 One RPC, not three

```proto
service Aggregator {
  rpc Subscribe(SubscribeRequest) returns (stream Update);
  rpc GetVenueStatus(VenueStatusRequest) returns (VenueStatusResponse);
}

message SubscribeRequest {
  string symbol = 1;                    // "BTCUSDT"
  repeated Feed feeds = 2;              // BBO | VOLUME_BANDS | PRICE_BANDS | DEPTH
  repeated int64 notional_bands = 3;    // scaled; defaults applied if empty
  repeated uint32 bps_bands = 4;        // defaults applied if empty
  uint32 max_depth = 5;                 // for DEPTH
  uint32 throttle_ms = 6;               // client-requested conflation rate
}

message Update {
  uint64 seq = 1;                       // server-side monotonic; gaps = conflation
  uint64 server_ts_ns = 2;
  string symbol = 3;
  repeated VenueStatus venues = 4;
  uint32 price_scale = 5;                // exponent: divide price fields by 10^price_scale.
  uint32 qty_scale = 6;                  // exponent: divide qty fields by 10^qty_scale.
  oneof payload {
    Bbo bbo = 10;
    VolumeBands volume_bands = 11;
    PriceBands price_bands = 12;
    Depth depth = 13;
  }
}
```

Rationale for one generic streaming RPC over three specialized ones: a new derived view becomes a new `oneof` arm and a new `Feed` enum value, with no service-definition churn and no new connection. It also lets one client subscribe to several feeds over one stream, which the three client binaries don't need but a real consumer would.

### 9.2 Wire conventions

- **All prices/quantities are scaled `int64`. No doubles, no decimal strings, ever, in server-side computation.** Band math, tie detection at the best price (§5.3), and every other comparison happen in exact `int64` — this is the part that actually breaks under floating point and it is never compromised.
- **The scale is carried on every message, not just documented in the proto.** `uint32 price_scale` and `uint32 qty_scale` (as an exponent — e.g. `8` means "divide by 10^8", not the raw multiplier) are fields on every `Update`, alongside `symbol`. Protobuf has no 8-bit integer type; varint encoding already makes a small value like `8` cost 1 byte on the wire regardless of the declared 32-bit width, so `uint32` loses nothing. This is a refinement of the original "scale declared in the proto" wording, not a reversal: a client now needs zero out-of-band knowledge to interpret a value, and — same reasoning as `symbol` being present despite one symbol today — nothing has to change on the wire if a future instrument needs a different scale (§1.2's multi-symbol seam). Also matches §7.4: every `Update` is meant to be self-contained, so scale shouldn't need to be remembered from an earlier message either.
- `symbol` present everywhere despite one symbol today — multi-symbol needs no protocol change.
- `seq` + `server_ts_ns` on every message so clients can detect conflation and measure end-to-end latency.
- `venue_status` on every message — freshness is part of the data, not a side channel.
- Reserved `version` handling and never-reused field numbers.
- Server-side conflation is documented in the proto comments, not just the README.

### 9.3 Client services

Three thin binaries over a shared `client_common` (connect, retry with backoff, subscribe, render). Each sets a different `Feed` and formats to stdout: aligned columns, one line per update, `--json` alternative for machine consumption. Client-side gap detection on `seq` reported to stderr — it demonstrates that the conflation contract is understood and honored.

---

## 10. Testing strategy

Test coverage is an explicit assessment criterion, and `md_core`'s I/O-free design is what makes it reachable.

| Level | What |
|---|---|
| Unit | Flat-vector book vs. `std::map` oracle: insert/update/delete/cross, top-of-book maintenance |
| Unit | Band math against hand-computed golden cases, including exhausted depth, single-level fill, crossed book, zero-liquidity side |
| Unit | Sequencing state machine per venue: gap, out-of-order, duplicate, snapshot-during-live, reconnect mid-sync |
| Unit | Per-venue sequencing rules: Bybit u+1 / restart, OKX prevSeqId chaining incl. keep-alive and maintenance reset, Binance snapshot reconciliation (`test_continuity.cpp`) |
| Property | Randomized delta streams: consolidated total qty at price == Σ per-venue qty, invariant after every update |
| Staleness | Synthetic replay with injected per-venue delays; assert admission/exclusion and hysteresis behave as specified |
| Concurrency | Seqlock under TSan: writer + readers, assert no torn reads and bounded retry |
| Integration | Aggregator + 3 replay providers + 3 clients in-process; assert client stdout matches expected golden output |
| Benchmark | Binance JSON parser latency (`benchmarks/bench_binance_parser.cpp`, `-DBUILD_BENCHMARKS=ON`) — **built**. Book apply throughput, merge+derive per tick, end-to-end tick→client latency — planned, not built. |

Sanitizer builds (ASan/UBSan/TSan) in CI. The one benchmark that exists (parser latency) backs the parser-reuse numbers in the README; every other optimization claim is still an estimate.

---

## 11. Packaging

Multi-stage Dockerfiles: a shared builder stage (vcpkg dependency layer cached separately from source, so source edits don't rebuild gRPC) and slim runtime stages. Four images, or one image with four entrypoints — chosen at build time; the assignment asks for "container files for every service", so four thin Dockerfiles sharing a base.

`docker-compose.yml` brings up `aggregator` + `client-bbo` + `client-volume-bands` + `client-price-bands`, with clients depending on the aggregator's health check. A `replay` profile runs the whole stack off capture files with no external network — which is how a reviewer with a geo-blocked IP or no exchange access still sees it work. That profile is worth more than it costs.

Config via environment + a mounted YAML: venues, symbol, thresholds, publish rate, band defaults.

---

## 12. Performance plan

Bottlenecks, in the order they are expected to matter, each with its mitigation and its measurement:

| # | Bottleneck | Mitigation | Measured by |
|---|---|---|---|
| 1 | JSON parsing | simdjson, parse directly into deltas, no DOM, no per-field `std::string` | parse latency bench (Binance, built) |
| 2 | Allocation/copying in hot path | reused buffers, pre-sized vectors, snapshot free list, protobuf arenas | debug allocation counter + bench |
| 3 | Slow-client head-of-line blocking | per-session depth-1 conflation, overwrite-pending | integration test with an artificially slow client |
| 4 | Lock contention on books | single owner thread; no lock at all on the book path | consolidator CPU% (the trigger for the seqlock design) |
| 5 | Publish amplification (3 venues × rate × M subs) | conflate at fixed rate; derive once; share immutable snapshot | tick cost bench vs. subscriber count |
| 6 | Cache misses in book traversal | flat contiguous structures; ladder documented as next step | book apply bench vs. `std::map` baseline |
| 7 | Wakeup/syscall churn | timerfd tick, batched reads, one io_context per thread | perf stat context-switch count |
| 8 | Resync storms | backoff with jitter + hysteresis | staleness test suite |

Row 1 has a first result: constructing simdjson's parser and the `BookUpdate` level vectors per message costs ~2–3× versus reusing them (Binance parser latency bench). Rows 2–8 remain estimates.

Not pursued in v1, documented as future work with rationale: tick-indexed ladder with hierarchical bitmap, lock-free SPSC rings between receive and parse, CPU pinning and NUMA placement, kernel bypass.

---

## 13. Risks

| Risk | Mitigation |
|---|---|
| Exchange geo-blocking or rate limits during review | replay profile in compose; capture files committed |
| Venue API changes mid-assignment | adapters isolated behind one interface; contract tests on captures |
| Subtle book desync going unnoticed | fast-BBO oracle + `std::map` test oracle + per-venue sequence-gap detection → resync. **Weaker than originally planned**: OKX deprecated its CRC32 (§4.3), so no venue offers a per-message check against its own book. Gap detection catches *missed* messages, not *misapplied* ones. |
| Optimization work crowding out correctness and docs | optimization is a fixed, time-boxed phase after a green end-to-end slice |
| Band definitions differing from the assessor's intent | both interpretations documented; reference point config-flagged; asked as a clarifying question |

---

## 14. Revised build order

The original roadmap is a deep vertical (perfect book first, API last). Four changes:

**0. Contracts first.** Domain model, `IMarketDataProvider`, `.proto`. Writing the Binance client before this forces a rewrite when the second venue lands.

**1. Thin end-to-end slice.** One venue → `std::map` book → BBO → gRPC → one client → stdout. Ugly but complete. Everything after this adds depth to a working system rather than hoping the pieces meet.

**2. Docker + compose here**, not at the end. It's cheap, and it's a hard deliverable that must not be at risk of being cut.

**3. ~~Replay provider + capture tool.~~ DROPPED.** It buys deterministic testing, which `md_core`'s no-I/O rule already delivers for the logic that matters. Recorded in the README as a deliberate omission, not an oversight.

**4. Venues 2 and 3**, plus the consolidator, attribution, and the k-way merge.

**5. Staleness policy** — see §6. Partially built; state below.

**6. Volume bands + price bands**, single-pass derivation, golden tests.

**7. Three client binaries**, `client_common`, formatting.

Steps 6 and 7 were completed **before** step 5 finished. That is why `VenueStatus` exists on the wire with nothing populating it.

---

### 14.1 Current state

| # | step | state |
|---|---|---|
| 0 | Contracts | done |
| 1 | Thin end-to-end slice | done |
| 2 | Docker + compose | done |
| 3 | Replay provider | **dropped, deliberately** |
| 4 | Venues 2 and 3 + consolidator | done |
| 5 | Staleness policy | **in progress** — see below |
| 6 | Volume + price bands | done |
| 7 | Three client binaries | done |
| 8 | Redundant connections + dedup | done (`SeqDedup`, §4.6) |
| 9 | JSON optimization | done — `ondemand::parser` reused per provider |

**Step 5 breakdown**, because "staleness" spans several commits and only part of it is live:

- ✅ two-clock stamps (`recv_ts_ns` wall, `recv_mono_ns` monotonic)
- ✅ liveness stamps in front of the dedup filter (`NoteDepthActivity`/`NoteBboActivity`, six sites)
- ✅ `VenueHealth` — `kNoData`, `kLive`, `kDisconnected`, `kStale`
- ✅ `ClassifyVenue` — the timer predicate, pure, 13 tests
- ✅ `MergeBooks(..., health)` — exclusion honoured, 9 tests
- ✅ provider watchdog (`steady_timer` on the provider's own `io_context`) + edge-triggered `VenueHealthEvent` (§6.5)
- ✅ `ClassifyFeed` — connection state then timer, 8 tests
- ✅ Core stores the pushed verdict; the merged Book excludes stale venues
- ✅ BBO rescan on transition, driven by a version counter (§6.6), 6 tests
- ✅ verified live: six streams report `NO_DATA -> LIVE` once each, then silence

- ✅ **resync admission** (`kResyncing`) — the *"∧ no outstanding gap"* clause of the §6.2b admission rule. `RequestResync()` announces `kResyncing` on both streams **before** tearing the sockets down, because `WebSocketSessionSSL::Stop` sets `stopped`, which suppresses `NotifyClosed` — so `depth_live_` never drops and connection state is blind at exactly the moment we are certain the book is invalid. Without it, Core kept merging the known-broken pre-gap book for the whole resync window.

  The state is **sticky**: the watchdog skips a stream that is `kResyncing`, since a timer has nothing useful to say about a stream we switched off ourselves. It is cleared by the first message after the venue returns.

Remaining:

- ❌ cross-venue corroboration (§6.2b signal 3) — the only sub-minute signal for Binance
- ❌ hysteresis on re-admission; today recovery from `kStale` waits for the next watchdog tick, which is a crude stand-in
- ❌ republish on a health change — a venue going stale in a *quiet* market leaves the client holding a book that still includes it, because nothing triggers the next merge
- ❌ `VenueStatus` populated on the wire

### 14.2 Remaining order

**10. Finish the staleness verdict.** Provider `steady_timer` + `VenueHealthEvent` → Core → `MergeBooks`. Ends the state where the policy exists but decides nothing.

**11. Benchmark harness, broadened.** `bench_binance_parser` exists; `VenueBook::ApplyUpdate` and `MergeBooks` do not. The parser was the target *least* in doubt — simdjson was already fast. The open question is whether `std::map` is the bottleneck, and no optimization of it is justifiable without that number.

This step also settles §7.2's unmeasured claims (1–3 µs delta apply, ~150k updates/sec saturation, ~200ns queue hop). None of them are currently measured by anything in the repo.

**12. Per-venue SPSC queues.** Replaces `Core::apply_mutex_` and the `CallBack` seam. **This is the item that makes the code match §7.2**, which today claims "there is no lock anywhere on the book path. No mutex" while `md_core.cpp` holds a `lock_guard`. Not primarily a performance change: a single-threaded book is deterministic, testable with fake input, and TSan-clean by construction.

**13. YAML server config.** Venue url/port/endpoint/market type/staleness thresholds in one file, replacing the compile-time constants in `types/venue.h` — including `kByBitPath = "/v5/public/spot"`, where the market type is currently baked into a path string.

**14. Spot / futures.** A `MarketType` dimension: books keyed by `(symbol, market_type)`, per-market paths and subscription messages, per-market symbol formats (OKX futures is `BTC-USDT-SWAP`). Never one book — §1.3.

**15. Synchronization review.** What remains after step 12. `aggregator_service::sessions_mutex_` is per-publish and low-contention — measure before touching. `conflated_channel`'s `condition_variable` **stays**: its job is to let the gRPC writer thread sleep when idle, and replacing it with a lock-free spin would burn a core per client to avoid an uncontended lock.

**16. Flat-vector book.** Only if step 11 shows `std::map` is the bottleneck. The `std::map` implementation is kept permanently as the test oracle either way.

Expected to help the **merge** at least as much as the per-venue apply, and that is the part worth measuring first. `MergeBooks` iterates three `std::map`s for ~1450 output levels, so roughly 4400 `++it` steps per merge — each one a red-black-tree traversal to a node that is almost certainly not in cache. Over three sorted vectors the same k-way merge is a linear scan with hardware prefetching working for it. Hypothesis, not a measurement.

**16b. Incremental merge — evaluate only after 16, and only with numbers.**

Rather than rebuilding the consolidated book on every update, apply the delta to the previous one, the way `UpdateBBOWithQuote` does for the BBO. Four reasons this is harder than it looks, and why it is sequenced last:

1. **Prefix sums dominate.** `MergedLevel` carries `cum_qty` and `cum_notional`. A change at level *i* invalidates every prefix sum from *i* to the end — and the common case is a change at the *top*, which invalidates all of them. Incremental merging avoids the k-way selection but not the accumulation pass. Keeping the sums in a Fenwick tree would make updates O(log n), at the cost of turning every band walk's O(1) reads into O(log n) queries.
2. **Insert and erase.** A new price is an O(n) memmove into a sorted vector, and levels enter and leave the `max_depth` window as venue depth changes.
3. **It forfeits the staleness property.** Today, excluding a stale venue is free because the merge is stateless — change the admission rule and the next output is correct. Incrementally, a venue going stale requires unwinding its contribution to ~1450 levels, which is the BBO's invalidation problem (§6.6) scaled up, and whose only sane implementation is a full rebuild. The optimization would reintroduce the exact complexity the eager merge avoids.
4. **If 16 succeeds, this may be unnecessary.** A flat-vector merge could be fast enough that the remaining win does not justify the loss in (3).

**Condition to build it:** step 11 shows the merge is a real bottleneck AND step 16 does not remove it.

**17. README + hardening.** Signal handling (the process currently dies only on an external kill, skipping `Stop()`), per-session reconnect, benchmark results, known limitations.

Testing is not a phase — it accompanies every step from 0 onward.

---

## 15. Open questions for the assessor

1. Volume bands — VWAP to fill the notional (assumed) or the price level at which cumulative notional crosses it?
2. Price bands — measured from the BBO (assumed, per the literal wording) or from the mid?
3. Consolidated crossed books — publish as-is with a flag (assumed) or uncross?
4. Is a per-venue attribution breakdown on each consolidated level wanted on the wire, or is total-only preferred?

All four are answered with a stated default, so implementation is not blocked on a reply.

---

## 16. Scaling beyond one symbol and one host

**None of this is built.** It is recorded because "how would this scale?" has a real answer, and because two of the obvious answers are wrong in ways worth stating.

### 16.1 The shard key is `(symbol, market_type)` — venue is not part of it

Instruments never reference each other: BTCUSDT's book needs nothing from ETHUSDT. So the symbol is an embarrassingly parallel shard key — no cross-shard locking, no coordination, linear scaling.

**Venue must not appear in the key.** The aggregator's entire job is to merge *across* venues for one symbol; if Binance-BTCUSDT and OKX-BTCUSDT landed in different shards there would be nothing left to consolidate. Market type *is* in the key, because §1.3 forbids one book carrying both.

```
shard "BTCUSDT/SPOT"   owns Binance + Bybit + OKX spot books
shard "BTCUSDT/PERP"   owns all three perp books, separately
shard "ETHUSDT/SPOT"   fully independent of both
```

### 16.2 Connections scale with venues, not with instruments

The naive model is `N(redundancy) × 3 venues × 2 streams` **per instrument** — 18 sockets for one symbol, 1,800 for a hundred. That hits venue connection limits long before it works.

All three venues multiplex many symbols on one connection: Binance combined streams (`/stream?streams=btcusdt@depth/ethusdt@depth`), OKX multiple `args` per subscribe, Bybit multiple topics. So:

```
sockets = N x 3 venues x 2 streams x buckets
```

where `buckets` is how symbols are **grouped**, not how many there are. 100 symbols in 10 buckets is 180 sockets, not 1,800.

The trade-off is real: one socket carrying 100 symbols means one parse thread demultiplexing to 100 shards, and that thread becomes the bottleneck. Bucket size is the tuning knob, and it is set by measurement.

### 16.3 If provider and core are split across processes

Shared memory beats any socket transport — no serialization, no payload copy, just a slot write and a release store. Three constraints decide whether it is written correctly:

1. **No pointers in shared memory.** The same physical page maps at a *different virtual address* in each process. Everything must be an **offset from the segment base**. This usually manifests as a crash in the second process, never the first.
2. **`BookUpdate` cannot cross as it stands.** It holds `std::vector<PriceLevel>` — heap pointers owned by the producer's allocator, meaningless to the consumer. Anything crossing must be flat POD: a fixed header plus a trailing variable-length array with a count, in one contiguous slot.
3. **Lifetime needs a protocol.** A preallocated ring of fixed slots (Disruptor-style): the producer claims a slot, fills it, publishes a sequence; the consumer validates that sequence *after* reading so it can detect being lapped. Without that check, a slow consumer silently reads a half-overwritten update.

Estimated cost, not measured: a same-host shm SPSC hop is on the order of 100–300 ns, against roughly 1–5 µs for a socket. In-process (§7.2) is lower still.

### 16.4 Across regions, physics dominates

Light in fiber travels at roughly 200,000 km/s (c divided by silica's refractive index ≈ 1.47), and real routes run about 1.2–1.5× the great-circle distance:

| link | great circle | one-way (derived) | typical real RTT |
|---|---|---|---|
| NY ↔ London | ~5,600 km | ~33 ms | ~70 ms |
| NY ↔ Tokyo | ~10,900 km | ~65 ms | ~150 ms |
| Tokyo ↔ Singapore | ~5,300 km | ~32 ms | ~70 ms |

Derived from distance; the RTT column is the commonly observed range for commercial routes, not something measured here.

A Tokyo provider shipping updates to a New York aggregator delivers data that is **already ~35 ms old on arrival**. No amount of shared memory, lock-free queueing or SIMD changes that by 0.01%. Optimizing a 1 µs hop while paying 35 ms of propagation is a 35,000× mismatch. **Geography is not an engineering problem to optimize; it is a constraint to design around.**

What it forces:

1. **Put the aggregator next to the venues, not next to the clients.** One consolidated book cannot be simultaneously fresh for venues on three continents. That is a fundamental limit, not an implementation gap.
2. **Ship the derived output across regions, never the raw feed.** BBO plus bands is a few hundred bytes per update; raw depth is megabytes per second. The gRPC `Update` message already *is* the cross-region payload.
3. **Staleness thresholds become per-venue *and* per-region** (§6.2c). A cross-region venue would be permanently stale against a threshold tuned for a local one.

For crypto specifically this is easier than it sounds — Binance, OKX and Bybit are all commonly reported to run in Asia-Pacific, so one well-placed aggregator is close to all three. **That should be confirmed by measuring RTT to each endpoint, not assumed.**

### 16.5 Rejected: splitting internal components into separate services

Already covered in §7.3b and unchanged by the above. A process boundary between provider and core serializes every book update to replace a pointer move. The boundary that *is* needed already exists in the right place: aggregator ↔ clients, over gRPC.

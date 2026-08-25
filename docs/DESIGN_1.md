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
| Integrity check | — | **CRC32 over top-25** | — |
| Fast BBO | `btcusdt@bookTicker` | `bbo-tbt` | `orderbook.1` |
| Heartbeat | server ping / 3m | `ping` text / 30s | `{"op":"ping"}` / 20s |

**OKX's CRC32 is worth more than it looks.** It's a free end-to-end validation that our book construction matches the venue's, and it doubles as a property test over recorded data: replay the feed, assert the checksum matches at every checkpoint. If our flat-vector book has an off-by-one anywhere, this catches it.

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

### 6.2 Mechanisms, in order of value

1. **Local monotonic receive timestamps for all staleness decisions.** Exchange timestamps are never compared *across* venues — clocks are unsynchronized and the semantics differ (matching-engine time vs. gateway send time). Exchange timestamps are used only within one venue.

2. **Per-venue watchdog.** Time since last message on that feed, heartbeats included. Past a configured threshold (default 250ms for BTCUSDT, per-venue overridable) the venue is marked `STALE` and excluded from the merge.

3. **One-way-delay drift detector.** Maintain an EWMA of `(recv_ts − exch_ts)` together with its running minimum. The absolute value is meaningless (clock offset), but the *deviation above the running minimum* is queueing delay — it signals a feed falling behind seconds before the watchdog fires. Best early warning available for free. Exposed as a metric and as `drift_ms` in venue status.

4. **Publish freshness; do not hide it.** Every message carries a `venue_status` block: state, `age_ms`, `last_seq`, `drift_ms`, and whether the venue contributed. Silently dropping a venue and shipping a thinner book with no explanation is worse than saying so — the consumer can then apply its own policy.

5. **Admission rule.** A venue contributes iff `state == LIVE` ∧ `age < threshold` ∧ no outstanding gap ∧ its own book is not internally crossed. Hysteresis on re-admission (must be healthy for M consecutive ticks) so a marginal feed doesn't flicker in and out of the consolidated book.

### 6.3 Crossed consolidated books

With three venues, venue A's bid **will** sometimes exceed venue B's ask. This is genuine cross-venue arbitrage plus propagation delay, not a bug.

**Decision: publish as-is, with a `crossed` flag and the crossing magnitude in bps.** It is the true consolidated state, and hiding it would destroy exactly the signal an aggregation consumer wants. The alternative — uncrossing by matching the overlap off — is documented in the README as a rejected option with reasoning.

Band math must therefore tolerate a crossed book: walking bids downward from a best bid that sits above the best ask is well-defined, and the VWAP results are still correct statements about available liquidity.

### 6.4 Rejected: delay equalization

Buffering every feed by D ms so slow-venue updates land in correct order. Used in market data recording and some fairness-regulated venues. **Rejected here**: it adds D latency to all three venues to compensate for one, which is the wrong trade for a consumer that wants current state. Recorded in the README — knowing what not to build is part of the design.

---

## 7. Threading, synchronization, and the publish path

### 7.1 The parallelism axis is the venue

One thread per venue. Each does WSS receive → TLS decrypt → JSON parse → produce a normalized `BookUpdate` → push to an SPSC queue. Parsing is roughly 80% of the ingest CPU, and this parallelizes it across cores with no coordination.

**Rejected: a worker pool parsing one venue's stream.** It breaks message ordering and forces a resequencer — real complexity bought for negative benefit. The venue is the correct sharding axis.

### 7.2 Ownership: one thread owns all books

**Chosen: the consolidator thread owns all three `VenueBook`s.** Venue threads only parse; they never touch a book. Deltas cross into the consolidator through per-venue SPSC ring buffers (one producer, one consumer — the simplest lock-free structure there is).

Consequence: **there is no lock anywhere on the book path.** No mutex, no seqlock, no atomics on book state. The book logic is single-threaded, so it is deterministic, trivially testable with fake input, and TSan-clean by construction rather than by care.

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

Hot paths allocate nothing after warm-up: reused parse buffers, pre-sized delta vectors, pre-sized merge scratch, snapshot objects recycled through a free list. Protobuf message reuse via `Arena` on the publish path. An allocation counter in debug builds asserts this in tests.

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

- **All prices/quantities are scaled `int64` with the scale declared in the proto.** No doubles, no decimal strings.
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
| Property | Replay a recorded OKX capture, assert our CRC32 matches the venue's at every checkpoint |
| Property | Randomized delta streams: consolidated total qty at price == Σ per-venue qty, invariant after every update |
| Staleness | Synthetic replay with injected per-venue delays; assert admission/exclusion and hysteresis behave as specified |
| Concurrency | Seqlock under TSan: writer + readers, assert no torn reads and bounded retry |
| Integration | Aggregator + 3 replay providers + 3 clients in-process; assert client stdout matches expected golden output |
| Benchmark | Book apply throughput, merge+derive per tick, parse throughput, end-to-end tick→client latency |

Sanitizer builds (ASan/UBSan/TSan) in CI. The benchmark suite exists so the optimization claims in the README have numbers behind them — an optimization section without before/after measurements reads as guesswork.

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
| 1 | JSON parsing | simdjson, parse directly into deltas, no DOM, no per-field `std::string` | parse throughput bench |
| 2 | Allocation/copying in hot path | reused buffers, pre-sized vectors, snapshot free list, protobuf arenas | debug allocation counter + bench |
| 3 | Slow-client head-of-line blocking | per-session depth-1 conflation, overwrite-pending | integration test with an artificially slow client |
| 4 | Lock contention on books | single owner thread; no lock at all on the book path | consolidator CPU% (the trigger for the seqlock design) |
| 5 | Publish amplification (3 venues × rate × M subs) | conflate at fixed rate; derive once; share immutable snapshot | tick cost bench vs. subscriber count |
| 6 | Cache misses in book traversal | flat contiguous structures; ladder documented as next step | book apply bench vs. `std::map` baseline |
| 7 | Wakeup/syscall churn | timerfd tick, batched reads, one io_context per thread | perf stat context-switch count |
| 8 | Resync storms | backoff with jitter + hysteresis | staleness test suite |

Not pursued in v1, documented as future work with rationale: tick-indexed ladder with hierarchical bitmap, lock-free SPSC rings between receive and parse, CPU pinning and NUMA placement, kernel bypass.

---

## 13. Risks

| Risk | Mitigation |
|---|---|
| Exchange geo-blocking or rate limits during review | replay profile in compose; capture files committed |
| Venue API changes mid-assignment | adapters isolated behind one interface; contract tests on captures |
| Subtle book desync going unnoticed | OKX CRC32 invariant + fast-BBO oracle + `std::map` test oracle |
| Optimization work crowding out correctness and docs | optimization is a fixed, time-boxed phase after a green end-to-end slice |
| Band definitions differing from the assessor's intent | both interpretations documented; reference point config-flagged; asked as a clarifying question |

---

## 14. Revised build order

The original roadmap is a deep vertical (perfect book first, API last). Four changes:

**0. Contracts first.** Domain model, `IMarketDataProvider`, `.proto`. Writing the Binance client before this forces a rewrite when the second venue lands.

**1. Thin end-to-end slice.** One venue → `std::map` book → BBO → gRPC → one client → stdout. Ugly but complete. Everything after this adds depth to a working system rather than hoping the pieces meet.

**2. Docker + compose here**, not at the end. It's cheap, and it's a hard deliverable that must not be at risk of being cut.

**3. Replay provider + capture tool.** Unlocks deterministic testing for everything that follows.

**4. Venues 2 and 3**, plus the consolidator, attribution, and the k-way merge.

**5. Staleness policy** — watchdog, drift, admission, hysteresis, `venue_status` on the wire.

**6. Volume bands + price bands**, single-pass derivation, golden tests.

**7. Three client binaries**, `client_common`, formatting.

**8. Benchmark harness**, then optimize against it: flat vector, seqlock, simdjson, allocation removal, conflation. Before/after numbers recorded.

**9. README + hardening.** Technical decisions, rejected alternatives with reasoning, known limitations, benchmark results.

Testing is not a phase — it accompanies every step from 0 onward.

---

## 15. Open questions for the assessor

1. Volume bands — VWAP to fill the notional (assumed) or the price level at which cumulative notional crosses it?
2. Price bands — measured from the BBO (assumed, per the literal wording) or from the mid?
3. Consolidated crossed books — publish as-is with a flag (assumed) or uncross?
4. Is a per-venue attribution breakdown on each consolidated level wanted on the wire, or is total-only preferred?

All four are answered with a stated default, so implementation is not blocked on a reply.

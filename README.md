# Consolidated Order Book Aggregator

Aggregates BTCUSDT spot market data from three exchanges (Binance, OKX, Bybit) into one consolidated order book, and publishes derived views to gRPC clients.

Four services:

| Binary | What it does |
|---|---|
| `aggregator_app` | Connects to the three exchanges, builds the consolidated book, serves gRPC |
| `client_app` | The client. Any combination of feeds, selected by command-line flags |
| `bbo_sub` | Fixed-subscription test binary: BBO only, no flags |
| `volume_band_sub` | Fixed-subscription test binary: volume bands only |
| `price_band_sub` | Fixed-subscription test binary: price bands only |

The three `*_sub` binaries exist to exercise one feed in isolation while debugging. `client_app` is what actually ships — all four share `client_common` for connection handling, gap detection and formatting.

> **Status: working end-to-end against live exchange data.** All three feeds publish, spot and futures both run, and the whole system runs under docker-compose. The staleness policy is built; `VenueStatus` is not yet populated on the wire. Benchmarks cover the Binance parser and the book hot path (apply, k-way merge, traversal, BBO, bytes-moved-per-diff) — see [Known limitations](#known-limitations) and `becnhmark_results.md`.

---

## Build

**Docker (recommended)** — nothing to install but Docker itself:

```bash
docker compose up --build
```

The first build takes roughly 30–60 minutes: gRPC, Boost, OpenSSL and Protobuf all compile from source inside the image. Later builds reuse that layer, and a BuildKit cache mount keeps the vcpkg binary cache outside the layer so even a `vcpkg.json` change rebuilds only the new package.

**Native** — needs CMake ≥ 3.20, a C++20 compiler, and [vcpkg](https://github.com/microsoft/vcpkg):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

`docker compose up` starts four containers — the aggregator plus three clients, each subscribing to a different feed:

```bash
docker compose up --build          # everything
docker compose logs -f client-bbo  # follow one client
```

All three client containers run the **same** `client_app` binary with different flags; that is the point of the flag-driven design.

To run the pieces by hand:

```bash
./build/aggregator/aggregator_app --depth=500

./build/client/client_app --bbo
./build/client/client_app --notional_band=1M,5M,10M,25M,50M
./build/client/client_app --price_band=50,100,200,500,1000
```

## Configuration

**`aggregator_app`**

| Flag | Default | Meaning |
|---|---|---|
| `--venues=` | `binance,bybit,okx` | Which exchanges to connect to |
| `--instruments=` | `BTCUSDT` | Symbols. Multi-symbol is designed for, not exercised |
| `--grpc_port=` | `50051` | gRPC listen port |
| `--depth=` | `500` | Desired book depth. Each venue rounds **up** to its nearest published tier, or falls back to its deepest — OKX caps at 400 |
| `--connections=` | `1` | Redundant sockets per stream, per venue. See [Redundant connections](#redundant-connections). Rejected above 8 |

**`client_app`** — at least one feed flag is required:

| Flag | Meaning |
|---|---|
| `--server=` | Aggregator address, default `localhost:50051` (compose uses `aggregator:50051`) |
| `--symbol=` | Symbol to subscribe to, default `BTCUSDT` |
| `--bbo` | Consolidated best bid/offer with per-venue attribution |
| `--notional_band=` | Comma-separated USDT targets, e.g. `1M,5M,10M`. VWAP, worst price, level count and slippage in bps |
| `--price_band=` | Comma-separated bps offsets from the top, e.g. `50,100,500`. Liquidity available within each band |
| `--volume_bands` | Volume bands at the server's default thresholds |
| `--price_bands` | Price bands at the server's default thresholds |

Notional values take `K`/`M` suffixes. A bare number is **dollars**, so `--notional_band=1` sweeps one dollar of the book, not one million.

Flags combine — one client can take every feed at once:

```bash
./build/client/client_app --bbo --volume_bands --price_bands
```

---

# Technical decisions

Each decision below lists what was chosen, why, and what was rejected. The full design document with more detail is in [`docs/DESIGN.md`](docs/DESIGN.md).

## Threading and ownership

**Three exchange threads parse. `Core` owns all the books, guarded by one mutex.**

Each exchange has its own thread doing WebSocket receive, TLS decrypt and JSON parse. Each then pushes into **its own SPSC ring buffer** and returns immediately. One consolidator thread drains all of them, owns every book, and does the merge and publish. There is **no lock on the book path**.

**This was the interim mutex until 2e4820e, and the measurement is the interesting part.** Replacing an uncontended mutex with lock-free queues made the median publish latency **worse** — 76.1 µs to 113.3 µs — because the consolidator kept finding the queues empty, sleeping, and paying a ~37 µs wakeup on nearly every message. It only beat the mutex once it **spun before sleeping**: 73.8 µs, with `lock_wait` at 0.0 µs.

> The lesson is worth stating plainly: removing a lock does not remove the cost, it moves it. Here it moved from lock contention (which was near zero) to thread wakeups (which were not). Full numbers in `becnhmark_results.md`.

The spin window is bounded — `kConsolidatorSpinLimit` — so a dead feed costs ~0% CPU rather than a permanently burned core. Measured cost of the spin under live load: ~15–20% of one core.

What this buys beyond latency: the book logic is single-threaded and deterministic, since exactly one thread is ever inside it. That makes it testable with fake input and TSan-clean by construction rather than by care — which was always the stronger argument for this design, and the one that survives regardless of what the microseconds say.

**Condition to revisit:** the consolidator failing to keep up — queue-full events, or publish latency rising with input rate. Both are measured now (`LatencyRecorder("book_publish")` live, `bench_md_core` offline).

### Rejected: each exchange thread owns its own book, with a seqlock for the reader

In that design each exchange thread applies its own updates directly, and the consolidator reads across a seqlock (a version counter that the writer bumps before and after writing; the reader copies the data and retries if the counter moved). The writer never waits for the reader, which is the right shape when the writer is on the latency path.

It was rejected because it parallelizes the wrong step.

Per update the work is: WebSocket read, TLS decrypt, **JSON parse**, apply the delta, then merge and calculate. Parsing is about 80% of the ingest cost, and it is already parallel in both designs. The only thing this alternative moves is the delta apply, which is the cheapest step.

The numbers:

- Applying one update — around five price levels — into a few-hundred-level sorted vector costs roughly 1-3 microseconds: a few binary searches and a few small memmoves.
- The consolidator does that for three exchanges, so one core saturates at roughly **150,000 updates per second**.
- The public throttled channels deliver about **60 updates per second** in total.

That is around 2,500 times more headroom than needed.

There is also a latency argument. Binance sends `depth@100ms`, which means the exchange itself groups 100 milliseconds of changes into one message. The data is already 100ms old on arrival. The queue hop this alternative would remove costs about 200 nanoseconds — five orders of magnitude smaller than the delay the data source already has. Removing it changes nothing measurable.

Against that, a seqlock is easy to write and hard to write *correctly*. A mistake in memory ordering produces a silently wrong order book at a low rate. In a two-week project that is the worst possible failure mode, because you will not find it.

**Condition to revisit:** consolidator CPU above about 50% of one core, which is roughly 50,000 updates per second. Reaching that needs tick-by-tick channels rather than the throttled public ones. `benchmarks/bench_md_core.cpp` is the benchmark that would detect it — merge, apply and traversal are all measured now.

For completeness, the third option — `shared_mutex` with multiple readers — is the worst of the three. Under contention it becomes a futex syscall, and readers block the writer. That is backwards here: the writer is on the latency path and the reader is a periodic publisher.

### Rejected: a worker pool parsing one exchange's stream

Several threads parsing one stream breaks message ordering, which then has to be repaired by a resequencer. Real complexity for negative benefit. The exchange is the correct axis to parallelize on.

### Rejected: ZeroMQ or another message transport inside the process

ZeroMQ between components in the same process would serialize a struct, copy it through a socket buffer and deserialize it — for work an in-memory ring buffer does by moving a pointer.

Splitting each exchange adapter into its own process and container *is* a real architecture: one adapter crashing would not affect the others, and each could be restarted alone. The cost is serialization on every update, extra latency, and more failure modes. For a single symbol on three exchanges, one process is the right trade. Noted because the isolation argument is genuine and would win at larger scale.

## Parsing: one stateful parser object per venue thread

Each venue has a parser class (`BinanceParser`, `OkxParser`, `BybitParser`) over a shared `Parser` base that owns a reused `simdjson::ondemand::parser` and a growable input buffer. simdjson's parser is meant to be constructed once and reused; a per-message parser re-allocates its internal buffers every time. Each `BookUpdate` also reserves its bid and ask vectors to the venue's depth tier at construction, so a full snapshot fills them without reallocating.

**One instance per thread.** The parser holds mutable state and is not thread-safe. A venue's depth and fast-BBO streams are handled on that venue's single io_context thread and share one parser. Binance's REST snapshot is parsed on a separate detached thread, so it gets its own local parser.

### Rejected: construct a parser per message

Simpler — no lifetime rule — but it re-allocates simdjson's working buffers and the level vectors on every message. Measured on the Binance parser: **~2–3× slower**. bookTicker went ~370 ns → ~120 ns, a 10-level-per-side depth delta ~1500 ns → ~600 ns (laptop, `-O3`, not pinned; see `benchmarks/bench_binance_parser.cpp`).

## The per-venue book: reverse-ordered flat vectors, with `std::map` kept as an oracle

Each venue's book is two sorted `std::vector<PriceLevel>` — **worst price first, best price at `back()`**. The `std::map` implementation is still compiled, linked and tested as `MapOrderBook`; `MergeBooks` and `ComputeBBO` are templated on the book array and instantiated for both, so the two run as arms of the *same* benchmark process and are driven by one property test.

**Why the best price is at the back.** Nearly every update lands at the top of book, and `back()` is the only end of a vector that is cheap to grow and shrink. With the best bid at `front()` — which is what the design document originally specified — a new best bid memmoves the entire book, ~16 KB at 1000 levels. At `back()` it is a `push_back`, and removing it is a `pop_back`. The cost is that readers walk backwards, which is close to free: prefetchers handle descending strides too.

**Measured**, one run, 20 000 iterations, both books in one process:

| | `std::map` | flat | |
|---|---|---|---|
| traversal only | 9833 ns | **584 ns** | 16.8× |
| top-of-book churn (20 erase + 20 insert) | 1833 ns | **167 ns** | 11.0× |
| same churn 500 levels deep | 2000 ns | 3333 ns | **1.67× slower** |
| quantity-only delta, 50 levels | 208 ns | 84 ns | 2.5× |
| full merge | 11 166 ns | 9333 ns | 1.20× |

Quantity-only deltas move **zero bytes** — the common case in a live feed — and erasing the top of book is free, because the region above the deepest touched level is empty. The deep-churn loss is real and stated rather than hidden; it stays net positive per depth update, since each one pays an apply *and* a merge.

**Two things here are worth more than the speedup.**

*The hypothesis was wrong, and the measurement said so.* The design document predicted the flat book would remove most of the merge cost, reasoning that pure traversal (`iterate_only`) cost about as much as the whole merge, so the merge must essentially *be* traversal. Traversal then fell 16.8× and the merge improved **16%**. Two costs being equal is not evidence that one causes the other. The merge is **write-bound**: `MergedLevel` is 176 bytes and a full merge writes ~352 KB of output against ~77 KB of input read.

*The oracle earned its keep on the first day.* The first in-place implementation chose its walk direction from the net size change, which is wrong whenever one delta contains both an insert and an erase — it writes into a slot it has not read yet. The resulting book was still **sorted and still the right length**, with one level's data duplicated. No assertion, no crash, no sanitizer finding. Only comparing full sequences against an independent implementation caught it.

## Consolidation: three books merged fresh, never one shared book

`Core` keeps three separate `MapOrderBook`s and rebuilds the merged view from them, rather than maintaining one shared consolidated book that every update writes into.

**Removing an exchange stays cheap.** If one feed breaks and must be excluded, the merge simply skips it — one `if`. In a shared consolidated book its quantities are already mixed into every price level, so excluding it means walking the whole book and subtracting, then walking it again to add it back on recovery. A resync after a sequence gap is worse: the whole book is replaced.

**When the merge runs changed during implementation.** The original plan was to merge only at publish time, on the reasoning that updates arrive faster than we publish. That stopped applying once publishing became eager: **every depth update now triggers a full k-way merge immediately**, and the result is handed out as an immutable `shared_ptr<const Book>` snapshot. Publish time *is* update time; there is no separate publish clock to amortize against.

That is a deliberate trade, not an oversight. It is the simplest thing that is correct, and it matches the same eager choice made for BBO. It is now **measured**, not estimated: the merge is 9–11 µs hot-cache at 1000 levels and **50.9 µs live** — the gap being cache eviction between merges, which the benchmark cannot reproduce. Against ~25 depth publishes/sec that is still wide headroom, but it is 69% of publish latency and therefore the thing to attack first. See [Hot paths](#hot-paths-not-yet-optimized).

The best bid and offer take a different path entirely: they are driven by the venues' fast-BBO channels, cached per exchange, and consolidated incrementally rather than by merging depth at all.

## Publishing is eager, with no timer

There is no consolidator thread and no publish timer. A depth update merges and publishes on the calling provider's thread; a fast-BBO quote updates and publishes the consolidated BBO the same way.

The conflation that used to be the consolidator's job now lives entirely in the per-client channel: if a client is slower than the publish rate, its pending update is replaced rather than queued (see below). So bursts still collapse — just at the client boundary rather than in a central loop.

`throttle_ms` was specified as a per-subscription rate floor. It is **not implemented** and has been removed from the proto rather than left on the wire doing nothing.

## Slow clients: send the newest state, skip what was replaced

Each client session holds one slot. If a new snapshot arrives while the previous write is still in flight, the pending one is **replaced**, not queued. The consolidator never blocks and never grows memory because of a slow client.

This is safe because an order book is a **state**, not a list of events. If states S1, S2 and S3 were produced and only S3 is sent, the client's view is exactly as correct as if all three had been sent. S1 was already wrong by the time it reached the wire.

A trade feed would be different — trades are events and skipping one loses information permanently.

This choice forces a related one: **every message carries full state, not a delta.** A client that skipped a message cannot apply the next delta correctly. The BBO and both band messages are small, so full state costs almost nothing and makes skipping safe automatically. No delta feed is published at all — the raw depth stream was dropped from the protocol, so every payload on the wire is self-contained.

Every message carries a sequence number that increases by one, so a client can see exactly how many states it skipped. The sequence is **per session**, not global: a gap must mean *this* client's channel conflated something, and a shared counter would make every client report gaps caused by traffic sent to someone else. The bundled clients report gaps to stderr. There are no metrics — nothing is exported.

Note that Binance already does this to us: `depth@100ms` is a conflated stream. The real book changes thousands of times per second and we receive one grouped message per 100ms. We are applying the same idea one level further down.

A client can opt out per subscription. Then it gets a bounded queue, and if the queue fills it is disconnected with a clear error. There is no third option — a client that reads slower than the data arrives either skips states or disconnects. Buffering is just a delayed disconnect that serves wrong data in the meantime.

## Stale and disconnected exchanges

> **Status: designed, NOT built.** None of the policy below exists in code. `VenueStatus` is on the wire but never populated, and a stale venue currently keeps contributing to the consolidated book with no indication to the client. What *is* built is the layer underneath it: per-venue sequence-gap detection with automatic resync (see [Sequencing](#websocket-for-updates-rest-only-for-the-initial-snapshot)), which catches a *broken* feed but not a merely *slow* one. The rest is the design that would sit on top.

An order book is state, not an event stream, so there is no merged timeline to reconstruct across exchanges. We hold the latest known state of each one and sum them. Latency does not need aligning — it needs a staleness policy.

The distinction that matters is **constant latency versus changing latency**. An exchange that is consistently 100ms away gives a book that is consistently 100ms old, and that book is fine — the quotes are almost certainly still there. An exchange whose latency just jumped from 10ms to 300ms is giving a book that is mid-transition, with unknown updates still in flight. Only the second one is dangerous.

Two detectors, because one cannot tell those apart:

**Watchdog** — time since the last message, on a local monotonic clock. Past a threshold (250ms by default) the exchange is excluded. This works because these are throttled streams on the most liquid pair in crypto: silence means a broken feed, not a quiet market. On an illiquid symbol the same threshold would flap, so it is configurable per exchange.

**Drift detector** — an exponential moving average of `receive_time − exchange_time`, tracked against its own running minimum. The absolute value is meaningless because the clock offset between us and the exchange is unknown, but that offset is constant, so subtracting the running minimum cancels it exactly. What remains is queueing delay above that feed's own best case. This catches a feed that is still delivering on time but delivering increasingly old data — which the watchdog cannot see. It fires seconds earlier.

Exchange timestamps are never compared *between* exchanges. Clocks are not synchronized and the semantics differ (matching engine time on one, gateway send time on another).

Exclusion is deliberately reluctant. Removing an exchange asserts that reporting zero liquidity is better than reporting 250ms-old liquidity, which is usually the worse error — the depth is probably still there, and removing it makes the volume bands report worse prices than the market really offers. Exclusion is for feeds that are **broken** (sequence gap, disconnected, backlogged), not merely far away.

Re-admission requires several consecutive healthy passes, and the message carries a `venue_set_changed` flag, so a client does not mistake liquidity reappearing for a market move.

Every message carries per-exchange status — state, age, drift, last sequence number — and every consolidated price level carries per-exchange attribution. One policy decision is made in the aggregator; everything else is handed to the client with the evidence.

### Rejected: delaying all feeds so they line up in time

Buffering every feed by D milliseconds so slower exchanges arrive in order. Two problems. It needs synchronized clocks across exchanges to define what "the same moment" means, and we do not have them — the estimation error is comparable to the latencies being corrected. And it makes the two fast exchanges as stale as the slowest one. Internal consistency is not the goal; being current is.

## Crossed books are published, not hidden

Across exchanges, one venue's best bid will sometimes be above another's best ask. In a single exchange that is impossible. Across exchanges the two books are separate and nothing matches them.

It happens for three reasons and we cannot always tell them apart from inside the aggregator: it is genuinely there and someone can arbitrage it; or our feed for one exchange is late and we are looking at a quote that no longer exists; or we misconfigured the instruments. The third one is separable by duration — a cross lasting milliseconds is normal, a cross lasting minutes is a bug, and that is logged as an error.

The book is published as it is, with a `crossed` flag and the size of the cross in bps.

Uncrossing was rejected. It deletes real orders that a client could actually hit, which makes the volume bands report worse prices than the market offers. It removes exactly the signal that a cross-exchange aggregator exists to show. And it is not reversible — a client receiving the raw book can compute a clean one, but a client receiving a cleaned book can never recover what was removed. Deciding what a cross means is a trading decision; reporting state truthfully is the aggregator's job.

An `uncross` option is available per subscription for clients that want the other behaviour.

The band calculations are written to handle a crossed book, and it is a required test case.

## Numbers are scaled integers

All prices and quantities are **`uint64` scaled by 1e8**, both internally and on the wire — unsigned because none of them can be negative. No floating point anywhere in the book or the protocol. Doubles accumulate error across VWAP sums and compare badly for price-level identity, which matters directly here: consolidating two venues at "the same price" is an equality comparison, and `78310.10` is not exactly representable in binary floating point.

Notional needs more width than the values it comes from: `price × qty` at 1e8 each lands at 1e16, and a 50M USDT band is ~5e23 — past `uint64` and past a double's exact-integer range. Internally that accumulation uses `unsigned __int128`; the wire carries the result divided back down to 1e8.

Responses declare their scale explicitly (`price_scale`/`qty_scale` on every `Update`), so a client needs no out-of-band knowledge to read one. Requests do not have such a field, so the fixed 1e8 scale is documented as a protocol constant in the `.proto` — an asymmetry that is deliberate but not ideal, and noted there.

**Not built:** a shared tick grid. The design called for the internal price grid to use the smallest tick among the configured exchanges, with any exchange whose tick does not divide evenly rejected at startup. Nothing validates tick sizes today; prices are taken as the venues send them and compared directly.

## Protocol: one streaming call, not three

One `Subscribe` server-streaming RPC with a `oneof` payload, rather than a separate RPC per publisher type. A new derived view becomes one new `oneof` arm with no change to the service definition, and one client can take several feeds over a single connection.

**Feeds are selected by presence, not by an enum list.** The original design had a `Feed` enum plus separate parameter arrays, which made an empty array ambiguous — "subscribe with server defaults" and "not subscribed" looked identical. Optional sub-messages remove the collision: presence means subscribed, and an empty array inside means defaults.

```proto
message SubscribeRequest {
  string symbol = 1;
  bool bbo = 2;
  optional VolumeBandsRequest volume_bands = 3;   // PRESENT = subscribed
  optional PriceBandsRequest price_bands = 4;     // PRESENT = subscribed
}
```

(`optional` cannot be applied to a `repeated` field in proto3, which is why the arrays are wrapped in a message rather than sitting here directly.)

Band thresholds are per-subscription: two clients can ask for different bands and each gets its own, computed from the same shared merged book. The server sorts them ascending before use — the single-pass band walk never rewinds, so unsorted input would silently produce wrong results.

The symbol is a field everywhere even though only BTCUSDT is used, so supporting more symbols needs no protocol change.

**Not built:** the unary `GetSnapshot` for late-joining clients, and `throttle_ms`. The latter was removed from the proto rather than left present and inert.

## Band definitions

Both had real ambiguity, so both interpretations are recorded here.

**Volume bands** are cumulative from the top of the book: the 5M band covers the first 5M USDT of notional, not the slice between 1M and 5M. Both prices are published — the **VWAP** (average price to fill that size) and the **worst price** (the last level reached) — along with filled quantity, filled notional, and a flag when the book runs out before the band is filled. Notional is in USDT, the quote currency.

**Price bands** are also cumulative, measured from the consolidated BBO because the assignment says "BBO+". The 100bps band includes everything from the BBO out to 100bps, which contains the 50bps band. Measuring from the mid price is the more common convention and behaves better when the spread is wide or the book is crossed — it is noted as the alternative but **not implemented**; there is no configuration flag for it.

The 1000bps band is 10% away from the BBO, which is deeper than any public depth channel reaches at any depth setting. It reports `insufficient_depth`, meaning the totals are a **lower bound** rather than the full liquidity within the band.

That flag was originally omitted from price bands, on the reasoning that "how much is within X bps" is fully answered by "not much". That reasoning was wrong: it holds when the *market* ends, but not when *our own depth budget* ends — and on the wire the two were indistinguishable. A truncated 1000bps band looked exactly like a complete one.

## Spot and futures, never in one book

Both are supported on all three exchanges. Spot and perpetual futures are different instruments at different prices, so mixing them into one book would be a correctness bug rather than a rounding artifact.

That separation is structural, not a check that could be forgotten. Every book, health array and queue is keyed by `InstrumentKey`, which packs symbol **and** market into one `uint32_t` — so spot and futures land in different entries and there is no code path that could merge them. The gRPC `SubscribeRequest` requires a market and **rejects `MARKET_UNSPECIFIED`**: a proto3 enum has no presence, so "forgot to set it" and "chose zero" are the same bytes on the wire, and serving spot by default would silently give a futures client the wrong instrument.

One market per aggregator process, selected in `server_config.json`. Running both at once means two processes — they share no book, no health state and no queue, so the market is the natural process boundary and a futures resync storm cannot stall the spot book.

## WebSocket for updates, REST only for the initial snapshot

All live updates arrive over WebSocket. **Only Binance needs REST** — its depth stream is differential-only and never sends a snapshot, so its documented procedure requires bootstrapping from `GET /api/v3/depth`. Bybit and OKX both send an in-channel snapshot on subscribe, so they need no HTTP client at all.

That difference means the venues do **not** run the same state machine, and their sequencing rules differ more than expected:

| Venue | Snapshot | Continuity rule |
|---|---|---|
| **Binance** | REST, buffer-then-reconcile | `U == previous u + 1` |
| **Bybit** | in-channel | `u` increments by exactly 1; `u == 1` means a service restart and is fresh snapshot data despite the message still saying `"delta"` |
| **OKX** | in-channel | `prevSeqId == previous seqId` — ids are **not** contiguous, so a "+1" rule would be wrong. A backwards `seqId` is a documented maintenance reset, not a gap |

A sequence gap means the book is *wrong*, which is worse than stale, so it triggers a resync. Resync is deliberately kept separate from the reconnect-failure path — a gap is a normal operational event and must not consume the reconnect-attempt budget that permanently stops a provider.

**The fast-BBO channels drive the published BBO.** `bookTicker` / `bbo-tbt` / `orderbook.1` are real-time or ~10ms, against 100ms-throttled depth, so the BBO feed is sourced from them directly. Their ticks are still **never** written into the depth book — the two streams are not mutually sequenced, so mixing them would corrupt it — but they are a publishing path, not just a check.

**Not built:** the oracle that was the original justification for subscribing to them — comparing the depth-derived BBO against the fast-BBO stream and forcing a resync when they disagree for ~200ms.

**OKX's CRC32 is gone.** This document previously described verifying it on every message as free end-to-end validation. OKX has since deprecated it: the field is still present but its **value is fixed to 0**, and they direct users to `seqId`/`prevSeqId` instead. Consequence worth stating plainly — no venue now offers a per-message check against its own book, so a *misapplied* update (as opposed to a *missed* one) has no live detector. That makes the unbuilt fast-BBO oracle more valuable than when it was first written down.

## Redundant connections

**`--connections=N` opens N sockets per stream per venue. The first copy of each message wins, the rest are dropped.** Default 1 — redundancy is opt-in, so the default configuration cannot trip a venue's connection limit.

The benefit is **failover**: one socket dies, the others are already delivering — no gap, no resync, no REST refetch. A latency benefit is **not** claimed; these sockets share one NIC and one route, so they are not independent paths, and it is not measured. The technique is called line arbitration.

Dedup is one integer compare ([`seq_dedup.h`](md_provider/seq_dedup.h)) — keep the highest venue id accepted, drop anything at or below it. 18 bytes of state per stream.

Three rules, each load-bearing:

1. **After parse, before the continuity check.** A duplicate reaching continuity looks like a sequence break and resyncs — three connections would cause two spurious resyncs per message.
2. **`<=`, not `!=`.** One `io_context` reads all N sockets and processes whatever is ready, so one socket delivering 4057 *and* 4058 before another delivers 4057 is ordinary event-loop batching. `!=` accepts the late duplicate.
3. **A reconnecting socket's snapshot is suppressed.** Bybit and OKX greet a new socket with a snapshot whose id is *behind* the others; honouring it moves the mark backwards and the next healthy message reads as a gap.

A *genuine* venue reset (OKX maintenance, Bybit `u == 1`) does move the mark backwards — and keeping that message's own id is what stops the other sockets' copies from each re-applying a snapshot.

**Failure mode:** if a real reset is never reported, every later id sits below the mark and the book freezes silently. `LooksStuck()` logs once after 1000 consecutive drops.

**Rejected — a hash set of seen ids.** Handles missing or out-of-order ids, but costs 8–46 MB and a tuning constant for generality we do not need: all six streams carry a monotonic id.

**Rejected — hashing the `(prevSeqId, seqId)` pair.** Survives the reset message, then fails on the next one — after a reset the stream replays pairs already seen.

**Cost:** N× bandwidth and N× TLS decrypt. Parse cost does not scale, since duplicates die before the book work. Venue connection limits are **not verified**; `--connections` is capped at 8 so a typo cannot open 1800 sockets.

---

# Hot paths not yet optimized

Everything here is a **known cost accepted deliberately**, not an oversight. The book hot path and the Binance parser both have benchmarks (`benchmarks/`, built with `-DBUILD_BENCHMARKS=ON`), and the rows below say which entries are measured and which are still estimates.

| Path | Cost | Fix, when a number justifies it |
|---|---|---|
| **`MergedLevel` is 176 bytes** | 128 of them are a `venues[8]` attribution array with **three** entries ever used. A full merge writes ~2000 levels — ~352 KB — against ~77 KB read. **MEASURED: this is what the merge actually spends its time on** | Shrink it. An earlier attempt (176 → 48 bytes) measured **40% slower**, but that was with `std::map` input when traversal cost 9833 ns and dominated; at 584 ns those conditions no longer hold and it should be re-run |
| **Eager merge per depth update** (`Core::ApplyUpdate`) | Full k-way merge over up to `kDefaultMaxDepth` levels × 3 venues on **every** depth message. **MEASURED: 9–11 µs hot-cache, 50.9 µs live — 69% of publish latency** | Shrink `MergedLevel` first (row above). Incremental merging is analysed and rejected for now: it forfeits the stateless merge that makes excluding a stale venue free |
| **Structural deltas deep in the book** | The flat book rewrites everything above the deepest level a delta touches. **MEASURED: 1.67× slower than `std::map`** at 500 levels deep; 11× *faster* at the top | Nothing yet. Real churn concentrates at the top of book, and the unknown is what fraction of live deltas are deep — a fast-path/relocate counter would settle it |
| **Band vectors in `PublishBook`** | Four `std::vector`s allocated per session per publish | Scratch buffers reused across publishes. Safe because it already runs under `sessions_mutex_` |
| **`sessions_mutex_` held across band math** | All per-session band computation happens inside the lock, blocking subscribe/unsubscribe | Snapshot the session list, compute outside the lock. Only matters with many clients |
| **`Book` snapshot allocation** | `AcquireBookBuffer` reuses buffers when `use_count() == 1`, but grows the pool if every buffer is still referenced | Bounded pool with a defined policy when exhausted. Currently assumes no slow subscriber |
| **Protobuf `Update` built per session** | Each subscriber gets its own serialized message, even when the payload is identical | Build once, share the serialized bytes across sessions with identical subscriptions |
| **Logging** | `fmt::print` from multiple provider threads can interleave mid-line; no level filtering, so `LogLevel::kDebug` is indistinguishable from `kInfo` at runtime | A level check that early-outs *before* formatting, plus a lock or a single logging thread |

---

# Known limitations

**Not built:**
- **Benchmarks.** Only the Binance JSON parser has one — a latency micro-benchmark (`benchmarks/bench_binance_parser.cpp`, built with `-DBUILD_BENCHMARKS=ON`) with before/after numbers for the parser-reuse work in [Parsing](#parsing-one-stateful-parser-object-per-venue-thread). Every other performance statement in this document is an estimate and is labelled as such.
- **Staleness policy** (§6): the watchdog, drift detector and admission rule. `VenueStatus` exists on the wire but is never populated — a stale venue still contributes silently.
- **The fast-BBO oracle** (§4.4): comparing the depth-derived BBO against the venues' own BBO channels to detect a desynced book. This matters more than planned, because OKX **deprecated its CRC32 checksum** (now fixed at 0), removing the only per-message integrity check any venue offered. Gap detection catches *missed* messages, not *misapplied* ones.
- **Record and replay** — capturing raw exchange frames and feeding them back through the same parsers, giving reproducible full-pipeline runs and an offline compose profile. Dropped deliberately: the components are unit-tested individually, and the time went to redundant connections instead. The cost is that nothing exercises the whole pipeline deterministically, so a bug seen against live data cannot be reproduced.
- **Per-session reconnect.** With `--connections>1`, a dead socket is replaced only once *every* socket on the provider is gone — `io_context::run()` returns only when no work remains. The loss is logged (`all depth connections down`) but nothing acts on it.
- **Graceful shutdown** — no signal handling; the process only stops on an external kill, which skips provider teardown.
- Unary `GetSnapshot`, and `throttle_ms`.

**Working, with caveats:**
- `--connections>1` has not been run against live venues. The failure to watch for is a venue refusing the extra sockets, since none of their limits have been verified.
- Single symbol in practice, though the code and protocol are parameterized for more.
- The 1000bps band cannot be covered by the public depth channels at any depth setting — no venue publishes 10% of book. Reported with `insufficient_depth` rather than presented as complete.
- Venue attribution is carried at *every* merged level but only exposed for the BBO. Surfacing it per band is a proto field and an accumulator away; the partial-fill level would need a stated attribution rule.
- No authentication or TLS on the gRPC connection.
- Exchange fees are not included in the consolidated prices. A production aggregator would need this, since taker fees are often larger than the spread between exchanges.
- No persistence.

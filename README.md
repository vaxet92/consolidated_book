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

> **Status: working end-to-end against live exchange data.** All three feeds publish. Docker, benchmarks, and the staleness policy are not built — see [Known limitations](#known-limitations).

---

## Build

*(to be completed)*

## Run

*(to be completed)*

## Configuration

*(to be completed)*

---

# Technical decisions

Each decision below lists what was chosen, why, and what was rejected. The full design document with more detail is in [`docs/DESIGN.md`](docs/DESIGN.md).

## Threading and ownership

**Three exchange threads parse. `Core` owns all the books, guarded by one mutex.**

Each exchange has its own thread doing WebSocket receive, TLS decrypt and JSON parse. Each then calls `Core::ApplyUpdate` (depth) or `Core::ApplyQuote` (fast-BBO) **directly**, and a single `std::mutex` inside `Core` serializes them.

**This is not the design described below, and the difference is worth being explicit about.** The intended architecture — per-venue SPSC ring buffers drained by one dedicated consolidator thread, with no lock anywhere on the book path — is the right shape and is analyzed in the rejected-alternatives section that follows. It was **not built**. What exists is the interim mutex, marked as such in `md_core.h`.

What that costs: the three parser threads contend on one lock, and the merge plus band publish happens inside the critical section rather than on a separate thread. At the rates the public channels actually deliver (~100 depth updates/sec combined, ~300 fast-BBO), contention is not measurable — but it is a real ceiling the SPSC design would not have.

What it keeps: the book logic itself is still effectively single-threaded and deterministic, since only one thread is ever inside it. The testability argument survives; the lock-free claim does not.

**Condition to revisit:** provider threads blocking measurably on `apply_mutex_`, or the merge cost inside the lock becoming visible in publish latency. Neither is measured today — no benchmark exists, which is itself a gap (see [Known limitations](#known-limitations)).

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

**Condition to revisit:** consolidator CPU above about 50% of one core, which is roughly 50,000 updates per second. Reaching that needs tick-by-tick channels rather than the throttled public ones. The benchmark that would detect it is built; the optimization is not.

For completeness, the third option — `shared_mutex` with multiple readers — is the worst of the three. Under contention it becomes a futex syscall, and readers block the writer. That is backwards here: the writer is on the latency path and the reader is a periodic publisher.

### Rejected: a worker pool parsing one exchange's stream

Several threads parsing one stream breaks message ordering, which then has to be repaired by a resequencer. Real complexity for negative benefit. The exchange is the correct axis to parallelize on.

### Rejected: ZeroMQ or another message transport inside the process

ZeroMQ between components in the same process would serialize a struct, copy it through a socket buffer and deserialize it — for work an in-memory ring buffer does by moving a pointer.

Splitting each exchange adapter into its own process and container *is* a real architecture: one adapter crashing would not affect the others, and each could be restarted alone. The cost is serialization on every update, extra latency, and more failure modes. For a single symbol on three exchanges, one process is the right trade. Noted because the isolation argument is genuine and would win at larger scale.

## Consolidation: three books merged fresh, never one shared book

`Core` keeps three separate `VenueBook`s and rebuilds the merged view from them, rather than maintaining one shared consolidated book that every update writes into.

**Removing an exchange stays cheap.** If one feed breaks and must be excluded, the merge simply skips it — one `if`. In a shared consolidated book its quantities are already mixed into every price level, so excluding it means walking the whole book and subtracting, then walking it again to add it back on recovery. A resync after a sequence gap is worse: the whole book is replaced.

**When the merge runs changed during implementation.** The original plan was to merge only at publish time, on the reasoning that updates arrive faster than we publish. That stopped applying once publishing became eager: **every depth update now triggers a full k-way merge immediately**, and the result is handed out as an immutable `shared_ptr<const Book>` snapshot. Publish time *is* update time; there is no separate publish clock to amortize against.

That is a deliberate trade, not an oversight. It is the simplest thing that is correct, it matches the same eager choice made for BBO, and at ~100 depth updates/sec against a merge estimated in low single-digit microseconds there is wide headroom. It is also unmeasured — see [Hot paths](#hot-paths-not-yet-optimized).

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

## Spot only

Spot BTCUSDT on all three exchanges. Spot and perpetual futures are different instruments at different prices, so mixing them into one book would be a correctness bug rather than a rounding artifact. This is checked at startup, not only documented. The exchange adapter interface supports futures without changes above it.

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

## Record and replay

**Not built.** The intent was a capture tool recording raw exchange frames with arrival timestamps, and a replay provider feeding them back through the same parsing path — giving repeatable tests for the book logic, sequencing state machines and band math, plus a docker-compose profile that runs with no network access at all.

What exists instead: unit tests drive the parsers, sequencing rules and band math directly with hand-built inputs, including real captured payloads pasted into the parser tests. That covers the logic but not timing, and it means nothing exercises the full pipeline deterministically.

---

# Hot paths not yet optimized

Everything here is a **known cost accepted deliberately**, not an oversight. None of it is measured — there is no benchmark in the project, which is the first thing to fix before optimizing any of it.

| Path | Cost | Fix, when a number justifies it |
|---|---|---|
| **Eager merge per depth update** (`Core::ApplyUpdate`) | Full k-way merge over up to `kDefaultMaxDepth` levels × 3 venues on **every** depth message, inside `apply_mutex_` | Throttle the merge to a fixed cadence, or merge incrementally. Estimated low single-digit µs against ~100 updates/sec — wide headroom, but unverified |
| **`apply_mutex_` in `Core`** | Three provider threads serialize on one lock; merge and publish happen inside the critical section | The SPSC-queue design in [Threading](#threading-and-ownership) — removes the lock from the book path entirely |
| **Band vectors in `PublishBook`** | Four `std::vector`s allocated per session per publish | Scratch buffers reused across publishes. Safe because it already runs under `sessions_mutex_` |
| **`sessions_mutex_` held across band math** | All per-session band computation happens inside the lock, blocking subscribe/unsubscribe | Snapshot the session list, compute outside the lock. Only matters with many clients |
| **`Book` snapshot allocation** | `AcquireBookBuffer` reuses buffers when `use_count() == 1`, but grows the pool if every buffer is still referenced | Bounded pool with a defined policy when exhausted. Currently assumes no slow subscriber |
| **Protobuf `Update` built per session** | Each subscriber gets its own serialized message, even when the payload is identical | Build once, share the serialized bytes across sessions with identical subscriptions |
| **Logging** | `fmt::print` from multiple provider threads can interleave mid-line; no level filtering, so `LogLevel::kDebug` is indistinguishable from `kInfo` at runtime | A level check that early-outs *before* formatting, plus a lock or a single logging thread |

---

# Known limitations

**Not built:**
- **Docker and docker-compose** — a required deliverable, missing.
- **Benchmarks.** No before-numbers exist, so every performance statement in this document is an estimate and is labelled as such.
- **Staleness policy** (§6): the watchdog, drift detector and admission rule. `VenueStatus` exists on the wire but is never populated — a stale venue still contributes silently.
- **The fast-BBO oracle** (§4.4): comparing the depth-derived BBO against the venues' own BBO channels to detect a desynced book. This matters more than planned, because OKX **deprecated its CRC32 checksum** (now fixed at 0), removing the only per-message integrity check any venue offered. Gap detection catches *missed* messages, not *misapplied* ones.
- **Record and replay**, and the offline compose profile that depended on it.
- **Graceful shutdown** — no signal handling; the process only stops on an external kill, which skips provider teardown.
- Unary `GetSnapshot`, and `throttle_ms`.

**Working, with caveats:**
- Single symbol in practice, though the code and protocol are parameterized for more.
- The 1000bps band cannot be covered by the public depth channels at any depth setting — no venue publishes 10% of book. Reported with `insufficient_depth` rather than presented as complete.
- Venue attribution is carried at *every* merged level but only exposed for the BBO. Surfacing it per band is a proto field and an accumulator away; the partial-fill level would need a stated attribution rule.
- No authentication or TLS on the gRPC connection.
- Exchange fees are not included in the consolidated prices. A production aggregator would need this, since taker fees are often larger than the spread between exchanges.
- No persistence.

# Consolidated Order Book Aggregator

Aggregates BTCUSDT spot market data from three exchanges (Binance, OKX, Bybit) into one consolidated order book, and publishes derived views to gRPC clients.

Four services:

| Service | What it does |
|---|---|
| `aggregator` | Connects to the three exchanges, builds the consolidated book, serves gRPC |
| `client-bbo` | Prints the consolidated best bid / offer |
| `client-volume-bands` | Prints VWAP for 1M / 5M / 10M / 25M / 50M+ USDT notional |
| `client-price-bands` | Prints liquidity within 50 / 100 / 200 / 500 / 1000bps of the BBO |

> **Status: design complete, implementation in progress.**
> Build and run sections are filled in as the code lands. The technical decisions below are final and are the part worth reading now.

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

**Three exchange threads parse. One consolidator thread owns all the books.**

Each exchange has its own thread doing WebSocket receive, TLS decrypt and JSON parse. The result is a normalized update pushed into a single-producer single-consumer (SPSC) ring buffer. The consolidator thread drains the queues, applies the updates to the three order books, merges them, calculates the derived views and publishes.

The important consequence: **there is no lock anywhere on the order book path.** No mutex, no seqlock, no atomics on book state. One thread owns the books, so nothing is shared. The book logic is single-threaded, which makes it deterministic, easy to test with fake input, and clean under ThreadSanitizer by construction instead of by being careful.

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

## Consolidation: merge at publish time, not per update

The consolidator keeps three separate order books and merges them when it publishes, rather than maintaining one shared consolidated book that every update writes into.

Two reasons.

**Removing an exchange stays cheap.** If one feed breaks and must be excluded, the merge simply skips it — one `if`. In a shared consolidated book its quantities are already mixed into every price level, so excluding it means walking the whole book and subtracting, then walking it again to add it back on recovery. A resync after a sequence gap is worse: the whole book is replaced.

**The cost follows the publish rate, not the feed rate.** Updates arrive faster than we publish, so merging per publish does strictly less work. It also means switching to tick-by-tick channels later does not change the publish cost at all.

The best bid and offer are the exception. They are cached per exchange and the consolidated BBO is a `max` and a `min` over three values, so that path stays cheap enough to run on every update.

## The consolidator wakes on updates, not on a timer

Any exchange update sets a dirty flag and signals the consolidator. It drains, works, publishes, and loops.

This is self-clocking. In a quiet market a single update wakes it and it publishes immediately, with no artificial delay. In a burst, the updates that arrive while it is working collapse into the next pass automatically. There is no fixed rate to tune. Clients that want less can set `throttle_ms` in the subscription.

## Slow clients: send the newest state, skip what was replaced

Each client session holds one slot. If a new snapshot arrives while the previous write is still in flight, the pending one is **replaced**, not queued. The consolidator never blocks and never grows memory because of a slow client.

This is safe because an order book is a **state**, not a list of events. If states S1, S2 and S3 were produced and only S3 is sent, the client's view is exactly as correct as if all three had been sent. S1 was already wrong by the time it reached the wire.

A trade feed would be different — trades are events and skipping one loses information permanently.

This choice forces a related one: **every message carries full state, not a delta.** A client that skipped a message cannot apply the next delta correctly. The BBO and both band messages are small, so full state costs almost nothing and makes skipping safe automatically. The raw depth feed does use deltas, so a client too slow for it is sent a fresh snapshot to restart from.

Every message carries a sequence number that increases by one on the server side, so a client can see exactly how many states it skipped. It is reported to stderr by the bundled clients and exported as a metric.

Note that Binance already does this to us: `depth@100ms` is a conflated stream. The real book changes thousands of times per second and we receive one grouped message per 100ms. We are applying the same idea one level further down.

A client can opt out per subscription. Then it gets a bounded queue, and if the queue fills it is disconnected with a clear error. There is no third option — a client that reads slower than the data arrives either skips states or disconnects. Buffering is just a delayed disconnect that serves wrong data in the meantime.

## Stale and disconnected exchanges

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

All prices and quantities are `int64` scaled by a fixed factor, both internally and on the wire, with the scale declared in the proto file. No floating point anywhere in the book or the protocol. Doubles accumulate error across VWAP sums and compare badly for price level identity.

The three exchanges have different tick sizes, so the internal price grid uses the smallest tick among the configured exchanges. Every exchange price then maps onto it exactly, with no rounding and therefore no rounding bias. An exchange whose tick does not divide evenly is rejected at startup rather than silently rounded.

## Protocol: one streaming call, not three

One `Subscribe` server-streaming RPC with a `oneof` payload, rather than a separate RPC per publisher type. A new derived view becomes one new enum value with no change to the service definition, and one client can take several feeds over a single connection.

Depth, band values and update rate are all subscription parameters. The values in the assignment (1M/5M/10M/25M/50M+, 50/100/200/500/1000bps) are **defaults**, not constants.

A unary `GetSnapshot` is also provided, for clients joining late or recovering.

The symbol is a field everywhere even though only BTCUSDT is used, so supporting more symbols needs no protocol change.

## Band definitions

Both had real ambiguity, so both interpretations are recorded here.

**Volume bands** are cumulative from the top of the book: the 5M band covers the first 5M USDT of notional, not the slice between 1M and 5M. Both prices are published — the **VWAP** (average price to fill that size) and the **worst price** (the last level reached) — along with filled quantity, filled notional, and a flag when the book runs out before the band is filled. Notional is in USDT, the quote currency.

**Price bands** are also cumulative, measured from the consolidated BBO because the assignment says "BBO+". The 100bps band includes everything from the BBO out to 100bps, which contains the 50bps band. Measuring from the mid price is the more common convention and behaves better when the spread is wide or the book is crossed, so it is available as a configuration flag.

The 1000bps band is 10% away from the BBO, which is deeper than the public depth channels reach. It will regularly report insufficient depth. That is expected behaviour and is flagged explicitly, never silently truncated.

## Spot only

Spot BTCUSDT on all three exchanges. Spot and perpetual futures are different instruments at different prices, so mixing them into one book would be a correctness bug rather than a rounding artifact. This is checked at startup, not only documented. The exchange adapter interface supports futures without changes above it.

## WebSocket for updates, REST only for the initial snapshot

All live updates arrive over WebSocket. REST is used only to fetch the starting snapshot, and again after a reconnect. This is not a preference — Binance's documented synchronization procedure requires the REST snapshot to bootstrap the diff stream.

Each exchange runs the same state machine: connect, subscribe, buffer updates, fetch the snapshot, discard buffered updates older than it, then go live. A sequence gap means the book is *wrong*, which is worse than stale, so it triggers an immediate resync with backoff and jitter to avoid a storm of snapshot requests.

The fast BBO channels (`bookTicker`, `bbo-tbt`, `orderbook.1`) are subscribed but their ticks are **never** written into the depth book — the two streams are not sequenced together, so mixing them would corrupt it. They are used only as a correctness check: if the book-derived BBO disagrees with the fast BBO stream for more than about 200ms, our book is wrong and a resync is forced.

OKX sends a CRC32 checksum over the top of its book. It is verified on every message, and it doubles as a property test over recorded data — if the book implementation has an off-by-one anywhere, this finds it.

## Record and replay

A capture tool records raw exchange frames with their arrival timestamps, and a replay provider feeds them back through the same parsing path.

This is what makes the book logic, the sequencing state machine, the staleness policy and the band calculations testable with repeatable results. It also gives docker-compose a profile that runs the entire system with no network access at all, which matters if the exchanges are unreachable from where this is being reviewed.

---

# Known limitations

- Single symbol in practice, though the code and protocol are parameterized for more.
- The 1000bps band cannot be fully covered by the public depth channels and is reported as best-effort with a flag.
- No authentication or TLS on the gRPC connection.
- Exchange fees are not included in the consolidated prices. A production aggregator would need this, since taker fees are often larger than the spread between exchanges.
- No persistence beyond the replay capture files.

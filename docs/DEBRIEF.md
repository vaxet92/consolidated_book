# Debrief preparation

Likely interviewer questions with short spoken answers. Every claim here is
traceable to code or to `docs/DESIGN.md`; where a number is not measured, it
says so.

**How to use this:** the answers are written to be *said*, not read. Two to
four sentences each. If a question needs more, the follow-up is in the linked
section rather than in the first answer.

**The three most valuable answers in this document are in §10** — the places
where the first design was wrong and was fixed. Interviewers trust "here is a
bug I found and what it taught me" far more than "everything went to plan."

---

## 0. The opening: "walk me through what you built"

> Three exchanges — Binance, OKX, Bybit — publish BTCUSDT spot order books
> over WebSocket. Each venue has its own provider that parses its feed,
> validates sequence continuity, and produces a normalised `BookUpdate`. A
> core component keeps one book per venue and merges them into a consolidated
> book with per-venue attribution. Derived views — BBO, volume bands, price
> bands — are computed from the merged book and published to clients over a
> single gRPC stream.
>
> The parts I would point at as the real engineering are: the per-venue
> sequencing rules, because all three venues do it differently; the staleness
> policy, because a frozen venue actively wins the merge; and the redundant
> connection design, because that is what makes a disconnect invisible.

Then stop. Let them pick the thread.

---

## 1. Architecture and threading

**Q: Why one process rather than a service per venue?**

> A process boundary between provider and core would serialize every book
> update to replace what is a pointer move in memory. For a three-venue,
> single-symbol aggregator that is the wrong trade. The isolation argument is
> real — one adapter crashing would not take the others down — and it wins at
> larger scale, so it is recorded as a rejected alternative rather than
> dismissed. The boundary that *is* needed already exists in the right place:
> aggregator to clients, over gRPC.

**Q: What is the parallelism axis?**

> The venue. One thread per venue does receive, TLS decrypt, and JSON parse —
> parsing is the bulk of the ingest cost and it parallelises with no
> coordination. I rejected a worker pool over one venue's stream: it breaks
> message ordering and forces a resequencer, which is real complexity bought
> for negative benefit.

**Q: Who owns the books?**

> The design is that one consolidator thread owns all three `VenueBook`s and
> venue threads reach it through per-venue SPSC queues, so there is no lock on
> the book path at all. **That is not what the code does today** — the queues
> are not built, and a mutex in `Core` guards the books instead. It is
> commented as interim at the declaration and flagged in `DESIGN.md §7.2`. I
> would rather say that than let you find it.

**Q: Why not a seqlock, with each venue owning its own book?**

> It parallelises the delta apply, which is the cheapest step — parsing was
> already parallel in both designs. And a seqlock is easy to write and hard to
> write *correctly*; a memory-ordering mistake gives you a silently wrong book
> at a low rate, which is the worst failure mode available. `shared_mutex` is
> worse still: readers stall the writer, and the writer is the one on the
> latency path.

**Q: You have mutexes. Isn't this HFT?**

> Three of them, and they are not equivalent. `Core::apply_mutex_` is on the
> per-message path and should go — but the fix is SPSC queues, not a lock-free
> book. **The best way to remove a lock is to remove the sharing, not to make
> the shared thing lock-free**; a lock-free shared book still bounces the same
> cache lines between cores. The `conflated_channel` condition variable should
> *stay*: its job is to let the gRPC writer thread sleep when idle, and
> replacing it with a spin would burn a core per client to avoid an
> uncontended lock.

---

## 2. Sequencing, gaps and resync

**Q: How do the three venues differ?**

> Completely, and nothing in the core knows about any of it.
>
> - **Binance** — differential only. Seed from a REST snapshot, buffer live
>   events while syncing, then reconcile: `U <= lastUpdateId + 1`, and after
>   that each event must satisfy `U == previous u + 1`.
> - **Bybit** — `u` increments by one. `u == 1` means the service restarted.
> - **OKX** — `prevSeqId` must equal the previous `seqId`. `seqId < prevSeqId`
>   is a documented maintenance reset and is **not** a gap.
>
> Three different rules, three different meanings for "the sequence went
> backwards". Comparing sequence numbers across venues is meaningless and
> never happens.

**Q: What happens on a gap?**

> A gap means the book is **wrong**, not merely stale — applying the next
> delta would silently corrupt it. So we drop the book and resynchronise:
> Binance re-fetches the REST snapshot, Bybit and OKX self-heal from their
> in-channel snapshot. The resync path deliberately does not consume the
> reconnect-attempt budget, because it is not a connection failure.

**Q: While a venue is resyncing, does its old book still contribute?**

> It did, and that was a real bug — found late, and worth describing because
> the cause is subtle. `RequestResync()` stops the sockets, but `Stop()` sets a
> flag that deliberately *suppresses* the socket-closed notification, so that a
> deliberate teardown is not mistaken for a connection failure and reconnected
> twice. The side effect was that the live-socket count never dropped, health
> stayed `LIVE`, and Core went on merging a book we had already decided was
> wrong — for the whole resync window, on every update from the other venues.
>
> **The lesson: connection state was blind at exactly the moment we were most
> certain the data was invalid.** The fix is a separate `RESYNCING` state,
> announced *before* the teardown, so the verdict does not depend on a signal
> we have just switched off. It is sticky — the watchdog leaves it alone,
> because a timer has nothing useful to say about a stream we turned off
> ourselves — and it is cleared by the first message after the venue returns.

---

## 3. Redundant connections and duplicate rejection

**Q: Why open N connections to the same stream?**

> Line arbitration — the same idea as an A/B feed. N sockets carry identical
> data, the first copy of each message wins, the rest are dropped. The point is
> not throughput, it is that one socket dying leaves N−1 still delivering, so
> there is no gap, no resync, and no REST snapshot. CME publishes duplicate
> feeds for exactly this reason.

**Q: How do you reject the duplicates?**

> A monotonic high-water mark. All six streams carry ids that only move
> forward, so `id <= last_seen` means "already delivered". That is about
> eighteen bytes of state and one integer comparison per message.

**Q: Why not a hash set of seen ids?**

> It works, but it costs eight to forty-six megabytes depending on the window,
> and — worse — it introduces a tuning constant: how long do you remember an
> id? Get that window wrong and you either leak memory or start admitting
> duplicates. The monotonic property makes the whole question disappear.

**Q: Why `<=` and not `!=`?**

> Because a lagging socket can be more than one message behind. Everything runs
> on one `io_context`, so a burst is delivered in batches and multi-message lag
> between sockets is routine, not exceptional. With `!=` a socket two messages
> behind would be treated as new data and replayed. That was a real bug in an
> earlier version and there is a regression test named after it.

**Q: How do you know which connections are actually delivering?**

> A bitmask — one bit per connection, rotated when the id advances. It gives
> per-connection liveness with no timers, and `popcount` on it says how many
> sockets contributed the last message. It also detects a filter that has got
> stuck: a thousand consecutive drops logs once, because a stuck filter drops
> *every* message and logging each one would bury the alarm in its own noise.

---

## 4. Staleness — the most likely deep-dive

**Q: What happens when a venue goes down?**

> It gets excluded from the merge, and the client is told. Reporting alone is
> not enough, and this is the part worth being precise about.

**Q: Why is excluding it so important? Isn't slightly old data better than no data?**

> No, and the reason is the merge itself. The merge takes `max(bid)` and
> `min(ask)`. A frozen venue never moves — so when the market falls it
> **always** looks like the best bid, and when it rises it always looks like
> the best ask. Staleness is not noise that averages out; **the merge actively
> selects for it**. One frozen venue out of three corrupts the output nearly
> every time the market moves.
>
> There is a test with real numbers: a frozen venue at 50000 against live
> venues at 49900 produces a *crossed* consolidated book — a phantom 90-tick
> arbitrage nobody can trade.

**Q: How do you detect it?**

> Four signals, in order of certainty.
>
> 1. **Connection state.** Every socket down is certain. But note the
>    asymmetry — a socket being *up* proves nothing, because TCP stays
>    `ESTABLISHED` while a publisher wedges. **Connection state can condemn a
>    venue, never clear one.**
> 2. **Venue keepalives.** Bybit republishes L1 with the same `u` after three
>    seconds of no change; OKX sends `seqId == prevSeqId` after about sixty.
>    Those make silence into evidence.
> 3. **Cross-venue comparison.** Not built yet.
> 4. **An absolute backstop timer**, as a last resort.

**Q: A timer can't tell a dead feed from a quiet market. How do you handle that?**

> That is the central problem, and the answer is that **a quiet market is a
> market-wide property while a dead feed is per-venue.** If all three venues
> are silent, the market is quiet and nobody is stale. If two are busy and one
> is silent, that one is broken. That is the disambiguation, and it needs no
> threshold tuning in the quiet case.
>
> It is designed and specified in `DESIGN.md §6.2b` and **not implemented** —
> it is the main gap in the staleness work.

**Q: Where do the thresholds come from?**

> Two are derived from behaviour the venue documents about itself — Bybit's
> three-second L1 republish and OKX's sixty-second keepalive. Those I can
> defend. The rest are placeholders and are labelled as placeholders in
> `types/venue.h`, because Binance publishes no keepalive at all, so its
> silence carries no information and the number can only come from
> measurement.
>
> All of them are deliberately generous, because **too short is the dangerous
> direction**: a backstop below a venue's real quiet interval marks a healthy
> feed stale and flaps it in and out of the merge, which is worse than having
> no watchdog.

**Q: Which clock?**

> Monotonic, for staleness only. Both readings come from the same never-jumping
> clock so the epoch cancels exactly. With a wall clock an NTP step *backwards*
> blinds the watchdog and a step *forwards* marks every venue stale at once and
> publishes an empty book that reads like a total exchange outage.
>
> There is a second, wall-clock stamp for drift against the exchange's own
> timestamp. One field cannot be both — the comment in the code originally
> claimed `CLOCK_MONOTONIC` while the code used the wall clock, which is how I
> found it.

**Q: Why not measure staleness as `our time − exchange timestamp`?**

> Because that difference is staleness **plus** an unknown clock offset **plus**
> network delay — three unknowns and one equation. Our clock and the venue's
> are not synchronised, and the venue's can even be ahead of ours. Use it as a
> watchdog and a venue whose clock runs slow looks permanently stale, while one
> whose clock runs fast never looks stale even when it is dead. It is fit for
> drift *estimation* only and must never gate admission.

**Q: Who decides — the core or the provider?**

> The provider, for its own two streams, and it pushes the verdict to the core.
> Four reasons, but the important one is that **the provider's timer fires even
> when no data arrives.** If the core only checked health on an incoming
> update, then all three venues going silent means nothing calls the core and
> nothing gets checked — which is the case that matters most.

**Q: Why does the notification travel with the data instead of in a side channel?**

> Because a side channel gives an inconsistent view of one venue. If health
> lived in an atomic the core read directly while updates came through a queue,
> the core would be excluding a venue whose updates it is still applying —
> real-time signal, delayed data. In-band, the core learns about the staleness
> at exactly the right point in that venue's timeline. Late, but *consistently*
> late. Correct by construction rather than by timing.

**Q: What about heartbeats — do they count as liveness?**

> Protocol pings and pongs, no. A pong proves the socket is open, not that the
> data is flowing — and a connected-but-silent venue is exactly the failure
> being hunted. The parser rejects them before the liveness stamp.
>
> Venue *keepalives* are different — a Bybit L1 republish is a real orderbook
> message on our subscription, so it counts.

**Q: Doesn't your duplicate filter throw those keepalives away?**

> It did, and that is a good catch — a keepalive carries an id we have already
> seen, so the filter correctly drops it as a book update and would have
> destroyed the liveness signal with it. The fix is placement: the liveness
> stamp is taken immediately after a successful parse and **before** the
> filter. A duplicate from a redundant connection updates the same stamp, which
> is right — for liveness, a duplicate and a keepalive are the same event: the
> venue put bytes on the wire.

**Q: Book and BBO staleness — same thing?**

> No, separate. They are separate sockets and fail independently, so each has
> its own stamp and its own verdict. The visible consequence, which has to be
> documented, is that the published BBO and the published Book can legitimately
> disagree about which venues are included.

**Q: The merged book needs no repair when a venue goes stale, but the BBO does. Why?**

> The merge is a full rebuild every pass, so changing the admission rule makes
> the next output correct with nothing to repair. The BBO is maintained
> incrementally and only ever holds the top level — so a stale venue's price is
> already inside it, and the venue has gone quiet, so nothing displaces it.
> Filtering its future quotes achieves nothing when it sends none. It needs a
> forced full rescan on the transition.
>
> **Stateless recomputation is self-healing; incremental state needs explicit
> invalidation.**

**Q: Why a version counter for that and not a flag?**

> Because health is per venue and the BBO is per instrument — one venue going
> stale invalidates every instrument's BBO. A per-instrument flag means looping
> over all instruments on every health change; a single global flag cannot be
> cleared correctly, because clearing it after the first instrument rescans
> leaves the rest stale forever. A counter says "the world changed at version
> N" once, and each instrument notices independently.

---

## 5. Consolidation and derived views

**Q: How is the merged book built?**

> A k-way merge across the admitted venues, rebuilt at publish time rather than
> maintained incrementally. Sorted vectors, not maps — it is written once and
> then read strictly in order, so a map's logarithmic lookup would be paid for
> and never used. Prefix sums are computed during the merge, which turns every
> band query into "find the crossing level, read two numbers, divide."

**Q: Two venues quote the same price. What happens?**

> One consolidated level with both venues attached, quantities summed, and
> per-venue attribution kept so the client can decide what to do with the tie.
> Attribution is a fixed array, not a vector — there can never be more
> contributors than venues, and a vector there would mean one heap allocation
> per level, roughly five hundred per merge.

**Q: The consolidated book is crossed. Bug?**

> No. With three venues, one venue's bid *will* sometimes exceed another's ask
> — that is genuine cross-venue arbitrage plus propagation delay. We publish it
> as-is with a `crossed` flag, because it is the true consolidated state and
> hiding it destroys exactly the signal an aggregation consumer wants.
> Uncrossing is documented as a rejected alternative.

**Q: How do you define a volume band?**

> VWAP to fill N USDT of notional, sweeping the consolidated book from the top
> and splitting the final level proportionally. The alternative reading — "the
> price at which cumulative notional crosses N" — is a strictly weaker
> statement and not what an execution consumer wants. Both readings are
> documented; I picked one and said why.

**Q: What if the book runs out before the band is filled?**

> It is reported as a partial fill with `insufficient_depth` set, which on
> BTCUSDT is a legitimate answer rather than an error — the wider bands are
> always truncated, because no venue publishes anywhere near ten percent of
> depth. The important part is that a truncated result is otherwise
> indistinguishable from a complete one, so the flag is not optional.

**Q: All five bands means five walks of the book?**

> One. The bands are nested — one million is a subset of five million and so
> on — so a single forward walk records each band's result as it crosses the
> threshold and keeps going. Same for the bps bands. There is a test asserting
> the single-pass result equals repeated single calls.

**Q: Floating point anywhere?**

> Nowhere. Prices and quantities are scaled `int64`, at 1e8. Band accumulation
> is done in `unsigned __int128` because fifty million USDT at the raw
> price × quantity scale is about 5e23, which overflows `uint64` and a double's
> exact-integer range alike. Every comparison — including tie detection at the
> best price — is exact integer arithmetic.

---

## 6. The API

**Q: Why one RPC instead of three?**

> A new derived view becomes a new `oneof` arm and a new enum value, with no
> service-definition churn and no new connection. It also lets one client
> subscribe to several feeds over a single stream, which the three demo
> binaries do not need but a real consumer would.

**Q: How does a client know the scale?**

> It is carried on every message as an exponent, not just documented in the
> proto. A client needs zero out-of-band knowledge to interpret a value, and
> varint encoding makes a small value cost one byte regardless of the declared
> width. Same reasoning as carrying `symbol` on every message despite there
> being one symbol today.

**Q: What happens if a client is slow?**

> It cannot back-pressure the book. Each session holds a depth-one pending slot
> with overwrite semantics — if a new snapshot arrives while the previous write
> is still in flight, the pending one is replaced, not queued. Nothing upstream
> ever blocks.
>
> The framing matters: **this is a state-publishing API, not an event log.**
> Dropping intermediate states is correct by design, and the `seq` field lets
> the client observe that it happened.

**Q: How is the merged book shared with N subscribers?**

> One immutable snapshot behind a `shared_ptr`. Fan-out is N refcount bumps and
> zero copying, and the merge and band math happen once, not once per
> subscriber. Buffers are recycled through a free list, reused only when the
> refcount says no subscriber still holds them.

---

## 7. Performance — be careful here

**What is actually measured:**

- **Clock read cost.** `steady_clock::now()` at about 14 ns, `system_clock` at
  about 13 ns, against a 0.22 ns empty-loop baseline. Twenty million
  iterations, `-O2`, Apple M4 Pro. This is why the second timestamp per
  message was not worth avoiding.
- **A Binance parser benchmark** exists under `-DBUILD_BENCHMARKS=ON`.

**What is NOT measured — say so plainly if asked:**

- Delta apply cost, consolidator saturation point, and queue hop latency.
  `DESIGN.md §7.2` quotes figures for all three. **None of them are measured by
  anything in the repo.** If asked "how did you get 1–3 µs?", the honest answer
  is that it is an estimate carried over from the design document and not yet
  verified.
- Whether `std::map` is actually the bottleneck in the per-venue book. This is
  the open question, and it is why the flat-vector rewrite has not been done —
  there is no before-number to justify it.

**Q: What would you optimise first?**

> Nothing, until I have measured. The parser was the obvious target and it is
> already the one thing that was optimised — the `simdjson` on-demand parser is
> now constructed once per provider instead of once per message. The next step
> is benchmarking `ApplyUpdate` and `MergeBooks`, because that is what decides
> whether replacing `std::map` is worth doing at all.

**Q: Isn't rebuilding the whole merged book on every update wasteful?**

> Possibly, and it is marked as provisional pending that benchmark. But it buys
> something real: because the merge is stateless, a policy change like
> excluding a stale venue needs no repair step — the next output is already
> correct. An incrementally maintained merged book would need explicit
> invalidation, which is exactly the complexity the BBO path has and the book
> path does not.

**Q: So why not merge incrementally, the way you do the BBO?**

> Three reasons, and the first is the one that decides it. Every level carries
> a running prefix sum, so a change at level *i* invalidates every sum from *i*
> to the end — and the common case is a change at the *top*, which invalidates
> all fourteen hundred. Incremental merging avoids the k-way selection but not
> the accumulation, which is a linear scan over contiguous memory and probably
> the cheap part already.
>
> Second, it forfeits the staleness property: excluding a venue would mean
> unwinding its contribution to every level, and the only sane implementation
> of that is a full rebuild.
>
> Third — and this is why I have not done it — I suspect the merge's real cost
> is iterating three `std::map`s, roughly forty-four hundred red-black-tree
> steps per merge, each a pointer chase out of cache. If that is right, then
> replacing the map with a sorted vector speeds the merge up more than making
> it incremental would, and might make incremental unnecessary. That is a
> hypothesis with a benchmark attached, not a conclusion.

---

## 8. Scaling

**Q: How would this scale to a thousand symbols?**

> Shard by `(symbol, market_type)`. Instruments never reference each other, so
> it is embarrassingly parallel — no cross-shard locking. **Venue must not be
> part of the key**, because merging across venues for one symbol is the entire
> job; if Binance-BTCUSDT and OKX-BTCUSDT landed in different shards there
> would be nothing left to consolidate.

**Q: A thousand symbols times three venues times two streams is a lot of sockets.**

> It would be, if you opened one connection per symbol — but all three venues
> multiplex many symbols on one connection. So sockets scale with venues and
> with how you *bucket* symbols, not with how many symbols there are. A hundred
> symbols in ten buckets is a hundred and eighty sockets, not eighteen hundred.
> The trade-off is that one socket carrying a hundred symbols makes its parse
> thread the bottleneck, so bucket size is a tuning knob set by measurement.

**Q: What if the providers run in different data centres?**

> Then physics dominates and none of the software matters. New York to Tokyo is
> roughly 65 ms one way in fiber, so a Tokyo provider feeding a New York
> aggregator delivers data that is already 35 ms old on arrival — optimising a
> one-microsecond hop against that is a thirty-five-thousand-fold mismatch.
>
> What it forces: put the aggregator next to the *venues*, not the clients, and
> ship the derived output across regions rather than the raw feed. The gRPC
> `Update` message already is that cross-region payload — a few hundred bytes
> against megabytes per second of raw depth.

**Q: Shared memory between processes?**

> Faster than a socket, but three constraints decide whether it is written
> correctly. No pointers — the same page maps at different virtual addresses,
> so everything is an offset from the segment base. `BookUpdate` cannot cross
> as it stands, because it holds `std::vector`s whose heap pointers mean nothing
> to the consumer. And lifetime needs a protocol: a ring of fixed slots where
> the consumer validates the sequence *after* reading, so it can detect being
> lapped.

---

## 9. Testing

**Q: How do you test any of this without a network?**

> `md_core` has no I/O and no networking, and no clock — that is deliberate and
> it is what makes the logic testable. Staleness takes `now` as a parameter
> rather than reading a clock, so every staleness test is three integers and an
> expected result: no sleeping, no injected fake clock.

**Q: How do you know the optimised paths are right?**

> Oracle tests. The `std::map` book is kept permanently as the reference, and
> the incremental BBO is checked against a full rescan over a randomised
> stream. The incremental path is the dangerous one — a bug in it does not fail
> loudly, it accumulates silently across thousands of updates.

**Q: Anything unusual in the test suite?**

> A few tests assert *broken* behaviour on purpose, with a comment saying why.
> The clearest one shows that filtering a stale venue's future quotes cannot
> remove a price already folded into the incremental BBO — it asserts the price
> is still there, then shows the rescan removing it. It documents the reason
> the rescan exists, and it will fail loudly if someone ever makes the
> incremental path handle transitions.

---

## 10. The three best answers you have

Interviewers weight these heavily. Each is a real bug, found in this project,
with a lesson.

### 10.1 The redundant-connection design manufactured a sequence gap

Live log: `[BYBIT] depth gap: expected u=42348956, got 42348963`.

> Each socket was told at *creation* whether to honour its opening snapshot as
> a sequence reset. But all N sockets are created in a tight loop before any of
> them connect, so creation order says nothing about connection order. A socket
> that connected last honoured a stale snapshot as a reset, dragged the
> high-water mark backwards, and the next healthy message then read as a
> forward gap.
>
> **The lesson: "is this snapshot newer than what I hold?" is a property of the
> data, not of the socket.** The fix deleted the flag entirely and narrowed
> resets to genuine backwards moves — Bybit's `u == 1`, OKX's documented
> maintenance reset. The design got *simpler*: one flag and one concept
> removed.

### 10.2 A comment claimed one clock while the code used another

> `recv_ts_ns` was documented as `CLOCK_MONOTONIC` and filled from
> `system_clock`. And the comment contradicted itself — it also said the field
> was used for drift against the exchange timestamp, which only makes sense in
> wall-clock terms.
>
> **The lesson: one field cannot answer two questions.** Staleness needs a
> clock that never jumps; drift needs a clock comparable to the venue's. Now
> there are two stamps, each with one job.

### 10.3 A watchdog timer that would have stopped reconnection

> Adding the health timer to the provider's `io_context` broke reconnection,
> and I caught it before it ran. `io_context::run()` returns only when no work
> remains — and a repeating timer is work forever. After a total disconnect,
> `run()` would never return, the teardown-and-reconnect path would never
> execute, and a dead venue would have stayed dead for the life of the process.
>
> **The lesson: adding a periodic task to an event loop changes when that loop
> terminates.** The fix is that the timer does not rearm once every socket is
> gone — it reports the outage and then gets out of the way.

If asked "what was the hardest bug?", 10.1 is the answer: it only appeared
under live multi-connection load, the symptom pointed at the wrong subsystem,
and the fix removed code rather than adding it.

---

## 11. What is not done — say it before they find it

Volunteering this is worth more than being caught by it.

- **SPSC queues are not built.** `DESIGN.md §7.2` describes a lock-free book
  path; the code has a mutex. Flagged in the doc and at the declaration.
- **Cross-venue corroboration is not built.** It is the main staleness gap and
  the only sub-minute signal for Binance.
- **`VenueStatus` is on the wire and never populated.** Bands and clients were
  finished before the staleness policy, which is how that happened.
- **No hysteresis.** Recovery from stale waits for the next watchdog tick — a
  crude stand-in.
- **No signal handling.** The process stops only on an external kill, which
  skips `Stop()`.
- **No per-session reconnect.** One dead socket out of N is not replaced while
  the others live.
- **Record-and-replay was dropped deliberately**, not forgotten — `md_core`'s
  no-I/O rule already gives deterministic testing for the logic that matters.
- **Most performance numbers in `DESIGN.md §7.2` are unmeasured estimates.**

**Q: If you had another week?**

> Benchmark `ApplyUpdate` and `MergeBooks` first, because several claims in my
> own design document are unverified and one architectural decision — whether
> to replace `std::map` — depends entirely on that number. Then the SPSC
> queues, because that is what makes the code match the design. Then
> cross-venue staleness. In that order, because the first one tells me whether
> the third is even on the right path.

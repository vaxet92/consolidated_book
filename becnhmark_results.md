<!-- [feat/ob_aggregator] consolidated_book % ./build-bench/benchmarks/bench_md_core 5000

md_core order book latency  (iterations=5000, warmup=1000)
fixture: binance=1000 bybit=1000 okx=400  ->  1000 merged levels (venues share one tick grid)
prices x1e8 around 5000000000000, tick 1000000

MapOrderBook::ApplyUpdate
  qty_update_5      min     41.0  median     83.0  p99     125.0  mean    112.4  ns/call  (checksum 12000)
  qty_update_50     min    625.0  median    792.0  p99     875.0  mean    844.4  ns/call  (checksum 12000)
  churn_20x2        min   2916.0  median   3333.0  p99    4750.0  mean   3567.3  ns/call  (checksum 6000000)

MergeBooks - venue count at constant 1000 OUTPUT levels
  merge_1venue      min   6583.0  median   7250.0  p99   14292.0  mean   7650.5  ns/call  (checksum 6000000)
  merge_2venue      min   7250.0  median   7792.0  p99   10917.0  mean   8096.7  ns/call  (checksum 6000000)
  merge_3venue      min   8792.0  median   9542.0  p99   14083.0  mean   9935.8  ns/call  (checksum 6000000)

MergeBooks - realistic depths (Binance 1000 / OKX 400 / Bybit 1000), by max_depth
  merge_depth_50    min    333.0  median    416.0  p99     541.0  mean    432.2  ns/call  (checksum 300000)
  merge_depth_400   min   3458.0  median   3875.0  p99    5625.0  mean   4041.6  ns/call  (checksum 2400000)
  merge_full        min   7916.0  median   8500.0  p99   12250.0  mean   8878.2  ns/call  (checksum 6000000)

Tree traversal alone (no merge logic)
  iterate_only      min   8333.0  median   9000.0  p99   13083.0  mean   9251.8  ns/call  (checksum 14879970041821894688)

BBO
  bbo_incremental   min      0.0  median      0.0  p99      42.0  mean     32.4  ns/call  (checksum 29999994000000000)
  bbo_fullscan      min     42.0  median     83.0  p99     125.0  mean    106.1  ns/call  (checksum 60000000000000000)

NOTE: this loop keeps the tree nodes HOT in cache. In production there are
milliseconds between merges and those nodes may be evicted, so these numbers
UNDERSTATE std::map's disadvantage - the real gap is at least this wide.
Multiply by the measured message rate before drawing any conclusion. -->


<!-- hint insert -->
<!-- [feat/ob_aggregator] consolidated_book % ./build-bench/benchmarks/bench_md_core 5000        

md_core order book latency  (iterations=5000, warmup=1000)
fixture: binance=1000 bybit=1000 okx=400  ->  1000 merged levels (venues share one tick grid)
prices x1e8 around 5000000000000, tick 1000000

MapOrderBook::ApplyUpdate
  qty_update_5      min      0.0  median     42.0  p99      84.0  mean     94.5  ns/call  (checksum 12000)
  qty_update_50     min    458.0  median    542.0  p99     708.0  mean    603.4  ns/call  (checksum 12000)
  churn_20x2        min   3166.0  median   3667.0  p99    9375.0  mean   3944.8  ns/call  (checksum 6000000)

MergeBooks - venue count at constant 1000 OUTPUT levels
  merge_1venue      min   6375.0  median   7416.0  p99   13667.0  mean   7601.1  ns/call  (checksum 6000000)
  merge_2venue      min   7166.0  median   7667.0  p99   10875.0  mean   7969.6  ns/call  (checksum 6000000)
  merge_3venue      min   8708.0  median   9291.0  p99   13250.0  mean   9681.7  ns/call  (checksum 6000000)

MergeBooks - realistic depths (Binance 1000 / OKX 400 / Bybit 1000), by max_depth
  merge_depth_50    min    291.0  median    375.0  p99     500.0  mean    384.8  ns/call  (checksum 300000)
  merge_depth_400   min   3500.0  median   4166.0  p99    5583.0  mean   4151.8  ns/call  (checksum 2400000)
  merge_full        min   7917.0  median   8250.0  p99   11750.0  mean   8637.0  ns/call  (checksum 6000000)

Tree traversal alone (no merge logic)
  iterate_only      min   8292.0  median   9042.0  p99   13292.0  mean   9317.2  ns/call  (checksum 14879970041821894688)

BBO
  bbo_incremental   min      0.0  median      0.0  p99      42.0  mean     32.2  ns/call  (checksum 29999994000000000)
  bbo_fullscan      min     83.0  median     83.0  p99     125.0  mean    104.5  ns/call  (checksum 60000000000000000) -->

<!-- refactroing -->

<!-- [feat/ob_aggregator] consolidated_book % ./build-bench/benchmarks/bench_md_core 5000 
md_core order book latency  (iterations=5000, warmup=1000)
fixture: binance=1000 bybit=1000 okx=400  ->  1000 merged levels (venues share one tick grid)
prices x1e8 around 5000000000000, tick 1000000

MapOrderBook::ApplyUpdate
  qty_update_5      min      0.0  median     41.0  p99      42.0  mean     57.8  ns/call  (checksum 12000)
  qty_update_50     min    250.0  median    333.0  p99     334.0  mean    354.5  ns/call  (checksum 12000)
  churn_20x2        min   2292.0  median   2583.0  p99    4792.0  mean   2777.3  ns/call  (checksum 6000000)

MergeBooks - venue count at constant 1000 OUTPUT levels
  merge_1venue      min   6583.0  median   7417.0  p99   10084.0  mean   7566.3  ns/call  (checksum 6000000)
  merge_2venue      min   7583.0  median   8167.0  p99   11541.0  mean   8434.4  ns/call  (checksum 6000000)
  merge_3venue      min   9375.0  median  10667.0  p99   15458.0  mean  10621.0  ns/call  (checksum 6000000)

MergeBooks - realistic depths (Binance 1000 / OKX 400 / Bybit 1000), by max_depth
  merge_depth_50    min    416.0  median    459.0  p99     583.0  mean    489.9  ns/call  (checksum 300000)
  merge_depth_400   min   3667.0  median   4083.0  p99    6375.0  mean   4271.5  ns/call  (checksum 2400000)
  merge_full        min   8791.0  median  10000.0  p99   13750.0  mean   9967.8  ns/call  (checksum 6000000)

Tree traversal alone (no merge logic)
  iterate_only      min   9791.0  median  10417.0  p99   14959.0  mean  10931.6  ns/call  (checksum 14879970041821894688)

BBO
  bbo_incremental   min      0.0  median      0.0  p99      42.0  mean     32.4  ns/call  (checksum 29999994000000000)
  bbo_fullscan      min     41.0  median     83.0  p99     125.0  mean    105.0  ns/call  (checksum 60000000000000000)

NOTE: this loop keeps the tree nodes HOT in cache. In production there are
milliseconds between merges and those nodes may be evicted, so these numbers
UNDERSTATE std::map's disadvantage - the real gap is at least this wide.
Multiply by the measured message rate before drawing any conclusion. -->

---

## 2026-09-03 — venue-slot migration (DESIGN.md §17.6): regression found and fixed

Recorded because CLAUDE.md §7 requires a before/after for any change on a
measured path, and because the first attempt made things ~40% worse.

### Read the RATIO, not the absolute number

The two runs below are days apart on the same laptop, and the machine was not
in the same state. `iterate_only` is the control: it contains **none** of these
changes, so whatever it does between runs is drift, not code.

    iterate_only    9.0 us (baseline run)  ->  10.4 us (today)     +16%, unchanged code
    qty_update_50   542 ns (baseline run)  ->   333 ns (today)     -39%, unchanged code

Today's machine is slower on long tree traversals and faster on small map
operations. Comparing raw microseconds across the two runs would have mixed a
real regression with that drift, and no amount of staring at either number
alone separates them.

    ratio = merge_X median / iterate_only median, same run

### The regression

Attribution on each merged level was changed from `static_cast<VenueId>(i)` to
`books[i]->venue()`, so that a venue's identity comes from the book rather than
from its slot index (needed once slot != VenueId). Correct, but placed inside
the **per-level** loop:

    ~1000 output levels x 3 venues = ~3000 lookups per merge
    each one: unique_ptr load -> member load on a separate heap object
    whose header is evicted while the maps are being traversed

    merge_full ratio    0.94 (baseline)  ->  1.36 (regressed)
    absolute            8.5 us           ->  ~13.5 us
    cost                ~3.5-4 us per merge   (est.: drift-corrected)

### The fix

The venue occupying a slot cannot change during a merge, so this is a loop
invariant. Read once into a `std::array<VenueId, kMaxVenues>` in the setup loop
that already builds `it[]`, `end[]` and `active[]`, then index that array per
level. Same cost as the original cast.

### After (3 runs, medians, 5000 iterations)

    merge_1venue   0.81 (baseline)  ->  0.71
    merge_2venue   0.87            ->  0.78
    merge_3venue   1.06            ->  1.02
    merge_full     0.94            ->  0.96      <- the one that regressed

Every merge is at or below its baseline ratio. No regression remains.

### Also changed in the same step, with no measurable effect

- `MergeBooks` now takes `venue_count` and every per-venue loop runs to it
  instead of to `kVenueCount`. Before this, a venue registered beyond the enum
  count would register successfully and then never appear in the output - no
  error, just a venue silently missing from the merge.
- Per-venue arrays are sized `kMaxVenues` (8) instead of `kVenueCount` (3).
  Capacity only; the loop bound is what the benchmark sees.

Call sites deliberately pass `kVenueCount` (3), NOT each fixture's real venue
count. `merge_1venue` fills one venue but loops to three; passing 1 would make
it faster and the before/after comparison meaningless. Tightening those bounds
is a separate change and needs its own number.

### Still not measured

- Parse cost. A 1000-level snapshot is reported at ~140 us, but that number is
  not produced here. `bench_binance_parser` exists - run it and record the
  figure with the message size it was measured at.
- Anything above one instrument. Every number in this file is single-symbol.
  DESIGN.md §17.9 estimates 10 instruments at ~2% of a core from these figures
  and flags that the estimate is optimistic, because the working set grows
  linearly and these loops keep the tree nodes hot.

---

## 2026-09-05 - SPSC queues replace `apply_mutex_` (§14.2 step 12)

Provider threads no longer touch the books. They push a `ProviderMessage` into
a per-venue SPSC ring; one consolidator thread drains every ring and owns all
`MapOrderBook`s. `Core::apply_mutex_` is deleted.

### End-to-end publish latency, live, 1000 samples each

Measured by `LatencyRecorder("book_publish")`: provider stamp
(`BookUpdate::recv_mono_ns`) to book published. Both runs are the real
aggregator against Binance/Bybit/OKX at `--depth=100`.

| | mutex (42ea2ae) | queued (no mutex) | change |
|---|---|---|---|
| median   | **76.1 us**  | **113.3 us** | **+49%** |
| p99      | 286.8 us     | 376.1 us     | +31% |
| max      | 529.7 us     | 1450.4 us    | 2.7x |
| mean     | 88.1 us      | 122.2 us     | +39% |

Component breakdown, same runs:

| | mutex | queued |
|---|---|---|
| `lock_wait` med / p99 / max | 0.1 / 18.2 / 176.8 us | **0.0 / 0.0 / 0.0 us** |
| `book_apply` med            | 7.6 us  | 7.7 us  |
| `merge` med                 | 63.1 us | 64.5 us |

### The finding: removing an uncontended mutex made it slower

`book_apply` and `merge` are unchanged - the same `Process*` code runs either
way - so the entire +37 us is the handoff.

The mutex was **uncontended**: median wait 0.1 us. There was nothing to win.
What replaced it is a thread handoff: the producer pushes to the ring (~200 ns)
and then has to WAKE A SLEEPING THREAD, which is a futex syscall plus a
scheduler round trip - tens of microseconds.

At ~30 messages/sec the consolidator is asleep when almost every message
arrives, so nearly every message pays that wakeup. DESIGN.md §7.2 argued the
queue costs "a ~200 ns queue hop"; that is true of the ring itself and ignores
the wakeup. The ring is cheap. Waking a sleeping thread is not.

This is a LOW-RATE penalty, not a throughput penalty. Under sustained load the
consolidator stays awake and draining, and the wakeup disappears.

### Why the change was still made

Not for speed. §7.2 chose per-venue SPSC queues so the book path is
single-threaded: deterministic, testable with fake input, and TSan-clean by
construction rather than by careful locking. Those properties hold and are
covered by `CoreQueuedPathTest.QueuedPathMatchesSynchronousPath`, which runs
the same input through both paths and compares published books level by level,
including per-level slot attribution.

The cost is 37 us on a path whose slowest venue throttles at 100 ms - three
orders of magnitude larger.

### Not measured / open

- **Spin before sleeping.** The consolidator currently calls
  `condvar.wait_for` immediately after an empty drain. A short spin
  (~20-50 us) before sleeping should catch the "next message is close behind"
  case without a syscall and recover most of the 37 us. At 30 msg/sec a 50 us
  spin is ~0.15% of one core. NOT built, NOT measured.
- The producer-side spin (`kEnqueueSpinAttempts = 64`) only fires when a ring
  is **full**, which never happened in these runs (`OverflowCount` stayed 0).
  It protects a path that does not run; the path that runs every message
  sleeps unconditionally.
- **Rigour caveat.** These are two live runs at different times against a
  moving market, not a controlled comparison. `delta_levels` averaged 11.5
  (mutex) vs 8.0 (queued) and `merged_depth_peak` 850 vs 717. The near-equal
  `book_apply`/`merge` medians say the workloads were close, but the exact
  +49% should not be quoted to three digits - the direction and rough
  magnitude are what hold.

## 2026-09-05 - MergedLevel attribution: out-of-line layout REJECTED

Attribution is 73% of `MergedLevel` and the band walks
(`FillToNotionalBands`, `FillToBpsBands`) never read it - they read only
`price`, `cum_qty`, `cum_notional`. Moving it to a packed `Book::attribution`
array shrank the struct from 176 to 48 bytes (343 KB -> 94 KB for a
1000-level book, both sides).

`merge_full / iterate_only`, 3-run medians, `iterate_only` as the unchanged
control:

| layout | `sizeof(MergedLevel)` | ratio | merge |
|---|---|---|---|
| inline, `kVenueCount` (3) - the buggy original | 96 B  | 0.92 | ~9.2 us |
| inline, `kMaxVenues` (8) + bounds guard        | 176 B | **1.07-1.13** | ~11 us |
| out of line, packed side array                 | 48 B  | 1.57 | ~15.8 us |

**Reverted.** The 3.7x smaller struct cost 40% on the merge.
`out.attribution.push_back(...)` per contributor costs a size+capacity load, a
branch, a store to a SECOND write stream, and a size bump - about 3000 times
per merge. The inline store wins because the level was written microseconds
ago and is still in L1.

The predicted gain was always in the band walk, and **nothing measures the
band walk** - `bench_md_core` covers merge and tree traversal only. So the
loss is measured and the gain is not. Building that benchmark is the only way
to settle it; until then the inline layout stands.

### The bug that started it (fixed, kept fixed)

`MergedLevel::venues` was `std::array<VenueQuote, kVenueCount>` (3) while the
merge loop is bounded by Core's runtime venue count, which reaches
`kMaxVenues` (8). A fourth venue quoting the same price wrote past the end of
the array into the next `MergedLevel` - silent heap corruption, latent only
because exactly three venues run. Fixed by sizing the array `kMaxVenues` AND
adding a runtime bounds guard at the write site, because the two constants had
already drifted apart once.

## 2026-09-05 - consolidator spins before sleeping

`ConsolidatorLoop` used to call `condvar.wait_for` the moment a drain came
back empty, so nearly every message had to wake a descheduled thread. It now
spins (`kConsolidatorSpinLimit` CpuPause iterations) first and only sleeps
when nothing arrives for the whole window.

Same live measurement as above - `book_publish`, 1000 samples, 3 venues,
`--depth=100`:

| | mutex (42ea2ae) | queue + sleep | **queue + spin** |
|---|---|---|---|
| median | 76.1 us  | 113.3 us | **73.8 us** |
| p99    | 286.8 us | 376.1 us | 337.7 us |
| mean   | 88.1 us  | 122.2 us | 114.3 us |
| max    | 529.7 us | 1450.4 us | **15117 us** |
| `lock_wait` med/p99 | 0.1 / 18.2 us | 0.0 / 0.0 | 0.0 / 0.0 |
| `book_apply` med | 7.6 us | 7.7 us | 5.8 us |
| `merge` med | 63.1 us | 64.5 us | 50.9 us |

**The median regression is gone.** 113.3 -> 73.8 us, marginally better than the
mutex version, with no lock anywhere on the message path. Under live load the
consolidator never reaches the sleep, so producers never touch
`doorbell_mutex_` and the consumer never makes a syscall.

### CPU cost - measured, and lower than expected

~15-20% of one core (process CPU time 28.55s -> 29.16s over 3s wall). NOT the
100% a permanent spin would cost, because the spin window is finite: during a
genuine gap it exhausts and the thread sleeps. A dead feed costs ~0%, which is
the 24/7 requirement.

### The outlier was COLD START - confirmed, then removed

`max` was 15.1 ms with `merge` max at 14.96 ms, 300x the 50 us median. The
hypothesis was that the first merges are not representative: the Book's level
vectors grow from empty to ~1500 entries with several reallocations, the book
buffer pool allocates its first buffer, and the std::map books take a node
allocation per price for a fresh 1000-level Binance snapshot.

Tested by discarding the first 200 samples (`LatencyRecorder`/
`TimingBreakdown` now take a `warmup` count, and print how many were
discarded so nothing goes missing silently):

| | spin, no warmup | spin, warmup=200 |
|---|---|---|
| median | 73.8 us | **73.1 us** (unchanged) |
| p99    | 337.7 us | 400.5 us |
| max    | **15117 us** | **1304 us** |
| `merge` max | 14962 us | 1212 us |

**11.6x reduction in max, median unchanged.** The outlier was startup cost, not
scheduler preemption and not a cost of spinning. The median comparisons in the
table above are unaffected and stand.

CAVEAT: only this run discards warm-up. The mutex and sleep runs above do not,
so their maxima are not comparable with this one - they simply did not happen
to catch a cold-start sample as extreme. Medians across all four runs are
comparable; maxima are only comparable within the warm-up-discarding runs.
Re-running the mutex baseline with `warmup` set would fix that and has not
been done.

### Sizing, and what is measured

- MEASURED: a bare `CpuPause` is **0.66 ns** on this machine (Apple M4, stable
  from 10k to 1M iterations). 200k bare pauses is ~132 us.
- NOT MEASURED: each spin iteration also runs an empty `DrainOnce()`, which
  dominates the pause. The real window is in the low milliseconds; the
  per-iteration cost has not been isolated.
- The sizing RULE is what matters: the spin must exceed the expected gap
  between messages. Measured aggregate input is ~240 msg/sec across 3 venues
  (~215 BBO + ~25 depth publishes/sec), a ~4.2 ms gap; spot+futures across 8
  venues would be ~1300 msg/sec, ~0.8 ms. Below the gap, every message pays
  the ~37 us wakeup; above it, none do.
- `kConsolidatorSpinLimit` is one knob: raise toward infinity for a pinned-core
  HFT spin that never sleeps, set to 0 for the sleep-immediately behaviour.

---

## 2026-09-05 - FlatOrderBook vs MapOrderBook (§14.2 step 16): built, measured, NOT yet shippable

Both implementations now live in the tree and are benchmarked **in the same
process, in the same run**. That is deliberate: the 2026-09-03 entry above
exists because two runs days apart mixed a real 40% regression with machine
drift. Comparing map against flat inside one process removes the problem
instead of correcting for it - there is no drift between two arms of the same
loop.

Fixture unchanged: binance=1000, bybit=1000, okx=400, ~1000 merged levels,
20000 iterations, warmup=1000. Medians.

### The numbers

| | map | flat | ratio |
|---|---|---|---|
| `iterate_only` | 10166 ns | **584 ns** | **0.057 (17.4x faster)** |
| `merge_full` | 11041 | 9292 | 0.84 |
| `merge_depth_400` | 4917 | 4166 | 0.85 |
| `merge_depth_50` | 500 | 500 | 1.00 |
| `merge_1venue` | 8250 | 7167 | 0.87 |
| `merge_2venue` | 10042 | 8792 | 0.88 |
| `merge_3venue` | 12000 | 10625 | 0.89 |
| `qty_update_5` | 41 | 1500 | **36x SLOWER** |
| `qty_update_50` | 292 | 1500 | **5.1x SLOWER** |
| `churn_20x2` | 2167 | 2708 | 1.25x slower |

Bytes memmoved per diff message, flat book only:

    flat_qty_update_50    32000 bytes    mean = median = p99 = max
    flat_churn_20x2       63360 bytes    mean = median = p99 = max

### The inference that was WRONG, and how the experiment caught it

The entry above this one reported `merge_full` 8500 ns against `iterate_only`
9000 ns and concluded: **the merge is ~100% tree traversal**, therefore a flat
vector should remove most of it.

That conclusion was false, and this run proves it. Traversal fell by 94%.
The merge fell by 16%.

    if merge = traversal + logic, then
      map:   11041 = 10166 traversal +  875 logic
      flat:   9292 =   584 traversal + 8708 logic     <- these disagree by 10x

The model does not hold, so the premise was wrong. Two costs being roughly
equal is not evidence that one causes the other - the ratio was equally
consistent with "the merge IS traversal" and with "the merge is something else
that happens to cost about the same". Only the intervention separated them.

KEY: this is the case for CLAUDE.md section 7. The reasoning was plausible,
was written down as a conclusion, and was wrong. Nothing but building the
alternative and measuring it would have found that.

### Where the merge time actually goes: writing the output

`MergedLevel` is **176 bytes**, and that is derived, not guessed:

    price            8
    venues[8]      128   <- 8 x VenueQuote{VenueSlot, QtyUnits}, 16 bytes each
    venue_count      1   (padded)
    cum_qty          8
    cum_notional    16   (unsigned __int128, 16-byte aligned)
                   ---
                   176

`merge_full` writes ~1000 of them per side:

    output written   ~352 KB per merge     2 x 1000 x 176
    input read        ~77 KB per merge     2 x (1000 + 1000 + 400) x 16, flat

The merge is **write-bound, roughly 4.6:1**. The flat book made the input side
nearly free and left the dominant term untouched, which is exactly the 16% it
delivered.

Of those 176 bytes, **128 are the attribution array**, sized `kMaxVenues` (8),
of which 3 entries are ever populated today.

### Consequence for the 2026-09-05 "out-of-line layout REJECTED" entry

That entry measured MergedLevel 176 -> 48 bytes as **40% SLOWER**
(ratio 1.13 -> 1.57). It was measured with `std::map` input, when traversal
cost 10166 ns and dominated everything else in the loop.

Traversal now costs 584 ns. The conditions that produced that result no longer
exist, so **the result no longer applies and the experiment should be re-run**.
It is not being reversed here - it is being marked as measured under
assumptions that have since changed. This is the highest-value remaining
experiment, because it attacks the term that actually dominates.

### Verdict: this version is NOT shippable

`flat_qty_update_5` and `flat_qty_update_50` have the **identical median of
1500 ns**. A 5-level delta costs exactly what a 50-level delta costs. The bytes
column says the same thing without statistics: 32000 bytes every single time,
mean = median = p99 = max, which is precisely 2 sides x 1000 levels x 16 bytes
- the whole book, rewritten to change five quantities.

This first version applies a delta by merging the whole side into a scratch
buffer and swapping. Cost is O(book size), not O(delta size).

KEY: the blocking problem is not the speed, it is the SCALING. The live
Binance book grows without bound - its diff stream reports changes across a
~$30,000 price range and nothing trims it (consolidated_book.h). An O(book)
apply on an unbounded book moves more memory the longer the process runs. The
in-place version's cost depends on the DELTA, which the venue bounds for us, so
it is indifferent to book growth. That is a stability argument, not a
performance one, and it is why the in-place version is required rather than
optional.

Expected after the in-place change: quantity-only diffs move **0 bytes** and
cost O(delta); the 16% merge win becomes net gain instead of paying for a 5x
apply regression.

### Corrected from the 5000-iteration run

At 5000 iterations `flat_churn_20x2` measured 3042 ns against the map's 3750 -
i.e. the flat book looked FASTER on churn. At 20000 it is 2708 against 2167,
i.e. 25% slower. The 5000-iteration reading was noise; the 20000 one stands.
Recorded because the wrong number was briefly believed.

### Not measured

- Live end-to-end publish latency with the flat book. Every number here is the
  hot-cache micro-benchmark. The live `merge` median was 50.9 us against this
  benchmark's ~9-11 us, so the production merge is dominated by effects this
  loop does not reproduce, and the flat book's live gain is UNKNOWN.
- The production gap should be WIDER than measured here in the merge: the hot
  loop keeps tree nodes in cache, which flatters `std::map`, while a sequential
  scan over contiguous memory prefetches whether it starts cold or hot.
  Direction is defensible; magnitude is not claimed.
- AoS vs SoA for `PriceLevel` (splitting price and qty into separate vectors).
  The merge's selection loop reads only `price`, so SoA would fit 8 prices per
  cache line instead of 4 - but it doubles the memmoves. Not attempted.

---

## 2026-09-05 - FlatOrderBook in-place apply: the regression is gone

The version recorded above rebuilt the whole side on every message - O(book),
32000 bytes/diff - which made it unshippable regardless of the merge win. This
entry is the fix, measured.

`ApplySide` now walks the book ONCE, backward from `back()` (the best price),
against the delta best-first, and stops the moment the delta is exhausted. It
writes matched quantities in place as it goes. Only if a level actually enters
or leaves does `Relocate` rewrite the touched region.

All numbers below are ONE run, 20000 iterations, medians. Cross-run comparison
is not valid here and is not attempted - see below.

### Within-run results

| | map | flat | |
|---|---|---|---|
| `churn_top_20x2` | 1833 ns | **167 ns** | flat **11.0x faster** |
| `churn_20x2` (deep) | 2000 | 3333 | flat 1.67x SLOWER |
| `qty_update_50` | 208 | 84 | flat 2.5x faster |
| `qty_update_5` | 0 | 0 | below the timer floor - no result |
| `merge_full` | 11166 | 9333 | flat 1.20x faster |
| `iterate_only` | 9833 | 584 | flat 16.8x faster |

Bytes memmoved per diff (write-back only; staging costs an equal pass again):

    flat_qty_update_50       0 bytes    was 32000
    flat_churn_top_20x2    640 bytes
    flat_churn_20x2      32576 bytes    was 63360

### The asymmetry IS the design

    flat:  deep 3333  vs  top 167    ->  20x
    map:   deep 2000  vs  top 1833   ->  1.09x

std::map does not care where in the book an edit lands - a node is a node. The
flat book cares enormously, because its cost is set by how deep the delta
reaches and the best price lives at back(). That is the reverse layout doing
exactly its job: real churn concentrates at the top of book, and that is the
end that was made cheap.

`churn_top_20x2` was added for this entry precisely so the deep arm does not
stand alone. Reporting only the deep case measures the flat book at its worst;
reporting only the shallow case flatters it. Both, or neither.

### Removing the top of book is FREE

640 bytes is exactly half what a naive count predicts (20 levels x 16 bytes x
2 sides x 2 updates = 1280), and the reason is worth knowing:

- ERASE the top 20 of a 1000-level side: `deepest` = 980, region = [980, 1000),
  all 20 erased, so the merged region is EMPTY - nothing to copy back, just a
  resize down. **Zero bytes.**
- INSERT 20 new best prices: region is empty, 20 inserts -> 320 bytes/side.

640 = 2 sides x 20 levels x 16 bytes, all of it from the insert half.

### The deep case is a real loss, and it is not being hidden

A structural edit 500 levels deep costs two passes over half the book, which is
more than std::map's 40 node operations. It is still net positive per depth
update, because each one pays an apply AND a merge:

    deep structural apply   +1333 ns
    merge                   -1833 ns
                            ---------
                             -500 ns    still ahead

Top-of-book churn is -1666 ns on the apply alone, before the merge saving.

### The bug the oracle caught - worth keeping

The first in-place version chose its walk direction from the NET size change:
backward when growing, forward when shrinking. That is wrong, and
`RandomMultiLevelDeltasMatchTheMapOracle` failed at update 10.

    book  [96, 97, 98, 99]     delta best-first:  100 -> 5,  98 -> 0
    inserts 1, erases 1, net 0  ->  rule picks BACKWARD
    first step writes the new 100 into side[3], which still held 99

In the backward pass `write - read` starts at inserts - erases and each insert
shrinks it, so meeting the inserts first drives it negative. The forward pass
fails the mirror image. ANY delta holding both an insert and an erase can break
either direction depending on the order they appear in.

KEY: the corruption is SILENT - the side stays sorted and stays the right
length, with one level's data duplicated. Nothing but an oracle comparing full
sequences against an independent implementation would have found it. This is
the concrete payoff for keeping the std::map book (CLAUDE.md section 6).

The fix stages the merged region through a buffer, so the destination is never
an input and the aliasing question does not arise. Cost: two write passes over
the region instead of one. Still O(region), never O(book).

### Methodology note, again

`qty_update_50` on UNCHANGED std::map code read 209 ns in one run and 500 ns in
the next - 2.4x drift on identical code. Every comparison in this entry is
within a single run for that reason. An earlier draft of this analysis compared
churn across runs (2708 -> 4458 -> 2833) and drew conclusions from it; that was
wrong and none of it is repeated here.

### Known limitation of the fixtures

Both delta fixtures emit levels WORST-FIRST (offset increasing), which is
storage order for the flat book. Real venues send BEST-FIRST. So `ApplySide`'s
order detection succeeds on its first scan here, while in production it fails
and runs a second - the benchmark under-measures that detection cost by roughly
half. Not corrected, because changing the fixtures would move every existing
number in this file.

### Still not measured

- Live end-to-end publish latency with the flat book. Everything here is the
  hot-cache micro-benchmark; the live `merge` median was 50.9 us against this
  benchmark's ~9-11 us.
- The real mix of quantity-only versus structural deltas in a live feed. That
  ratio decides which arm above dominates in production, and nothing in the
  repo measures it. A fast-path / relocate counter on the book would settle it.

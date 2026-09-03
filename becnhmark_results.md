<!-- [feat/ob_aggregator] consolidated_book % ./build-bench/benchmarks/bench_md_core 5000

md_core order book latency  (iterations=5000, warmup=1000)
fixture: binance=1000 bybit=1000 okx=400  ->  1000 merged levels (venues share one tick grid)
prices x1e8 around 5000000000000, tick 1000000

VenueBook::ApplyUpdate
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

VenueBook::ApplyUpdate
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

VenueBook::ApplyUpdate
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

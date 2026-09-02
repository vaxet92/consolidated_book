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
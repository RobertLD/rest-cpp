---------------------------------- #1
Note: Google Test filter = RestClientPerf.*
[ PERF ] Warm (same client -> local server)
        iters=200 total_ms=30.82 avg_ms=0.15 min_ms=0.09 max_ms=0.38
[ PERF ] Cold (new client each req -> local server)
        iters=100 total_ms=17.81 avg_ms=0.18 min_ms=0.11 max_ms=0.37
[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=88191 avg_rps=8819.10 peak_rps=9209
---------------------------------- #2
# Use a single resolver
[ PERF ] Warm (same client -> local server)
        iters=200 total_ms=28.52 avg_ms=0.14 min_ms=0.08 max_ms=0.41

[ PERF ] Cold (new client each req -> local server)
        iters=100 total_ms=16.76 avg_ms=0.17 min_ms=0.11 max_ms=0.42

[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=82759 avg_rps=8275.90 peak_rps=9274

---------------------------------- #3
# Use a keep-alive tcp stream

[ PERF ] Warm (same client -> local server)
        iters=200 total_ms=9.10 avg_ms=0.05 min_ms=0.03 max_ms=0.31

[ PERF ] Cold (new client each req -> local server)
        iters=100 total_ms=13.79 avg_ms=0.14 min_ms=0.09 max_ms=0.35

[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=231375 avg_rps=23137.50 peak_rps=24547

--------------------------------- #4
# Better stream management

[ PERF ] Warm (same client -> local server)
        iters=200 total_ms=11.28 avg_ms=0.06 min_ms=0.03 max_ms=0.26

[ PERF ] Cold (new client each req -> local server)
        iters=100 total_ms=14.72 avg_ms=0.15 min_ms=0.09 max_ms=0.33

[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=230006 avg_rps=23000.60 peak_rps=24654
--------------------------------- #5
More utilities to shared
Host

[ PERF ] Warm (same client -> local server)
        iters=200 total_ms=7.62 avg_ms=0.04 min_ms=0.03 max_ms=0.10
[ PERF ] Cold (new client each req -> local server)
        iters=100 total_ms=15.60 avg_ms=0.16 min_ms=0.09 max_ms=0.47
[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=216100 avg_rps=21610.00 peak_rps=23979
---------------------------- #5
Using unordered map and parser instead of flat buffer copy
[ PERF ] Max RPS over 30s (same client -> local server)
        duration_s=10 total_reqs=245845 avg_rps=24584.50 peak_rps=24891
---------------------------- #6
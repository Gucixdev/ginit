# ginit benchmarks

Appended by `./bench`. Interpretation and anything wrong that the numbers show is under Findings.

## Findings

1. **Fixed, found by the bench:** when `start NAME` failed halfway through loading (a missing hard dependency, more than
   512 services, more than 24 dependencies), the services already loaded for that call stayed in the table with a cut-short
   dependency list. The second `ginitctl start NAME` then found the half-loaded service by name and reported it started
   without the missing dependency (exit 0). With 600 generated services the first start was refused and the second
   said `boot: started`. `gi_load` now drops everything a failed call loaded; `tests/run` has a check for it (it fails on the
   old code, 3 of 65 checks). Commit 80e8ccc.
2. **Not fixed, deliberate:** settle time grows quadratically with the *depth* of a dependency chain, not with the number of
   services. A chain of 500 takes 7.9 ms (16 us per service), 1000 takes 27.5 ms (28 us), 2000 takes 89 ms (44 us): each
   pass of `gi_settle` can start only the next link and every pass walks all services. A tree of 2001 services is linear
   (9 to 11 us per service). Real boot graphs are a few levels deep, so this costs nothing in practice; a fix would be
   a worklist instead of repeated full passes, which is not a small change.
3. The hard limit is 512 services loaded at once (`GI_MAXSVC`) and 24 dependencies of one kind per service. The sizes above
   512 in the graph table use a copy of the source with the limit raised, not the shipped build. A real distribution has
   a few hundred services at most, so the limit is fine, but it is the first thing a big machine would hit.
4. Spawning is cheap: 475 services up in 130 to 165 ms, 300 to 350 us each, and the bash reference loop needs about 250 ms for
   the same number of `/usr/bin/true` runs, so ginit adds no visible overhead on top of fork and exec. Stopping 475 sleeping
   processes (SIGTERM, reap) takes about 12 ms.
5. Memory: about 2.2 MB for 9 services, 2.4 MB for 96, and between 2.9 and 4.9 MB for 502 running services (bimodal; the extra
   2 MB probably is a transparent huge page, THP is `always` here, not verified). The arena is a 4 MB static array whose
   pages count only once touched.

How sure: the timings are real wall-clock numbers on a loaded machine (load average about 11 on 12 threads while this ran,
other people's compiles and editors), so the absolute values are an upper bound and the spread shows it. The ratios between
rows and the quadratic chain growth are stable across the three runs made while developing the script. Not measured: PID 1
duties, reboot paths, logfile handling, restart storms. UNSURE: the cause of the RSS jump.

## Results log

## 2026-11-15, commit 80e8ccc

machine: Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz, 12 threads, Linux 7.3.3-1-cachyos; gcc (GCC) 16.2.1 20260810; shared with the owner's editors
load average at start: 11.06 7.57 4.91; each cell: 1 warm-up run, then 7 runs; values are median [min..max]
command: `./bench` (builds with `make`, gcc -std=c89 -O2; sizes above 512 services use a GI_MAXSVC=4096 copy in a temp dir)

### Start and stop of running processes (`sleep`), time until `ginitctl start boot` returns / `stop boot` returns

| shape | services (total) | processes | up ms | down ms | VmRSS kB |
|---|---|---|---|---|---|
| tree, fan 20 | 9 | 8 | 3.6 [3.4..4.1] | 1.3 [1.1..2.2] | 2240.0 [2236.0..4276.0] |
| tree, fan 20 | 96 | 90 | 27.5 [25.1..30.9] | 3.5 [1.8..4.0] | 2356.0 [2352.0..4392.0] |
| tree, fan 20 | 502 | 475 | 164.9 [140.3..180.7] | 12.2 [11.3..16.5] | 4300.0 [2940.0..4908.0] |

### Reaping: one-shot `scripted` services running `true` (fork, exec, exit, waitpid, mark started)

| services (total) | one-shots | up ms | one-shots per second (median) |
|---|---|---|---|
| 9 | 8 | 3.6 [3.4..4.1] | 2222 |
| 96 | 90 | 25.9 [22.1..27.5] | 3475 |
| 502 | 475 | 146.7 [125.8..156.2] | 3238 |

reference, a bash loop running `/usr/bin/true` 475 times one after the other: 258.7 237.6 281.9 ms (median min max)

### Graph: only `type = internal` services (parse, cycle check, want and settle passes, no process); up ms

| shape | services | build | up ms | microseconds per service (median) |
|---|---|---|---|---|
| tree, fan 10 | 100 | stock | 1.9 [1.8..2.4] | 19 |
| tree, fan 10 | 252 | stock | 2.8 [2.8..2.9] | 11 |
| tree, fan 10 | 501 | stock | 4.7 [4.6..4.9] | 9 |
| tree, fan 10 | 1000 | limit 4096 | 8.9 [8.7..10.6] | 9 |
| tree, fan 10 | 2001 | limit 4096 | 19.9 [19.2..23.3] | 10 |
| chain | 100 | stock | 1.8 [1.8..2.3] | 18 |
| chain | 250 | stock | 3.4 [3.3..3.5] | 14 |
| chain | 500 | stock | 7.9 [6.9..9.3] | 16 |
| chain | 1000 | limit 4096 | 27.5 [23.4..28.8] | 28 |
| chain | 2000 | limit 4096 | 89.0 [87.8..106.8] | 44 |

### Limit of the stock build

667 internal services (600 leaves): `ginitctl start boot` exit code 1, daemon says: `ginit: boot: g0_44: too many services: l445 ginit: shutting down `, ginitctl says: `g0_44: too many services: l445 `
load average at end: 11.21 7.72 4.99

# Test evidence

## release-20261112/: the final release regression

`make release` on commit **b936db3** (the submitted code), on the development Mac (Darwin arm64, Apple
clang 16.0.0), from 2026-11-12 16:15:58 to 18:55 PST. The run passed **23 of 23 steps**, plus all 16 suites
again in the ASan build. The Mac did not sleep during the run: the release script checks `pmset`'s log for
the run's time window and records the result in `summary.txt`.

| File | What it is |
|---|---|
| `summary.txt` | Run header (date, commit, host, compiler), one line per suite (PASS/FAIL, seconds, last output line), and the sleep check |
| `<suite>.out` | Each suite's full output (`unit`, `m2_session` … `separation`, `matrix`, `perf02`, `scale`, `soak`, `longrun`, `demo`, `appendix_a`) |
| `asan/` | The same 16 suites built with AddressSanitizer and UBSan: `summary.txt` and the `.out` files |
| `run_matrix/<row>.<seed>/` | One folder per impairment run: rows M1–M5, M8, M9 × seeds 1–10. Each holds `demo.txt` (the demo's 34 checks), `db_dump.txt`, `check_logs.txt` and `check_invariants.txt` |
| `run_matrix/M3.1/logs/` | One complete run kept whole: every server and device log, under 15% loss, 10% reorder and 5% duplication (the demo configuration) |
| `perf02/M4.<seed>/` | TC-PERF-02: row M4 × 20 seeds, the same files as above |
| `run_scale/` | TC-PERF-01: the dump and log checks for 50 devices |
| `run_soak/` | TC-PERF-04: the dump and the macOS `leaks` report for each of the 6 processes after the hour (0 leaks each) |

The suites' own working folders (state directories, journals, binaries' scratch files; 117 MB) are not
committed. `make release` recreates all of this in `results/<date-time>-release/`.

### Results

| Suite | Result |
|---|---|
| `make test`, 16 suites | 1082 unit checks and 618 scenario checks, 0 failed |
| ASan build, 16 suites | Same, with no sanitizer report |
| Impairment matrix (QA §6) | M1–M5, M8, M9: 10/10 runs each, 40/40 devices OPERATIONAL (medians 0.3–9.6 s) |
| TC-PERF-02 | M4 × 20: 80/80 OPERATIONAL, median 3.8 s, p95 9.3 s (limits 60 / 90 s) |
| TC-PERF-01 | 50 devices OPERATIONAL in 18 s, server peak RSS 8 MB (limits 120 s / 64 MB) |
| TC-PERF-03 | 1.15 / 1.35 / 1.69 transmissions per frame at 10 / 20 / 30% loss (model bounds 1.23 / 1.56 / 2.04) |
| TC-PERF-04 | 1 h with 5 devices: every RSS lower at the end than after warm-up (server 1040 → 976 kB), 0 leaks, 9155 keepalives, 0 session timeouts |
| TC-NAT-07 / 13 | 30 min at 20% loss with production timers: 0 session timeouts, 1 REGISTER; 10 min with demo timers: 1 session timeout, recovered in 0.4 s |
| Demo and Appendix A | `demo PASS (34 checks)`; Appendix A reference run 8/8, copied to `logs/appendix_a/` |

## Earlier release runs (not committed)

| Commit | Result | Why it is not the evidence |
|---|---|---|
| 3f9a06c | 3 steps failed | Two test-harness bugs and three matrix scenario checks; fixed in 821d134 |
| 821d134 | 23/23 PASS | Its logs showed false PMTU black-hole step-downs at loss; the device was fixed in a1c5bd3 (DESIGN §5.8) |
| a1c5bd3 | 3 steps failed | The Mac slept during the run (59 sleeps in `pmset`'s log); the sleep check was added in b936db3 |

## ci/: CI run reports

The `evidence` job of [the CI workflow](../.github/workflows/ci.yml) writes a Markdown report for each run
(every job's result, duration and link, its steps and its notices) with `scripts/ci_evidence.sh`. Nightly and
manual runs on `main` commit it here; every run also shows it on the run page and uploads it. See README §17.2.

| Report | Run | Result |
|---|---|---|
| [2026-11-13-ddfc8fb-run47.md](ci/2026-11-13-ddfc8fb-run47.md) | manual, ddfc8fb | all 11 test jobs passed; matrix M1–M4 × 10 seeds on Ubuntu and macOS, TC-PERF-01/02 |

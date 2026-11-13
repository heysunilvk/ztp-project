# Test evidence

## release-20261113-063341/: the final release regression

`make release` on commit **86d8ac2** (the submitted device and server code), on the development Mac (Darwin
arm64, Apple clang 16.0.0), from 2026-11-13 06:33:41 to 09:13 PST. The run passed **24 of 24 steps**, plus all 16
suites again in the ASan build. The Mac did not sleep during the run: the release script checks `pmset`'s log for
the run's time window and records the result as the last line of `summary.txt`.

| File | What it is |
|---|---|
| `summary.txt` | Run header (date, commit, host, compiler), one line per step (PASS/FAIL, seconds, last output line), and the sleep check |
| `<step>.out` | Each step's full output (`unit`, `m2_session` … `separation`, `matrix`, `perf02`, `scale`, `soak`, `longrun`, `demo`, `appendix_a`, `scenario_logs`) |
| `asan/` | The same 16 suites built with AddressSanitizer and UBSan: `summary.txt` and the `.out` files |
| `run_matrix/<row>.<seed>/` | One folder per impairment run: rows M1–M5, M8, M9 × seeds 1–10. Each holds `demo.txt` (the demo's 38 checks), `db_dump.txt` (five rows), `check_logs.txt` and `check_invariants.txt` |
| `run_matrix/M3.1/logs/` | One complete run kept whole: every server and device log, under 15% loss, 10% reorder and 5% duplication (the demo configuration) |
| `perf02/M4.<seed>/` | TC-PERF-02: row M4 × 20 seeds, the same files as above |
| `matrix_counters.txt` | Per-row totals of every process's `[UDP] Counters:` line (transmissions, retransmissions, duplicates, out-of-order), with the command that produced them; TEST_REPORT §2 quotes it |
| `linkage_macos.txt` | TC-DEL-01: `otool -L` of both binaries (only libSystem) |
| `run_scale/` | TC-PERF-01: the dump and log checks for 50 devices |
| `run_soak/` | TC-PERF-04: the dump and the macOS `leaks` report for each of the 6 processes after the hour (0 leaks each). The reports' binary paths were shortened to `./build/…` before committing; `leaks` itself already writes the home folder as `/Users/USER` |

The steps' own working folders (state directories, journals, the other 69 matrix runs' full logs; about 120 MB)
are not committed. `make release` recreates all of this in `results/<date-time>-release/`. The committed logs in
`logs/` were written by this run's last three steps (`demo`, `appendix_a`, `scenario_logs`), except
`logs/appendix_a/linux-10.10.10.5/`, which comes from CI.

### Results

| Step | Result |
|---|---|
| `make test`, 16 suites | 1090 unit checks and 646 scenario checks, 0 failed |
| ASan build, 16 suites | Same, with no sanitizer report |
| Impairment matrix (QA §6) | M1–M5, M8, M9: 10/10 runs each, 40/40 valid devices OPERATIONAL (medians 0.4–12.0 s); ZTP-84BC19 FAILED in every run, as expected |
| TC-PERF-02 | M4 × 20: 80/80 OPERATIONAL, median 3.9 s, p95 10.6 s (limits 60 / 90 s) |
| TC-PERF-01 | 50 devices OPERATIONAL in 24 s, server peak RSS 9 MB (limits 120 s / 64 MB) |
| TC-PERF-03 | 1.16 / 1.35 / 1.68 transmissions per frame at 10 / 20 / 30% loss (model 1.23 / 1.56 / 2.04, allowed +10%) |
| TC-PERF-04 | 1 h with 5 devices: every RSS lower at the end than after warm-up (server 1024 → 992 kB), 0 leaks, 9155 keepalives, 0 session timeouts |
| TC-NAT-07 / 13 | 30 min at 20% loss with production timers: 0 session timeouts, 1 REGISTER; 10 min with demo timers: 1 session timeout, recovered in 0.5 s, no re-registration |
| Demo, Appendix A, scenario logs | `demo PASS (38 checks)` with five devices; Appendix A 8/8; scenario logs 10/10 |

## Earlier release runs (not committed)

| Commit | Result | Why it is not the evidence |
|---|---|---|
| 3f9a06c | 3 steps failed | Two test-harness bugs, timing-sensitive demo checks, and a device defect: a false PMTU black-hole step-down under loss (matrix M3 seed 3); fixed in 821d134 |
| 821d134 | 23/23 PASS | Its logs still showed two false step-downs: buffered full-size chunks were not counted as data; fixed in a1c5bd3 (DESIGN §5.8) |
| a1c5bd3 | 3 steps failed | The Mac slept during the run (59 sleeps in `pmset`'s log); the sleep check was added in b936db3 |
| b936db3 | 23/23 PASS | Superseded: an external-style review then found and fixed 11 code defects, a missing feature (the forced re-key) and weak tests (0b2e460; TEST_REPORT §4) |
| 0b2e460 | 24/24 PASS | Superseded by the `--bad-key` hook (the demo's fifth device and the PDF dump's failed row, 10b6850) |
| 10b6850 | 24/24 PASS | Its matrix showed one false step-down: the only missing chunk was a 262-byte last chunk; fixed in 86d8ac2 |

## ci/: CI run reports

The `evidence` job of [the CI workflow](../.github/workflows/ci.yml) writes a Markdown report for each run (every
job's result, duration and link, its steps and its notices) with `scripts/ci_evidence.sh`. Nightly and manual
runs on `main` commit it here; every run also shows it on the run page and uploads it. A report counts the test
jobs only (it cannot report on the `evidence` job that writes it), so "all 11 jobs passed" means the run's 12
jobs were green. See README §17.2.

| Report | Run | Result |
|---|---|---|
| [2026-11-13-285c11b-run54.md](ci/2026-11-13-285c11b-run54.md) | nightly, 285c11b | all 11 test jobs passed |
| [2026-11-13-23757cd-run52.md](ci/2026-11-13-23757cd-run52.md) | manual, 23757cd | all 11 test jobs passed; matrix M1–M4 × 10 seeds on Ubuntu and macOS, TC-PERF-01/02, netem × 10 |
| [2026-11-13-ddfc8fb-run47.md](ci/2026-11-13-ddfc8fb-run47.md) | manual, ddfc8fb | all 11 test jobs passed; matrix M1–M4 × 10 seeds on Ubuntu and macOS, TC-PERF-01/02 |

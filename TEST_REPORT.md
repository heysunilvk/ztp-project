# ZTP over UDP: Test Report

| | |
|---|---|
| Version | 1.1 (M7 release, after the final review) |
| Owner | Sunil Vallamkonda |
| Code under test | commit `0b2e460` (release regression); later commits change only tests, scripts and documents |
| Test plan | [QA_TEST_PLAN.md](QA_TEST_PLAN.md): 165 cases (§5), impairment matrix (§6), traceability (§10) |
| Evidence | [evidence/release-20261112-220052/](evidence/release-20261112-220052/) (the release run), [evidence/ci/](evidence/ci/) (CI reports), [logs/](logs/) (committed logs, [logs/README.md](logs/README.md)) |
| Date | 2026-11-13 |

This is the release test report that QA §11 asks for: the result of every test case, the impairment matrix
with times and retransmission ratios, the performance numbers, and what is open or waived.

## 1. Summary

| Result | Cases | Meaning |
|---|---|---|
| ✅ PASS | 139 | Run and passed in the release regression (and in CI where noted) |
| ⚠ Partial | 10 | Run and passed, except one part: either it needs the open test tooling (QA §12.5), or one platform or sub-case was not run (each row says which) |
| ⚠ Open | 15 | Logic implemented, not exercised end to end: each needs a hostile-traffic tool that remains with the project owner (PRD R-15, QA §12.5) |
| ⚠ Waived | 1 | TC-SIM-04 (macOS dummynet, needs `sudo`); TC-SIM-03 (netem in CI) and the in-code simulator cover the same behaviour |
| ❌ Fail | 0 | |
| **Total** | **165** | |

**Most visible gap.** The PDF's example dump has a failed row (ZTP-84BC19, "Failed / Invalid HMAC"). The server
records that verdict (DESIGN §10.3), but no committed run shows it: the device needs a wrong key, made with the
`--bad-key` test hook, which was not built (TC-AUTH-03 Open; TC-PDF-02, TC-SRV-01, TC-SRV-05, TC-E2E-02 and
TC-DEL-02 Partial). The demo runs four of the PDF's five devices.

**Runs behind these results**

| Run | Where | Result |
|---|---|---|
| `make release` on `0b2e460`, 2026-11-12 22:00 → 2026-11-13 00:39 PST | Development Mac (Darwin arm64, Apple clang 16.0.0), no sleep during the run | 24/24 steps PASS; ASan build 16/16 ([summary.txt](evidence/release-20261112-220052/summary.txt)) |
| CI run on `0b2e460` (push) | GitHub Actions: Ubuntu gcc and clang, macOS clang, ASan on both, platform, matrix, netem, valgrind | 12/12 jobs green ([run 43375735275](https://github.com/heysunilvk/ztp-project/actions/runs/43375735275)); the 10.10.10.5 Appendix A logs from it are in [logs/appendix_a/linux-10.10.10.5/](logs/appendix_a/linux-10.10.10.5/) |
| CI run 52 on `23757cd` (manual, nightly seed counts) | The same jobs, 10-seed matrix, TC-PERF-01/02, netem × 10 | 12/12 green ([report](evidence/ci/2026-11-13-23757cd-run52.md)); referenced below for the CI-only cases (the device and server code is that of 0b2e460) |
| Fresh clone of `6903ab4` (the code of 0b2e460) | Development Mac | `make`: no warnings; `make demo`: PASS (TC-DEL-01, TC-DEL-04) |

## 2. Impairment matrix (QA §6)

The demo (four devices) over each row, seeds 1–10, release run. "Retransmissions" is the share of all datagrams
sent (server and devices, every run of the row) that were retransmissions; it and the out-of-order and
duplicate counts are totals of the processes' `Counters:` lines ([matrix_counters.txt](evidence/release-20261112-220052/matrix_counters.txt)
has the table and the command; per-run checks and dumps are in `run_matrix/<row>.<seed>/`, and one run is kept
whole in `run_matrix/M3.1/logs/`).

| Row | Impairment (each direction) | Runs | Devices OPERATIONAL | Median | p95 | Retransmissions | Out of order / dup received | Result |
|---|---|---|---|---|---|---|---|---|
| M1 | none | 10/10 | 40/40 | 0.3 s | 0.4 s | 0.0% | 0 / 0 | PASS |
| M2 | 10% loss, 5% reorder, 2% dup, 1 ms | 10/10 | 40/40 | 1.6 s | 4.0 s | 12.6% | 730 / 243 | PASS |
| M3 | 15% loss, 10% reorder, 5% dup, 2±1 ms (the demo) | 10/10 | 40/40 | 2.7 s | 6.3 s | 16.6% | 1000 / 378 | PASS |
| M4 | 20% loss, 10% reorder, 5% dup, 5±2 ms | 10/10 | 40/40 | 4.3 s | 9.7 s | 19.8% | 1017 / 372 | PASS |
| M5 | 30% loss, 20% reorder, 10% dup (stress, ≥ 95% required) | 10/10 | 40/40 | 11.0 s | 35.4 s | 27.4% | 1076 / 596 | PASS |
| M6 | 50% loss | not run | | | | | | not part of the release run |
| M7 | 15% loss correlated 25%, 10% reorder, 5% dup, 5 ms (kernel netem, CI Ubuntu) | 10/10 (CI run 52) | all | | | | | PASS |
| M8 | 20% loss device → server only | 10/10 | 40/40 | 1.4 s | 5.6 s | 14.6% | 26 / 296 | PASS |
| M9 | 20% loss server → device only | 10/10 | 40/40 | 1.6 s | 5.7 s | 13.7% | 760 / 124 | PASS |

In CI (run 52) rows M1–M4 × 10 seeds passed on Ubuntu and macOS with every device OPERATIONAL. Every run also
checks the invariants: no duplicate DB rows, no second REGISTER after a restart, and every artifact hash of an
OPERATIONAL device verified (`tests/check_invariants.sh`). The PMTU black-hole detector re-requested chunks in
8 of the 360 device runs of the matrix and perf02 and never stepped down falsely (DESIGN §5.8).

## 3. Performance (QA §8)

| Case | Measure | Target | Result |
|---|---|---|---|
| TC-PERF-01 | 50 devices at 15% loss | all OPERATIONAL ≤ 120 s, server RSS ≤ 64 MB | 50/50 in 21 s, peak RSS 9 MB (Mac); 17 s, 5 MB (CI macOS) and 24 s, 8 MB (CI Ubuntu), run 52 |
| TC-PERF-02 | Time to OPERATIONAL, row M4 × 20 | median ≤ 60 s, p95 ≤ 90 s | median 3.4 s, p95 8.7 s (Mac); 3.7 s / 9.6 s (CI macOS) and 3.9 s / 10.5 s (CI Ubuntu), run 52 |
| TC-PERF-03 | Transmissions per frame at 10 / 20 / 30% loss | within 10% of the model 1.23 / 1.56 / 2.04 | 1.16 / 1.36 / 1.76 ([m6_perf03.out](evidence/release-20261112-220052/m6_perf03.out)) |
| TC-PERF-04 | 1 h soak, 5 devices | no RSS growth, no leaks | every process's RSS lower at the end than after warm-up (server 1024 → 928 kB); `leaks`: 0 in all 6 processes; 9155 keepalives, 0 session timeouts; valgrind (CI, 3 min): clean |
| TC-NAT-07 | 30 min at 20% loss, production timers | 0 session timeouts | 0 timeouts, 87 keepalives, 1 REGISTER |
| TC-NAT-13 | 10 min at 20% loss, demo timers | timeouts recover without re-registration | 1 timeout, recovered in 0.3 s, no re-registration |

## 4. Defects found by the release runs and the final review

| Found by | Defect | Fix |
|---|---|---|
| Release run on 3f9a06c, matrix M3 seed 3 | Device: a false PMTU black-hole step-down to 508 B under ordinary loss | 821d134: re-request once, step down only after a second silent interval (DESIGN §5.8) |
| Release run on 821d134 | Device: full-size chunks buffered out of order were not counted as data | a1c5bd3: any authenticated CHUNK_DATA over 508 B restarts the interval |
| Release runs on 3f9a06c and a1c5bd3 | Test harness: S4 rebinding check, TC-LOG-02 on loss-only rows, shared suite folders; the Mac slept during one run | 821d134, b936db3 (the run fails itself if `pmset` logged a sleep) |
| CI on dc3c139 | Test: TC-SRV-09 counted too few log lines to see interleaving | cfcb3fa |
| Final review (code) | Server: `limits.max_sessions` above 1536 could hang the session table; an Invalid HMAC REGISTER discarded the half-open session (a forged frame could abort a genuine registration); the rate limit was spent before the MAC check; NAK resends were unbounded; a device with a smaller datagram limit was stepped down to 508 | 0b2e460 (DESIGN §10.3, §5.8) |
| Final review (code) | DB journal: a torn last line could swallow the next record; non-ASCII strings made lines unreplayable; write/fsync errors were ignored before the ACK; the session history grew forever | 0b2e460 (DESIGN §14.4) |
| Final review (code) | Device: the forced re-key at 2³⁰ (DESIGN §8.9) was not implemented; a crash between the rename and the state save cost a hash attempt; a rollback could be reported as an upgrade after a crash; a queued probe counted as acknowledged | 0b2e460 |
| Final review (tests) | TC-UNIT-06 compared a constant with itself; weak checks in TC-UPG-02/03, TC-DISC-02, the `sha()` fallback and the GNU `nm` pattern | 0b2e460 |

One review finding is accepted as a residual risk: a replayed resume HELLO just after a server restart can stall
a session briefly (DESIGN §18.3, item 5).

## 5. Results per test case

Each row: the case from QA §5, its result, where it ran, and the output that shows it. Suites marked "(Mac;
CI build-test ×3, asan ×2)" are part of `make test`: they ran in the release run, in its ASan build, and in
every CI run on Ubuntu (gcc, clang) and macOS, with and without AddressSanitizer. Each `.out` file is the
suite's full output; a suite prints every failed check (`FAIL: TC-…`) and ends with its passed and failed
counts, so a PASS row means none of that case's checks failed. To rerun one suite or case: README §16.3.

### Unit (L1)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-UNIT-01 | SHA-256 known-answer tests | NFR-01, FR-18 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-02 | HMAC-SHA256 known-answer tests | FR-13 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-03 | Header and TLV encode/decode round trip | FR-05, BON-02 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-04 | TLV parser bounds | NFR-07 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-05 | Constant-time compare | NFR-06 | P2 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-06 | Serial-number arithmetic and wraparound | FR-05 | P1 | ✅ PASS | `unit`, `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UNIT-07 | RTO estimator, Karn, backoff | FR-06 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-08 | Reorder buffer: in-order delivery | BON-04 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-09 | Duplicate classification | FR-08, BON-05 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-10 | Generating and parsing SACK | FR-07 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-11 | Policy parser and precedence | FR-29 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-12 | Version comparison | FR-21 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-13 | Atomic file write | NFR-10 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-14 | Persisting and loading the chunk bitmap | FR-19 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-15 | Netsim determinism and rates | NFR-09, BON-06 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-16 | Key-derivation known-answer test | FR-13 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-17 | Config key validation | FR-24 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-18 | Strict JSON parser | FR-34, NFR-07 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-19 | Bitmap conversion on chunk-size change | FR-19, FR-20 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-UNIT-20 | Precedence and policy validation | FR-34, FR-35 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |

### Discovery (DISC)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-DISC-01 | Soft serial generated on first boot | FR-01 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2); `scenario_logs` (Mac; [logs/scenarios/](logs/scenarios/)) | [m2_session.out](evidence/release-20261112-220052/m2_session.out), [scenario_logs.out](evidence/release-20261112-220052/scenario_logs.out) |
| TC-DISC-02 | Soft serial loaded on later boots, and assigned by configuration | FR-01 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2); `scenario_logs` (Mac; [logs/scenarios/](logs/scenarios/)) | [m2_session.out](evidence/release-20261112-220052/m2_session.out), [scenario_logs.out](evidence/release-20261112-220052/scenario_logs.out) |
| TC-DISC-03 | Static IP discovery | FR-02 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-DISC-04 | Multicast discovery | BON-01 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-DISC-05 | Broadcast discovery | BON-01 | P2 | ⚠ Partial | `m2_session` (Mac; CI build-test ×3, asan ×2); CI `build-test` (Ubuntu): broadcast, part (b) | [m2_session.out](evidence/release-20261112-220052/m2_session.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) — part (b), broadcast on Ubuntu CI, runs; part (a), broadcast on the Mac, is skipped (no BCAST address set locally) |
| TC-DISC-06 | Fallback to static | BON-01 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-DISC-07 | Forged OFFER ignored | BON-01, NFR-06 | P1 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-DISC-08 | Replayed OFFER ignored | BON-01 | P2 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |

### Session (SESS)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-SESS-01 | Handshake happy path | FR-04 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-SESS-02 | HELLO lost: idempotent retransmission | FR-06, BON-05 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-SESS-03 | HELLO_ACK lost | FR-06, BON-05 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-SESS-04 | Session IDs are random and unique | FR-04 | P2 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-SESS-05 | Server down: retry exhaustion and recovery | FR-06, FR-11 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-SESS-06 | Unknown session_id dropped | NFR-06 | P1 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-SESS-07 | Stale epoch dropped | FR-23 | P1 | ⚠ Open | — | QA §12.5; needs an old-epoch frame injector |
| TC-SESS-08 | UDP only | FR-03, DEL-04 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-SESS-09 | A HELLO can't tear down a live session | FR-14, NFR-06 | P1 | ⚠ Open | — | QA §12.5; needs a HELLO takeover tool |

### Reliability (REL)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-REL-01 | Every frame carries a seq | FR-05, BON-02 | P1 | ✅ PASS | CI `platform-linux`: packet capture checked frame by frame | [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-REL-02 | Retransmission on timeout | FR-06, FR-07, BON-03 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-REL-03 | The PDF scenario, reproduced verbatim: seq 2 lost, seq 3 buffered, duplicate of seq 2 | BON-04, BON-08 | P1 | ✅ PASS | `unit`, `m2_peer` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-REL-04 | Lost ACK: data received | BON-07, BON-05 | P1 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-REL-05 | Heavy duplication | FR-08, BON-05 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-REL-06 | Window limit respected | FR-06 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-REL-07 | Fast retransmit | FR-06 | P2 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-REL-08 | Out-of-window frames dropped | NFR-07 | P2 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-REL-09 | Completes at 10/15/20% loss | FR-09, NFR-03 | P1 | ✅ PASS | `matrix` (Mac: M1–M5, M8, M9 × 10); CI `impair-matrix` (Ubuntu, macOS) | [matrix.out](evidence/release-20261112-220052/matrix.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-REL-10 | Heavy reordering | BON-04 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-REL-11 | No ACK loops | FR-07 | P2 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |

### NAT and keepalive (NAT)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-NAT-01 | Keepalives when idle | FR-10 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-NAT-02 | No keepalives during a transfer | FR-10 | P2 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-NAT-03 | Device detects a dead server | FR-11 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2); `scenario_logs` (Mac; [logs/scenarios/](logs/scenarios/)) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out), [scenario_logs.out](evidence/release-20261112-220052/scenario_logs.out) |
| TC-NAT-04 | Server detects an idle device | FR-11 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-NAT-05 | NAT rebinding | FR-11, FR-23 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-NAT-06 | Spoofed source can't hijack | NFR-06 | P1 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-NAT-07 | No false timeouts under loss (production timers) | FR-11, NFR-03 | P2 | ✅ PASS | `longrun` (Mac) | [longrun.out](evidence/release-20261112-220052/longrun.out) |
| TC-NAT-13 | False timeouts with demo timers recover cleanly | FR-11, FR-23 | P2 | ✅ PASS | `longrun` (Mac) | [longrun.out](evidence/release-20261112-220052/longrun.out) |
| TC-NAT-08 | NAT idle timeout shorter than the keepalive | FR-36, FR-35, FR-11 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-NAT-09 | NAT reboot: all mappings flushed | FR-36, FR-23 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-NAT-10 | Public IP change | FR-36 | P2 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2); CI `build-test` (Ubuntu): strict-IP resume needs 127.0.0.2, which macOS lacks (the Mac run prints "skipped") | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-NAT-11 | Outbound UDP blocked by a customer firewall | FR-36, A-09 | P1 | ✅ PASS | `m2_session` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out) |
| TC-NAT-12 | A replayed valid frame from a new address can't move the peer | NFR-06, FR-14, FR-11 | P1 | ⚠ Partial | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) — KEEPALIVE half passes; the CHUNK_REQ replay half needs a replay tool |

### Authentication and registration (AUTH)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-AUTH-01 | REGISTER contents | FR-12 | P1 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-AUTH-02 | Valid registration | FR-13 | P1 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-AUTH-03 | Invalid HMAC (wrong model key) | FR-13, FR-30 | P1 | ⚠ Open | — | QA §12.5; needs `--bad-key` |
| TC-AUTH-04 | Replay in the same session | FR-14 | P1 | ⚠ Open | — | QA §12.5; needs `--replay-test` |
| TC-AUTH-05 | Replay spliced into a new session | FR-14 | P1 | ⚠ Open | — | QA §12.5; needs `--replay-test` |
| TC-AUTH-06 | Reused nonce | FR-14 | P1 | ⚠ Open | — | QA §12.5; needs `--reuse-nonce` |
| TC-AUTH-07 | Replayed resume HELLO | FR-14, FR-23 | P1 | ⚠ Open | — | QA §12.5; needs a resume replay tool |
| TC-AUTH-08 | Tampering with any byte | NFR-06 | P1 | ⚠ Open | — | QA §12.5; needs a bit-flip injector (TC-UNIT-03 flips every bit of a frame) |
| TC-AUTH-09 | Unknown model | FR-29, FR-13 | P1 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-AUTH-10 | HELLO flood is rate-limited | NFR-05 | P2 | ⚠ Open | — | QA §12.5; needs a HELLO flood tool |
| TC-AUTH-11 | Rogue server rejected | NFR-06 | P1 | ⚠ Open | — | QA §12.5; needs a rogue-server `peer_sim` mode |
| TC-AUTH-12 | Known model, no policy | FR-29, FR-13 | P1 | ✅ PASS | `m3_register` (Mac; CI build-test ×3, asan ×2) | [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-AUTH-13 | Wrong fleet key (not a fleet device) | NFR-06, FR-36 | P2 | ⚠ Open | — | QA §12.5; needs a wrong-fleet-key sender |
| TC-AUTH-14 | Forged or replayed NAK is ignored | FR-14, NFR-06 | P1 | ⚠ Open | — | QA §12.5; needs forged/replayed NAKs |
| TC-AUTH-15 | Replayed terminal ERROR is ignored | FR-14, NFR-06 | P1 | ⚠ Open | — | QA §12.5; needs replayed terminal errors |

### Provisioning (PROV)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-PROV-01 | Config in chunks with hash | FR-15, FR-17, FR-18 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-02 | Config is valid JSON | FR-15 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-03 | Firmware in chunks with hash | FR-16, FR-17, FR-18 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-04 | Datagram size cap | FR-20, NFR-02 | P1 | ✅ PASS | CI `platform-linux`: packet capture | [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PROV-05 | DF set, no IP fragments | FR-20 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2); CI `platform-linux`: packet capture | [unit.out](evidence/release-20261112-220052/unit.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PROV-06 | Chunk boundaries | FR-17 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-07 | Hash mismatch handling | FR-18 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-08 | Resume after kill -9 | FR-19 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-09 | Resume after a server restart | FR-19, FR-23 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-PROV-10 | Artifact changed between attempts | FR-18, FR-19 | P2 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-11 | Duplicate chunks are harmless | BON-05 | P1 | ✅ PASS | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-PROV-12 | 1280-byte MTU path: no fallback needed | FR-20, NFR-02 | P1 | ✅ PASS | CI `platform-linux`: loopback MTU 1280 | [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PROV-13 | Small-MTU path: step-down to 508 B | FR-20, FR-19 | P1 | ⚠ Partial | `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) — parts (b) and (c) run on the Mac and in CI; part (a), a real 576-byte MTU path, needs a CI job that was not built |

### Upgrade and restart (UPG)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-UPG-01 | Outdated firmware upgraded | FR-21 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-02 | Process restart | FR-22 | P1 | ✅ PASS | `m5_gate`, `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out), [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-03 | Resume without re-registration | FR-23 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-04 | One DB row per serial | FR-23, FR-30 | P1 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-UPG-05 | Already current: no upgrade | FR-21 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-06 | Legacy device path | FR-29, FR-21 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-UPG-07 | Crash at each upgrade step | NFR-10 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-08 | Grace period expired before the resume | FR-23 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-09 | Rollback on failed validation | FR-25 | P2 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-10 | Fast restart while the server session is still live | FR-23, FR-19 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-UPG-11 | Crash during resume, then resume again (no deadlock) | FR-23, NFR-10 | P1 | ✅ PASS | `m5_gate`, `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out), [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |

### Validation (VAL)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-VAL-01 | Happy-path validation | FR-24..27 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-VAL-02 | Config missing at validation | FR-24 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-VAL-03 | Firmware version mismatch | FR-25 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |
| TC-VAL-04 | Unhealthy session | FR-26 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-VAL-05 | Server records OPERATIONAL | FR-27, FR-30 | P1 | ✅ PASS | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) |

### Server (SRV)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-SRV-01 | 5-device concurrent demo | FR-28 | P1 | ⚠ Partial | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) — 4 concurrent devices; the expected dump includes the ZTP-84BC19 row, which needs `--bad-key` |
| TC-SRV-02 | Per-device assignment | FR-29 | P1 | ✅ PASS | `unit`, `m6_server` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m6_server.out](evidence/release-20261112-220052/m6_server.out) |
| TC-SRV-03 | DB record fields | FR-30 | P1 | ✅ PASS | `unit`, `m3_register` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-SRV-04 | Dump on demand | FR-31, DEL-11 | P1 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-SRV-05 | Summary | FR-32 | P1 | ⚠ Partial | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) — success count checked; the failure count needs the S2 device (`--bad-key`) |
| TC-SRV-06 | DB persists across a server restart | NFR-10 | P1 | ✅ PASS | `unit`, `m6_demo` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-SRV-07 | Fuzzing malformed datagrams | NFR-07 | P1 | ✅ PASS | `m6_fuzz` (Mac; CI build-test ×3, asan ×2) | [m6_fuzz.out](evidence/release-20261112-220052/m6_fuzz.out) |
| TC-SRV-08 | Session limits and half-open expiry | NFR-05, NFR-07 | P2 | ⚠ Open | — | QA §12.5; needs a half-open flood tool |
| TC-SRV-09 | Interleaved progress | FR-28 | P1 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-SRV-10 | Resume across a server restart mid-upgrade | FR-23, NFR-10 | P1 | ✅ PASS | `m6_server` (Mac; CI build-test ×3, asan ×2) | [m6_server.out](evidence/release-20261112-220052/m6_server.out) |
| TC-SRV-11 | Failed-record cap | NFR-07, NFR-05 | P2 | ⚠ Open | — | QA §12.5; needs 20,000 failing registrations (cap unit-tested at 1,000 over 100) |

### Simulation (SIM)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-SIM-01 | Observed loss rate, per direction and end to end | BON-06 | P1 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-SIM-02 | Reordering and duplication are observable | BON-06 | P1 | ✅ PASS | `m2_session`, `m4_provision` (Mac; CI build-test ×3, asan ×2) | [m2_session.out](evidence/release-20261112-220052/m2_session.out), [m4_provision.out](evidence/release-20261112-220052/m4_provision.out) |
| TC-SIM-03 | The netem alternative | BON-06 | P2 | ✅ PASS | CI `impair-netem` | [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-SIM-04 | The macOS dummynet alternative | BON-06 | P2 | ⚠ Waived | — | Waived: macOS dummynet needs `sudo`; TC-SIM-03 and the in-code simulator cover it |

### Performance (PERF)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-PERF-01 | 50 concurrent devices | NFR-05, FR-28 | P1 | ✅ PASS | `scale` (Mac); CI `impair-matrix` (Ubuntu, macOS) | [scale.out](evidence/release-20261112-220052/scale.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PERF-02 | Time to OPERATIONAL | NFR-04 | P1 | ✅ PASS | `matrix` (Mac: M1–M5, M8, M9 × 10); CI `impair-matrix` (Ubuntu, macOS) | [matrix.out](evidence/release-20261112-220052/matrix.out), [perf02.out](evidence/release-20261112-220052/perf02.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PERF-03 | Retransmission overhead against the model | FR-09 | P3 | ✅ PASS | `m6_perf03` (Mac; CI build-test ×3, asan ×2) | [m6_perf03.out](evidence/release-20261112-220052/m6_perf03.out) |
| TC-PERF-04 | Soak with no leaks | NFR-07 | P2 | ✅ PASS | `soak` (Mac, 1 h); CI `valgrind`: 3 min under memcheck | [soak.out](evidence/release-20261112-220052/soak.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |

### Logs (LOG)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-LOG-01 | Required tags present | FR-33 | P1 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-LOG-02 | Bonus log lines present in the demo | BON-08 | P1 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-LOG-03 | Format and files | NFR-08 | P2 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |
| TC-LOG-04 | Counters at exit | NFR-08 | P2 | ✅ PASS | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) |

### Deliverables (DEL)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-DEL-01 | Clean build on both platforms | DEL-02, NFR-01, NFR-11 | P1 | ✅ PASS | Fresh clone of 6903ab4, `make` (0 warnings), and the CI build jobs (Ubuntu gcc and clang, macOS clang; `-Werror`) | the binaries link only the C library: [linkage_macos.txt](evidence/release-20261112-220052/linkage_macos.txt) |
| TC-DEL-02 | The demo produces its artifacts | DEL-01, DEL-09, DEL-10, DEL-11 | P1 | ⚠ Partial | `make demo` (Mac, release run) | [demo.out](evidence/release-20261112-220052/demo.out), [logs/](logs/) — 4 of 5 device logs; ZTP-84BC19 needs `--bad-key` |
| TC-DEL-03 | Device/server separation | DEL-03 | P1 | ✅ PASS | `make check-separation` (suite `separation`) | [separation.out](evidence/release-20261112-220052/separation.out) |
| TC-DEL-04 | The README quick start works | DEL-05 | P1 | ✅ PASS | Fresh clone of 6903ab4, README quick start | `make` and `make demo` PASS |
| TC-DEL-05 | README: 5 required answers | DEL-06 | P1 | ✅ PASS | README §11 (review) | Q1–Q5 present |
| TC-DEL-06 | README: design sections | DEL-07 | P1 | ✅ PASS | README §4–§10 (review) | all 6 design topics present |
| TC-DEL-07 | README: bonus coverage | DEL-08 | P1 | ✅ PASS | README §13 (review) | 3 topics and B1–B4 present |
| TC-DEL-08 | Sanitizer-clean demo | NFR-11 | P1 | ✅ PASS | `make asan` (ASan build, `m6_demo`) | [asan/m6_demo.out](evidence/release-20261112-220052/asan/m6_demo.out) |
| TC-DEL-09 | Logs committed | DEL-09, DEL-10 | P1 | ✅ PASS | `git ls-files logs/`; `check_pdf05`; `check_pdf05` (Mac; CI build-test ×3, asan ×2) | [check_pdf05.out](evidence/release-20261112-220052/check_pdf05.out), [logs/](logs/) |
| TC-DEL-10 | README: why keepalives matter | DEL-12 | P1 | ✅ PASS | README §12 (review) | keepalive section present |

### End-to-end (E2E)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-E2E-01 | Appendix A flow, single device | FR-33, FR-01..27 | P1 | ✅ PASS | `check_appendix_a` (Mac and CI) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-E2E-02 | Full demo under impairment | FR-28..32, NFR-03 | P1 | ⚠ Partial | `matrix` (Mac: M1–M5, M8, M9 × 10); CI `impair-matrix` (Ubuntu, macOS) | [matrix.out](evidence/release-20261112-220052/matrix.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) — all devices OPERATIONAL in every run; ZTP-71DD08 failed_attempts = 1 needs `--replay-test` |

### Conformance with PDF Appendix A (APX), one test per step

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-APX-01 | Step 1: Device boot (device information: ZTP-93AF21, EDGE-GW-100, v1.0) | FR-01, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-02 | Step 2: Server discovery (static IP 10.10.10.5, UDP port 5555) | FR-02, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2); CI `platform-linux`: on 10.10.10.5:5555 | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-APX-03 | Step 3: UDP session (HELLO seq=1 with soft_serial; HELLO_ACK seq=1 with session_id=7421) | FR-03, FR-04, FR-05, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2); CI `platform-linux`: on 10.10.10.5:5555 | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-APX-04 | Step 4: NAT keepalive | FR-10, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-05 | Step 5: Secure registration (session_id=7421, soft_serial, model, firmware v1.0, nonce=984312) | FR-12, FR-13, FR-29, FR-33 | P1 | ⚠ Partial | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) — REGISTER fields and order checked; the literal nonce 984312 needs `--nonce` |
| TC-APX-06 | Step 6: Config provisioning (the example config, 3 chunks) | FR-15, FR-17, FR-18, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-07 | Step 7: Firmware upgrade (v1.0 → v1.1) | FR-16, FR-21, FR-22, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-08 | Step 8: Resume after restart, without duplicate registration | FR-22, FR-23, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-09 | Step 9: Validation and operational mode | FR-24..27, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |
| TC-APX-10 | Steps 1–9 under impairment | FR-09, NFR-03, FR-33 | P1 | ✅ PASS | `check_appendix_a` (Mac; CI build-test ×3, asan ×2) | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out) |

### Conformance with the PDF's other examples and its scope (PDF)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-PDF-01 | §1 "Example output" | FR-01, FR-02, FR-33 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-PDF-02 | §9 "Example of a Dump from Server DB" | FR-31, FR-30, FR-32, DEL-11 | P1 | ⚠ Partial | `m6_demo` (Mac; CI build-test ×3, asan ×2) | [m6_demo.out](evidence/release-20261112-220052/m6_demo.out) — the dump shows 4 of the PDF's 5 rows; the ZTP-84BC19 "Invalid HMAC" row needs `--bad-key` |
| TC-PDF-03 | Appendix A "Example Configuration" | FR-15 | P1 | ✅ PASS | `m5_gate` (Mac; CI build-test ×3, asan ×2) | [m5_gate.out](evidence/release-20261112-220052/m5_gate.out) |
| TC-PDF-04 | Appendix A addressing (10.10.10.5:5555) | FR-02 | P3 | ✅ PASS | `check_appendix_a`, `m5_gate` (Mac; CI build-test ×3, asan ×2); CI `platform-linux`: on 10.10.10.5:5555 | [check_appendix_a.out](evidence/release-20261112-220052/check_appendix_a.out), [m5_gate.out](evidence/release-20261112-220052/m5_gate.out), [CI run 52](evidence/ci/2026-11-13-23757cd-run52.md) |
| TC-PDF-05 | Overview / Scope & Expectations: UDP only, "not a REST or API exercise", C or Python, no UI | FR-03, NFR-01, DEL-04 | P1 | ✅ PASS | `check_pdf05` (Mac; CI build-test ×3, asan ×2) | [check_pdf05.out](evidence/release-20261112-220052/check_pdf05.out) |

### Configuration (CFG)

| Case | Title | Req | Pri | Result | Where it ran | Evidence |
|---|---|---|---|---|---|---|
| TC-CFG-01 | Built-in defaults | FR-34 | P1 | ✅ PASS | `unit`, `cli_config` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [cli_config.out](evidence/release-20261112-220052/cli_config.out) |
| TC-CFG-02 | A config file overrides the defaults | FR-34 | P1 | ✅ PASS | `unit` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out) |
| TC-CFG-03 | Precedence: defaults < file < profile < CLI | FR-34 | P1 | ✅ PASS | `unit`, `cli_config` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [cli_config.out](evidence/release-20261112-220052/cli_config.out) |
| TC-CFG-04 | Range and type validation (fail fast) | FR-34, NFR-07 | P1 | ✅ PASS | `unit`, `cli_config` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [cli_config.out](evidence/release-20261112-220052/cli_config.out) |
| TC-CFG-05 | Unknown keys rejected (typo protection) | FR-34 | P1 | ✅ PASS | `unit`, `cli_config` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [cli_config.out](evidence/release-20261112-220052/cli_config.out) |
| TC-CFG-06 | Per-tenant, model and serial NAT override delivered to the device | FR-35, FR-29 | P1 | ✅ PASS | `unit` (precedence), `m6_server` (the override in the server's policy line) (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m6_server.out](evidence/release-20261112-220052/m6_server.out) |
| TC-CFG-07 | Profiles | FR-34 | P2 | ✅ PASS | `unit`, `cli_config` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [cli_config.out](evidence/release-20261112-220052/cli_config.out) |
| TC-CFG-08 | Device clamp on advertised values | FR-35, NFR-06 | P1 | ✅ PASS | `m2_peer` (Mac; CI build-test ×3, asan ×2) | [m2_peer.out](evidence/release-20261112-220052/m2_peer.out) |
| TC-CFG-09 | Key-file permissions, and policy/artifact checks | FR-34, NFR-06 | P1 | ✅ PASS | `unit`, `m3_register` (Mac; CI build-test ×3, asan ×2) | [unit.out](evidence/release-20261112-220052/unit.out), [m3_register.out](evidence/release-20261112-220052/m3_register.out) |
| TC-CFG-10 | Settings from the resolved decisions take effect (DESIGN §23) | FR-34, FR-35 | P2 | ⚠ Partial | `m5_upgrade` (Mac; CI build-test ×3, asan ×2) | [m5_upgrade.out](evidence/release-20261112-220052/m5_upgrade.out) — sub-case (f) runs (strict-IP resume, Linux CI); the other per-tenant/model/serial override sub-cases (a)-(e), (g) have no end-to-end check (the policy precedence is unit-tested, TC-UNIT-20) |

## 6. Open and waived cases

The 15 open cases and the open parts of the partial ones that depend on tooling all need test tools that
generate hostile or forged traffic: device hooks (`--bad-key`, `--replay-test`, `--reuse-nonce`, `--nonce`) and
`peer_sim` modes (a rogue server, forged or replayed frames, floods). They were not built in this project and
remain with the project owner (PRD R-15). They are counted as Open, not Waived: the owner accepted submitting
without them, but they are not replaced by another test the way TC-SIM-04 is. The protection logic
behind every one of them is implemented and was reviewed (DESIGN §18.3); README §18 lists, for each, which other
tests reach that logic today. QA §12.5 is the register. The other partial cases are a platform or sub-case not
run (each row says which).

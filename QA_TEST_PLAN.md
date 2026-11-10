# QA Test Plan: UDP-Based Zero Touch Provisioning (ZTP)

| Field | Value |
|---|---|
| Document | QA_TEST_PLAN.md (QA persona) |
| Version | 0.1, Draft for review |
| Date | 2026-11-10 |
| Owner | Sunil Vallamkonda (QA Lead) |
| Companion documents | [PRD_ZTP.md](PRD_ZTP.md) (requirement IDs and acceptance criteria) · [DESIGN.md](DESIGN.md) (the behaviour under test; constants in Appendix B) |
| Test basis | PRD v0.1 (requirement IDs), DESIGN v0.1 (§5 wire spec, §8 reliability, §10 auth, §19 demo) |

> **Alignment rules.** Every test case (`TC-*`) traces to at least one PRD requirement ID, and every PRD requirement has at least one test case (§10). Constants such as timers, sizes and ports are **not** restated here: tests refer to them by name from DESIGN Appendix B, so a design change can't silently invalidate a test. The demo fixture is the five devices defined in PRD §5.2 and DESIGN §19.3.

---

## Table of contents
1. [Scope and objectives](#1-scope-and-objectives)
2. [Test strategy and levels](#2-test-strategy-and-levels)
3. [Test environment and tooling](#3-test-environment-and-tooling)
4. [Test data and fixtures](#4-test-data-and-fixtures)
5. [Test case catalogue](#5-test-case-catalogue)
6. [Loss and impairment matrix](#6-loss-and-impairment-matrix)
7. [Security test plan](#7-security-test-plan)
8. [Performance, scale and soak](#8-performance-scale-and-soak)
9. [Log and deliverable verification](#9-log-and-deliverable-verification)
10. [Requirements traceability matrix](#10-requirements-traceability-matrix)
11. [Entry and exit criteria, defect management, reporting](#11-entry-and-exit-criteria-defect-management-reporting)
12. [QA timeline, dependencies, assumptions, risks](#12-qa-timeline-dependencies-assumptions-risks)
13. [Caveats: what is not or cannot be tested](#13-caveats-what-is-not-or-cannot-be-tested)
14. [QA enhancement suggestions](#14-qa-enhancement-suggestions)

---

## 1. Scope and objectives

### 1.1 Objectives
1. Show that every functional, bonus and deliverable requirement in the PRD (§6, §8, §9) is met.
2. Show that the NFR targets are met: reliability under 20% loss, time to OPERATIONAL, scale, robustness and security (PRD §7, §3.3 success metrics SM-1…SM-8).
3. Find defects early in the highest-risk area, the reliability engine (PRD R-01), using deterministic, seeded tests.
4. Produce the evidence the submission needs: logs, the DB dump and a test report.

### 1.2 In scope
`ztp-device`, `ztp-server`, `libztpcommon`, `mkartifacts`, `run_demo.sh`, and README content.

### 1.3 Out of scope
Real NAT hardware, real flash or power-loss rigs, IPv6, Windows, and production packaging (PRD §3.2). See §13 for how each gap is mitigated.

---

## 2. Test strategy and levels
| Level | What | How | Automated | Owner | When |
|---|---|---|---|---|---|
| **L1 Unit** | Crypto, codec, TLV, reliability engine, util | C test binaries under `tests/`; the reliability engine runs against a **simulated clock and in-memory link**, with no sockets | `make test` | Dev (QA reviews) | Every build, from M1 |
| **L2 Component** | One binary against a scripted peer | A test harness that sends crafted datagrams (`tests/peer_sim`) | Yes | QA | M2+ |
| **L3 Integration** | device ↔ server on loopback | Shell scripts `tests/it_*.sh` with netsim flags; assertions on logs, exit codes and the DB dump | Yes | QA | M2+ per milestone |
| **L4 System / E2E** | The full demo, scenarios S1–S5 | `make demo` plus a results checker | Yes | QA | M6, M7 |
| **Negative / security** | Bad MAC, replay, tampering, floods, fuzzing | §7 | Yes | QA | M3+ |
| **Resilience / chaos** | Crash points, kill -9, SIGSTOP, rebinding, server restart | Test hooks (DESIGN §19.2) and signals | Yes | QA | M4+ |
| **Performance / soak** | 50+ devices, timing, memory | `make scale`, `/usr/bin/time`, `leaks` or valgrind | Semi | QA | M6, M7 |
| **Regression** | Everything above | CI script `tests/run_all.sh` | Yes | QA | M7 and every fix |
| **Documentation review** | README coverage | Checklist (§9) | Manual | QA + PM | M7 |

**Principles**
- **Determinism first:** every impairment test is run with a fixed `--seed` and recorded with it, so a failure reproduces exactly. Statistical tests run over N seeds (§6).
- **Assert on behaviour, not timing**, wherever possible: counts of retransmissions, states and DB rows, not "within 2.3 s". Timing assertions use generous upper bounds (PRD R-05).
- **Tests run from the logs:** every assertion in L3/L4 uses the tagged log lines (DESIGN §17) or the DB dump. That also checks the logs themselves.

---

## 3. Test environment and tooling
| Item | Specification |
|---|---|
| Platforms | **Local:** macOS 14 on the developer's Mac (Apple clang 16, GNU Make 3.81). **CI:** GitHub Actions `ubuntu-latest` (gcc) and `macos-latest` (clang); see DESIGN §19.4. There is **no local Linux host**, so the Ubuntu runner is the Linux platform for the DEL-02 / NFR-01 sign-off |
| Network | Loopback (`127.0.0.1`). The broadcast test TC-DISC-05 runs locally on the Mac's Wi-Fi/Ethernet interface (its directed broadcast address) and in CI on the Ubuntu runner |
| Build variants | `make` (release, `-O2 -Wall -Wextra -Werror`); `make asan` (ASan + UBSan); `make debug` (`-O0 -g`) |
| Impairment | In-code netsim (`--loss/--reorder/--dup/--delay/--seed`), everywhere; macOS dummynet (`scripts/dummynet.sh`), local, TC-SIM-04; Linux `tc netem` (`scripts/netem.sh`), CI Ubuntu runner only, TC-SIM-03 |
| Capture | `tcpdump -i lo0 -w run.pcap udp` (macOS) or `-i lo` (Linux); Wireshark; `scripts/pcapcheck.sh` (maximum datagram size, fragment filter) |
| Memory | Locally: ASan + LeakSanitizer (`make asan`) and `leaks --atExit` (built into macOS). CI: valgrind on the Ubuntu runner |
| Disk footprint (local) | Build outputs, logs, test artifacts and pcaps: < 100 MB in total. `make clean` and `make clean-logs` reclaim it. Large pcaps are limited with `tcpdump -c`. No VMs or containers locally |
| Fuzzing | `tests/fuzz_decode` (a random and mutation-based datagram generator); libFuzzer if available (§14) |
| Prerequisite | Xcode Command Line Tools on the macOS host: **verified on 2026-11-10** (Apple clang 16.0.0, GNU Make 3.81; PRD D-08) |

---

## 4. Test data and fixtures

### 4.1 Device fixture (shared with PRD §5.2 and DESIGN §19.3)
| Serial | Model | Start fw | Hooks | Expected final DB row |
|---|---|---|---|---|
| ZTP-93AF21 | EDGE-GW-100 | v1.0 | — | Success, v1.1, v3, OPERATIONAL |
| ZTP-84BC19 | EDGE-GW-100 | v1.0 | `--bad-key` | Failed, —, —, FAILED, "Invalid HMAC", attempts 3 |
| ZTP-71DD08 | EDGE-GW-100 | v1.0 | `--crash-after-chunks 40`, `--replay-test` | Success, v1.1, v3, OPERATIONAL, "Transfer resumed" |
| ZTP-55AC90 | EDGE-GW-100 | v1.1 | `--rebind-after 3` | Success, v1.1, v3, OPERATIONAL |
| ZTP-19FE77 | EDGE-GW-050 | v0.9 | static discovery | Success, v1.0, v2, OPERATIONAL, "Legacy device" |

### 4.2 Artifact fixtures (`mkartifacts --test-set`)
| Name | Size | Purpose |
|---|---|---|
| `fw_tiny` | 1 B | One chunk, minimal |
| `fw_1023` / `fw_1024` / `fw_1025` | 1023 / 1024 / 1025 B | Chunk-boundary checks (1 / 1 / 2 chunks) |
| `fw_64k` | 65,536 B | Standard (64 chunks) |
| `fw_1m` | 1 MiB | Stress (1024 chunks; many CHUNK_REQ batches) |
| `fw_corrupt` | 64 KiB with one flipped byte at serve time (`--corrupt-chunk 17` server hook) | Hash mismatch |
| `cfg_v3.json` / `cfg_v2.json` | ≈ 2.3 KB / ≈ 1.8 KB | Standard configs |
| `cfg_missing_key.json` / `cfg_malformed.json` | — | Validation failures |

### 4.3 Policy and key fixtures
`conf/policy.conf` (DESIGN §14.3); `conf/policy_override.conf` (adds a `[serial ZTP-55AC90]` override); `conf/keys.conf` with models EDGE-GW-100 and EDGE-GW-050; and the model `EDGE-GW-999` for which no key exists.

---

## 5. Test case catalogue
**Priority:** P1 = must pass for release, P2 = should pass (a waiver needs sign-off), P3 = informational.
Each case gives its preconditions and steps in short form, followed by the expected result, which includes the expected **log line** where relevant. All L3/L4 cases run with the release build and the ASan build.

### 5.1 Unit (L1)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-UNIT-01 | SHA-256 known-answer tests | NFR-01, FR-18 | P1 | FIPS 180-4 vectors: "", "abc", the 448-bit message, 1,000,000 × "a"; streaming across chunk boundaries | All digests match |
| TC-UNIT-02 | HMAC-SHA256 known-answer tests | FR-13 | P1 | RFC 4231 test cases 1–7, including case 5 (truncation) and long keys | All match |
| TC-UNIT-03 | Header encode/decode round-trip | FR-05, BON-02 | P1 | Every type × flag combination, random field values, encode → decode | Fields identical; `seq` present on every type; length 44 |
| TC-UNIT-04 | TLV parser bounds | NFR-07 | P1 | Truncated TLV, length beyond the payload, duplicate tag, unknown tag, zero-length string, oversize string | Unknown tag skipped; duplicate or overrun → reject; never reads out of bounds (ASan) |
| TC-UNIT-05 | Constant-time compare | NFR-06 | P2 | `ct_memeq` on equal and unequal buffers; check there is no early exit | Correct results; no data-dependent branches |
| TC-UNIT-06 | Serial-number arithmetic and wraparound | FR-05 | P1 | `seq_lt` across 2³² − 1 → 0; the forced re-key threshold | Correct ordering; re-key triggered when `next_seq > 2^30` |
| TC-UNIT-07 | RTO estimator, Karn, backoff | FR-06 | P1 | Feed RTT samples; mark one frame retransmitted; trigger repeated timeouts | Matches the RFC 6298 formulas; no sample taken from a retransmitted frame; RTO doubles to the cap; clamped to [min, max] |
| TC-UNIT-08 | Reorder buffer: in-order delivery | BON-04 | P1 | Simulated link: 10,000 seeded random permutations, losses and duplicates of 1..200 frames | The application receives exactly 1..N in order, once each |
| TC-UNIT-09 | Duplicate classification | FR-08, BON-05 | P1 | Inject seq ≤ rcv_ack, and a seq that's already buffered | Both dropped, counted as dup, and an immediate ACK is scheduled |
| TC-UNIT-10 | Generating and parsing SACK | FR-07 | P1 | Buffer patterns that produce known bitmaps; sender marks frames sacked | Bit i ⇔ seq ack+2+i; fast retransmit fires after 3 later frames are SACKed |
| TC-UNIT-11 | Policy parser and precedence | FR-29 | P1 | Model rules, a serial override, default reject, a malformed file | Serial > model > default; a malformed file is a fatal error at startup |
| TC-UNIT-12 | Version comparison | FR-21 | P1 | v0.9 < v1.0 < v1.1 < v1.10; v1.1 == v1.1; malformed values | Correct ordering; malformed → error |
| TC-UNIT-13 | Atomic file write | NFR-10 | P1 | Inject a failure after `write`, after `fsync`, before `rename` | The file is always the old or the new version, never torn; `.bak` fallback works |
| TC-UNIT-14 | Persisting and loading the chunk bitmap | FR-19 | P1 | Save a map with arbitrary bits; reload it; mismatched sha | Bits identical; sha mismatch → map discarded |
| TC-UNIT-15 | Netsim determinism and rates | NFR-09, BON-06 | P1 | Same seed twice; 100k packets at loss 0.15 | Identical decision sequences; observed rate 15% ± 1% |
| TC-UNIT-16 | Key-derivation known-answer test | FR-13 | P1 | Fixed K_mfg, serial, Nd, Ns, sid, epoch | K_boot, K_dev and K_s equal the reference values (computed independently, e.g. with `openssl dgst -hmac`) |
| TC-UNIT-17 | Config key validation | FR-24 | P1 | `cfg_v3`, `cfg_missing_key`, `cfg_malformed` | Pass / fail (missing key named) / fail |

### 5.2 Discovery (DISC)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-DISC-01 | Soft serial generated on first boot | FR-01 | P1 | Empty state dir, no `--serial` | `[BOOT] Soft serial generated: ZTP-XXXXXX` matching `^ZTP-[0-9A-F]{6}$`; persisted in `device.state` |
| TC-DISC-02 | Soft serial loaded on later boots | FR-01 | P1 | Rerun with the same state dir | `[BOOT] Soft serial loaded:` with the same value; the file is unchanged |
| TC-DISC-03 | Static IP discovery | FR-02 | P1 | `--discover static --server 127.0.0.1:5555` | `[DISCOVERY] Using static server IP 127.0.0.1:5555`; session established |
| TC-DISC-04 | Multicast discovery | BON-01 | P1 | `--discover mcast` with no `--server` | `[DISCOVERY] Provisioning server found … (OFFER verified)` |
| TC-DISC-05 | Broadcast discovery | BON-01 | P2 | (a) Locally on the Mac, connected to Wi-Fi/Ethernet: `--discover bcast --bcast-addr $(ifconfig en0 \| awk '/broadcast/{print $NF}')`. (b) CI `broadcast` job on the Ubuntu runner | Server found via broadcast on both |
| TC-DISC-06 | Fallback to static | BON-01 | P1 | Server's discovery socket disabled (`--disc-port 0`), with `--server` given | 3 DISCOVER attempts, then `Using static server IP` |
| TC-DISC-07 | Forged OFFER ignored | BON-01, NFR-06 | P1 | A rogue responder (`tests/peer_sim --rogue-offer`) answers first with a bad MAC | Device logs `OFFER MAC invalid, ignoring`; uses the genuine server |
| TC-DISC-08 | Replayed OFFER ignored | BON-01 | P2 | The rogue responder replays a captured genuine OFFER (old disc_nonce) | Ignored (nonce mismatch) |

### 5.3 Session (SESS)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-SESS-01 | Handshake happy path | FR-04 | P1 | One device, no impairment | `[UDP] HELLO sent (seq=1)`, `[UDP] Session established (session_id=N)`, N ≠ 0; the server log has the same sid |
| TC-SESS-02 | HELLO lost: idempotent retransmission | FR-06, BON-05 | P1 | Drop the first HELLO (netsim scripted drop `--drop-seq tx:1`) | The retransmission carries the same Nd; exactly **one** half-open session on the server; the HELLO_ACK cache is hit |
| TC-SESS-03 | HELLO_ACK lost | FR-06, BON-05 | P1 | Drop the first HELLO_ACK on the server's TX | The device retransmits HELLO; the server answers from its cache; one session |
| TC-SESS-04 | Session IDs are random and unique | FR-04 | P2 | 1,000 handshakes | No duplicates among live sessions; values aren't sequential (a simple runs test) |
| TC-SESS-05 | Server down: retry exhaustion and recovery | FR-06, FR-11 | P1 | Start the device with no server; start the server after 20 s | Retries with backoff and jitter, logged; the device recovers and completes once the server is up |
| TC-SESS-06 | Unknown session_id dropped | NFR-06 | P1 | `peer_sim` sends a frame with a random sid | Dropped; counter increased; no reply; no state change |
| TC-SESS-07 | Stale epoch dropped | FR-23 | P1 | After a resume (epoch 1), replay an epoch 0 frame | Dropped before the MAC check; counter increased |
| TC-SESS-08 | UDP only | FR-03, DEL-04 | P1 | `lsof -i -a -p <pid>` on both processes during the demo; a pcap filter `not udp` on the test ports; `grep -r SOCK_STREAM src` | Only UDP sockets; no non-UDP packets; no `SOCK_STREAM` in the code |

### 5.4 Reliability (REL)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-REL-01 | Every frame carries a seq | FR-05, BON-02 | P1 | Decode the pcap of a full run (`pcapcheck.sh --dump-hdr`) | Every datagram has a valid header with seq; reliable seqs increase per direction and epoch |
| TC-REL-02 | Retransmission on timeout | FR-06, FR-07, BON-03 | P1 | Drop one data frame (scripted) | `[UDP] Retransmitting seq=N (timeout)` once; delivered once; the ACK advances |
| TC-REL-03 | The PDF scenario: seq 2 lost, seq 3 buffered | BON-04, BON-08 | P1 | Scripted: drop TX seq 2 once, in a run with ≥ 3 consecutive frames | Logs, in order: `Received packet seq=3 (out of order, expected=2)`, `Buffered packet seq=3`, `Retransmitting seq=2 (timeout)` or fast; application order is 1, 2, 3 |
| TC-REL-04 | Lost ACK: data received | BON-07, BON-05 | P1 | Drop the ACK of REGISTER on the server's TX (scripted) | The device retransmits REGISTER; server logs `Duplicate packet seq=2 ignored`; **one** REGISTER processed (DB attempts = 1) |
| TC-REL-05 | Heavy duplication | FR-08, BON-05 | P1 | `--dup 1.0` (every datagram duplicated) on both sides | Completes; dup counters ≈ the number of frames; DB rows, stage transitions and chunk writes each happen once |
| TC-REL-06 | Window limit respected | FR-06 | P1 | Debug log of in-flight counts during a 1 MiB transfer | Never more than `ZTP_TX_WINDOW` unacknowledged |
| TC-REL-07 | Fast retransmit | FR-06 | P2 | Drop one frame in the middle of the window (scripted) | `Retransmitting seq=N (fast, sack)` before the RTO expires |
| TC-REL-08 | Out-of-window frames dropped | NFR-07 | P2 | `peer_sim` sends seq = rcv_ack + 100 with a valid MAC | Dropped, counted as `out_of_window`, not buffered |
| TC-REL-09 | Completes at 10/15/20% loss | FR-09, NFR-03 | P1 | Impairment matrix rows M2–M4 (§6), 10 seeds each | 100% of the valid devices reach OPERATIONAL |
| TC-REL-10 | Heavy reordering | BON-04 | P1 | `--reorder 0.25 --reorder-delay 50` | Completes; out-of-order and buffered logs present; no out-of-order delivery to the application (debug assert) |
| TC-REL-11 | No ACK loops | FR-07 | P2 | Idle session for 60 s with debug logging | ACK and KEEPALIVE_ACK frames are never acknowledged; no ping-pong |

### 5.5 NAT and keepalive (NAT)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-NAT-01 | Keepalives when idle | FR-10 | P1 | An OPERATIONAL device with demo timers, observed for 20 s | `[NAT] Keepalive sent` about every interval; `[NAT] Keepalive acknowledged` for each |
| TC-NAT-02 | No keepalives during a transfer | FR-10 | P2 | During the `fw_1m` transfer | No KEEPALIVE frames while data is flowing |
| TC-NAT-03 | Device detects a dead server | FR-11 | P1 | `kill -STOP` the server once the device is OPERATIONAL | After about 3 intervals: `[NAT] Session timeout detected`; resume attempts with backoff; `kill -CONT` → `Session resumed` |
| TC-NAT-04 | Server detects an idle device | FR-11 | P1 | `kill -STOP` a device | Server: `Session N idle, entering grace` after 3 intervals; deleted after the grace period; **DB row kept** |
| TC-NAT-05 | NAT rebinding | FR-11, FR-23 | P1 | `--rebind-after 3` | Server: `[NAT] Peer address changed A -> B (rebinding)`; the session continues; no REGISTER; DB attempts unchanged |
| TC-NAT-06 | Spoofed source can't hijack | NFR-06 | P1 | `peer_sim` sends a frame for a live sid from a new port with a bad MAC | The peer address is unchanged; frame dropped; `mac_fail` increased |
| TC-NAT-07 | No false timeouts under loss | FR-11, NFR-03 | P2 | OPERATIONAL device, 20% loss, 10 min | 0 session timeouts declared |

### 5.6 Authentication and registration (AUTH)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-AUTH-01 | REGISTER contents | FR-12 | P1 | `-v` debug decode of REGISTER, or pcap TLV decode | SERIAL, MODEL, FW_VER, HW_REV, NONCE_REG (64-bit) and NONCE_SRV present |
| TC-AUTH-02 | Valid registration | FR-13 | P1 | ZTP-93AF21 | `[AUTH] Registration successful`, `[AUTH] Assigned tenant: cruise_ops`; DB Success |
| TC-AUTH-03 | Invalid HMAC | FR-13, FR-30 | P1 | ZTP-84BC19 `--bad-key` | 3 × REGISTER_NAK(BAD_MAC); device `FAILED … giving up`; DB: Failed, notes "Invalid HMAC", attempts 3; **no** config or firmware sent |
| TC-AUTH-04 | Replay in the same session | FR-14 | P1 | `--replay-test` phase A | The server logs a duplicate or anti-replay drop; no second DB transition |
| TC-AUTH-05 | Replay into a new session | FR-14 | P1 | `--replay-test` phase B | MAC fails on the new session; logged as an auth failure; an existing **Success row is not overwritten** (failed_attempts + 1 only) |
| TC-AUTH-06 | Reused nonce | FR-14 | P1 | `--reuse-nonce` (the second registration reuses NONCE_REG with a valid MAC) | REGISTER_NAK(REPLAY) |
| TC-AUTH-07 | Replayed resume HELLO | FR-14, FR-23 | P1 | `peer_sim` replays the captured resume HELLO (resume_ctr = 2) after the device has resumed with 3 | `RESUME_REJECTED`; the live session is unaffected |
| TC-AUTH-08 | Tampering with any byte | NFR-06 | P1 | For each byte offset of a captured REGISTER and CHUNK_DATA, flip one bit and send | Every variant is dropped; no state change |
| TC-AUTH-09 | Unknown model | FR-29 | P1 | `--model EDGE-GW-999` | HELLO dropped (no K_mfg), or NAK(UNKNOWN_MODEL); the device eventually FAILS; DB Failed "Unknown model" (if the serial was seen) |
| TC-AUTH-10 | HELLO flood is rate-limited | NFR-05 | P2 | `peer_sim --flood 5000/s` from one IP while 5 devices provision | Rate-limited log lines; all 5 legitimate devices still reach OPERATIONAL |
| TC-AUTH-11 | Rogue server rejected | NFR-06 | P1 | `peer_sim --rogue-server` answers HELLO with a bad MAC | The device ignores it and keeps retrying with the genuine server |

### 5.7 Provisioning (PROV)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-PROV-01 | Config in chunks with hash | FR-15, FR-17, FR-18 | P1 | ZTP-93AF21 | `Receiving config (chunk 1/3)` … `(3/3)`, `Config checksum verified` |
| TC-PROV-02 | Config is valid JSON | FR-15 | P1 | Parse the saved `config.json` with an external tool (`python3 -m json.tool` or `jq`) | Valid; contains `interfaces`, `mtu`, `keepalive_interval`, `config_version` |
| TC-PROV-03 | Firmware in chunks with hash | FR-16, FR-17, FR-18 | P1 | ZTP-93AF21 | 64 chunks; `Firmware checksum verified`; the slot file's SHA equals the server artifact's |
| TC-PROV-04 | Datagram size cap | FR-20, NFR-02 | P1 | pcap of the full demo → maximum UDP payload | ≤ 1200 bytes (expected maximum 1076) |
| TC-PROV-05 | No IP fragments | FR-20 | P1 | pcap filter `ip[6:2] & 0x3fff != 0` | 0 packets; DF bit set (Linux) |
| TC-PROV-06 | Chunk boundaries | FR-17 | P1 | `fw_tiny`, `fw_1023`, `fw_1024`, `fw_1025`, `fw_1m` | Chunk counts 1, 1, 1, 2, 1024; all hashes verified |
| TC-PROV-07 | Hash mismatch handling | FR-18 | P1 | Server `--corrupt-chunk 17` (persistent) | `Checksum mismatch, discarding (attempt 1/2)`, again for 2/2, then STATUS_REPORT FAILED HASH_MISMATCH; DB stage FAILED |
| TC-PROV-08 | Resume after kill -9 | FR-19 | P1 | ZTP-71DD08 `--crash-after-chunks 40`, plus a separate run with an external `kill -9` at a random time | After the restart: `Resuming firmware: 32/64 chunks present, requesting 32`; the total number of chunks served is < 64 + 32; the hash verifies |
| TC-PROV-09 | Resume after a server restart | FR-19 | P1 | Restart the server in the middle of the firmware download | The device re-registers (fresh session, same DB row) and reuses the partial download (manifest sha matches) |
| TC-PROV-10 | Artifact changed between attempts | FR-18, FR-19 | P2 | Replace the firmware artifact (new sha) while the device is down | The partial is discarded; full download; verified |
| TC-PROV-11 | Duplicate chunks are harmless | BON-05 | P1 | `--dup 0.5` on the server's TX during the firmware download | No rewrites (write counter = chunk count); hash verifies |

### 5.8 Upgrade and restart (UPG)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-UPG-01 | Outdated firmware upgraded | FR-21 | P1 | ZTP-93AF21 (v1.0) | `[UPGRADE] Current: v1.0 -> Target: v1.1`; `Upgrade complete, restarting device` |
| TC-UPG-02 | Process restart | FR-22 | P1 | Both `--restart-mode exec` and `exit` (supervised) | `[BOOT] Restart detected`; boot_count increased; no leaked fds (`lsof` before and after shows the same count) |
| TC-UPG-03 | Resume without re-registration | FR-23 | P1 | After TC-UPG-01 | `[UDP] Session resumed (session_id=<same>, epoch=1)`; the device log has exactly **1** `REGISTER sent`; the server log has exactly 1 `Registration successful` for the serial |
| TC-UPG-04 | One DB row per serial | FR-23, FR-30 | P1 | After the full demo | The dump has exactly one row per serial; attempts = 1 for every device except the bad-key one |
| TC-UPG-05 | Already current: no upgrade | FR-21 | P1 | ZTP-55AC90 (v1.1) | `Firmware v1.1 is current, no upgrade required`; no restart; no firmware chunks transferred |
| TC-UPG-06 | Legacy device path | FR-29, FR-21 | P1 | ZTP-19FE77 (EDGE-GW-050, v0.9) | Upgrades to v1.0; cfg v2; note "Legacy device" |
| TC-UPG-07 | Crash at each upgrade step | NFR-10 | P1 | Hook `--crash-at {download,verified,committed,post-exec}` (the 4 points in DESIGN §12.4) | Each case recovers to OPERATIONAL with the correct firmware; no re-download after "verified"; no duplicate registration |
| TC-UPG-08 | Grace period expired before the resume | FR-23 | P1 | Delay the restart past the grace period (`--restart-delay` > grace) | Resume → `SESSION_EXPIRED`; fresh registration; **same** DB row; note "re-registered after session expiry" |
| TC-UPG-09 | Rollback on failed validation | FR-25 | P2 | `--fail-validate` | `Validation failed on v1.1, rolled back to v1.0`; STATUS FAILED VALIDATION_FAILED; DB registration Success, stage FAILED |

### 5.9 Validation (VAL)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-VAL-01 | Happy-path validation | FR-24..27 | P1 | ZTP-93AF21 | `Configuration present`, `Firmware v1.1 verified`, `Session healthy`, `[STATE] Device is OPERATIONAL` |
| TC-VAL-02 | Config missing at validation | FR-24 | P1 | A hook deletes `config.json` before VALIDATE | One re-fetch of the config, then pass; with `--no-refetch` → FAILED |
| TC-VAL-03 | Firmware version mismatch | FR-25 | P1 | A hook writes a wrong version into the active slot's header | Rollback / FAILED (as TC-UPG-09) |
| TC-VAL-04 | Unhealthy session | FR-26 | P1 | `kill -STOP` the server at the start of VALIDATE | Not OPERATIONAL until the session recovers; `kill -CONT` within the deadline → OPERATIONAL; past it → FAILED |
| TC-VAL-05 | Server records OPERATIONAL | FR-27, FR-30 | P1 | After TC-VAL-01 | Server `[STATE] … OPERATIONAL`; DB stage OPERATIONAL |

### 5.10 Server (SRV)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-SRV-01 | 5-device concurrent demo | FR-28 | P1 | `make demo` | Every final DB row matches §4.1 |
| TC-SRV-02 | Per-device assignment | FR-29 | P1 | Demo + `policy_override.conf` | Each device gets the firmware and config of its model, or of its serial override |
| TC-SRV-03 | DB record fields | FR-30 | P1 | Inspect the dump and `db.jsonl` | Device ID, timestamp, firmware assigned, config version and outcome present for every row |
| TC-SRV-04 | Dump on demand | FR-31, DEL-11 | P1 | `kill -USR1 <server>` during the demo; SIGINT at the end | The dump is printed both times; its columns match DESIGN §14.5 (a superset of the PDF's table) |
| TC-SRV-05 | Summary | FR-32 | P1 | At the end of the demo | Lists 4 successes, 1 failure with reason, and firmware/config per device |
| TC-SRV-06 | DB persists across a server restart | NFR-10 | P1 | Restart the server after the demo; dump | The same rows are rebuilt from the journal |
| TC-SRV-07 | Fuzzing malformed datagrams | NFR-07 | P1 | 1M random and mutated datagrams at the control and discovery ports, ASan build | No crash, hang or sanitizer report; counters increase |
| TC-SRV-08 | Session limits and half-open expiry | NFR-05, NFR-07 | P2 | 300 half-open HELLOs from `peer_sim` | Capped at 256; `SERVER_BUSY`; expire after the TTL; memory returns to baseline |
| TC-SRV-09 | Interleaved progress | FR-28 | P1 | Server log of the demo | Lines from ≥ 3 different sids interleave during provisioning (not one device after another) |

### 5.11 Simulation (SIM)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-SIM-01 | Observed loss rate | BON-06 | P1 | 10k datagrams at `--loss 0.15` | `[SIM]` totals within 15% ± 2% |
| TC-SIM-02 | Reordering and duplication are observable | BON-06 | P1 | `--reorder 0.1 --dup 0.05` | `[SIM]` counters non-zero; matching OOO/dup events in `[UDP]` |
| TC-SIM-03 | The netem alternative | BON-06 | P2 | CI `impair-netem` job (Ubuntu runner): `scripts/netem.sh on`, netsim off, run the demo | Same PASS result as with netsim. *If `sch_netem` can't be loaded on the hosted runner, this is waived (PRD R-11)* |
| TC-SIM-04 | The macOS dummynet alternative | BON-06 | P2 | Locally: `sudo scripts/dummynet.sh on 0.15 5` (loss and delay only), netsim off, run the demo; then `scripts/dummynet.sh off` and check that the pf rules were restored | Same PASS result; `[UDP]` retransmission counters show OS-level loss |

### 5.12 Performance (PERF)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-PERF-01 | 50 concurrent devices | NFR-05, FR-28 | P1 | `make scale N=50`, 15% loss | All valid devices OPERATIONAL; all done ≤ 120 s; server peak RSS ≤ 64 MB |
| TC-PERF-02 | Time to OPERATIONAL | NFR-04 | P1 | 20 runs × the 5-device demo at 20% loss | Median ≤ 60 s, p95 ≤ 90 s |
| TC-PERF-03 | Retransmission overhead against the model | FR-09 | P3 | Counters at 10/20/30% loss | Ratio of transmissions to frames ≤ DESIGN §21.1 Q5 bounds (+10%) |
| TC-PERF-04 | Soak with no leaks | NFR-07 | P2 | 5 OPERATIONAL devices for 1 h; `leaks` or valgrind | No leaks; RSS stable (± 5%) |

### 5.13 Logs (LOG)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-LOG-01 | Required tags present | FR-33 | P1 | grep the demo logs | Each of `[BOOT] [DISCOVERY] [UDP] [NAT] [AUTH] [PROVISION] [UPGRADE] [VALIDATION] [STATE]` appears in at least one device log, and the relevant ones in the server log |
| TC-LOG-02 | Bonus log lines present | BON-08 | P1 | grep the demo logs | All four lines from the PDF sample appear (out of order, Buffered, Retransmitting, Duplicate ignored) |
| TC-LOG-03 | Format and files | NFR-08 | P2 | Inspect | ISO-8601 timestamps with ms, monotonic within each file; one file per device; the device log continues across the restart |
| TC-LOG-04 | Counters at exit | NFR-08 | P2 | End of the demo | Counter summary present on both sides |

### 5.14 Deliverables (DEL)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-DEL-01 | Clean build on both platforms | DEL-02, NFR-01, NFR-11 | P1 | Fresh clone → `make` on macOS and Linux | Builds with no warnings (`-Werror`); links only libc |
| TC-DEL-02 | The demo produces its artifacts | DEL-01, DEL-09, DEL-10, DEL-11 | P1 | `make demo` | Exit 0, PASS; `logs/server.log`, five `logs/device_*.log`, `logs/db_dump.txt` |
| TC-DEL-03 | Device/server separation | DEL-03 | P1 | Directory layout check; `nm` symbol check; include check | No server symbols in the device binary and vice versa; no cross-includes |
| TC-DEL-04 | The README quick start works | DEL-05 | P1 | A reviewer who has never seen the project follows the README only | Builds and runs the demo successfully |
| TC-DEL-05 | README: 5 required answers | DEL-06 | P1 | §9 checklist | All 5 present and accurate against the implementation |
| TC-DEL-06 | README: design sections | DEL-07 | P1 | §9 checklist | All 6 topics present |
| TC-DEL-07 | README: bonus coverage | DEL-08 | P1 | §9 checklist | 3 bonus topics + 4 bonus answers present |
| TC-DEL-08 | Sanitizer-clean demo | NFR-11 | P1 | `make asan demo` | No ASan or UBSan reports |
| TC-DEL-09 | Logs committed | DEL-09, DEL-10 | P1 | `git ls-files logs/` (in the submission) | The server and every client log are present |

### 5.15 End-to-end (E2E)
| TC ID | Title | Req | Pri | Steps / setup | Expected result |
|---|---|---|---|---|---|
| TC-E2E-01 | Appendix A flow, single device | FR-33, FR-01..27 | P1 | ZTP-93AF21 alone, no impairment | The log lines appear in the order of PDF Appendix A Steps 1–9 (an ordered-subsequence check) |
| TC-E2E-02 | Full demo under impairment | FR-28..32, NFR-03 | P1 | `make demo` (15% loss, 10% reorder, 5% dup) × 10 seeds | PASS for all seeds; the dump matches §4.1 |

**Total: 119 catalogued test cases** (P1: 100, P2: 18, P3: 1), plus the exploratory TC-PERF-01b (§8).

### 5.16 Where each test runs
| Environment | What runs there |
|---|---|
| **Local Mac** (developer, daily) | All of L1, all of L3/L4 with netsim, TC-DISC-05a (Wi-Fi broadcast), TC-SIM-04 (dummynet), the pcap cases, the performance cases (TC-PERF-01/02: timing is measured locally, not on shared runners), `leaks`/ASan soak |
| **CI `macos-latest`** | Clean build, `make test`, demo, ASan, impairment rows M1–M4 (a second, clean macOS environment) |
| **CI `ubuntu-latest`** (the only Linux) | gcc build, `make test`, demo, ASan, rows M1–M4, TC-SIM-03 (netem), TC-DISC-05b (broadcast), valgrind (TC-PERF-04, Linux half), row M7 |

---

## 6. Loss and impairment matrix
The 5-device demo is run against each row, with **10 seeds per row** (seeds 1–10), on the local Mac and both CI runners for rows M1–M4, on the local Mac for M5–M6 and M8–M9, and on the CI Ubuntu runner only for M7 (netem). CI runs 3 seeds per push and all 10 nightly or on demand.

| Row | Loss (each direction) | Reorder | Dup | Delay | Pass criterion | Metrics recorded |
|---|---|---|---|---|---|---|
| M1 | 0% | 0 | 0 | 0 | 100% of valid devices OPERATIONAL | Baseline time, frame counts |
| M2 | 10% | 0.05 | 0.02 | 1 ms | 100% OPERATIONAL | Retx ratio, time |
| M3 | 15% | 0.10 | 0.05 | 2±1 ms | 100% OPERATIONAL (**the demo configuration**) | Retx ratio, time, OOO and dup counts |
| M4 | 20% | 0.10 | 0.05 | 5±2 ms | 100% OPERATIONAL; median time ≤ 60 s (SM-1, SM-4) | All of the above |
| M5 | 30% | 0.20 | 0.10 | 5±2 ms | ≥ 95% OPERATIONAL (stress, P2) | Session failures/resumes |
| M6 | 50% | 0.20 | 0.10 | 5 ms | No crash and no corrupted state; completion not required (P3) | Behaviour notes |
| M7 | 15% correlated 25% (netem) | 10% | 5% | 5 ms | 100% OPERATIONAL | Comparison with M3 |
| M8 | Asymmetric: 20% device→server only | 0 | 0 | 0 | 100% OPERATIONAL | Retx per direction |
| M9 | Asymmetric: 20% server→device only | 0 | 0 | 0 | 100% OPERATIONAL | Retx per direction |

For every run: **zero** duplicate DB rows, **zero** duplicate REGISTERs after a restart, and every artifact hash verified (invariants checked by `tests/check_invariants.sh`).

---

## 7. Security test plan
| Area | Cases | Technique |
|---|---|---|
| Authenticity | TC-AUTH-02/03/08/11, TC-DISC-07 | Bad keys, bit flips at every offset, a rogue server and a rogue OFFER |
| Replay | TC-AUTH-04..07, TC-DISC-08, TC-SESS-07 | The built-in `--replay-test`; `peer_sim` replaying captured datagrams (`tcpdump` → `peer_sim --replay file.pcap`) |
| DB integrity | TC-AUTH-05 (Success not overwritten), TC-UPG-04 | Hostile attempts against existing serials |
| Input handling | TC-UNIT-04, TC-SRV-07, TC-REL-08 | Fuzzing and boundary datagrams (0, 43, 44, 1200, 1201, 65507 bytes) |
| DoS resistance | TC-AUTH-10, TC-SRV-08 | Floods, half-open exhaustion |
| Crypto correctness | TC-UNIT-01/02/05/16 | Known-answer tests, constant-time review |
| Secrets hygiene | Manual review | No session keys on disk (`grep` the state dir for the K_s hex from debug runs); keys zeroised on free |

**Known residual risks that are accepted and not tested as defects:** no confidentiality, the shared model key, no forward secrecy (DESIGN §18.3). QA confirms that these are documented in the README (TC-DEL-06).

---

## 8. Performance, scale and soak
| Test | Load | Duration | Targets | Tooling |
|---|---|---|---|---|
| TC-PERF-01 | 50 devices (spawned by `run_scale.sh`), 15% loss | Until complete | All OPERATIONAL in ≤ 120 s; server RSS ≤ 64 MB; CPU < 1 core | `/usr/bin/time -l` (macOS) / `-v` (Linux) |
| TC-PERF-01b (P3, exploratory) | 200 devices | Until complete | Record only | Same |
| TC-PERF-02 | The 5-device demo, 20% loss, 20 runs | — | Median ≤ 60 s, p95 ≤ 90 s | Timestamps from the logs |
| TC-PERF-04 | 5 OPERATIONAL devices | 1 h | No leaks, stable RSS | `leaks`, valgrind |

---

## 9. Log and deliverable verification
README checklist (TC-DEL-05..07). Each item must be present **and** must agree with the implemented behaviour:

| # | README item | PRD ID | ✓ |
|---|---|---|---|
| 1 | Q1 How reliability is implemented over UDP | DEL-06 | ☐ |
| 2 | Q2 How replay attacks are prevented | DEL-06 | ☐ |
| 3 | Q3 Why IP fragmentation is avoided | DEL-06 | ☐ |
| 4 | Q4 How NAT affects the protocol design | DEL-06 | ☐ |
| 5 | Q5 What happens under packet loss | DEL-06 | ☐ |
| 6 | Overall architecture and control flow | DEL-07 | ☐ |
| 7 | UDP session management | DEL-07 | ☐ |
| 8 | Identity, authentication, registration | DEL-07 | ☐ |
| 9 | Config and firmware provisioning | DEL-07 | ☐ |
| 10 | Version upgrades and restarts | DEL-07 | ☐ |
| 11 | Known limitations and tradeoffs | DEL-07 | ☐ |
| 12 | Bonus: packet loss and out-of-order handling | DEL-08 | ☐ |
| 13 | Bonus: duplicate detection | DEL-08 | ☐ |
| 14 | Bonus: simulation method | DEL-08 | ☐ |
| 15 | Bonus Q1–Q4 | DEL-08 | ☐ |
| 16 | Build and run instructions | DEL-05 | ☐ |
| 17 | Where the server and client logs are | DEL-09/10 | ☐ |
| 18 | Why "keepalives matter in real deployments" (PDF §4) | FR-10 | ☐ |

Log checks (TC-LOG-01..04, TC-DEL-09) are automated in `tests/check_logs.sh`.

---

## 10. Requirements traceability matrix
Status values: ☐ Not run · ✅ Pass · ❌ Fail · ⚠ Waived. Every row is ☐ at v0.1.

### 10.1 Functional
| PRD ID | DESIGN § | Test cases | Status |
|---|---|---|---|
| ZTP-FR-01 | §6.1 | TC-DISC-01, TC-DISC-02 | ☐ |
| ZTP-FR-02 | §6.3 | TC-DISC-03 | ☐ |
| ZTP-FR-03 | §4.3 | TC-SESS-08 | ☐ |
| ZTP-FR-04 | §7 | TC-SESS-01, TC-SESS-04 | ☐ |
| ZTP-FR-05 | §5.2, §8.1 | TC-UNIT-03, TC-UNIT-06, TC-REL-01 | ☐ |
| ZTP-FR-06 | §8.3 | TC-UNIT-07, TC-SESS-02/03/05, TC-REL-02/06/07 | ☐ |
| ZTP-FR-07 | §8.4 | TC-UNIT-10, TC-REL-02, TC-REL-11 | ☐ |
| ZTP-FR-08 | §8.5 | TC-UNIT-09, TC-REL-05 | ☐ |
| ZTP-FR-09 | §8, §16 | TC-REL-09, TC-PERF-03 | ☐ |
| ZTP-FR-10 | §9.2 | TC-NAT-01, TC-NAT-02 | ☐ |
| ZTP-FR-11 | §9.3 | TC-NAT-03/04/05/07, TC-SESS-05 | ☐ |
| ZTP-FR-12 | §10.2 | TC-AUTH-01 | ☐ |
| ZTP-FR-13 | §10.1–10.3 | TC-UNIT-02, TC-UNIT-16, TC-AUTH-02, TC-AUTH-03 | ☐ |
| ZTP-FR-14 | §10.4 | TC-AUTH-04/05/06/07 | ☐ |
| ZTP-FR-15 | §11.1 | TC-PROV-01, TC-PROV-02 | ☐ |
| ZTP-FR-16 | §11.1 | TC-PROV-03 | ☐ |
| ZTP-FR-17 | §11.2 | TC-PROV-01/03/06 | ☐ |
| ZTP-FR-18 | §11.3 | TC-UNIT-01, TC-PROV-07, TC-PROV-10 | ☐ |
| ZTP-FR-19 | §11.4 | TC-UNIT-14, TC-PROV-08/09/10 | ☐ |
| ZTP-FR-20 | §5.5 | TC-PROV-04, TC-PROV-05 | ☐ |
| ZTP-FR-21 | §12.1 | TC-UNIT-12, TC-UPG-01/05/06 | ☐ |
| ZTP-FR-22 | §12.2 | TC-UPG-02 | ☐ |
| ZTP-FR-23 | §12.3 | TC-UPG-03/04/08, TC-SESS-07, TC-NAT-05, TC-AUTH-07 | ☐ |
| ZTP-FR-24 | §13 | TC-UNIT-17, TC-VAL-01, TC-VAL-02 | ☐ |
| ZTP-FR-25 | §13, §12.5 | TC-VAL-03, TC-UPG-09 | ☐ |
| ZTP-FR-26 | §13 | TC-VAL-04 | ☐ |
| ZTP-FR-27 | §13 | TC-VAL-01, TC-VAL-05 | ☐ |
| ZTP-FR-28 | §14.1 | TC-SRV-01, TC-SRV-09, TC-PERF-01, TC-E2E-02 | ☐ |
| ZTP-FR-29 | §14.3 | TC-UNIT-11, TC-SRV-02, TC-UPG-06, TC-AUTH-09 | ☐ |
| ZTP-FR-30 | §14.4 | TC-SRV-03, TC-AUTH-03, TC-UPG-04, TC-VAL-05 | ☐ |
| ZTP-FR-31 | §14.5 | TC-SRV-04 | ☐ |
| ZTP-FR-32 | §14.5 | TC-SRV-05 | ☐ |
| ZTP-FR-33 | §17 | TC-LOG-01, TC-E2E-01 | ☐ |

### 10.2 Bonus
| PRD ID | DESIGN § | Test cases | Status |
|---|---|---|---|
| ZTP-BON-01 | §6.2 | TC-DISC-04/05/06/07/08 | ☐ |
| ZTP-BON-02 | §5.2, §8.1 | TC-UNIT-03, TC-REL-01 | ☐ |
| ZTP-BON-03 | §8.3 | TC-REL-02 | ☐ |
| ZTP-BON-04 | §8.4 | TC-UNIT-08, TC-REL-03, TC-REL-10 | ☐ |
| ZTP-BON-05 | §8.6 | TC-UNIT-09, TC-SESS-02/03, TC-REL-04/05, TC-PROV-11 | ☐ |
| ZTP-BON-06 | §16 | TC-UNIT-15, TC-SIM-01/02/03/04 | ☐ |
| ZTP-BON-07 | §8.7 | TC-REL-04 | ☐ |
| ZTP-BON-08 | §17.2 | TC-REL-03, TC-LOG-02 | ☐ |

### 10.3 Non-functional
| PRD ID | DESIGN § | Test cases | Status |
|---|---|---|---|
| ZTP-NFR-01 | §4.2, §19 | TC-DEL-01, TC-UNIT-01 | ☐ |
| ZTP-NFR-02 | §5.5 | TC-PROV-04 | ☐ |
| ZTP-NFR-03 | §8 | TC-REL-09, TC-NAT-07, TC-E2E-02, §6 matrix | ☐ |
| ZTP-NFR-04 | §16.3 | TC-PERF-02 | ☐ |
| ZTP-NFR-05 | §14 | TC-PERF-01, TC-SRV-08, TC-AUTH-10 | ☐ |
| ZTP-NFR-06 | §10, §18 | TC-UNIT-05, TC-AUTH-08, TC-AUTH-11, TC-NAT-06, TC-SESS-06, TC-DISC-07 | ☐ |
| ZTP-NFR-07 | §5.6, §14.6 | TC-UNIT-04, TC-SRV-07, TC-SRV-08, TC-REL-08, TC-PERF-04 | ☐ |
| ZTP-NFR-08 | §17 | TC-LOG-03, TC-LOG-04 | ☐ |
| ZTP-NFR-09 | §16.1 | TC-UNIT-15 | ☐ |
| ZTP-NFR-10 | §15.3, §14.4 | TC-UNIT-13, TC-UPG-07, TC-SRV-06 | ☐ |
| ZTP-NFR-11 | §19.1 | TC-DEL-01, TC-DEL-08 | ☐ |

### 10.4 Deliverables
| PRD ID | DESIGN § | Test cases | Status |
|---|---|---|---|
| ZTP-DEL-01 | §19 | TC-DEL-02, TC-E2E-02 | ☐ |
| ZTP-DEL-02 | §19.1 | TC-DEL-01 | ☐ |
| ZTP-DEL-03 | §4.2 | TC-DEL-03 | ☐ |
| ZTP-DEL-04 | §4.3 | TC-SESS-08 | ☐ |
| ZTP-DEL-05 | §19, §21.3 | TC-DEL-04 | ☐ |
| ZTP-DEL-06 | §21.1 | TC-DEL-05 | ☐ |
| ZTP-DEL-07 | §21.3 | TC-DEL-06 | ☐ |
| ZTP-DEL-08 | §21.2, §21.3 | TC-DEL-07 | ☐ |
| ZTP-DEL-09 | §17.3 | TC-DEL-02, TC-DEL-09 | ☐ |
| ZTP-DEL-10 | §17.3 | TC-DEL-02, TC-DEL-09 | ☐ |
| ZTP-DEL-11 | §14.5 | TC-DEL-02, TC-SRV-04 | ☐ |

**Coverage:** 63 of 63 PRD requirements have at least one test case. No test case is orphaned: each one in §5 references at least one PRD ID.

---

## 11. Entry and exit criteria, defect management, reporting

### 11.1 Entry criteria (per milestone)
- The milestone's code builds with `-Werror` on both platforms, and `make test` (L1) passes.
- The DESIGN sections concerned are frozen; the wire spec (§5) is frozen from M2 onward.
- The test hooks the milestone needs are available (DESIGN §19.2).

### 11.2 Exit criteria (release, M7)
1. **100% of P1 cases pass** on macOS and on Linux, in the release build and the ASan build.
2. ≥ 90% of P2 cases pass; every P2 failure has a signed waiver.
3. Impairment rows M1–M4 and M7–M9 pass for all seeds; M5 ≥ 95%.
4. Success metrics SM-1…SM-8 (PRD §3.3) met.
5. No open Sev-1 or Sev-2 defects.
6. The traceability matrix (§10) shows every requirement as ✅ or ⚠ (waived with sign-off).
7. The README checklist (§9) is complete.

### 11.3 Defect severity
| Sev | Definition | Examples | Rule |
|---|---|---|---|
| 1 Critical | Crash, data corruption, security bypass, or a required flow broken | Accepting a bad MAC; a duplicate REGISTER after a restart; ASan error | Blocks the milestone gate |
| 2 Major | A requirement fails under some conditions | Fails at 20% loss for one seed; resume re-downloads everything | Must be fixed before release |
| 3 Minor | A deviation that doesn't affect the outcome | Wrong log wording; misaligned dump columns | Fix or waive |
| 4 Trivial | Cosmetic or documentation | Typo | Backlog |

Every defect report includes: TC ID, platform, build variant, **seed and netsim flags**, the log excerpts, and the pcap if relevant.

### 11.4 Reporting
- Per milestone: a one-page test summary (passed, failed, blocked; new defects; risks).
- At release: `TEST_REPORT.md` with the traceability matrix filled in, the impairment matrix results (time and retx ratio per row), and the perf numbers.

---

## 12. QA timeline, dependencies, assumptions, risks
*The overall plan is owned by [PRD §11](PRD_ZTP.md#11-release-plan-milestones-and-timeline). This is the QA view of it.*

### 12.1 QA timeline (in parallel with development; indicative start Mon 2026-11-16)
| Window | QA activity | Effort |
|---|---|---|
| P0 (16–17 Nov) | Review PRD/DESIGN for testability; freeze this plan | 1 d |
| M1 (18–19 Nov) | Build `peer_sim`, the log checkers (`check_logs.sh`, `check_invariants.sh`) and `pcapcheck.sh`; review the L1 tests | 2 d |
| M2 (20–24 Nov) | Run SESS, REL and NAT; seeded matrix M1–M4 | 2 d |
| M3 (25–26 Nov) | Run AUTH, replay and security cases | 1 d |
| M4 (27–30 Nov) | Run PROV, including kill -9 and server restart | 1 d |
| M5 (1–2 Dec) | Run UPG and VAL, crash points | 1 d |
| M6 (3 Dec) | Run SRV, PERF and E2E | 1 d |
| M7 (4–7 Dec) | Full regression on both platforms, the whole matrix, soak, README checklist, test report | 2–3 d |
| **Total QA effort** | | **≈ 11–12 days**, overlapping development; about 2.5 days on the critical path after code freeze |

### 12.2 QA dependencies
- QA-D1: test hooks built into the device and server (`--bad-key`, `--replay-test`, `--reuse-nonce`, `--crash-after-chunks`, `--crash-at`, `--rebind-after`, `--fail-validate`, `--corrupt-chunk`, `--drop-seq`). These must land with each milestone.
- QA-D2: **GitHub Actions** on https://github.com/heysunilvk/ztp-project (PRD D-09) as the Linux environment for netem (TC-SIM-03), broadcast (TC-DISC-05b), valgrind and the gcc build. There's no local Linux host or VM, because of limited disk space on the Mac. CI is set up in M1.
- QA-D3: the Xcode CLT on the macOS host. Satisfied: verified on 2026-11-10 (PRD D-08).
- QA-D4: `tcpdump` permission on the test host (sudo), for the pcap-based cases.

### 12.3 QA assumptions
- Loopback timing is representative enough of LAN behaviour; WAN conditions are approximated with `--delay`.
- The test hooks don't change production code paths beyond the injection point (verified by code review).
- Seeds 1–10 give sufficient coverage of the impairment matrix; a failing seed is added to the regression set permanently.

### 12.4 Risks to testing
| Risk | Mitigation |
|---|---|
| Timing-dependent flaky tests | Assert on counts and states; generous bounds; re-run with the same seed to separate a flake from a bug |
| A test hook masks real behaviour | Hooks are reviewed; at least one kill -9 is a *real* external kill (TC-PROV-08) |
| Platform-specific socket behaviour (broadcast, DF) | Run both platforms; known platform differences documented (DESIGN §6.2) |
| Limited test time at M7 | Most cases are automated and run from M2 onward, so M7 is mainly regression |

---

## 13. Caveats: what is not or cannot be tested
| Gap | Why | Mitigation / substitute |
|---|---|---|
| Real NAT devices (port-restricted, symmetric, CGNAT) | No NAT hardware in the lab | Rebinding simulation (TC-NAT-05), idle-timeout logic (TC-NAT-03/04). Optional: a Linux `iptables MASQUERADE` namespace test (§14) |
| Real power loss and flash behaviour | No hardware | Crash injection at every commit point (TC-UPG-07); atomic-write unit tests |
| Real wireless or satellite loss patterns | No RF test bed | Correlated loss with netem on CI (M7); a range of seeds |
| Interactive Linux debugging | No local Linux host | Reproduce with the same seed on the Mac (netsim is deterministic); CI uploads logs and pcaps as artifacts; a temporary `tmate` debug step in CI can be enabled if needed |
| Long-term fleet scale (10k+ devices) | Out of scope | 50-device test plus a 200-device exploratory run; design analysis (DESIGN §14.1) |
| Cryptographic strength | Not a QA activity | Standard primitives with known-answer tests; design review |
| Confidentiality | Not implemented by design | Documented limitation (DESIGN §18.3) |

---

## 14. QA enhancement suggestions
1. **Coverage-guided fuzzing** (libFuzzer or AFL++) of `ztp_decode()` and the TLV parser, run in CI for every change.
2. **Network-namespace NAT lab** on Linux (`ip netns` + `iptables MASQUERADE` with short UDP conntrack timeouts) for realistic NAT tests.
3. **Property-based tests** of the reliability engine: arbitrary interleavings of loss, reorder, duplication and delay, with the invariants "exactly once, in order, eventually complete when loss < 100%".
4. **Extend the CI matrix.** The base matrix (Ubuntu and macOS × release and ASan × rows M1–M4) is already in scope (DESIGN §19.4). Next steps: nightly runs of all 10 seeds on every row, and badges in the README.
5. **Code coverage** (`gcov`/`llvm-cov`) with a target of ≥ 85% lines in `common/` and ≥ 75% overall.
6. **A Wireshark Lua dissector** for the ZTP header, which makes pcap review and defect reports much faster.
7. **Chaos mode in the demo**: random kill -9 of devices and the server during runs, checked against the invariants.

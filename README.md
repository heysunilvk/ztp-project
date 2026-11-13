# UDP-Based Zero Touch Provisioning (ZTP)

A device that boots with no configuration finds a provisioning server, opens an authenticated session over
UDP, registers, downloads its configuration and firmware in chunks, upgrades and restarts itself, resumes
without registering again, validates, and becomes **OPERATIONAL**. Everything is written in C11 with no
dependencies beyond libc, runs on macOS and Linux, and uses UDP only.

This README answers the assignment ("UDP‑Based Zero Touch Provisioning (ZTP) Assignment.pdf", here *the PDF*).
The detailed documents behind it are [PRD_ZTP.md](PRD_ZTP.md) (requirements), [DESIGN.md](DESIGN.md)
(architecture and protocol), [QA_TEST_PLAN.md](QA_TEST_PLAN.md) (test plan) and [TEST_REPORT.md](TEST_REPORT.md)
(the result of every test case); PDF copies are in [pdf/](pdf/).
Section references like *DESIGN §8.3* point into those documents.

> **For reviewers: status in brief**
> - **Implemented:** every numbered requirement of the PDF (§1–§9), the five README questions, Appendix A step by
>   step, and the whole bonus section (sequence numbers on every message, ACKs and retransmissions, idempotent
>   processing, loss/reorder/duplication simulated in code and with netem). Multicast discovery is the bonus option.
> - **The demo runs the PDF's five devices**, and the committed dump has the PDF's five rows, including the failed
>   one: ZTP-84BC19 has a wrong model key (`--bad-key` test hook) and is rejected with "Invalid HMAC" (§10).
> - **Open:** the hostile-traffic tests (forged, replayed or flooded frames) and the Appendix A literal nonce
>   984312 need test tools that were not built (`--replay-test`, `--nonce`, `peer_sim` attack modes). The
>   protection logic is implemented; this is a waiver by the project owner, listed in §18.
> - **Verify in five minutes:** `make && make demo` (about 30 s), then read [logs/README.md](logs/README.md) and
>   compare [logs/appendix_a/](logs/appendix_a/) with the PDF's Appendix A (§14.2). Every test case's result, with
>   a link to its output: [TEST_REPORT.md](TEST_REPORT.md). The full regression: `make release` (§16, §17).

**Contents**
1. [Quick start](#1-quick-start)
2. [Where the PDF's requirements are answered](#2-where-the-pdfs-requirements-are-answered)
3. [Folder structure](#3-folder-structure)
4. [Architecture and control flow](#4-architecture-and-control-flow)
5. [UDP session management](#5-udp-session-management)
6. [Identity, authentication and registration](#6-identity-authentication-and-registration)
7. [Configuration and firmware provisioning](#7-configuration-and-firmware-provisioning)
8. [Version upgrades and restarts](#8-version-upgrades-and-restarts)
9. [Validation and operational mode](#9-validation-and-operational-mode)
10. [Server robustness, the registration database and its dump](#10-server-robustness-the-registration-database-and-its-dump)
11. [README requirements: the five questions](#11-readme-requirements-the-five-questions)
12. [Why keepalives and session-timeout detection matter](#12-why-keepalives-and-session-timeout-detection-matter)
13. [Bonus: packet loss, reordering and duplicates](#13-bonus-packet-loss-reordering-and-duplicates)
14. [Logs](#14-logs)
15. [Configuration reference](#15-configuration-reference)
16. [Running the tests](#16-running-the-tests)
17. [Test evidence and CI](#17-test-evidence-and-ci)
18. [Test coverage and known gaps](#18-test-coverage-and-known-gaps)
19. [Known limitations and tradeoffs](#19-known-limitations-and-tradeoffs)

---

## 1. Quick start

Requirements: a C compiler (clang ≥ 11 or gcc ≥ 9), GNU make (3.81 or later), POSIX `sh`. The programs need
nothing else; some test scripts also use `python3` as an independent JSON and timing checker (preinstalled on
macOS and the CI runners).

```sh
git clone https://github.com/heysunilvk/ztp-project.git && cd ztp-project
make              # build ztp-server, ztp-device and the mock artifacts (-Wall -Wextra -Werror)
make demo         # one server and five devices over a lossy network; writes logs/ and prints PASS/FAIL
cat logs/db_dump.txt
```

`make demo` takes about 30 s. It runs the PDF's five devices concurrently at 15% loss, 10% reordering and 5%
duplication per direction, then checks the dump and every log (section 10). The logs it writes are the ones
committed in [logs/](logs/).

Running the two programs by hand (two terminals, from the repository root):

```sh
./ztp-server --profile demo                                   # listens on UDP 5555 (discovery on 5556)
./ztp-device --profile demo --state-dir state/d1 --serial ZTP-93AF21 --fw v1.0 \
             --discover static --server 127.0.0.1:5555        # or leave out --discover/--server: multicast
```

The device upgrades to v1.1, restarts itself, resumes and reports OPERATIONAL. Stop the server with Ctrl-C to
print the database dump and summary. `--profile demo` uses short timers (2 s keepalives); without it the
production timers apply (20 s). Run the same device again and it resumes from its state directory; add
`--reset-state` to start from scratch. Every flag is listed by `--help`.

To run every test: `make test` (about 15 minutes; section 16).

---

## 2. Where the PDF's requirements are answered

| PDF section | Requirement | Where | Tested by (QA) |
|---|---|---|---|
| §1 Boot & discovery | Soft serial; static IP; broadcast/multicast (bonus) | [§4](#4-architecture-and-control-flow), DESIGN §6 | TC-DISC-01..08, TC-PDF-01 |
| §2 Session | Sequence numbers, timeouts, retransmission, session id | [§5](#5-udp-session-management) | TC-SESS-01..08 |
| §3 Reliable messaging | ACKs, duplicate detection, retransmission; 10–20% loss | [§5](#5-udp-session-management), [§11 Q1, Q5](#11-readme-requirements-the-five-questions) | TC-REL-01..11, TC-SIM-01..03 |
| §4 NAT & keepalives | Keepalives, timeout detection, *why it matters* | [§5](#5-udp-session-management), [§12](#12-why-keepalives-and-session-timeout-detection-matter) | TC-NAT-01..13 |
| §5 Secure registration | Serial, metadata, nonce; HMAC; replay protection | [§6](#6-identity-authentication-and-registration), [§11 Q2](#11-readme-requirements-the-five-questions) | TC-AUTH-01..15 |
| §6 Provisioning | JSON config, firmware blob, chunks, hash, resume, no fragmentation | [§7](#7-configuration-and-firmware-provisioning), [§11 Q3](#11-readme-requirements-the-five-questions) | TC-PROV-01..13 |
| §7 Upgrade & restart | Upgrade, restart the process, resume without duplicate registration | [§8](#8-version-upgrades-and-restarts) | TC-UPG-01..11 |
| §8 Validation | Config, firmware version, session health → OPERATIONAL | [§9](#9-validation-and-operational-mode) | TC-VAL-01..05 |
| §9 Server robustness | Concurrent devices, per-device assignment, DB, dump and summary | [§10](#10-server-robustness-the-registration-database-and-its-dump) | TC-SRV-01..11, TC-PDF-02 |
| README requirements | Reliability, replay, fragmentation, NAT, packet loss | [§11](#11-readme-requirements-the-five-questions) | TC-DEL-05 |
| Appendix A | The nine-step journey of ZTP-93AF21 | [§14.2](#142-the-appendix-a-journey), [logs/appendix_a/](logs/appendix_a/) | TC-APX-01..10 |
| Bonus | Loss, reordering, duplicates, simulation, four questions | [§13](#13-bonus-packet-loss-reordering-and-duplicates) | TC-REL-*, TC-SIM-*, TC-LOG-02 |
| Expected Submission §1 | Runnable code, device/server separation, UDP only, build instructions | [§1](#1-quick-start), [§3](#3-folder-structure) | TC-DEL-01..03, TC-PDF-05 |
| Expected Submission §2 | Architecture, session, identity, provisioning, upgrades, limitations | [§4](#4-architecture-and-control-flow)–[§9](#9-validation-and-operational-mode), [§19](#19-known-limitations-and-tradeoffs) | TC-DEL-06, TC-DEL-07 |
| Expected Submission §3 | Server and client logs | [§14](#14-logs), [logs/](logs/) | TC-DEL-09 |

---

## 3. Folder structure

The device and the server are separate programs that share only `common/`. Neither includes the other's
headers, and the build checks that neither binary contains the other's symbols (`make check-separation`).

```
.
├── README.md                 this file
├── PRD_ZTP.md                product requirements: requirement IDs, scenarios S1–S5, milestones, risks (R-15: open tooling)
├── DESIGN.md                 architecture, wire protocol, state machines, configuration model, README drafts
├── QA_TEST_PLAN.md           test strategy, every test case (TC-*), the impairment matrix, gates and the open-tooling register
├── TEST_REPORT.md            release test report: the result, where it ran and the evidence for each of the 165 cases
├── UDP‑Based Zero Touch Provisioning (ZTP) Assignment.pdf   the assignment
├── pdf/                      PDF exports of PRD, DESIGN, QA_TEST_PLAN and TEST_REPORT (regenerated from the .md files)
├── Makefile                  build and every run: test, demo, appendix-a, scenario-logs, matrix, scale, soak, longrun,
│                             release, asan, debug, check-separation, spike, clean, clean-logs (GNU Make 3.81-compatible)
├── .github/workflows/ci.yml  GitHub Actions: 8 jobs on Ubuntu and macOS (section 17)
├── .gitignore                build outputs, state/, results/
│
├── common/                   shared by both binaries (libztpcommon.a); no device or server logic
│   ├── protocol.[ch]         wire format: 44-byte header, TLVs, HMAC-SHA256 MAC apply and verify
│   ├── reliable.[ch]         reliability engine: send window, RTO, SACK, fast retransmit, reorder buffer, duplicates
│   ├── netsim.[ch]           in-code loss, reordering, duplication, delay, jitter and small-MTU simulation
│   ├── sha256.c, hmac.c, crypto.h   SHA-256, HMAC-SHA256, key derivation, constant-time compare
│   ├── rand.[ch]             CSPRNG (arc4random_buf on macOS and the BSDs, /dev/urandom on Linux)
│   ├── net.[ch]              UDP sockets, multicast and broadcast; IPv4 only
│   ├── timer.[ch]            monotonic clock and timer heap
│   ├── json.[ch]             strict JSON parser (64 KiB, depth 16; no YAML)
│   ├── config.[ch]           configuration loader: defaults, file, profile, --set; ranges; --print-config
│   ├── log.[ch], logmsg.h    tagged millisecond logs to stdout and a file; every log string in one header
│   ├── hooks.[ch]            test hooks, refused unless --test-hooks is given; each fires once per state directory
│   └── util.[ch]             atomic file writes, hex, version comparison
│
├── device/                   ztp-device
│   ├── main.c                command line, state directory, soft serial, boot
│   ├── fsm.c, dev.h          event loop and state machine (BOOT … OPERATIONAL / FAILED)
│   ├── discovery.c           static server, multicast or broadcast discovery with an authenticated OFFER
│   ├── session.c             HELLO, resume, path probe, keepalives, liveness, REGISTER
│   ├── keys.c                factory keys (derived from a demo seed; a real device would use a secure element)
│   ├── provision.c           per-file download: manifest, chunk requests, hash check, small-MTU step-down
│   ├── transfer.[ch]         chunk bitmap persisted for resume
│   ├── upgrade.c             A/B firmware slots, commit point, restart (execv or exit 75), rollback
│   ├── validate.[ch]         configuration, firmware and session-health checks
│   ├── persist.c             device.state (key=value, atomic, with a .bak copy)
│   └── natsim.c              NAT emulator for tests: mapping expiry, rebinding, IP change, port blocking
│
├── server/                   ztp-server (single-threaded, poll())
│   ├── main.c                command line, startup checks, sockets, signals, event loop
│   ├── handlers.c            receive pipeline and one handler per message type; nonce cache
│   ├── session.c, srv.h      session table, timer heap, serial index, grace and expiry
│   ├── auth.[ch]             keys.json, key derivation, MAC checks, rate limits
│   ├── policy.[ch]           firmware/config assignment per model or serial, NAT overrides per tenant/model/serial
│   ├── artifacts.[ch]        loads and hashes every config and firmware file, serves chunks
│   ├── db.[ch]               registration database: JSONL journal, dump and summary
│   └── fuzz.[ch]             in-process datagram fuzzer (test hook --self-fuzz)
│
├── conf/                     runtime configuration (section 15)
│   ├── server.json           server defaults
│   ├── device.json           device "factory defaults"
│   ├── policy.json           which firmware and config each model gets
│   ├── keys.json             fleet and per-model keys (demo values; must be mode 0600)
│   └── profiles/{server,device}/   default, demo, strict-nat, cgnat, lan, metered overlays
│
├── tools/
│   ├── mkartifacts.c         builds the mock firmware images and configs in artifacts/ (deterministic)
│   └── netspike.c            platform probe: does multicast/broadcast work on this host?
│
├── scripts/
│   ├── run_demo.sh           the five-device demo (S1–S5) and its checks (make demo)
│   ├── run_matrix.sh         the demo over the impairment matrix rows and seeds (make matrix)
│   ├── run_scale.sh          N devices at once at 15% loss (make scale)
│   ├── run_soak.sh           5 devices for an hour: memory growth and leaks (make soak)
│   ├── run_release.sh        the release regression: every suite, ASan, matrix, perf, soak, long runs, logs (make release)
│   ├── run_scenario_logs.sh  writes logs/scenarios/: a generated serial, a session timeout (make scenario-logs)
│   └── ci_evidence.sh        turns a CI run into a Markdown report (needs curl and jq)
│
├── tests/
│   ├── unit_main.c, test.h, test_*.c   unit tests: crypto vectors, codec, JSON, config, reliability engine, …
│   ├── run_suite.sh          runs one suite and records it in results/<run>/summary.txt
│   ├── cli_config.sh, m2_*.sh … m6_*.sh   integration suites, one per milestone (section 16.3)
│   ├── check_appendix_a.sh   the Appendix A journey, step by step (also writes logs/appendix_a/)
│   ├── check_logs.sh, check_invariants.sh, check_pdf05.sh   log, invariant and "UDP only" checks
│   ├── peer_sim/peer_sim.c   a scripted protocol peer for negative tests
│   ├── pcapcheck/pcapcheck.c checks every frame in a packet capture: valid header, no IP fragments, max size, no seq gaps
│   └── conf/                 good and bad configuration fixtures
│
├── logs/                     committed: the demo (server, 5 devices, dump), appendix_a/ (the PDF's Appendix A, also
│                             on 10.10.10.5 from CI) and scenarios/; logs/README.md explains each file (section 14)
│   └── appendix_a/           committed: the single-device Appendix A run
├── evidence/                 committed: release-<date>/ (the final release regression run) and ci/ (CI run reports)
│                             with evidence/README.md as the index (section 17)
│
├── artifacts/                generated by make: mock firmware and configs the policy points at
├── build/, build-asan/, build-debug/   generated: objects and binaries (ztp-server, ztp-device are symlinks to build/)
├── state/                    generated: server journal and device state directories
└── results/                  generated, not committed: one folder per test run (section 16.2)
```

**Limitations worth knowing about here:** IPv4 only; keys are demo values derived in code and in
`conf/keys.json`; the test hooks are compiled into the binaries (refused without `--test-hooks`); the tests
need a free range of local UDP ports (see each script's header) and must not run twice at the same time on
one machine.

---

## 4. Architecture and control flow

```
        device (ztp-device)                                    server (ztp-server)
 ┌────────────────────────────────┐                    ┌──────────────────────────────────┐
 │ FSM: BOOT → DISCOVERY → SESSION│                    │ poll() loop, one thread          │
 │  → REGISTER → CONFIG → FIRMWARE│   UDP 5555 (+5556  │ handlers: HELLO, REGISTER,       │
 │  → UPGRADE → RESTART → RESUME  │◀─ for discovery) ─▶│  MANIFEST, CHUNK, STATUS, …      │
 │  → VALIDATE → OPERATIONAL      │                    │ session table + timer heap       │
 │ reliable.c  netsim.c  natsim.c │                    │ policy  artifacts  auth  db      │
 │ state dir: device.state, slots,│                    │ state/db.jsonl (journal)         │
 │  config.json, chunk bitmaps    │                    │ logs/db_dump.txt (dump, summary) │
 └────────────────────────────────┘                    └──────────────────────────────────┘
```

Both programs are single-threaded event loops around `poll()`. Every frame is a 44-byte header (magic,
version, type, flags, epoch, session id, seq, ack, SACK bitmap, payload length, key id, 16-byte MAC) followed by
TLVs, in one UDP datagram of at most 1200 bytes. The device always initiates; the server only replies.

The control flow follows the PDF's Appendix A (DESIGN §4.5 has the full sequence diagram):

| Step | Device | Messages | Server |
|---|---|---|---|
| 1 Boot | Loads or generates its soft serial (`ZTP-` + 6 hex digits), reads the running firmware from its active slot | — | — |
| 2 Discovery | Static IP, or a multicast/broadcast DISCOVER answered by an authenticated OFFER | DISCOVER, OFFER | Rate-limited per source IP |
| 3 Session | Sends HELLO with a fresh nonce | HELLO, HELLO_ACK | Allocates a session id, sends its own nonce and the keepalive/grace values; both derive the session key |
| 4 Keepalive | A path probe right away, then a keepalive whenever idle | KEEPALIVE, KEEPALIVE_ACK | Tracks last activity per session |
| 5 Registration | Serial, model, hardware revision, firmware, a 64-bit nonce, under the session key | REGISTER, REGISTER_ACK / NAK | Checks the MAC and nonce, applies the policy, writes the DB row |
| 6 Config | Pulls the configuration in chunks, verifies SHA-256, keeps it | MANIFEST_REQ, MANIFEST, CHUNK_REQ, CHUNK_DATA, TRANSFER_DONE | Serves chunks; keeps no transfer state |
| 7 Upgrade | Pulls the firmware, verifies it, installs it into the inactive slot, commits, restarts its own process | (same), UPGRADE_STATUS | Waits for the resume (AWAIT_RESUME) |
| 8 Resume | Resumes the same session id under its device key; no REGISTER | HELLO (resume), HELLO_ACK (resumed) | Accepts the resume, bumps the epoch, updates the return address |
| 9 Validation | Config present, firmware equals the target, session healthy | KEEPALIVE (health probe), STATUS_REPORT, STATUS_ACK | Marks the device OPERATIONAL |

---

## 5. UDP session management

- **Handshake (DESIGN §7).** HELLO carries the serial, model, firmware and a device nonce, authenticated with
  the fleet bootstrap key. HELLO_ACK returns a random 32-bit session id, a server nonce and the timers. Both
  sides then derive the session key `K_s` from the device key and both nonces. A retransmitted HELLO gets the
  same answer (it is keyed by serial and nonce), so duplicates never create two sessions.
- **Sequence numbers and acknowledgements (DESIGN §8).** Each direction has its own 32-bit sequence space.
  Every header carries a cumulative ACK and a 32-bit SACK bitmap; up to 8 reliable frames may be in flight.
- **Timeouts and retransmission.** The retransmission timeout follows RFC 6298 (smoothed RTT, Karn's rule),
  starting at 500 ms with a 200 ms floor and a 4 s cap, doubling on each retry, at most 8 retries. Three
  SACKed frames after a gap trigger a fast retransmit.
- **Session id, not address.** A session is found by its session id, never by IP and port, so a NAT
  rebinding does not break it. The server moves the return address only for a frame with a valid MAC and a
  sequence number it has never seen.
- **Keepalives and liveness (DESIGN §9).** The device sends a keepalive after `keepalive_interval_s` of
  silence (20 s in production, 2 s in the demo profile). Three missed intervals mean the session is dead; the
  server applies the same rule and then keeps the registration for a grace period (120 s).
- **Resume (DESIGN §12.3).** After a restart or a dead session the device sends a resume HELLO for the same
  session id, authenticated with its device key and a persisted monotonic counter. The server accepts it,
  starts a new epoch (fresh sequence spaces), and the device carries on where it stopped. If the server has
  forgotten the session, the device starts a fresh one and the server's REGISTER is an idempotent upsert.

---

## 6. Identity, authentication and registration

- **Identity.** The soft serial is generated once from the CSPRNG and stored in the state directory (or set
  with `--serial`). The model, hardware revision and factory firmware come from `conf/device.json`.
- **Keys (DESIGN §10.1).** The PDF says the device has "no certificates and no stored credentials", but the
  server must check an HMAC, so the device needs *some* secret. The design uses a fleet bootstrap key
  `K_fleet` (shared, used only for HELLO) and a per-model manufacturing key `K_mfg`. The device key is
  `K_dev = HMAC(K_mfg, serial)`, and each session key `K_s` is derived from `K_dev` and both nonces. The
  server holds `K_fleet` and every `K_mfg` in `conf/keys.json` (mode 0600). A real product would keep them in
  a secure element (section 19).
- **REGISTER (PDF §5).** It carries the soft serial, the metadata (model, hardware revision, firmware) and a
  64-bit random nonce, and the whole frame is authenticated with HMAC-SHA256 under `K_s` (a 16-byte tag).
  The server recomputes the device key from the serial, so a device with the wrong key fails with
  "Invalid HMAC"; three failures make the device stop (FAILED) and the server record a Failed row.
- **Replay protection.** See [Q2](#q2-how-are-replay-attacks-prevented).
- **Outcome.** The server applies the policy (model, or a serial override) and records the verdict once per
  device: Success with the firmware and config versions, or Failed with the reason.

---

## 7. Configuration and firmware provisioning

- **Artifacts.** `mkartifacts` writes deterministic mock files into `artifacts/`: the configuration
  `cfg_v3.json`, the legacy `cfg_v2.json`, and
  64 KiB firmware images with a small header naming the model and version. `conf/policy.json` says which one
  each model gets. `cfg_v3.json` contains the PDF's example configuration word for word (`interfaces`, `mtu`,
  `keepalive_interval`) plus realistic extra settings (tenant, NTP, syslog, VLANs, 44 ACL entries and a login
  banner padded with spaces). The padding is deliberate: the PDF's example alone is about 100 bytes, one chunk,
  while Appendix A shows `chunk 1/3` to `3/3`, so the file is sized to 2310 bytes, three 1024-byte chunks. The
  device validates the file's keys and stores it; the values describe the provisioned device (its WAN/LAN
  interfaces, its own keepalive setting) and are not applied to the ZTP session, whose keepalive interval comes
  from the server's policy in the HELLO_ACK (section 12).
- **Pull transfer (DESIGN §11.2).** The device asks for a manifest (size, chunk size, SHA-256), then
  requests missing chunks in batches of up to 32. The server reads chunks from the file and keeps no transfer
  state, so it can serve any number of devices and survives its own restart.
- **Chunks, not fragments.** Chunks are 1024 bytes, so every datagram is at most 1200 bytes and never needs
  IP fragmentation (DF is set). If the path is smaller, the session steps down to 508-byte datagrams and
  256-byte chunks and keeps what it already has ([Q3](#q3-why-is-ip-fragmentation-avoided)).
- **Integrity.** Each file is checked against the manifest's SHA-256 before it is used; a mismatch discards
  it and fetches it again (twice at most). The firmware header must also name the device's model and the
  target version.
- **Resume after failure.** The device persists a bitmap of received chunks every 16 chunks. After a crash,
  restart or lost session it requests only the chunks it does not have (the demo crashes a device at chunk
  40 of 64; it re-requests 32, not 64).

---

## 8. Version upgrades and restarts

- **Decision.** REGISTER_ACK names the target firmware. If the running version is older, the device
  downloads it (PDF §7: "If the firmware version is outdated").
- **Install (DESIGN §12.1, §12.4).** The image goes into the inactive slot (A/B). The commit point is one
  atomic write of `device.state` that switches the active slot and sets `pending_reboot`. A crash before the
  commit leaves the old firmware running; a crash after it boots the new one. Crash points at each stage are
  tested (TC-UPG-07).
- **Restart.** The device restarts its own process with `execv` (or exits with code 75 for a supervisor).
  The log shows `[UPGRADE] Upgrade complete, restarting device`, then `[BOOT] Restart detected`.
- **Resume without duplicate registration.** At boot the device sees `pending_reboot`, resumes its session
  ([section 5](#5-udp-session-management)) and **does not send REGISTER**. The server keeps the single DB
  row, logs `Device running v1.1 after the upgrade`, and notes "Resumed after upgrade" in the verbose dump
  (`db_dump_verbose.txt`). This is checked in every demo run (no REGISTER after a restart, one
  row per device).
- **Rollback (DESIGN §12.5).** If validation fails after the upgrade, the device switches back to the old
  slot, restarts, and reports the failure.

---

## 9. Validation and operational mode

After provisioning the device checks the three things the PDF lists (DESIGN §13):

| Check | How |
|---|---|
| Configuration availability | `config.json` exists, parses, and its SHA-256 matches the manifest; if not, it is fetched once more |
| Firmware version | The running slot's header names the target version (or the approved one when no upgrade was needed) |
| Session health | A health-probe keepalive is acknowledged and the smoothed RTT is under `val.srtt_max_ms` |

When all three pass it sends STATUS_REPORT OPERATIONAL, the server records the stage, and the device logs
`[STATE] Device is OPERATIONAL`. It then keeps its session alive with keepalives. If validation cannot pass
before `val.deadline_s` (180 s), the device reports FAILED.

---

## 10. Server robustness, the registration database and its dump

**Concurrency (DESIGN §14).** One thread runs a `poll()` loop over the control and discovery sockets. Each
device has its own session and reliability state, and timers sit in one heap, so many devices progress
independently with no locks. Tested with 50 devices at 15% loss (TC-PERF-01) and a fuzzer feeding 300,000 (in `make test`; 1 million under ASan in CI)
random and mutated datagrams (TC-SRV-07).

**Assignment.** The policy gives each model (or a specific serial) its firmware, config and tenant; NAT
timers can be overridden per tenant, model or serial. Unknown models are rejected.

**What is stored (PDF §9).** Each registration verdict is appended to `state/db.jsonl` (a journal, read back
at startup) and kept in memory: device id, registration time, firmware assigned, config version assigned,
outcome, and notes, plus the model, stage, attempts and last address for the verbose dump.

**Dump and summary.** On Ctrl-C/SIGTERM (or `kill -USR1` while running) the server prints the dump in the
PDF's format and a summary, and writes them to `logs/db_dump.txt` (and `logs/db_dump_verbose.txt` with extra
columns). The committed dump from the demo run:

```
Device Registration Details
+------------+---------------------+-------------------+----------------+---------------------+------------------+
| Device ID  | Registration Status | Firmware Assigned | Config Version | Registration Time   | Notes            |
+------------+---------------------+-------------------+----------------+---------------------+------------------+
| ZTP-55AC90 | Success             | v1.1              | v3             | 2026-11-13 17:11:06 | —                |
| ZTP-19FE77 | Success             | v1.0              | v2             | 2026-11-13 17:11:06 | Legacy device    |
| ZTP-71DD08 | Success             | v1.1              | v3             | 2026-11-13 17:11:06 | —                |
| ZTP-93AF21 | Success             | v1.1              | v3             | 2026-11-13 17:11:06 | —                |
| ZTP-84BC19 | Failed              | —                 | —              | 2026-11-13 17:11:07 | Invalid HMAC     |
+------------+---------------------+-------------------+----------------+---------------------+------------------+
Summary
  Registered successfully : 4   ZTP-55AC90, ZTP-19FE77, ZTP-71DD08, ZTP-93AF21
  Failed registration     : 1   ZTP-84BC19 (Invalid HMAC, 3 attempts)
  Firmware / config per device:
    ZTP-55AC90  EDGE-GW-100  v1.1 (current)   cfg v3  cruise_ops
    ZTP-19FE77  EDGE-GW-050  v0.9 -> v1.0     cfg v2  cruise_ops  Legacy device
    ZTP-71DD08  EDGE-GW-100  v1.0 -> v1.1     cfg v3  cruise_ops
    ZTP-93AF21  EDGE-GW-100  v1.0 -> v1.1     cfg v3  cruise_ops
  Protocol counters: rx=83 tx=320 retx_timeout=21 retx_fast=38 dup=7 ooo=2 out_of_window=0 mac_fail=3 replay_rejected=0 rate_limited=0
```

**Compared with the PDF's example dump:**

| PDF row | Demo device | Result here |
|---|---|---|
| ZTP-93AF21 Success v1.1 v3 | v1.0, upgrades to v1.1 | Matches |
| ZTP-84BC19 Failed — — "Invalid HMAC" | a wrong model key (`--bad-key`): three REGISTERs rejected, then FAILED | Matches |
| ZTP-71DD08 Success v1.1 v3 | crashes at chunk 40, relaunched, resumes the transfer | Matches |
| ZTP-55AC90 Success v1.1 v3 | already on v1.1, its NAT rebinding is simulated | Matches |
| ZTP-19FE77 Success v1.0 v2 "Legacy device" | legacy model EDGE-GW-050 | Matches |

Times are real, so they differ from the PDF's, and rows are listed by device ID (the PDF lists them by time;
the verbose dump has each device's first-seen and registration times). **How the failed row is made:** ZTP-84BC19 is started with the
`--bad-key` test hook, a unit provisioned with the wrong model key. Its fleet key is right, so HELLO and the path
probe pass; its REGISTER, under a session key derived from the wrong model key, fails the server's HMAC check. The
server answers REGISTER_NAK "Invalid HMAC" and records a Failed row; the device verifies the NAK, retries with a
fresh session twice more, and stops in FAILED (`[AUTH] Registration failed: Invalid HMAC (3/3), giving up`). The
summary lists it under "Failed registration" with its attempt count (TC-AUTH-03, TC-SRV-05).

---

## 11. README requirements: the five questions

### Q1. How is reliability implemented over UDP?
Every frame carries a 32-bit sequence number in its own space per direction. Reliable frames stay in a send
window of 8 until acknowledged. Each header piggybacks a cumulative ACK and a 32-bit SACK bitmap, so the
sender knows exactly which frames arrived even when they arrive out of order. An unacknowledged frame is
retransmitted when its timeout expires (RFC 6298 estimator with Karn's rule; 500 ms initially, 200 ms floor,
4 s cap, exponential backoff, at most 8 retransmissions), or earlier by fast retransmit when three later
frames are SACKed. The receiver buffers out-of-order frames (32 slots), delivers them in order exactly once,
and drops and re-acknowledges duplicates. When the retry budget runs out the session is declared failed and
the device resumes it without losing progress, because all progress is persisted and every handler is
idempotent. (DESIGN §8)

### Q2. How are replay attacks prevented?
In layers, none of which needs a clock (the device has no real-time clock):
1. Every session key is derived from a fresh server nonce and a fresh device nonce, so a frame captured in
   one session fails the MAC check in every other session.
2. Within a session, the receive window is an anti-replay window: a sequence number already received is
   dropped.
3. REGISTER carries a 64-bit nonce that the server caches per serial; a reused nonce is rejected.
4. A resume carries a persisted, monotonic counter, so an old resume request cannot hijack or reset a live
   session.
5. An epoch number separates the frames before and after each resume.
(DESIGN §10.4)

### Q3. Why is IP fragmentation avoided?
Losing any fragment loses the whole datagram, so loss is amplified (two fragments at 20% loss lose 36% of
datagrams). Fragments after the first carry no UDP ports, so NATs, stateful firewalls and ACLs often cannot
match them and drop them; many networks drop fragments outright (RFC 8900). Reassembly costs receiver memory
and is a classic denial-of-service and evasion vector, and at high rates the 16-bit IPv4 ID wraps and can
mis-reassemble (RFC 4963). So every datagram is capped at 1200 bytes (safe on any IPv6 path and on IPv4 paths
with an MTU of 1228 or more), DF is set so an oversize datagram fails visibly instead of fragmenting, and
data is split into 1024-byte chunks at the application layer, where each datagram is useful on its own and
retransmitted on its own. If a path is smaller (an EMSGSIZE error, or large frames silently lost while small
ones arrive) the session steps down to 508-byte datagrams and 256-byte chunks. (DESIGN §5.5, §5.8; checked
on the wire in CI, where the largest datagram captured is 1076 bytes.)

### Q4. How does NAT affect the protocol design?
The NAT and firewall belong to the customer or ISP: we cannot configure them and they can change at any time,
so the protocol needs nothing from them except permission for outbound UDP.

| NAT behaviour | Design consequence |
|---|---|
| A mapping exists only after the inside host sends | The device always initiates; the server only replies. No port forwarding or hole punching |
| Idle UDP mappings expire (often 30–60 s, sometimes 20 s) | Keepalives when idle (20 s, configurable per profile or tenant), and a path probe right after the handshake |
| Silent expiry or a NAT reboot | Liveness detection on both sides and an authenticated resume that keeps the registration |
| The external port or IP changes (rebinding) | Sessions are identified by an authenticated session id, not IP:port; the return address moves only for a new authenticated frame |
| Stateful NATs accept replies only from the address they sent to | The server replies from the address the device targeted |
| Many devices behind one public IP (CGNAT) | Per-IP rate limits are generous and never used as identity |
| Middleboxes mishandle fragments | No fragmentation (Q3) |
| ALGs rewrite addresses in payloads | No IP addresses in payloads (except the OFFER on the local segment); every frame is MAC-protected |
| A firewall blocks UDP | Cannot be solved in the protocol: a clear diagnostic and a documented prerequisite (outbound UDP 5555/5556) |

(DESIGN §9, §9.7; tested with the NAT emulator in TC-NAT-01..13.)

**Deployment prerequisite.** The site's firewall must allow outbound UDP from the device to the server's
port 5555 (and to 5556 for discovery), and the replies to it. Nothing inbound is needed and no NAT
configuration is required; the NAT and firewall stay the customer's or ISP's.

### Q5. What happens under packet loss?
| Loss per direction | Transmissions per frame (model bound 1/(1−p)²) | Measured (TC-PERF-03) | Effect |
|---|---|---|---|
| 0% | 1.00 | — | Baseline |
| 10% | ≤ 1.23 | 1.16 | Mostly fast retransmits; barely visible |
| 20% | ≤ 1.56 | 1.35 | A few timeout stalls; time to OPERATIONAL still far under 60 s |
| 30% | ≤ 2.04 | 1.68 | Slower; an occasional session failure, then an automatic resume |
| 50% | ≤ 4.00 | not run | Expected: frequent session failures, converging slowly through resumes. Matrix row M6 (50%) is not part of the release run, so this row is a prediction, not a measurement |

In detail: a lost HELLO or REGISTER is retransmitted with identical bytes and answered idempotently; lost
chunks are re-sent individually (SACK, fast retransmit); a lost ACK is repaired by the next cumulative ACK or
by the re-ACK a duplicate triggers; a lost keepalive is retransmitted until the dead deadline, so with
production timers a false "dead session" needs about 14 consecutive failed round trips; loss beyond the retry
budget tears the session down and the device resumes it with jittered backoff, keeping every chunk it has.
Loss never corrupts state: frames are MAC-checked, handlers are idempotent and progress is written
atomically. Measured on the demo at 20% loss with 10% reordering and 5% duplication (matrix row M4 × 20 runs on the Mac,
TC-PERF-02): median 3.9 s and p95 10.6 s from first boot to OPERATIONAL. (DESIGN §8, §9.3, §11.4)

---

## 12. Why keepalives and session-timeout detection matter

The PDF (§4) asks why this is important in real deployments:

1. **NAT and firewall state expires silently.** If nothing is sent for longer than the middlebox's UDP idle
   timer (often 30–60 s, sometimes 20 s), the mapping disappears. The server's next reply is dropped and
   neither side notices without a liveness mechanism. Keepalives refresh the mapping.
2. **The server cannot reach the device on its own.** Behind NAT only device-initiated traffic opens the
   path, so the device must keep it open whenever it expects replies — during long verifications, or while
   OPERATIONAL and waiting for future changes.
3. **Failures are detected quickly.** Without timeouts, a device whose server crashed or whose link changed
   (Wi-Fi to cellular, DHCP renumbering) would wait forever. Three missed intervals bound detection to about
   60 s, and a resume then restores the session.
4. **Server resources are reclaimed.** Devices that disappear (power loss, decommissioning) must not hold
   session memory forever. Idle detection plus a grace period reclaims it while still allowing a quick
   resume.
5. **Rebinding is detected.** A keepalive from a new source port shows the NAT has rebound, so the server
   updates the return path before real traffic is lost.
6. **It is cheap.** Keepalives are sent only when idle and are small (92 bytes of UDP payload per round
   trip), and the interval is set per deployment for battery-powered or metered links.

Measured: an OPERATIONAL device with the production timers (20 s keepalive, 3 intervals) at 20% loss per
direction ran 30 minutes with no false session timeout (TC-NAT-07); the one-hour soak (five EDGE-GW-100 devices already on v1.1, kept OPERATIONAL) exchanged
9155 keepalives with no timeouts (TC-PERF-04); and devices behind the NAT emulator survive mapping expiry,
NAT reboots, rebinding and IP changes (TC-NAT-08..10).

---

## 13. Bonus: packet loss, reordering and duplicates

**How loss and out-of-order delivery are handled.** As in Q1: SACK tells the sender exactly what is missing,
fast retransmit repairs a gap within about one round trip, and the timeout repairs the rest. A frame ahead of
the next expected sequence number (within 32) is buffered and its SACK bit set; when the missing frame
arrives the buffer drains in order, so the application always sees strictly increasing sequence numbers.

**How duplicates are detected and ignored.** At two levels. The transport drops any sequence number at or
below the cumulative ACK, or already buffered, so a duplicate never reaches the application within an epoch.
Above that, handlers are idempotent across epochs and restarts: HELLO is keyed by (serial, nonce), a resume
by its counter, REGISTER is an upsert by serial, chunks are written at fixed offsets with a bitmap, and stage
reports only move forward.

**How loss and reordering were simulated.** Three ways:
- **In code (default, both platforms):** `common/netsim.c` wraps every send. `--loss`, `--reorder`, `--dup`,
  `--delay`, `--jitter` and `--sim-mtu` apply at the sender only, so 15% means 15% per direction. A seeded
  PRNG makes every run reproducible (`--seed`). The demo uses 15% loss, 10% reordering and 5% duplication.
- **Linux netem (CI):** the `impair-netem` job runs the demo with netsim off and
  `tc qdisc … netem loss 15% 25% delay 5ms reorder 10% 50% duplicate 5%` on loopback, which adds correlated
  (bursty) loss the in-code shim does not model.
- **NAT emulation:** `device/natsim.c` expires mappings, rebinds ports, changes the device's IP and blocks
  the server port, to test section 12's cases.

**The PDF's bonus log lines** appear in the logs (checked by TC-LOG-02), for example from
`logs/device_ZTP-19FE77.log`:
```
2026-11-13T17:11:07.060Z [UDP] Received packet seq=9 (out of order, expected=7)
2026-11-13T17:11:07.060Z [UDP] Buffered packet seq=9
2026-11-13T17:11:07.264Z [UDP] Duplicate packet seq=7 ignored
2026-11-13T17:11:07.541Z [UDP] Retransmitting seq=9 (timeout)
```

### B1. How does your implementation handle out-of-order packets?
A frame ahead of the next expected seq (within the 32-frame window) is stored in a ring buffer and its SACK
bit set, and an immediate ACK tells the sender about the gap. When the missing frame arrives it is delivered
and the buffer drained in order. Frames beyond the window are dropped. (DESIGN §8.4)

### B2. How do you prevent duplicate processing?
At the transport, by sequence number (above). At the application, by idempotent handlers keyed on stable
identifiers (serial and nonce, resume counter, chunk index, monotonic stage). So even a duplicate that
crosses a resume or a restart changes nothing. (DESIGN §8.5–8.6)

### B3. What happens if an ACK is lost but the data packet is received?
The receiver has already processed the data. The sender's timeout fires and it retransmits; the receiver
recognises the duplicate, does not apply it again, and re-acknowledges immediately. Any later frame from the
receiver also carries the cumulative ACK, so often the retransmission never happens. A response such as
REGISTER_ACK is its own reliable frame with its own retransmission. (DESIGN §8.7)

### B4. What tradeoffs did you make between simplicity and correctness?
Correctness is kept (exactly-once delivery, authentication, crash safety) and efficiency or generality is
given up: a fixed window instead of congestion control; a 200 ms timeout floor instead of RFC 6298's 1 s; a
single-threaded server; a shared per-model key instead of a PKI; integrity without confidentiality; an
append-only journal instead of a database; a small strict JSON parser instead of a library or YAML; a fixed
1200-byte datagram with one 508-byte fallback instead of full path-MTU discovery; and the chunk bitmap
persisted every 16 chunks instead of every chunk. (DESIGN §22.1)

---

## 14. Logs

### 14.1 Where the logs are

| Location | Written by | Contents | Committed |
|---|---|---|---|
| [logs/](logs/) | `make demo` | `server.log`, `device_<serial>.log` for each of the five demo devices, `db_dump.txt` (dump and summary), `db_dump_verbose.txt`; [logs/README.md](logs/README.md) lists every file | yes |
| [logs/appendix_a/](logs/appendix_a/) | `make appendix-a` | The single-device Appendix A run (ZTP-93AF21, model EDGE-GW-100, v1.0 → v1.1, session id 7421) on 127.0.0.1: `server.log`, `device_ZTP-93AF21.log`, `db_dump.txt`; and `linux-10.10.10.5/`, the same run on the PDF's address in CI | yes |
| [logs/scenarios/](logs/scenarios/) | `make scenario-logs` | `generated_serial/` (`[BOOT] Soft serial generated`, then `Soft serial loaded` on the next boot) and `session_timeout/` (`[NAT] Session timeout detected`, then a resume without a second REGISTER) | yes |
| `results/<date-time>/` | `make test`, `make matrix`, `make scale`, `make soak` | Every suite's output, logs and state, and `summary.txt` (section 16.2) | no |
| [evidence/](evidence/) | copied from one release run | The release regression's `results/` folder and the CI reports (section 17) | yes |

Running the binaries by hand writes `logs/server.log` and `logs/device_<serial>.log` (set by `paths.log`).

### 14.2 The Appendix A journey

`make appendix-a` runs the PDF's Appendix A with its exact values: ZTP-93AF21, EDGE-GW-100, firmware v1.0,
static server on port 5555, first session id 7421 (a test hook makes it reproducible). On the Mac it uses
127.0.0.1:5555, because the PDF's 10.10.10.5 would need a `sudo` address alias; CI runs it on 10.10.10.5
(job `platform-linux`), and that run's logs are committed in
[logs/appendix_a/linux-10.10.10.5/](logs/appendix_a/linux-10.10.10.5/), where Step 2 reads exactly
`[DISCOVERY] Using static server IP 10.10.10.5:5555`. Each PDF line appears in [logs/appendix_a/device_ZTP-93AF21.log](logs/appendix_a/device_ZTP-93AF21.log), in order, as
the start of a line (some lines add detail after the PDF's words, such as `(rtt=0 ms)` or `epoch=1`):

```
# Step 1
2026-11-13T17:11:23.099Z [BOOT] Device starting (boot #1)
2026-11-13T17:11:23.099Z [BOOT] Soft serial: ZTP-93AF21
2026-11-13T17:11:23.099Z [BOOT] Firmware version: v1.0
# Step 2
2026-11-13T17:11:23.100Z [DISCOVERY] Using static server IP 127.0.0.1:5555
# Step 3
2026-11-13T17:11:23.100Z [UDP] HELLO sent (seq=1)
2026-11-13T17:11:23.100Z [UDP] Session established (session_id=7421)
# Step 4
2026-11-13T17:11:23.100Z [NAT] Keepalive sent
2026-11-13T17:11:23.100Z [NAT] Keepalive acknowledged (rtt=0 ms)
# Step 5
2026-11-13T17:11:23.100Z [AUTH] REGISTER: session_id=7421 soft_serial=ZTP-93AF21 model=EDGE-GW-100 firmware=v1.0 nonce=16416830223895452106
2026-11-13T17:11:23.102Z [AUTH] Registration successful
2026-11-13T17:11:23.102Z [AUTH] Assigned tenant: cruise_ops
# Step 6
2026-11-13T17:11:23.111Z [PROVISION] Receiving config (chunk 1/3)
2026-11-13T17:11:23.111Z [PROVISION] Receiving config (chunk 2/3)
2026-11-13T17:11:23.111Z [PROVISION] Receiving config (chunk 3/3)
2026-11-13T17:11:23.112Z [PROVISION] Config checksum verified
# Step 7
2026-11-13T17:11:23.118Z [UPGRADE] Current: v1.0 -> Target: v1.1
2026-11-13T17:11:23.310Z [UPGRADE] Upgrade complete, restarting device
# Step 8
2026-11-13T17:11:23.318Z [BOOT] Restart detected (pending upgrade to v1.1)
2026-11-13T17:11:23.334Z [UDP] Session resumed (session_id=7421) epoch=1
# Step 9
2026-11-13T17:11:23.334Z [VALIDATION] Configuration present (v3, sha ok)
2026-11-13T17:11:23.334Z [VALIDATION] Firmware v1.1 verified
2026-11-13T17:11:23.343Z [STATE] Device is OPERATIONAL
```

One difference from the PDF: the Step 5 nonce is random (`nonce=` a 64-bit value), not the PDF's 984312.
Fixing it needs the `--nonce` hook, which is part of the open tooling (section 18).

### 14.3 Format
Each line is `<UTC time, ISO 8601 with milliseconds> [TAG] message`, for example
`2026-11-12T05:50:40.842Z [BOOT] Soft serial: ZTP-93AF21`; server lines about a session add `[sid=… serial]`. Tags: `BOOT`, `DISCOVERY`, `UDP`, `NAT`,
`AUTH`, `PROVISION`, `UPGRADE`, `VALIDATION`, `STATE`, `DB`, `CONFIG`, `SIM`, `ERROR`, and `TEST` (a test hook,
only with `--test-hooks`; the demo uses some, for example the S3 crash and the S4 rebinding, and the Appendix A run forces session id 7421). The PDF's
lines start with the PDF's words; some add detail after them. At exit each process prints its counters (frames, retransmissions
by timeout and fast retransmit, duplicates, out-of-order frames, MAC failures). `-v` adds debug lines
(every simulated drop, every ACK).

---

## 15. Configuration reference

### 15.1 Files

| File | Read by | Purpose |
|---|---|---|
| `conf/server.json` | server | Network, NAT/session timers, reliability, transfer, limits, paths, logging, simulation |
| `conf/device.json` | device | "Factory defaults": identity, discovery, NAT bounds, reliability, transfer, validation, paths, simulation |
| `conf/policy.json` | server | Firmware/config assignment per model or serial; optional NAT overrides per tenant, model, serial |
| `conf/keys.json` | server | `fleet` key and one key per model, 64 hex digits (32 bytes) each. Must be mode 0600 (the server refuses otherwise; `make` sets it) |
| `conf/profiles/server/<name>.json`, `conf/profiles/device/<name>.json` | each side | Overlays applied after the file with `--profile <name>` |
| `<state-dir>/config.json` | device | The provisioned configuration (from the server) |

**Precedence (later wins, key by key):** built-in defaults → the config file (`--config`, default
`conf/server.json` or `conf/device.json`) → the profile overlay (`--profile`) → command-line flags and
`--set key=value`. The server then applies policy NAT overrides (tenant → model → serial) per device and
advertises the keepalive, dead-interval and grace values in HELLO_ACK; the device uses them, clamped to its
`nat.keepalive_min_s..keepalive_max_s`. `--print-config` prints the merged result with the source of every
value. Unknown keys, wrong types and out-of-range values stop the program at startup with exit code 78 and a
message naming the key and file (`--config-lenient` turns unknown keys into warnings). Files are strict JSON
(no comments; keys starting with `_`, such as `"_comment"`, are ignored).

### 15.2 Keys

**S** = server, **D** = device. Ranges are inclusive. Values marked † are cross-checked (15.3).

| Key | Side | Type | Default | Range / values | Effect |
|---|---|---|---|---|---|
| `schema_version` | S D | int | 1 | 1 | File format version |
| `profile` | S D | string | `default` | a name in `conf/profiles/` | Profile to apply when `--profile` is not given |
| `net.bind` | S | IPv4 | 0.0.0.0 | — | Address to listen on |
| `net.server_port` | S | int | 5555 | 1–65535 † | Control port |
| `net.discovery_port` | S | int | 5556 | 1–65535 † | Discovery port (≠ server port) |
| `net.mcast_group` | S | IPv4 | 239.255.90.90 | multicast address | Discovery group |
| `net.mcast_ttl` | S | int | 1 | 1–255 | TTL of multicast OFFERs |
| `net.mcast_iface_addr` | S | IPv4 | 0.0.0.0 | — | Interface for multicast (0.0.0.0 = that of `net.bind`) |
| `net.max_datagram` | S D | int | 1200 | 508–1200 | Largest datagram; never above 1200 |
| `net.set_df` | S D | bool | true | — | Set Don't Fragment |
| `net.mtu_fallback` | S D | bool | true | — | Allow the step-down to 508-byte datagrams |
| `net.reconnect_min_s` / `reconnect_max_s` | D | number | 1 / 60 | 0.1–3600 | Backoff between reconnect attempts |
| `net.resume_cycles` | D | int | 10 | 1–100 | Resume attempts before starting a fresh session |
| `net.pmtu_blackhole_s` | D | int | 8 | 2–60 | Silent interval with no chunk data: the first re-requests the chunks, a second one in a row means large frames are lost (step down to 508 B); detection ≈ 2 × this |
| `nat.keepalive_interval_s` | S D | int | 20 | 1–300 † (< 5 warns) | Idle time before a keepalive; the server's value is advertised |
| `nat.dead_intervals` | S D | int | 3 | 2–10 † | Missed intervals before a session is dead |
| `nat.server_idle_intervals` | S | int | 3 | 2–10 † (≥ dead_intervals) | Server-side idle limit, in intervals |
| `nat.grace_s` | S | int | 120 | 1–3600 † (≥ 2 × keepalive × idle intervals) | How long a dead session can still be resumed |
| `nat.half_open_ttl_s` | S | int | 10 | 1–300 | Lifetime of a session that never registers |
| `nat.allow_ip_change` | S | bool | true | — | Accept a resume from a different IP address |
| `nat.rebind_advice` | S | bool | true | — | Tell the device when its address changed |
| `nat.path_probe` | D | bool | true | — | Probe the return path right after the handshake |
| `nat.accept_server_keepalive` | D | bool | true | — | Use the server's advertised timers |
| `nat.keepalive_min_s` / `keepalive_max_s` | D | int | 5 / 300 | 1–3600 † | Clamp for advertised keepalives |
| `nat.blocked_hint` | D | bool | true | — | Log a hint when the server port looks blocked |
| `rel.tx_window` | S D | int | 8 | 1–16 | Reliable frames in flight |
| `rel.rto_initial_ms` / `rto_min_ms` / `rto_max_ms` | S D | int | 500 / 200 / 4000 | 100–60000 † (min ≤ initial ≤ max) | Retransmission timeout |
| `rel.max_retries` | S D | int | 8 | 1–16 | Retransmissions before the session fails |
| `rel.fast_retx_threshold` | S D | int | 3 | 1–16 | SACKed frames after a gap that trigger a fast retransmit |
| `rel.delayed_ack_ms` | S D | int | 20 | 0–1000 | How long an ACK may wait for a frame to ride on |
| `xfer.chunk_size` | S | int | 1024 | 256, 512, 1024 † (≤ max_datagram − 52) | Chunk size |
| `xfer.chunk_req_max` | S D | int | 32 | 1–32 | Chunks per request |
| `xfer.tx_queue_max` | S | int | 64 | 1–4096 | Chunks queued per session |
| `xfer.persist_every_chunks` | D | int | 16 | 1–4096 | How often the chunk bitmap is saved |
| `xfer.hash_attempts` | D | int | 2 | 1–10 | Downloads of a file whose hash fails |
| `limits.max_sessions` | S | int | 1024 | 1–1536 | Concurrent sessions (the session table has 2048 slots and is kept at most 3/4 full) |
| `limits.max_half_open` | S | int | 256 | 1–1000000 † (≤ max_sessions) | Sessions not yet registered |
| `limits.max_failed_records` | S | int | 10000 | 1–10000000 | Failed rows kept |
| `limits.hello_rate_per_ip` / `hello_burst` | S | int | 10 / 20 | 1–1000000 | HELLO rate limit per source IP (token bucket) |
| `limits.nonce_cache_total` / `nonce_cache_per_serial` | S | int | 4096 / 64 | 1–10000000 / 1–100000 | REGISTER nonces remembered |
| `limits.nonce_ttl_s` | S | int | 600 | 1–86400 | How long a nonce is remembered |
| `paths.policy`, `paths.keys`, `paths.artifacts` | S | string | `conf/policy.json`, `conf/keys.json`, `artifacts/` | must exist | Input files |
| `paths.db`, `paths.dump` | S | string | `state/db.jsonl`, `logs/db_dump.txt` | — | Journal and dump |
| `paths.state_dir` | D | string | `state/device` | — | Device state directory (`--state-dir`) |
| `paths.log` | S D | string | `logs/server.log` / `logs/device_%s.log` | `%s` = serial | Log file |
| `log.level` | S D | enum | info | debug, info, warn | `-v` = debug, `-q` = warn |
| `log.dump_verbose` | S | bool | false | — | Also write the verbose dump |
| `sim.loss`, `sim.reorder`, `sim.dup` | S D | number | 0 | 0–1 | Simulated loss, reordering, duplication (sender side) |
| `sim.reorder_delay_ms`, `sim.delay_ms`, `sim.jitter_ms` | S D | int | 30, 0, 0 | 0–10000 | Simulated delays |
| `sim.mtu` / `sim.mtu_mode` | S D | int / enum | 0 / emsgsize | 0 or 68–1500 / emsgsize, blackhole | Simulated path MTU |
| `sim.seed` | S D | unsigned 64-bit int | 0 | 0–2⁶⁴−1 | PRNG seed for the simulation |
| `sim.nat.idle_timeout_s` / `reboot_at_s`, `change_ip_at_s` | D | number / int | 0 | 0–86400 | NAT emulator: mapping expiry, NAT reboot, IP change (0 = off) |
| `sim.nat.block_server_port` | D | bool | false | — | NAT emulator: block the server port |
| `identity.model` / `hw_rev` / `factory_fw` | D | string | EDGE-GW-100 / A1 / v1.0 | — | Device metadata |
| `identity.serial` | D | string or null | null | `ZTP-` + 6 uppercase hex digits | null = generate on first boot |
| `discovery.mode` | D | enum | mcast | mcast, bcast, static | How to find the server |
| `discovery.server` | D | host:port | 10.10.10.5:5555 | — | Static server address |
| `discovery.mcast_group`, `mcast_iface_addr`, `bcast_addr` | D | IPv4 | 239.255.90.90, 0.0.0.0, 255.255.255.255 | — | Discovery addresses |
| `discovery.discovery_port` | D | int | 5556 | 1–65535 | Discovery port |
| `discovery.attempts` / `interval_ms` | D | int | 3 / 1000 | 1–100 / 100–60000 | Discovery retries |
| `auth.register_attempts` | D | int | 3 | 1–10 | REGISTER attempts before FAILED |
| `run.exit_when_operational` / `operational_hold_s` | D | bool / int | false / 2 | — / 0–3600 | Exit after OPERATIONAL (demo), after holding this long |
| `val.srtt_max_ms` | D | int | 1000 | 1–60000 | Largest smoothed RTT counted as healthy |
| `val.deadline_s` | D | int | 180 | 1–3600 | Time allowed for validation |

### 15.3 Cross-checks
`rto_min_ms ≤ rto_initial_ms ≤ rto_max_ms`; `server_port ≠ discovery_port`; `max_half_open ≤ max_sessions`;
`chunk_size` a power of two and ≤ `max_datagram − 52`; `server_idle_intervals ≥ dead_intervals`;
`grace_s ≥ 2 × keepalive_interval_s × server_idle_intervals`; on the device
`keepalive_min_s ≤ keepalive_interval_s ≤ keepalive_max_s`, `reconnect_min_s ≤ reconnect_max_s`,
`discovery.discovery_port` differs from the port in `discovery.server`, `identity.factory_fw` is a version like
`v1.0`, `identity.model` is not empty, and `paths.log` holds at most one `%s` (the serial) and no other `%`;
`sim.mtu` is 0 or at least 68; on the server `limits.max_sessions ≤ 1536`. Every policy entry is
checked the same way after its NAT overrides are applied, and every `firmware_file` and `config_file` it names
must exist and parse.

### 15.4 Policy and profiles
`conf/policy.json`:
```json
{
  "default": { "action": "reject" },
  "tenants": { "cruise_ops": {} },
  "models": {
    "EDGE-GW-100": { "firmware": "v1.1", "firmware_file": "artifacts/fw_EDGE-GW-100_v1.1.bin",
                     "config_version": 3, "config_file": "artifacts/cfg_v3.json", "tenant": "cruise_ops" },
    "EDGE-GW-050": { "firmware": "v1.0", "firmware_file": "artifacts/fw_EDGE-GW-050_v1.0.bin",
                     "config_version": 2, "config_file": "artifacts/cfg_v2.json", "tenant": "cruise_ops",
                     "notes": "Legacy device" }
  },
  "serials": {}
}
```
A model or serial entry may set `firmware`, `firmware_file`, `config_version`, `config_file`, `tenant`,
`notes`, and a `nat` block (`keepalive_interval_s`, `dead_intervals`, `server_idle_intervals`, `grace_s`,
`allow_ip_change`); tenants may set `nat`. Resolution per device: serial > model > tenant > server. An
example with overrides is `tests/conf/policy_override.json`.

| Profile | For | Differences from the defaults |
|---|---|---|
| `default` | Production | none |
| `demo` | Demo and tests | keepalive 2 s (device min 1 s), grace 30 s, reconnect 0.5–5 s, 3 resume cycles, black-hole detection 3 s, discovery every 500 ms, validation deadline 30 s, exit 5 s after OPERATIONAL |
| `strict-nat` | NATs with ≤ 20 s UDP timers | keepalive 10 s |
| `cgnat` | Many devices behind one IP | keepalive 15 s; HELLO rate 100/s per IP, burst 200 |
| `lan` | No NAT | keepalive 60 s, grace 360 s |
| `metered` | Battery or cellular | keepalive 25 s, 4 dead/idle intervals, grace 200 s |

---

## 16. Running the tests

### 16.1 Full regression

```sh
make test         # 16 suites: the first 15 rows of the table in 16.3 and the pcapcheck selftest (about 16 min)
make release      # the release regression: everything below into one results folder (about 2.5 h)
make scenario-logs # rewrites logs/scenarios/ (a generated serial; a session timeout and resume)
make asan         # the same with AddressSanitizer + UndefinedBehaviorSanitizer (build-asan/)
make matrix ROWS="M1 M2 M3 M4" SEEDS=10   # the demo over impairment rows and seeds (QA §6)
make scale N=50   # 50 devices at once at 15% loss (TC-PERF-01)
make soak         # 5 devices for an hour: memory and leaks (TC-PERF-04; macOS `leaks`, or VALGRIND=1 on Linux)
make longrun      # TC-NAT-07 (30 min idle behind an emulated NAT) and TC-NAT-13 (10 min), in parallel
make appendix-a   # the Appendix A journey; rewrites logs/appendix_a/
make demo         # the five-device demo; rewrites logs/
```

Needs: macOS or Linux, a C compiler, GNU make, `sh`, `python3`, and free local UDP ports (the suites use fixed ranges
between 20000 and 60000). No root, no network access beyond loopback. On macOS the long targets keep the Mac
awake with `caffeinate -is` (an idle or system sleep freezes timed tests; a closed laptop lid still sleeps the Mac,
and `make release` fails itself if `pmset` logged a sleep during the run). On Linux, `make soak VALGRIND=1` needs
`valgrind`. Only one regression should run on a machine at a time.

### 16.2 Where the results go

Every `make test` run writes **`results/<YYYYMMDD-HHMMSS>/`** (`make asan` adds `asan/` inside it; `make release`
writes `results/<…>-release/`):

```
results/20261112-093000/
├── summary.txt          run header (date, commit, host, compiler) and one line per suite: PASS/FAIL, seconds, last line
├── unit.out             each suite's full output
├── m5_upgrade.out
├── m5_upgrade/          each suite's servers' and devices' logs, state directories and dumps
└── …
```

`make scale` and `make soak` write `results/<…>/run_scale/` and `run_soak/`, and `make matrix`
`results/<…>/run_matrix/` (one folder per row and seed). Set `RESULTS=/some/dir` to choose the folder. A suite
run directly (without `RESULTS`) uses a temporary folder, deleted when it passes and kept (and printed) when
it fails. `results/` is not committed; one release run is kept in [evidence/](evidence/).

### 16.3 One suite or one case

Each suite is a script that takes the binaries as arguments; run it from the repository root after `make`:

| Suite | Command | Cases (QA_TEST_PLAN.md) |
|---|---|---|
| unit | `build/unit-tests [crypto\|protocol\|json\|config\|util\|netsim\|timer\|policy\|transfer\|reliable\|db]` | TC-UNIT-01..20 |
| cli_config | `sh tests/cli_config.sh build/ztp-server build/ztp-device` | TC-CFG-01, 03–05, 07 |
| m2_session | `sh tests/m2_session.sh build/ztp-server build/ztp-device` | TC-DISC-01..06, TC-SESS-01..03, 08, TC-REL-02, TC-NAT-11, TC-SIM-02 |
| m2_peer | `sh tests/m2_peer.sh build/ztp-server build/ztp-device build/peer_sim` | TC-DISC-07/08, TC-SESS-04/06, TC-REL-03/08, TC-NAT-06/12, TC-SIM-01, TC-CFG-08 |
| m3_register | `sh tests/m3_register.sh build/ztp-server build/ztp-device build/mkartifacts build/peer_sim` | TC-AUTH-01/02/09/12, TC-REL-04/11, TC-SRV-03, TC-CFG-09 |
| m4_provision | `sh tests/m4_provision.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-PROV-01..03, 06..08, 10, 11, 13, TC-REL-06/07, TC-NAT-05 |
| m5_upgrade | `sh tests/m5_upgrade.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-UPG-01..03, 05, 07..11, TC-VAL-01..03, 05, TC-NAT-10, TC-CFG-10 |
| m5_gate | `sh tests/m5_gate.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-NAT-01..04, 08, 09, TC-PDF-01/03/04, TC-PROV-09, TC-REL-05/10, TC-SESS-05, TC-UPG-02/06/11, TC-VAL-04 |
| check_appendix_a | `APX_SEEDS=3 sh tests/check_appendix_a.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-APX-01..10 |
| m6_fuzz | `sh tests/m6_fuzz.sh build/ztp-server build/mkartifacts` | TC-SRV-07 |
| m6_server | `sh tests/m6_server.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-SRV-02/10 |
| m6_demo | `sh tests/m6_demo.sh build` | TC-SRV-01, and through the demo TC-PDF-02, TC-SRV-04..06, 09, TC-UPG-04, TC-LOG-01..04 |
| m6_perf03 | `sh tests/m6_perf03.sh build/ztp-server build/ztp-device build/mkartifacts` | TC-PERF-03 |
| check_pdf05 | `sh tests/check_pdf05.sh build/ztp-server build/ztp-device` | TC-PDF-05 |
| separation | `make check-separation` | TC-DEL-03 |
| matrix | `ROWS=M3 SEEDS=1 sh scripts/run_matrix.sh` | TC-REL-09, TC-E2E-02, TC-PERF-02 |
| scale, soak, longrun | `sh scripts/run_scale.sh 50`, `SOAK_S=600 sh scripts/run_soak.sh`, `sh tests/m5_longrun.sh …` | TC-PERF-01, TC-PERF-04, TC-NAT-07/13 |

A suite prints `FAIL: TC-…: reason` for each failing check and a `N passed, M failed` line; the exit status is
0 only if everything passed. To check a single case, run the suite that contains it (most take one to two minutes;
`m5_gate` about five) and look for its ID in the output, or read the case's steps in QA_TEST_PLAN.md and run
them by hand as in [section 1](#1-quick-start): every scenario is the two binaries with flags, for example
the S3 crash and resume:

```sh
./ztp-server --profile demo --test-hooks &
./ztp-device --profile demo --test-hooks --state-dir state/s3 --serial ZTP-71DD08 --fw v1.0 \
             --discover static --server 127.0.0.1:5555 --crash-after-chunks 40 ; echo "exit $?"   # aborts at chunk 40
./ztp-device --profile demo --test-hooks --state-dir state/s3 --serial ZTP-71DD08 --fw v1.0 \
             --discover static --server 127.0.0.1:5555 --crash-after-chunks 40                   # resumes, re-requests 32 chunks
kill -INT %1                                                                                    # dump and summary
```

Most suites take environment knobs listed in their header comment (ports, seeds, durations).

### 16.4 On Linux
The same commands work on Linux (gcc or clang). There is no local Linux machine in this project, so Linux
runs on GitHub Actions; to run there by hand, open the repository's **Actions → ci → Run workflow**, which
runs every job, including the 10-seed matrix, and commits a report to `evidence/ci/` (section 17). Linux-only
checks that need root (the 10.10.10.5 address, netem, MTU 1280 on loopback, packet captures) are in the
`platform-linux` and `impair-netem` jobs of [.github/workflows/ci.yml](.github/workflows/ci.yml); each step
there can be copied into a root shell on any Linux machine.

---

## 17. Test evidence and CI

[TEST_REPORT.md](TEST_REPORT.md) gathers the results below by test case: one row per case with its result,
where it ran and a link to the output.

### 17.1 The release run
[evidence/release-20261113-063341/](evidence/release-20261113-063341/) holds the final release regression: `make
release` on commit **86d8ac2**, on the development Mac (Darwin arm64, Apple clang 16.0.0), 2026-11-13 06:33 to
09:13 PST, with no sleep during the run (checked against `pmset`'s log; the result is the last line of
`summary.txt`). Its device and server code is the submitted code. [evidence/README.md](evidence/README.md) lists
every file.

| Step | Result | Time | Output |
|---|---|---|---|
| `make test` (16 suites) | PASS | 17 min | 1090 unit checks and 646 scenario checks, 0 failed |
| `make asan` (the same 16) | PASS | 18 min | 0 failed, no sanitizer report |
| matrix: M1–M5, M8, M9 × 10 seeds | PASS | 25 min | every row 10/10 runs and 40/40 valid devices OPERATIONAL (4 per run; the fifth, ZTP-84BC19, must and does end FAILED); medians 0.4 / 1.4 / 3.0 / 4.8 / 12.0 / 1.3 / 1.6 s |
| perf02: M4 × 20 (TC-PERF-02) | PASS | 7 min | 80/80 OPERATIONAL, median 3.9 s, p95 10.6 s (limits 60 / 90 s) |
| scale: 50 devices (TC-PERF-01) | PASS | 29 s | 50 OPERATIONAL in 24 s, server peak RSS 9 MB |
| soak: 1 h, 5 devices (TC-PERF-04) | PASS | 61 min | RSS lower at the end than after warm-up for all 6 processes, `leaks` 0, 0 session timeouts |
| longrun (TC-NAT-07, TC-NAT-13) | PASS | 30 min | 30 min at 20% loss, production timers: 0 session timeouts, 1 REGISTER |
| demo | PASS | 17 s | 38 checks, five devices (ZTP-84BC19 Failed / "Invalid HMAC"); `logs/` regenerated |
| appendix-a | PASS | 91 s | 8 checks; `logs/appendix_a/` regenerated |
| scenario-logs | PASS | 13 s | 10 checks; `logs/scenarios/` regenerated |

Earlier release runs are not the evidence; evidence/README.md lists them and what each one found (two harness
bugs, timing-sensitive demo checks, three defects in the device's PMTU black-hole detector, a run during which
the Mac slept, and the review fixes and the `--bad-key` hook). To reproduce: `make release` (about 2.7 h; keep the
Mac awake, lid open and on power).

### 17.2 CI
[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on every push, every night (03:17 UTC, with the
larger seed counts) and on demand:

| Job | Runner | What it runs |
|---|---|---|
| `build-test` | Ubuntu (gcc, clang), macOS (clang) | `make`, `make test` (all suites; broadcast discovery on Ubuntu) |
| `asan` | Ubuntu, macOS | `make asan`, with a 1M-input JSON fuzz and a 1M-datagram server fuzz |
| `platform-linux` | Ubuntu | Multicast/broadcast probes; a packet capture of the session, provisioning and upgrade tests checked frame by frame (TC-REL-01: no datagram over 1200 bytes, no gaps); the Appendix A run on 10.10.10.5:5555 with a capture; TC-PROV-12 with loopback MTU 1280; netem availability |
| `impair-matrix` | Ubuntu, macOS | The demo over rows M1–M4 (3 seeds per push, 10 nightly); 50 devices (TC-PERF-01); TC-PERF-02 nightly |
| `impair-netem` | Ubuntu | The demo under kernel netem instead of the in-code simulator (row M7, TC-SIM-03) |
| `valgrind` | Ubuntu | A 3-minute soak with the server and one device under memcheck (TC-PERF-04, Linux) |
| `platform-macos` | macOS | Multicast on lo0, broadcast probe |
| `evidence` | Ubuntu | Writes the run's report with `scripts/ci_evidence.sh` |

**Evidence kept from CI.** Each job uploads its `results/` folder as an artifact and posts each suite summary
as a notice. The `evidence` job turns the run (every job's result, duration and link, every step, and every
notice) into Markdown, shows it on the run page, uploads it, and on nightly and manual runs commits it to
[evidence/ci/](evidence/ci/). GitHub deletes job logs and artifacts after 90 days; the committed reports stay.
Run history: https://github.com/heysunilvk/ztp-project/actions. To regenerate a report for any run:
`scripts/ci_evidence.sh <run-id> out.md` (needs `curl` and `jq`; public runs need no token).

CI on the final commits (every job on Ubuntu gcc and clang, macOS, both ASan builds, the platform, matrix,
netem and valgrind jobs, and the evidence report):

| Commit | Run | Result |
|---|---|---|
| 821d134 | [43252475092](https://github.com/heysunilvk/ztp-project/actions/runs/43252475092) | 12/12 jobs green (11 test jobs and the `evidence` job) |
| a1c5bd3 | [43285234948](https://github.com/heysunilvk/ztp-project/actions/runs/43285234948) | 12/12 jobs green (11 test jobs and the `evidence` job) |
| b936db3 | [43335677454](https://github.com/heysunilvk/ztp-project/actions/runs/43335677454) | every test step passed; `build-test (macos)` failed on its *Upload results* step, a transient storage error. Uploads have since been marked `continue-on-error`, so the tests alone decide a job |
| dc3c139 | [43355813851](https://github.com/heysunilvk/ztp-project/actions/runs/43355813851) | 11/12 green; `impair-matrix (macos)` failed one demo check, TC-SRV-09, on M2 seed 1 (all 12 devices OPERATIONAL). The check counted session switches over the few `[PROVISION]` lines only; it now counts every session line in the provisioning window. On all 91 release-run server logs the new count passes with a margin of 8 or more switches, where the old one's smallest margin was 2 |
| cfcb3fa | [43358206325](https://github.com/heysunilvk/ztp-project/actions/runs/43358206325) | 12/12 jobs green (push; 11 test jobs and `evidence`) |
| ddfc8fb | [43364354282](https://github.com/heysunilvk/ztp-project/actions/runs/43364354282) | 12/12 jobs green (the committed report says "all 11 jobs passed": it cannot report on its own `evidence` job), manual run with the nightly seed counts: matrix M1–M4 × 10 seeds on Ubuntu and macOS (40/40 OPERATIONAL in every row), TC-PERF-02 M4 × 20, TC-PERF-01 50 devices. Committed report: [evidence/ci/2026-11-13-ddfc8fb-run47.md](evidence/ci/2026-11-13-ddfc8fb-run47.md) |
| 0b2e460 | [43375735275](https://github.com/heysunilvk/ztp-project/actions/runs/43375735275) | 12/12 jobs green (push) on the release-run commit; its `platform-linux` job ran Appendix A on 10.10.10.5, and those logs are committed in [logs/appendix_a/linux-10.10.10.5/](logs/appendix_a/linux-10.10.10.5/) |
| 23757cd | [43395552552](https://github.com/heysunilvk/ztp-project/actions/runs/43395552552) (push), [43395562835](https://github.com/heysunilvk/ztp-project/actions/runs/43395562835) (manual) | both 12/12 green; the manual run used the nightly seed counts (matrix M1–M4 × 10 on Ubuntu and macOS, TC-PERF-02 M4 × 20, netem × 10). Committed report: [evidence/ci/2026-11-13-23757cd-run52.md](evidence/ci/2026-11-13-23757cd-run52.md) |

Later runs: https://github.com/heysunilvk/ztp-project/actions. A new committed report: Actions → ci → Run workflow
(or `gh workflow run ci.yml --ref main`); nightly runs add one too.

---

## 18. Test coverage and known gaps

QA_TEST_PLAN.md defines 165 test cases. Where each one runs:

| Where | Cases |
|---|---|
| `make test` on the Mac **and** in CI on Ubuntu (gcc, clang), macOS and both ASan builds | All 20 unit cases (TC-UNIT-*); TC-CFG-01..10; TC-DISC-01..08; TC-SESS-01..06, 08; TC-REL-02..08, 10, 11; TC-NAT-01..06, 08..12 (TC-NAT-12: the KEEPALIVE half); TC-AUTH-01, 02, 03, 09, 12; TC-PROV-01..03, 06..11, 13; TC-UPG-01..11; TC-VAL-01..05; TC-SRV-01..07, 09, 10; TC-APX-01..10; TC-E2E-01; TC-PDF-01..05; TC-LOG-01..04; TC-PERF-03; TC-SIM-01, 02; TC-DEL-03, 08 |
| CI only (needs root on Linux) | TC-REL-01 and TC-PROV-04/05 (packet capture checked frame by frame), TC-PROV-12 (loopback MTU 1280), TC-APX-02/03 and TC-PDF-04 on the PDF's 10.10.10.5:5555, TC-DISC-05 (b) broadcast, TC-SIM-03 (netem), the valgrind half of TC-PERF-04 |
| CI `impair-matrix` and on the Mac (`make matrix`, `make scale`) | TC-REL-09, TC-E2E-02 (rows M1–M4 × 10 seeds), TC-PERF-01 (50 devices), TC-PERF-02 (row M4 × 20) |
| The Mac only (too long for CI) | TC-PERF-04 (one-hour soak, `make soak`), TC-NAT-07 and TC-NAT-13 (`make longrun`), matrix rows M5, M8, M9 |
| M7 checks on the final tree | TC-DEL-01, 02, 04..07, 09, 10 (clean build, demo artifacts, README checklist, committed logs) |

**Open: implemented but not exercised end to end (14 cases, plus the CHUNK_REQ half of TC-NAT-12).** Each of these needs a test tool that
generates hostile or forged traffic: device hooks (`--replay-test`, `--reuse-nonce`, `--nonce`) and `peer_sim` modes (a rogue server, forged or replayed frames, floods). Those tools were not
built in this project and remain with the project owner (PRD R-15, QA §12.5). The protocol logic behind every
case is implemented; how much of it other tests reach is listed in QA §12.5.

| Case | What it would show | Logic covered today by |
|---|---|---|
| TC-AUTH-04, 05 | A captured REGISTER replayed into the same session and spliced into another | Duplicate drop in the receive window (TC-REL-04, TC-NAT-12); per-session keys |
| TC-AUTH-06 | A reused REGISTER nonce is rejected | Nonce cache (code review) |
| TC-AUTH-07 | A replayed resume is rejected | Resume counter check (code review) |
| TC-AUTH-08 | Every bit flip of a REGISTER or chunk fails the MAC | TC-UNIT-03 flips every bit of a frame |
| TC-AUTH-10 | HELLO flood from one IP is rate-limited | Token bucket, half-open cap (code review) |
| TC-AUTH-11 | The device rejects a rogue server's HELLO_ACK | HELLO_ACK MAC and nonce echo (code review) |
| TC-AUTH-13 | A wrong fleet key is dropped silently | `mac_fail` counter (TC-NAT-11) |
| TC-AUTH-14 | Forged or replayed REGISTER_NAKs are ignored | NAK accepted only with the pending nonce (code review) |
| TC-AUTH-15 | Terminal errors replayed to another device are ignored | Nonce echo on ERROR (code review) |
| TC-SESS-07 | A frame from an old epoch is dropped after a resume | Epoch check before the MAC (code review) |
| TC-SESS-09 | A fresh HELLO cannot take over a registered session | Supersede rule; cached HELLO_ACK (TC-SESS-02/03) |
| TC-NAT-12 (CHUNK_REQ half) | A replayed chunk request from another port does not move the session | Same rule as the KEEPALIVE half, which passes |
| TC-SRV-08 | 300 half-open sessions hit `SERVER_BUSY` | Half-open cap and TTL (TTL tested in M2) |
| TC-SRV-11 | 20,000 failing registrations stay under the failed-row cap | Cap and eviction unit-tested at 1,000 rows over a cap of 100 |

Also not exercised end to end: the replays of the demo's S3 device (the S3 crash-and-resume part runs) and the
Appendix A Step 5 nonce literal 984312. (The wrong-key device, TC-AUTH-03 and the demo's S2, runs: `--bad-key`.)

**Waived.** TC-SIM-04 (macOS dummynet) needs `sudo` to replace the firewall rules and was not run; TC-SIM-03
(netem on CI) and the in-code simulator cover the same behaviour.

The result of every case, where it ran and the output that shows it: [TEST_REPORT.md](TEST_REPORT.md) (144 PASS,
6 partial, 14 open, 1 waived, 0 failed). Per requirement: the traceability matrix in QA §10.

---

## 19. Known limitations and tradeoffs

1. **No confidentiality.** "Secure" is taken as authenticity, integrity and replay protection (the PDF asks
   for an HMAC). Payloads are not encrypted, and there is no forward secrecy. DTLS 1.3 or AEAD frames would be
   the next step.
2. **Shared keys.** One manufacturing key per model and one fleet key. Leaking a model key lets someone
   impersonate devices of that model; the demo keys are derived in code. Production would use per-device keys
   in a secure element (IDevID/BRSKI) and signed firmware.
3. **No congestion control and no upward path-MTU discovery.** A fixed window of 8 and 1200-byte datagrams
   with one 508-byte fallback: about 36% more packets than 1472-byte payloads would need.
4. **IPv4 only**; discovery reaches only the local multicast scope or broadcast domain.
5. **One server instance**, a file journal, no high availability. The REGISTER nonce cache is lost on a
   restart (the other replay layers still hold).
6. **24-bit soft serials** collide at fleet scale (about 4,800 devices for a 50% chance).
7. **The JSON parser is a strict subset** (64 KiB, depth 16, no `\u` surrogate pairs); YAML is not supported.
8. **Simulated, not real, networks and hardware.** The in-code simulator does not model queues or bursts
   (netem on CI does); there was no testing on real customer NATs, flash or power loss. macOS dummynet
   (TC-SIM-04) was not run, since it needs `sudo` to change the firewall rules.
9. **Demo timers are aggressive.** With 2 s keepalives, false session timeouts happen at high loss; they are
   recovered by resume but make the logs noisier than production would be.
10. **The attack-simulation test tooling is incomplete** (section 18): forged, replayed and flooded traffic is
    handled by implemented and reviewed logic but not exercised end to end. (The wrong-key device is: `--bad-key`.)
11. **The MAC is truncated to 128 bits** (standard practice), and firmware is authenticated only by the
    transport, not signed separately (DESIGN §18.3).
12. **Server restarts:** half-open sessions (not yet registered) are not persisted, so a device that was
    mid-handshake starts again; no config or policy hot reload (restart the server to change them). A replayed
    resume HELLO just after a restart can stall a session briefly until it resumes again (DESIGN §18.3, item 5).
13. **Linux is tested only in CI.** There was no local Linux machine: Linux builds, netem, valgrind and the
    reduced-MTU test run on GitHub Actions. Planned CI jobs for a 576-byte MTU path and a real conntrack NAT
    were not built (QA §3).
14. **The NAT's idle timeout is inferred, not measured,** and keepalive intervals are configured, not tuned
    automatically (DESIGN §9.7). UDP only: there is no fallback when outbound UDP is blocked; the device logs a
    hint instead.
15. **Test coverage is uneven in places** (TEST_REPORT.md lists each case): matrix row M6 (50% loss) was not run,
    some cases run only on one platform (for example broadcast discovery and strict-IP resume only on Linux CI),
    and the suites print failures and totals rather than one line per passed case.

The full list of tradeoffs, with the alternatives considered, is DESIGN §22; future work is DESIGN §22.3.

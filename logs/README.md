# Committed logs

The PDF asks for server and client logs (Expected Submission §3). Everything here except `appendix_a/linux-10.10.10.5/`
(from CI, below) was written by the release regression run that `evidence/release-20261112-220052/` records
(`make release`; README §17.1); `make demo`,
`make appendix-a` and `make scenario-logs` regenerate it. Every line starts with an ISO-8601 UTC timestamp and a
`[TAG]` (README §14.3).

## logs/: the demo (the PDF's five devices, at 15% loss, 10% reordering, 5% duplication)

| File | What it shows |
|---|---|
| `server.log` | The server: discovery, five concurrent sessions interleaving, three Invalid HMAC rejections for ZTP-84BC19, registrations, per-device policy (v1.1/cfg 3, and v1.0/cfg 2 for the legacy model), chunked transfers, resumes after the upgrade restart, OPERATIONAL, and at Ctrl-C the **database dump and summary** (PDF §9) |
| `device_ZTP-93AF21.log` | S1: multicast discovery (bonus), registration, config and firmware download, upgrade v1.0 → v1.1, restart, resume without REGISTER, validation, OPERATIONAL |
| `device_ZTP-84BC19.log` | S2: a wrong model key (`--bad-key`): HELLO and the path probe pass, each REGISTER is rejected with "Invalid HMAC" (1/3, 2/3, 3/3), and the device stops in FAILED |
| `device_ZTP-71DD08.log` | S3: the same flow, but the process is killed mid-download (`--crash-after-chunks 40`); the relaunched process resumes and fetches only the missing chunks |
| `device_ZTP-55AC90.log` | S4: already on v1.1 (no upgrade); a NAT rebinding (`--rebind-after 3`) moves its source port and the server follows it |
| `device_ZTP-19FE77.log` | S5: a legacy model, assigned v1.0 and config v2 ("Legacy device") |
| `db_dump.txt` | The dump in the PDF's table format, and the summary (successes, failures, firmware/config per device) |
| `db_dump_verbose.txt` | The same rows with every stored field (stage, attempts, provisioning notes such as "Transfer resumed" and "Resumed after upgrade") |
| `server_restart.log`, `db_dump_restart.txt` | TC-SRV-06: the server restarted on the same journal rebuilds the same rows |
| `device_*.out`, `server.out` | Each process's stdout/stderr (the supervisor's relaunch notice for S3 is here) |
| `check_logs.txt`, `check_invariants.txt` | The demo's own checks of these logs: tags, timestamps, the bonus lines, one row per device, no duplicate REGISTER, every artifact hash |

The dump therefore has the PDF's five rows: four Success and ZTP-84BC19 Failed / "Invalid HMAC". Not shown: the
replays of S3's REGISTER (`--replay-test`, not built); the demo reports that skip.

## logs/appendix_a/: the PDF's Appendix A, one device

| File | What it shows |
|---|---|
| `device_ZTP-93AF21.log` | Steps 1–9 of Appendix A with the PDF's values (ZTP-93AF21, EDGE-GW-100, v1.0, port 5555, session_id 7421), on 127.0.0.1 on the Mac. README §14.2 maps each PDF line to its line here |
| `server.log` | The server's side of the same run |
| `db_dump.txt`, `db_dump_verbose.txt` | The resulting row |

The PDF's address 10.10.10.5 needs an address alias (sudo) on the Mac, so the run here uses 127.0.0.1; CI runs
the same check on 10.10.10.5 (job `platform-linux`) and its logs are in `linux-10.10.10.5/`. The PDF's literal
nonce (984312) needs the `--nonce` hook, also not built: the device uses a random 64-bit nonce.

## logs/scenarios/: behaviours the two runs above do not show

| Folder | What it shows |
|---|---|
| `generated_serial/` | PDF §1 "generate or load a soft serial": `device_first_boot.log` has `[BOOT] Soft serial generated` (the PDF's example line), `device_second_boot.log` has `[BOOT] Soft serial loaded` with the same serial and resumes the session (no second REGISTER) |
| `session_timeout/` | PDF §4 "session timeout detection": the server stops answering while the device is OPERATIONAL; the device logs `[NAT] Session timeout detected`, tries to resume with backoff, and resumes the same session when the server answers again, with one REGISTER in total |

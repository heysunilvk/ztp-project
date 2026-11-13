# ZTP over UDP — build (DESIGN §19.1).
# Must stay compatible with GNU Make 3.81 (macOS): no .ONESHELL, $(file ...), grouped targets.

CC      ?= cc
BUILD   ?= build
OPT     ?= -O2
SAN     ?=
CSTD    := -std=c11
WARN    := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
DEFS    := -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CFLAGS  ?=
CFLAGS  += $(CSTD) $(OPT) $(WARN) $(DEFS) $(SAN) -Icommon -MMD -MP
LDFLAGS += $(SAN)
LDLIBS  += -lm

COMMON_SRC := $(wildcard common/*.c)
SERVER_SRC := $(wildcard server/*.c)
DEVICE_SRC := $(wildcard device/*.c)
TEST_SRC   := $(wildcard tests/*.c)

COMMON_OBJ := $(COMMON_SRC:%.c=$(BUILD)/%.o)
SERVER_OBJ := $(SERVER_SRC:%.c=$(BUILD)/%.o)
DEVICE_OBJ := $(DEVICE_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ   := $(TEST_SRC:%.c=$(BUILD)/%.o)
# Everything except main.o goes into a per-binary library, so unit tests can link it.
SERVER_MAIN := $(BUILD)/server/main.o
DEVICE_MAIN := $(BUILD)/device/main.o
SERVER_LIB_OBJ := $(filter-out $(SERVER_MAIN),$(SERVER_OBJ))
DEVICE_LIB_OBJ := $(filter-out $(DEVICE_MAIN),$(DEVICE_OBJ))

LIB        := $(BUILD)/libztpcommon.a
SRV_LIB    := $(BUILD)/libztpserver.a
DEV_LIB    := $(BUILD)/libztpdevice.a
SERVER_BIN := $(BUILD)/ztp-server
DEVICE_BIN := $(BUILD)/ztp-device
TEST_BIN   := $(BUILD)/unit-tests

.PHONY: all server device test demo appendix-a scenario-logs release matrix scale soak longrun asan debug clean clean-logs check-separation spike artifacts

# The default goal: a plain `make` builds both binaries and the artifacts (TC-DEL-01).
all:

# Deterministic mock artifacts and demo keys (tools/mkartifacts; links the device library for the
# demo key derivation). `make artifacts` writes artifacts/ for conf/policy.json.
MKARTIFACTS := $(BUILD)/mkartifacts
$(MKARTIFACTS): tools/mkartifacts.c $(DEV_LIB) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Idevice -o $@ $< $(DEV_LIB) $(LIB) $(LDFLAGS) $(LDLIBS)
artifacts: $(MKARTIFACTS)
	$(MKARTIFACTS) --out artifacts

# QA protocol peer (tests/peer_sim): links the server library for keys.json loading.
PEER_SIM := $(BUILD)/peer_sim
$(PEER_SIM): tests/peer_sim/peer_sim.c $(SRV_LIB) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iserver -o $@ $< $(SRV_LIB) $(LIB) $(LDFLAGS) $(LDLIBS)

all: server device artifacts
	@chmod 600 conf/keys.json   # git does not keep file modes; the server refuses anything looser
	@ln -sf $(SERVER_BIN) ztp-server
	@ln -sf $(DEVICE_BIN) ztp-device

server: $(SERVER_BIN)
device: $(DEVICE_BIN)

$(LIB): $(COMMON_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(SRV_LIB): $(SERVER_LIB_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(DEV_LIB): $(DEVICE_LIB_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^

$(SERVER_BIN): $(SERVER_MAIN) $(SRV_LIB) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(SERVER_MAIN) $(SRV_LIB) $(LIB) $(LDLIBS)

$(DEVICE_BIN): $(DEVICE_MAIN) $(DEV_LIB) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(DEVICE_MAIN) $(DEV_LIB) $(LIB) $(LDLIBS)

# Unit tests may link both sides' libraries (the separation rule applies to the binaries).
$(TEST_OBJ): CFLAGS += -Iserver -Idevice
$(TEST_BIN): $(TEST_OBJ) $(SRV_LIB) $(DEV_LIB) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(SRV_LIB) $(DEV_LIB) $(LIB) $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests (TC-UNIT-*). FUZZ_ITERS controls the JSON mutation fuzz (default 100000).
# Wire-level checker for packet captures (tests/pcapcheck; TC-REL-01).
PCAPCHECK := $(BUILD)/pcapcheck
$(PCAPCHECK): tests/pcapcheck/pcapcheck.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS) $(LDLIBS)

# Every run keeps its logs and results in RESULTS (default results/<date-time>; git ignores results/):
# <suite>.out is each suite's output, <suite>/ its logs and state, summary.txt one line per suite.
RUN_ID  := $(shell date +%Y%m%d-%H%M%S)
RESULTS ?= results/$(RUN_ID)
export RESULTS
SUITE   := env BUILD=$(BUILD) sh tests/run_suite.sh

test: $(TEST_BIN) $(SERVER_BIN) $(DEVICE_BIN) $(PEER_SIM) $(PCAPCHECK) artifacts
	@if [ -n "$(KEEP_AWAKE)" ]; then ( $(KEEP_AWAKE) -w $$PPID & ); fi   # awake until make exits
	@chmod 600 conf/keys.json
	@$(SUITE) unit $(TEST_BIN)
	@mkdir -p $(BUILD)/selftest && $(SUITE) pcapcheck $(PCAPCHECK) --selftest $(BUILD)/selftest
	@$(SUITE) cli_config sh tests/cli_config.sh $(SERVER_BIN) $(DEVICE_BIN)
	@$(SUITE) m2_session sh tests/m2_session.sh $(SERVER_BIN) $(DEVICE_BIN)
	@$(SUITE) m2_peer sh tests/m2_peer.sh $(SERVER_BIN) $(DEVICE_BIN) $(PEER_SIM)
	@$(SUITE) m3_register sh tests/m3_register.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS) $(PEER_SIM)
	@$(SUITE) m4_provision sh tests/m4_provision.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@$(SUITE) m5_upgrade sh tests/m5_upgrade.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@$(SUITE) m5_gate sh tests/m5_gate.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@APX_SEEDS=3 $(SUITE) check_appendix_a sh tests/check_appendix_a.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@$(SUITE) m6_fuzz sh tests/m6_fuzz.sh $(SERVER_BIN) $(MKARTIFACTS)
	@$(SUITE) m6_server sh tests/m6_server.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@$(SUITE) m6_demo sh tests/m6_demo.sh $(BUILD)
	@$(SUITE) m6_perf03 sh tests/m6_perf03.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	@$(SUITE) check_pdf05 sh tests/check_pdf05.sh $(SERVER_BIN) $(DEVICE_BIN)
	@$(SUITE) separation $(MAKE) --no-print-directory check-separation BUILD=$(BUILD)
	@echo "all suites passed; results in $(RESULTS)/summary.txt"

# The 5-device demo (DESIGN §19.3; QA TC-SRV-01, TC-E2E-02 with SEED=1..10): writes logs/ and state/.
demo: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	BIN=$(BUILD) $(KEEP_AWAKE) $(SUITE) demo sh scripts/run_demo.sh

# Timing-sensitive runs must not be frozen by the Mac's idle sleep (a sleeping process looks like a stalled
# one: M6 traced an intermittent TC-REL-11 failure to it). caffeinate -is holds idle- and system-sleep assertions
# while the command runs (-s only on AC power; closing a laptop lid still sleeps it); elsewhere it is empty.
KEEP_AWAKE := $(shell command -v caffeinate > /dev/null 2>&1 && echo "caffeinate -is")

# TC-PERF-01: N devices at 15% loss (make scale N=50); TC-PERF-04: 5 devices for an hour, RSS and leaks.
N ?= 50
scale: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	BIN=$(BUILD) $(KEEP_AWAKE) sh scripts/run_scale.sh $(N)
soak: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	BIN=$(BUILD) $(KEEP_AWAKE) sh scripts/run_soak.sh

# The impairment matrix (QA §6): make matrix ROWS="M1 M2 M3 M4" SEEDS=10
ROWS  ?= M2 M3 M4
SEEDS ?= 10
matrix: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	BIN=$(BUILD) ROWS="$(ROWS)" SEEDS=$(SEEDS) $(KEEP_AWAKE) sh scripts/run_matrix.sh

# The PDF's Appendix A journey (ZTP-93AF21, session 7421) on 127.0.0.1:5555; on success its logs replace
# logs/appendix_a/ (committed). CI runs the same check on 10.10.10.5:5555 (job platform-linux).
appendix-a: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	APX_SEEDS=0 APX_LOGS=logs/appendix_a $(SUITE) appendix_a sh tests/check_appendix_a.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS) 127.0.0.1 5555

# Committed log evidence for a generated soft serial and for session-timeout detection (logs/scenarios/).
scenario-logs: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	$(SUITE) scenario_logs sh scripts/run_scenario_logs.sh

# The release regression (QA §11.2): test, asan, the whole matrix, PERF-01/02/04, longrun, demo and
# appendix-a into one results folder (about 2.5 h; QUICK=1 skips the soak and longrun).
# The release run's default folder is results/<date-time>-release; RESULTS=... on the command line or in the
# environment is used as given.
RELEASE_RESULTS := $(if $(filter file,$(origin RESULTS)),$(RESULTS)-release,$(RESULTS))
release: all $(TEST_BIN) $(PEER_SIM) $(PCAPCHECK)
	RESULTS=$(RELEASE_RESULTS) $(KEEP_AWAKE) sh scripts/run_release.sh

# TC-NAT-07 (30 min) and TC-NAT-13 (10 min), in parallel: too long for every push.
longrun: $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)
	$(KEEP_AWAKE) sh tests/m5_longrun.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS)

# TC-DEL-03: device and server share only common/.
check-separation: $(SERVER_BIN) $(DEVICE_BIN)
	@nm $(DEVICE_BIN) > /dev/null && nm $(SERVER_BIN) > /dev/null || { echo "FAIL: nm cannot read the binaries"; exit 1; }
	@if nm $(DEVICE_BIN) | grep -q ' [TtDdBb] _*srv_'; then echo "FAIL: srv_ symbols in ztp-device"; exit 1; fi
	@if nm $(SERVER_BIN) | grep -q ' [TtDdBb] _*dev_'; then echo "FAIL: dev_ symbols in ztp-server"; exit 1; fi
	@if grep -l '#include *"\.\./server/' device/*.c device/*.h 2>/dev/null; then echo "FAIL: device includes server headers"; exit 1; fi
	@if grep -l '#include *"\.\./device/' server/*.c server/*.h 2>/dev/null; then echo "FAIL: server includes device headers"; exit 1; fi
	@echo "separation OK"

# M1 platform spike: multicast on loopback, broadcast to BCAST (PRD R-03).
SPIKE_BIN := $(BUILD)/netspike
$(SPIKE_BIN): tools/netspike.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<
spike: $(SPIKE_BIN)
	$(SPIKE_BIN) mcast 239.255.90.90 127.0.0.1
	@if [ -n "$(BCAST)" ]; then $(SPIKE_BIN) bcast $(BCAST); else echo "set BCAST=<directed broadcast> to test broadcast"; fi

# Same build and tests with AddressSanitizer + UBSan (TC-DEL-08).
asan:
	$(MAKE) BUILD=build-asan RESULTS=$(RESULTS)/asan OPT="-O1 -g -fno-omit-frame-pointer" \
	    SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined" test

debug:
	$(MAKE) BUILD=build-debug OPT="-O0 -g" all

clean:
	rm -rf build build-asan build-debug ztp-server ztp-device state

clean-logs:
	rm -rf logs *.pcap

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(DEVICE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

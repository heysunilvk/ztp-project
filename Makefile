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

.PHONY: all server device test asan debug clean clean-logs check-separation spike artifacts

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

test: $(TEST_BIN) $(SERVER_BIN) $(DEVICE_BIN) $(PEER_SIM) $(PCAPCHECK) artifacts
	@chmod 600 conf/keys.json
	$(TEST_BIN)
	@mkdir -p $(BUILD)/selftest && $(PCAPCHECK) --selftest $(BUILD)/selftest
	sh tests/cli_config.sh $(SERVER_BIN) $(DEVICE_BIN)
	sh tests/m2_session.sh $(SERVER_BIN) $(DEVICE_BIN)
	sh tests/m2_peer.sh $(SERVER_BIN) $(DEVICE_BIN) $(PEER_SIM)
	sh tests/m3_register.sh $(SERVER_BIN) $(DEVICE_BIN) $(MKARTIFACTS) $(PEER_SIM)
	@$(MAKE) --no-print-directory check-separation BUILD=$(BUILD)

# TC-DEL-03: device and server share only common/.
check-separation: $(SERVER_BIN) $(DEVICE_BIN)
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
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fno-omit-frame-pointer" \
	    SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined" test

debug:
	$(MAKE) BUILD=build-debug OPT="-O0 -g" all

clean:
	rm -rf build build-asan build-debug ztp-server ztp-device state

clean-logs:
	rm -rf logs *.pcap

-include $(COMMON_OBJ:.o=.d) $(SERVER_OBJ:.o=.d) $(DEVICE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

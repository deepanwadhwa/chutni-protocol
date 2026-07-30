# Chutni reference implementation.
#
# Portable C99. No package manager, no build system beyond make: SQLite and
# BLAKE3 are vendored under third_party/ and compiled from source.

CC      ?= cc
AR      ?= ar
BUILD   ?= build
PREFIX  ?= /usr/local

# The implementation's release version, and the only place it is written down.
# It is distinct from the protocol version in include/chutni.h: the spec version
# says which format a store is in, this says which build produced it. The two
# move independently, and a producer record carries this one (§16.1), so a
# stale copy in one source file would misattribute artifacts to a build that
# never made them.
VERSION := $(shell cat VERSION)

WARN    := -std=c99 -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Werror
OPT     ?= -O2
VERDEF  := -DCHUTNI_VERSION='"$(VERSION)"'
CFLAGS  += $(WARN) $(OPT) $(VERDEF) -Iinclude -Isrc -Ithird_party/blake3 -Ithird_party/sqlite

# The portable BLAKE3 build. Runtime SIMD dispatch is deliberately off: the
# reference implementation values one identical code path everywhere over
# throughput, and hashing has never been the bottleneck here.
B3FLAGS := -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
           -DBLAKE3_USE_NEON=0

SQLFLAGS := -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
            -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_JSON1

LIB_SRC  := src/chutni.c src/scan.c src/cj.c
LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD)/%.o)
B3_SRC   := third_party/blake3/blake3.c third_party/blake3/blake3_dispatch.c \
            third_party/blake3/blake3_portable.c
B3_OBJ   := $(B3_SRC:%.c=$(BUILD)/%.o)
SQL_OBJ  := $(BUILD)/third_party/sqlite/sqlite3.o

LIBCHUTNI := $(BUILD)/libchutni.a
CLI       := $(BUILD)/chutni
MCP       := $(BUILD)/chutni-mcp

.PHONY: all clean test install conformance sanitize
all: $(CLI) $(MCP)

$(LIBCHUTNI): $(LIB_OBJ) $(B3_OBJ) $(SQL_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(CLI): $(BUILD)/src/cli.o $(LIBCHUTNI)
	$(CC) $(CFLAGS) -o $@ $< $(LIBCHUTNI) -lpthread

$(MCP): $(BUILD)/src/mcp.o $(LIBCHUTNI)
	$(CC) $(CFLAGS) -o $@ $< $(LIBCHUTNI) -lpthread

# First-party code is held to -Werror. VERSION is a prerequisite because the
# release string is compiled in: without it, editing VERSION leaves stale
# objects reporting the previous version, which is the one kind of version bug
# that survives a rebuild and lies in provenance records.
$(BUILD)/src/%.o: src/%.c include/chutni.h src/cj.h VERSION
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(B3FLAGS) $(SQLFLAGS) -c $< -o $@

# Vendored code is compiled as its authors shipped it: warnings there are not
# ours to fix, and patching upstream to satisfy our flags makes updates painful.
$(BUILD)/third_party/blake3/%.o: third_party/blake3/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 $(OPT) -Ithird_party/blake3 $(B3FLAGS) -c $< -o $@

$(BUILD)/third_party/sqlite/%.o: third_party/sqlite/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -Ithird_party/sqlite $(SQLFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(B3FLAGS) $(SQLFLAGS) -c $< -o $@

# BLAKE3 checked against the official test vectors, not against itself.
$(BUILD)/blake3_vectors: tests/blake3_vectors.c $(B3_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -std=c99 $(OPT) -Ithird_party/blake3 $(B3FLAGS) $^ -o $@

$(BUILD)/conformance: tests/conformance/conformance.c $(LIBCHUTNI) VERSION
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIBCHUTNI) -lpthread -o $@

test: $(CLI) $(MCP) $(BUILD)/blake3_vectors $(BUILD)/conformance
	@python3 tests/run_blake3_vectors.py $(BUILD)/blake3_vectors
	@echo
	@rm -rf $(BUILD)/conformance-work
	@CHUTNI_HOME=$(BUILD)/conformance-work/home $(BUILD)/conformance $(BUILD)/conformance-work
	@echo
	@sh tests/conformance/run.sh $(CLI)
	@echo
	@CHUTNI_MCP=$(MCP) python3 tests/test_mcp.py

conformance: test

# This code parses untrusted file contents, so the suite is also run with the
# sanitizers on. Built from source in one unit rather than reusing build/,
# because sanitized and unsanitized objects must not be mixed.
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_SRC  := $(LIB_SRC) $(B3_SRC) third_party/sqlite/sqlite3.c
SAN_DEFS := -Iinclude -Isrc -Ithird_party/blake3 -Ithird_party/sqlite $(B3FLAGS) $(SQLFLAGS) $(VERDEF)

sanitize:
	@mkdir -p $(BUILD)/san
	$(CC) -std=c99 -g -O1 $(SAN) $(SAN_DEFS) $(SAN_SRC) tests/conformance/conformance.c \
	    -o $(BUILD)/san/conformance -lpthread
	$(CC) -std=c99 -g -O1 $(SAN) $(SAN_DEFS) $(SAN_SRC) src/cli.c \
	    -o $(BUILD)/san/chutni -lpthread
	$(CC) -std=c99 -g -O1 $(SAN) $(SAN_DEFS) $(SAN_SRC) src/mcp.c \
	    -o $(BUILD)/san/chutni-mcp -lpthread
	@rm -rf $(BUILD)/san/work $(BUILD)/san/home
	@CHUTNI_HOME=$(BUILD)/san/home $(BUILD)/san/conformance $(BUILD)/san/work
	@echo
	@sh tests/conformance/run.sh $(BUILD)/san/chutni
	@echo
	@CHUTNI_MCP=$(BUILD)/san/chutni-mcp python3 tests/test_mcp.py

install: $(CLI) $(MCP)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 755 $(CLI) $(DESTDIR)$(PREFIX)/bin/chutni
	install -m 755 $(MCP) $(DESTDIR)$(PREFIX)/bin/chutni-mcp
	install -m 644 include/chutni.h $(DESTDIR)$(PREFIX)/include/chutni.h
	install -m 644 $(LIBCHUTNI) $(DESTDIR)$(PREFIX)/lib/libchutni.a

clean:
	rm -rf $(BUILD)

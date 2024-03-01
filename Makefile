# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# binary name
APP = policyServer
APP2 = aggregator

# all source are stored in SRCS-pb
SRCS-pb := policyServer.c
SRCS-ag := aggregator.c

PKGCONF ?= pkg-config

# Build using pkg-config variables if possible
ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: shared aggregator stats logs
.PHONY: shared static aggregator logs
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)
aggregator: build/$(APP2)
stats:
	@mkdir -p $@
logs:
	@mkdir -p $@

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)
LDFLAGS_NETWORK = $(shell echo -lcurl -ljansson)
LDFLAGS_SHARED += -lsqlite3 # Add SQLite3 linker flag
LDFLAGS_STATIC += -lsqlite3 # Add SQLite3 linker flag

# Add librdkafka library flags
LDFLAGS_SHARED += $(shell $(PKGCONF) --libs rdkafka)
LDFLAGS_STATIC += $(shell $(PKGCONF) --static --libs rdkafka)

ifeq ($(MAKECMDGOALS),static)
# check for broken pkg-config
ifeq ($(shell echo $(LDFLAGS_STATIC) | grep 'whole-archive.*l:lib.*no-whole-archive'),)
$(warning "pkg-config output list does not contain drivers between 'whole-archive'/'no-whole-archive' flags.")
$(error "Cannot generate statically-linked binaries with this version of pkg-config")
endif
endif

CFLAGS += -DALLOW_EXPERIMENTAL_API

build/$(APP)-shared: $(SRCS-pb) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-pb) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED) $(LDFLAGS_NETWORK)

build/$(APP)-static: $(SRCS-pb) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-pb) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC) $(LDFLAGS_NETWORK)

build/$(APP2): build
	$(CC) $(SRCS-ag) -o $@ $(LDFLAGS_NETWORK)

build:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared build/$(APP2)
	test -d build && rmdir -p build && rm -rf stats && rm -rf logs || true

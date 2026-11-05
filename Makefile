# PrusaSlicer — Wayland build (GTK3 + EGL)
#
# Usage:
#   make              # build deps (once) + PrusaSlicer
#   make deps         # build static dependencies (once)
#   make build        # configure (if needed) + compile PrusaSlicer
#   make reconfigure  # force a fresh cmake configure of PrusaSlicer
#   make run          # launch the slicer
#   make test         # run tests
#   make clean        # remove the PrusaSlicer build dir
#   make clean-deps   # remove the deps build dir
#   make clean-all    # remove both
#
# Build dirs:  build/ + deps/build/                (host, default)
#              build-sandbox/ + deps/build-sandbox/ (make SANDBOX=1 ...: temporary build for an
#                                                    agent sandbox that mounts the tree at another
#                                                    path; remove with make clean-sandbox)
# Logs: <build dir>/build.log, overwritten by every run (header with branch/commit, OK/FAILED).
#
# Override parallelism:  make JOBS=8

# Use bash with pipefail so logged commands propagate failures through `tee`.
SHELL       := /bin/bash
.SHELLFLAGS := -eo pipefail -c

# CMake 4.0 (Ubuntu 26.04) removed compatibility with policies < 3.5, which the
# vendored deps (Blosc, Boost, OCCT, ...) still request. Exporting this restores
# the old floor for every cmake invocation, including the ExternalProject child
# configures. Harmless on CMake 3.x. Verified building all 25 deps on CMake 4.2.
export CMAKE_POLICY_VERSION_MINIMUM := 3.5

JOBS      ?= $(shell nproc 2>/dev/null || echo 4)
ifeq ($(SANDBOX),1)
BUILD_DIR := build-sandbox
DEPS_DIR  := deps/build-sandbox
else
BUILD_DIR := build
DEPS_DIR  := deps/build
endif
DESTDIR   := $(CURDIR)/$(DEPS_DIR)/destdir/usr/local
LOG       := $(BUILD_DIR)/build.log
DEPS_LOG  := $(DEPS_DIR)/build.log
LOGRUN    := scripts/logrun.sh

.PHONY: all deps prusa build reconfigure run test install \
        clean clean-deps clean-all clean-sandbox

all: deps build

# --- Dependencies (static, GTK3) -------------------------------------------

deps: $(DEPS_DIR)/deps.stamp

$(DEPS_DIR)/deps.stamp:
	@$(LOGRUN) new $(DEPS_LOG) "dependencies ($(DEPS_DIR))"
	@$(LOGRUN) step $(DEPS_LOG) configure cmake -B $(DEPS_DIR) -S deps -DDEP_WX_GTK3=ON
	@$(LOGRUN) step $(DEPS_LOG) build $(MAKE) -C $(DEPS_DIR) -j1
	@touch $@

# --- PrusaSlicer configure + build -----------------------------------------

CONFIGURE := cmake -B $(BUILD_DIR) -S . \
		-DCMAKE_BUILD_TYPE=Release \
		-DSLIC3R_STATIC=1 \
		-DSLIC3R_GTK=3 \
		-DSLIC3R_PCH=ON \
		-DCMAKE_PREFIX_PATH="$(DESTDIR)"

prusa: build

build: $(DEPS_DIR)/deps.stamp
	@$(LOGRUN) new $(LOG) "PrusaSlicer ($(BUILD_DIR))"
	@if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then $(LOGRUN) step $(LOG) configure $(CONFIGURE); fi
	@$(LOGRUN) step $(LOG) build cmake --build $(BUILD_DIR) -j$(JOBS)

reconfigure: $(DEPS_DIR)/deps.stamp
	@$(LOGRUN) new $(LOG) "PrusaSlicer reconfigure ($(BUILD_DIR))"
	@$(LOGRUN) step $(LOG) configure $(CONFIGURE)

# --- Run --------------------------------------------------------------------

run: build
	./$(BUILD_DIR)/src/prusa-slicer

# --- Tests ------------------------------------------------------------------

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

# --- Install ----------------------------------------------------------------

install: build
	cmake --build $(BUILD_DIR) --target install

# --- Clean ------------------------------------------------------------------

clean:
	$(RM) -r $(BUILD_DIR)

clean-deps:
	$(RM) -r $(DEPS_DIR)

clean-all: clean clean-deps

clean-sandbox:
	$(RM) -r build-sandbox deps/build-sandbox

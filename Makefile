# Makefile for narrator.wyoming
#
# Source lives in src/. Protocol code (wyoming.c) and program mains are
# portable; the socket/clock and audio backends are selected per platform:
#   net_posix.c  / audio_host.c  -> host build  (protocol + .wav verification)
#   net_amiga.c  / audio_ahi.c   -> Amiga build (bsdsocket.library + AHI)
#
# Programs:
#   wyomingtest  (main.c)    - latency probe
#   saytest      (saytest.c) - full pipeline: text -> Wyoming -> audio
#
# Targets:
#   make host        - native binaries in build/host/   (uses system cc)
#   make amiga       - cross-compile Amiga binaries in build/amiga/
#   make docker      - run 'make amiga' inside the cross-compiler container
#   make dist        - build the uploadable Aminet archive (in the container)
#   make clean
#
# Amiga toolchain: Bebbo m68k-amigaos GCC in stefanreinauer/amiga-gcc:latest.
# Use 'make docker' if you don't have it locally.

SRCDIR := src
BUILD  := build

# Version (VERSION.REVISION) -- single source of truth; bump via make bump/release.
include version.mk

PROTO  := $(SRCDIR)/wyoming.c

# --- Host toolchain ---
HOST_CC     ?= cc
HOST_CFLAGS ?= -O2 -Wall -Wextra -Werror -DPLATFORM_POSIX
HOST_NET    := $(SRCDIR)/net_posix.c
HOST_AUDIO  := $(SRCDIR)/audio_host.c

# --- Amiga cross toolchain (Bebbo m68k-amigaos GCC) ---
# ':=' not '?=': make pre-defines CC to "cc" (origin default), which '?=' will
# not override. Command-line 'make CC=...' still wins.
CC          := m68k-amigaos-gcc
# -mcrt=clib2: use the clib2 C runtime, not the default (newlib). newlib's
# startup mishandles this launch context (garbage argv) and its bsdsocket recv
# path faults; clib2 is the proven-good runtime here (it's what the working
# MicroPython Amiga port links against).
# -msoft-float: the target 68020 has no (working) FPU; hardware float opcodes
# trap as Guru #8000000B (F-line). Keep all float in software.
CFLAGS      ?= -O2 -Wall -Wextra -Werror -m68020 -msoft-float -mcrt=clib2 -DPLATFORM_AMIGA
AMIGA_NET   := $(SRCDIR)/net_amiga.c
AMIGA_AUDIO := $(SRCDIR)/audio_ahi.c

# --- Device build (freestanding: libnix devinit.o framework, no C runtime) ---
# Produces a real loadable narrator.device. Container-only (needs devinit.o).
DEVINIT     ?= /opt/amiga/m68k-amigaos/libnix/lib/devinit.o
# -m68020: target is 68030/40+ (PiStorm/emu68k), so use the 020 ISA. Critical
# for the -nostdlib device: 020 has inline 32-bit muls.l/divs.l, so runtime
# 32-bit *//* don't become libgcc __mulsi3/__divsi3 calls we can't link.
# -msoft-float: keep FPU opcodes out (they trap as F-line on this config).
DEV_CFLAGS  ?= -nostdlib -O3 -Wall -Wextra -Werror -fomit-frame-pointer -fbaserel -m68020 -msoft-float -DPLATFORM_AMIGA
# Vendored upstream codesets.library headers (third_party/codesets/include).
# The Bebbo NDK does not include them; we vendor jens-maus/libcodesets's so
# the engine can speak the library's published ABI rather than re-derive it.
DEV_INCLUDES := -Ithird_party/codesets/include
DEV_SRCS    := $(SRCDIR)/narrator_device.c $(SRCDIR)/nw_engine.c
DEV_BIN     := $(BUILD)/amiga/narrator.device
# Nothing about the server/voice is baked into the binary — the device reads it
# at runtime from ENV:narrator.wyoming (see nw_read_prefs). Only a localhost
# fallback is compiled in.

# Pass-through translator.library (libnix libinit.o; -fbaserel, no globals).
LIBINIT     ?= /opt/amiga/m68k-amigaos/libnix/lib/libinit.o
LIB_CFLAGS  ?= -nostdlib -O3 -Wall -Wextra -Werror -fomit-frame-pointer -fbaserel -m68020 -msoft-float
LIB_BIN     := $(BUILD)/amiga/translator.library

HDRS := $(wildcard $(SRCDIR)/*.h)

# Version + build-date stamped into the $VER strings of narrator.device /
# translator.library. VERSION/REVISION come from version.mk; the date is the
# Amiga "(dd.mm.yyyy)" form (portable across GNU and BSD date), computed when
# make parses this file — for 'make docker' that is inside the container.
VERDATE := $(shell date '+%d.%m.%Y')
VERDEF  := -DNW_VERSION=$(VERSION) -DNW_REVISION=$(REVISION) -DNW_BUILD_DATE='"($(VERDATE))"'

# --- Aminet distribution archive ---
DISTNAME  := NarratorWyomingDevice
DISTSTAGE := $(BUILD)/dist/$(DISTNAME)
DISTLHA   := $(BUILD)/$(DISTNAME).lha

# --- Docker ---
# Pin the cross-compiler image by digest, not by mutable `:latest`. This is
# what makes a 44.0 build today and a 44.0 build a year from now produce
# identical binaries, regardless of what stefanreinauer pushes upstream
# afterwards. To roll forward intentionally:
#   docker pull stefanreinauer/amiga-gcc:latest
#   docker image inspect --format='{{index .RepoDigests 0}}' \
#       stefanreinauer/amiga-gcc:latest
# then paste the resulting `stefanreinauer/amiga-gcc@sha256:...` below.
IMAGE      ?= stefanreinauer/amiga-gcc@sha256:68f3233fe3b270654b471e11e0b2e25fae0ac34350959b5be360c9af84ef388a
DOCKER_RUN := docker run --rm -v "$(CURDIR)":/work -w /work $(IMAGE)

.PHONY: all host amiga docker dist dist-pack bump release version readme-version refresh-codesets clean

all: host

host:  $(BUILD)/host/wyomingtest  $(BUILD)/host/saytest
amiga: $(BUILD)/amiga/wyomingtest $(BUILD)/amiga/saytest $(DEV_BIN) $(LIB_BIN) $(BUILD)/amiga/devtest $(BUILD)/amiga/failtest

device: $(DEV_BIN)
translator: $(LIB_BIN)

$(LIB_BIN): $(SRCDIR)/translator_library.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) $(VERDEF) $(LIBINIT) $(SRCDIR)/translator_library.c -o $@

$(DEV_BIN): $(DEV_SRCS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(DEV_CFLAGS) $(DEV_INCLUDES) $(VERDEF) $(DEVINIT) $(DEV_SRCS) -o $@

# devtest: normal CLI program that opens+drives narrator.device (clib2).
$(BUILD)/amiga/devtest: $(SRCDIR)/devtest.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/devtest.c

# failtest: graceful-failure scenarios -- unreachable IP, DNS failure,
# bad voice. Rewrites ENV:narrator.wyoming on the fly per scenario and
# restores it at the end. clib2 (uses stdio for the env-file dance).
$(BUILD)/amiga/failtest: $(SRCDIR)/failtest.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/failtest.c

# ---- host binaries ----
$(BUILD)/host/wyomingtest: $(PROTO) $(SRCDIR)/main.c $(HOST_NET) $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(PROTO) $(SRCDIR)/main.c $(HOST_NET)

$(BUILD)/host/saytest: $(PROTO) $(SRCDIR)/saytest.c $(HOST_NET) $(HOST_AUDIO) $(HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(PROTO) $(SRCDIR)/saytest.c $(HOST_NET) $(HOST_AUDIO)

# ---- Amiga binaries ----
$(BUILD)/amiga/wyomingtest: $(PROTO) $(SRCDIR)/main.c $(AMIGA_NET) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PROTO) $(SRCDIR)/main.c $(AMIGA_NET)

$(BUILD)/amiga/saytest: $(PROTO) $(SRCDIR)/saytest.c $(AMIGA_NET) $(AMIGA_AUDIO) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(PROTO) $(SRCDIR)/saytest.c $(AMIGA_NET) $(AMIGA_AUDIO)

# Build the Amiga binaries inside the cross-compiler container.
docker:
	$(DOCKER_RUN) make amiga

# Build the uploadable Aminet archive. Runs in the container because it has both
# the cross-compiler AND LhA; the result (build/NarratorWyomingDevice.lha) plus the
# repo's NarratorWyomingDevice.readme are what you upload to Aminet.
dist:
	$(DOCKER_RUN) make dist-pack

# Stage the drop-in pair + docs + installer and pack them. The archive holds a
# single NarratorWyomingDevice/ drawer. (Container-only: builds the Amiga binaries.)
dist-pack: $(DEV_BIN) $(LIB_BIN) NarratorWyomingDevice.readme docs/README.txt dist/Install dist/Install.info LICENSE config/narrator.wyoming.example
	rm -rf $(BUILD)/dist $(DISTLHA)
	mkdir -p $(DISTSTAGE)
	cp $(DEV_BIN)                      $(DISTSTAGE)/narrator.device
	cp $(LIB_BIN)                      $(DISTSTAGE)/translator.library
	cp NarratorWyomingDevice.readme         $(DISTSTAGE)/NarratorWyomingDevice.readme
	sed -i.bak "s/^Version:.*/Version:      $(VERSION).$(REVISION)/" $(DISTSTAGE)/NarratorWyomingDevice.readme && rm -f $(DISTSTAGE)/NarratorWyomingDevice.readme.bak
	cp docs/README.txt                 $(DISTSTAGE)/README.txt
	cp config/narrator.wyoming.example $(DISTSTAGE)/narrator.wyoming.example
	cp dist/Install                    $(DISTSTAGE)/Install
	cp dist/Install.info               $(DISTSTAGE)/Install.info
	cp LICENSE                         $(DISTSTAGE)/LICENSE
	cd $(BUILD)/dist && lha a ../$(DISTNAME).lha $(DISTNAME)
	@echo "Created $(DISTLHA) (upload it + NarratorWyomingDevice.readme to Aminet)"

# Print the current version.
version:
	@echo "$(VERSION).$(REVISION)"

# Sync the Aminet readme's Version: field to the current version.mk value (the
# committed readme is uploaded standalone, so it must match the binaries).
readme-version:
	@sed -i.bak "s/^Version:.*/Version:      $(VERSION).$(REVISION)/" NarratorWyomingDevice.readme && rm -f NarratorWyomingDevice.readme.bak
	@echo "NarratorWyomingDevice.readme Version: $(VERSION).$(REVISION)"

# Bump the REVISION in version.mk (44.0 -> 44.1) and sync the readme. This is a
# POST-release step: run it AFTER `make release` to open the next test cycle, so
# the working tree's version always names the release you're building toward (not
# the last one shipped). For a major/compat change, edit VERSION in version.mk by
# hand then `make readme-version`.
bump:
	@new=$$(( $(REVISION) + 1 )); \
	  sed -i.bak "s/^REVISION := .*/REVISION := $$new/" version.mk && rm -f version.mk.bak; \
	  echo "version.mk: $(VERSION).$(REVISION) -> $(VERSION).$$new"
	@$(MAKE) --no-print-directory readme-version

# Cut a release: package the CURRENT version and tag it. We do NOT bump here —
# version.mk already holds the revision you've been testing, so you ship exactly
# the number you validated. Clean-builds the Aminet archive ($VER date stamps
# fresh), commits the readme only if it needed syncing, then tags v<VERSION>.<REVISION>
# at HEAD. Afterwards, `make bump` to open the next test cycle, then push.
release:
	@$(MAKE) --no-print-directory readme-version
	$(MAKE) clean dist
	@if ! git diff --quiet HEAD -- NarratorWyomingDevice.readme; then \
	   git add NarratorWyomingDevice.readme; \
	   git commit -m "Sync readme to $(VERSION).$(REVISION)"; \
	 fi
	@git tag -a v$(VERSION).$(REVISION) -m "narrator.wyoming $(VERSION).$(REVISION)"; \
	  echo "Tagged v$(VERSION).$(REVISION)  ->  $(DISTLHA)"; \
	  echo "Next: 'make bump' to open the next test cycle, then 'git push --follow-tags'"

# Refresh vendored codesets.library headers from upstream (latest GitHub
# release by default; pass V=<tag> for a specific one — e.g. V=6.22). The
# script lives next to the vendored tree so it's easy to find. Re-run the
# on-target devtest afterwards (write 3 covers the codesets path).
refresh-codesets:
	@if [ -n "$(V)" ]; then \
	   third_party/codesets/refresh.sh --version "$(V)"; \
	 else \
	   third_party/codesets/refresh.sh; \
	 fi

clean:
	rm -rf $(BUILD)

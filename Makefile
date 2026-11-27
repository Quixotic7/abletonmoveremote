# SPDX-License-Identifier: MPL-2.0
# Move Remote.
#
#   make                 the simulator (build/sim/moveremote-sim) and the host side (build/host: libmoveremote.a, mrctl)
#   make test            the simulator and mrctl selftest end to end, plus the page and the WebSocket handshake;
#                        the USB-A embedding hooks (tests/usba_check.c)
#   make move            build/move/moveremote-host and moveremote-heal for the Move (aarch64, in Docker)
#   make package         dist/moveremote-module.tar.gz, what Schwung Manager installs
#   scripts/deploy.sh --test   onto ableton@move.local and started, silent (scripts/deploy.sh says more)
TARGET ?= sim
CC ?= cc
B := build/$(TARGET)
CFLAGS ?= -O2 -g
WARN := -std=gnu11 -Wall -Wextra
GEN := build/gen/web_page.h build/gen/bridge_page.h

DEV_SRC := device/mr_core.c device/mr_ws.c common/font5x7.c
DEV_HDR := $(wildcard device/*.h common/*.h include/moveremote/*.h) $(GEN)
HOST_SRC := host/mr_client.c host/mr_gfx.c common/font5x7.c
HOST_HDR := $(wildcard include/moveremote/*.h common/*.h)

.PHONY: all sim host test move package clean
all: sim host
sim: build/sim/moveremote-sim
host: build/host/libmoveremote.a build/host/mrctl

# the page the device serves, as a C array (made here, on the Mac: the Docker image has no xxd)
build/gen/web_page.h: web/index.html
	mkdir -p build/gen
	cd web && xxd -i index.html | sed 's/index_html/mr_web_page/g' > ../$@
build/gen/bridge_page.h: web/bridge.html
	mkdir -p build/gen
	cd web && xxd -i bridge.html | sed 's/bridge_html/mr_bridge_page/g' > ../$@

build/sim/moveremote-sim: device/plat_sim.c $(DEV_SRC) $(DEV_HDR)
	mkdir -p build/sim
	$(CC) $(CFLAGS) $(WARN) -Ibuild/gen -o $@ device/plat_sim.c $(DEV_SRC) -lm -lpthread

build/host/%.o: host/%.c $(HOST_HDR)
	mkdir -p build/host
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@
build/host/font5x7.o: common/font5x7.c common/font5x7.h
	mkdir -p build/host
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@
build/host/libmoveremote.a: build/host/mr_client.o build/host/mr_gfx.o build/host/font5x7.o
	rm -f $@
	ar rcs $@ $^
build/host/mrctl: tools/mrctl.c build/host/libmoveremote.a
	$(CC) $(CFLAGS) $(WARN) -o $@ $< build/host/libmoveremote.a -lm

# the USB-A embedding hooks on the device core alone (no platform)
build/sim/usba_check: tests/usba_check.c $(DEV_SRC) $(DEV_HDR)
	mkdir -p build/sim
	$(CC) $(CFLAGS) $(WARN) -Ibuild/gen -o $@ tests/usba_check.c $(DEV_SRC) -lm -lpthread

test: sim host build/sim/usba_check
	build/sim/usba_check
	tests/loopback.sh

# the Move: Schwung's toolchain (debian bookworm, gcc-aarch64-linux-gnu), linked static so the unit's glibc is never
# the question
DOCKER_IMAGE ?= moveremote-move-build
move: $(GEN)
	docker build -q -t $(DOCKER_IMAGE) docker
	docker run --rm -v "$(abspath .)":/w -w /w $(DOCKER_IMAGE) \
	    make TARGET=move CC=aarch64-linux-gnu-gcc build/move/moveremote-host build/move/moveremote-heal

build/move/moveremote-host: device/plat_move.c $(DEV_SRC) $(DEV_HDR)
	mkdir -p build/move
	$(CC) $(CFLAGS) $(WARN) -Ibuild/gen -o $@ device/plat_move.c $(DEV_SRC) -lm -lpthread -lrt -static
build/move/moveremote-heal: device/heal.c
	mkdir -p build/move
	$(CC) -O2 $(WARN) -static $< -o $@

package: move
	scripts/package.sh

clean:
	rm -rf build dist

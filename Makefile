# gen_mpris — WACUP MPRIS2 Plugin
#
# Build targets:
#   gen_mpris.dll    — PE plugin (cross-compiled with mingw)
#   gen_mpris_host   — Native Linux helper (linked with libdbus)

# Cross-compiler for 32-bit Windows PE
MINGW_CC = i686-w64-mingw32-gcc
MINGW_CFLAGS = -Wall -Wextra -O2 -I.

# Native compiler
CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.
DBUS_CFLAGS = $(shell pkg-config --cflags dbus-1)
DBUS_LIBS = $(shell pkg-config --libs dbus-1)

# WACUP plugins directory (adjust for your Wine prefix)
WACUP_PLUGINS = $(HOME)/.wine/drive_c/Program Files (x86)/WACUP/Plugins
HOST_INSTALL_DIR    = $(HOME)/.local/bin

.PHONY: all clean install install-dll install-host test

all: gen_mpris.dll gen_mpris_host

test: gen_mpris.dll gen_mpris_host test_harness.exe
	@echo "Starting gen_mpris_host in background..."
	./gen_mpris_host & echo $$! > /tmp/gen_mpris_host.pid
	@sleep 0.5
	@echo "Running test harness under Wine..."
	wine test_harness.exe; kill $$(cat /tmp/gen_mpris_host.pid) 2>/dev/null; rm -f /tmp/gen_mpris_host.pid

test_harness.exe: test_harness.c sdk/wa_ipc.h
	$(MINGW_CC) $(MINGW_CFLAGS) -o $@ test_harness.c -luser32 -lkernel32

gen_mpris.dll: gen_mpris.c ipc_protocol.h sdk/gen.h sdk/wa_ipc.h
	$(MINGW_CC) $(MINGW_CFLAGS) -shared -o $@ gen_mpris.c -lws2_32 -luser32 -lkernel32

gen_mpris_host: gen_mpris_host.c ipc_protocol.h
	$(CC) $(CFLAGS) $(DBUS_CFLAGS) -o $@ gen_mpris_host.c $(DBUS_LIBS)

install: install-dll install-host

install-dll: gen_mpris.dll
	cp gen_mpris.dll "$(WACUP_PLUGINS)/"
	@echo "Installed gen_mpris.dll -> $(WACUP_PLUGINS)/"

install-host: gen_mpris_host
	mkdir -p "$(HOST_INSTALL_DIR)"
	cp gen_mpris_host "$(HOST_INSTALL_DIR)/"
	@echo "Installed gen_mpris_host -> $(HOST_INSTALL_DIR)/"


clean:
	rm -f gen_mpris.dll gen_mpris_host test_harness.exe

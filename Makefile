# knulli-overlay: a GL-injected status overlay (volume, brightness, ...)
#
#   make                          build for the host
#   make BOARD=a133               cross build with that board's buildroot toolchain
#   make CROSS_COMPILE=aarch64-linux-      ... or any other toolchain prefix
#   make BOARD=a133 test          also build the on-device checks
#   make install DESTDIR=... PREFIX=/usr
#
# Nothing but libc is needed for the library and the CLI, so a bare
# CROSS_COMPILE prefix is enough; only the test programs want the sysroot's
# libEGL/libGLESv2, which a buildroot toolchain wrapper already points at.

PREFIX  ?= /usr
DESTDIR ?=

# Convenience for this tree: point BOARD at a built buildroot output and the
# toolchain is discovered from it (a133, h700, rk3326, ...).
BOARD         ?=
KNULLI_OUTPUT ?= ../knulli-linux/output/$(BOARD)
ifneq ($(BOARD),)
  BOARD_GCC := $(firstword $(wildcard $(KNULLI_OUTPUT)/host/bin/*-buildroot-linux-*-gcc))
  ifeq ($(BOARD_GCC),)
    $(error no buildroot toolchain under $(KNULLI_OUTPUT)/host/bin -- build that board first, or set CROSS_COMPILE)
  endif
  CROSS_COMPILE ?= $(BOARD_GCC:%gcc=%)
endif

CROSS_COMPILE ?=
CC      = $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu99 -fvisibility=hidden -Isrc
LDFLAGS ?=

# Only the test programs link GL.  On the Mali boards libEGL.so is a stub in
# front of libmali, which the linker then wants named as well.
GL_LIBS ?= -lEGL -lGLESv2
ifneq ($(BOARD),)
  ifneq ($(wildcard $(KNULLI_OUTPUT)/host/*/sysroot/usr/lib*/libmali.so),)
    GL_LIBS += -lmali
  endif
endif

# Host and target artefacts never share a name, so both can live side by side.
OUTDIR ?= build/$(if $(BOARD),$(BOARD),host)

LIB     = $(OUTDIR)/libknulli-overlay.so
BIN     = $(OUTDIR)/knulli-overlay
# The library reads hud.config and decodes the bezel PNG itself (ov_png), so a
# game process still needs nothing but libc.
LIB_SRC = src/ov_inject.c src/ov_gl.c src/ov_layout.c src/ov_state.c \
          src/ov_anchor.c src/ov_atlas.c src/ov_bezel.c src/ov_png.c \
          src/ov_inflate.c src/ov_config.c
BIN_SRC = src/ov_ctl.c src/ov_state.c src/ov_battery.c src/ov_config.c \
          src/ov_anchor.c src/ov_bezel.c src/ov_png.c src/ov_inflate.c

all: $(LIB) $(BIN)

$(OUTDIR):
	mkdir -p $@

# Only the interposed hooks are exported (see the version script), so we cannot
# shadow a symbol the host process expects to resolve elsewhere.
$(LIB): $(LIB_SRC) src/ov_atlas.h src/ov_inject.map | $(OUTDIR)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(LIB_SRC) $(LDFLAGS) -ldl -lm \
	    -Wl,--version-script=src/ov_inject.map

$(BIN): $(BIN_SRC) src/ov_state.h src/ov_battery.h | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $(BIN_SRC) $(LDFLAGS)

# ov_test renders the panel offscreen; ov_app fakes a game and checks that the
# injection leaves the GL state alone.  Both run on the host and on the device.
test: $(OUTDIR)/ov_test $(OUTDIR)/ov_app

$(OUTDIR)/ov_test: test/ov_test.c src/ov_gl.c src/ov_layout.c src/ov_state.c \
                   src/ov_anchor.c src/ov_atlas.c src/ov_bezel.c src/ov_png.c \
                   src/ov_inflate.c src/ov_config.c | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(GL_LIBS) -lm

$(OUTDIR)/ov_app: test/ov_app.c | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(GL_LIBS)

install: all
	install -D -m 0755 $(LIB) $(DESTDIR)$(PREFIX)/lib/$(LIB)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -rf build *.ppm

.PHONY: all test install clean

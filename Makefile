# DWPurple - GNU make build for Linux/BSD (GTK3 Dynamic Windows), macOS and
# MSYS2/MinGW.  OS/2 uses Makefile.os2.
#
#   make                      build ./dwpurple
#   make HAVE_XSS=0           build without X11 screen saver idle detection
#   make DW_CFLAGS=... DW_LIBS=...   when Dynamic Windows has no .pc/config
#   make install PREFIX=/usr/local [DESTDIR=...]

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
SHAREDIR ?= $(PREFIX)/share/dwpurple

CC       ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS   ?= -O2 -g
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

UNAME_S  := $(shell uname -s 2>/dev/null)

# ---- Dynamic Windows: pkg-config, then dwindows-config, then defaults
DW_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags dwindows 2>/dev/null || dwindows-config --cflags 2>/dev/null)
DW_LIBS   ?= $(shell $(PKG_CONFIG) --libs dwindows 2>/dev/null || dwindows-config --libs 2>/dev/null || echo -ldwindows)

# ---- libpurple 2.x and GLib
PURPLE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags purple glib-2.0 gobject-2.0 gmodule-2.0)
PURPLE_LIBS   ?= $(shell $(PKG_CONFIG) --libs purple glib-2.0 gobject-2.0 gmodule-2.0)

EXTRA_CFLAGS :=
EXTRA_LIBS   :=

ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
  EXE := .exe
  EXTRA_LIBS += -lwinmm -mwindows
else
  EXE :=
  # gdklock.c: dlsym() and readable function names in its stack reports
  EXTRA_LIBS += -rdynamic
  ifeq ($(UNAME_S),Linux)
    EXTRA_LIBS += -ldl
  endif
  # X11 idle detection (optional; falls back to activity in DWPurple windows)
  HAVE_XSS ?= $(shell $(PKG_CONFIG) --exists xscrnsaver x11 2>/dev/null && echo 1 || echo 0)
  ifeq ($(HAVE_XSS),1)
    EXTRA_CFLAGS += -DHAVE_XSS $(shell $(PKG_CONFIG) --cflags xscrnsaver x11)
    EXTRA_LIBS   += $(shell $(PKG_CONFIG) --libs xscrnsaver x11)
  endif
endif

SRCS := main.c core.c eventloop.c util.c blist.c accounts.c conv.c emoji.c \
        request.c notify.c xfer.c plugins.c debugwin.c sound.c idle.c \
        pounces.c prefs.c roomlist.c privacy.c tray.c gdklock.c ircconsole.c clipimg.c
OBJS := $(addprefix build/,$(SRCS:.c=.o))
TARGET := dwpurple$(EXE)

ALL_CFLAGS := -std=gnu99 $(WARN) $(CFLAGS) $(DW_CFLAGS) $(PURPLE_CFLAGS) $(EXTRA_CFLAGS)

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(DW_LIBS) $(PURPLE_LIBS) $(EXTRA_LIBS)

build/%.o: src/%.c src/dwpurple.h | build
	$(CC) $(ALL_CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SHAREDIR)/icons
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 644 icons/*.png $(DESTDIR)$(SHAREDIR)/icons/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)$(SHAREDIR)

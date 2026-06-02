# Makefile for c2c — core-to-core latency measurement tool
# Target: Linux x86-64, GCC or Clang
#
# Targets:
#   make          Build c2cl
#   make clean    Remove build artefacts
#   make install  Install to PREFIX/bin (default: /usr/local)

CC      ?= gcc
PREFIX  ?= /usr/local

CFLAGS  := -O2 -march=native -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE -Isrc
LDFLAGS := -lpthread -lm -lrt

TARGET  := c2cl
SRCDIR  := src
OBJDIR  := build

SRCS := $(SRCDIR)/main.c         \
        $(SRCDIR)/tsc.c          \
        $(SRCDIR)/utils.c        \
        $(SRCDIR)/stats.c        \
        $(SRCDIR)/topology.c     \
        $(SRCDIR)/bench_cas.c    \
        $(SRCDIR)/bench_rw.c     \
        $(SRCDIR)/bench_msg.c    \
        $(SRCDIR)/bench_direct.c

OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

.PHONY: all clean install uninstall help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "  LD  $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@echo "  CC  $<"
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "Installed to $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build the c2c binary (default)"
	@echo "  clean      Remove build artefacts"
	@echo "  install    Install to PREFIX/bin (default: /usr/local)"
	@echo "  uninstall  Remove installed binary"
	@echo "  help       Print this message"
	@echo ""
	@echo "Variables:"
	@echo "  CC=gcc|clang   Compiler to use (default: gcc)"
	@echo "  PREFIX=<path>  Install prefix (default: /usr/local)"

# ---- Explicit header dependencies ----------------------------------
$(OBJDIR)/main.o:         $(SRCDIR)/main.c         $(SRCDIR)/c2c.h $(SRCDIR)/tsc.h $(SRCDIR)/utils.h $(SRCDIR)/stats.h $(SRCDIR)/topology.h
$(OBJDIR)/tsc.o:          $(SRCDIR)/tsc.c          $(SRCDIR)/tsc.h
$(OBJDIR)/utils.o:        $(SRCDIR)/utils.c        $(SRCDIR)/utils.h $(SRCDIR)/tsc.h $(SRCDIR)/c2c.h
$(OBJDIR)/stats.o:        $(SRCDIR)/stats.c        $(SRCDIR)/stats.h $(SRCDIR)/c2c.h $(SRCDIR)/utils.h
$(OBJDIR)/topology.o:     $(SRCDIR)/topology.c     $(SRCDIR)/topology.h $(SRCDIR)/c2c.h $(SRCDIR)/utils.h
$(OBJDIR)/bench_cas.o:    $(SRCDIR)/bench_cas.c    $(SRCDIR)/c2c.h $(SRCDIR)/tsc.h $(SRCDIR)/utils.h $(SRCDIR)/stats.h
$(OBJDIR)/bench_rw.o:     $(SRCDIR)/bench_rw.c     $(SRCDIR)/c2c.h $(SRCDIR)/tsc.h $(SRCDIR)/utils.h $(SRCDIR)/stats.h
$(OBJDIR)/bench_msg.o:    $(SRCDIR)/bench_msg.c    $(SRCDIR)/c2c.h $(SRCDIR)/tsc.h $(SRCDIR)/utils.h $(SRCDIR)/stats.h
$(OBJDIR)/bench_direct.o: $(SRCDIR)/bench_direct.c $(SRCDIR)/c2c.h $(SRCDIR)/tsc.h $(SRCDIR)/utils.h $(SRCDIR)/topology.h $(SRCDIR)/histogram.h

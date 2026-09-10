# Makefile for geany-pfiles - p-file (preprocess) support for Geany 2.x.
#
#   make            -> geany-pfiles.so
#   make install    -> system plugin dir (if writable) + ~/.local/bin helper
#   make clean
#
# Salt deploys this from vertelab/geany-odoo (workstation.geany).

CC      ?= cc
CFLAGS  ?= -O2 -g
PKGS     = geany gtk+-3.0 glib-2.0 gio-2.0
GEMM_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
GEMM_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

ifeq ($(strip $(GEMM_CFLAGS)),)
$(error "Missing dev packages (geany gtk3 glib gio). On Ubuntu: sudo apt install libgtk-3-dev")
endif

CFLAGS  += -fPIC -Wall -Wextra -std=c11 $(GEMM_CFLAGS) -Isrc
LDFLAGS += -shared

SRCDIR = src
OBJS   = $(SRCDIR)/geany-pfiles.o
PLUGIN = geany-pfiles.so
SYS_PLUGINDIR = /usr/lib/x86_64-linux-gnu/geany

all: $(PLUGIN)

$(PLUGIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(GEMM_LIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/geany-pfiles.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(PLUGIN)
	@if cp $(PLUGIN) $(SYS_PLUGINDIR)/$(PLUGIN) 2>/dev/null; then \
		echo "   $(SYS_PLUGINDIR)/$(PLUGIN)"; \
	else echo "   !! could not write $(SYS_PLUGINDIR) - use sudo or Salt"; fi
	@mkdir -p $(HOME)/.local/bin
	@cp scripts/geany-pfiles $(HOME)/.local/bin/geany-pfiles
	@chmod +x $(HOME)/.local/bin/geany-pfiles
	@echo "   $(HOME)/.local/bin/geany-pfiles"

clean:
	rm -f $(OBJS) $(PLUGIN)

.PHONY: all clean install

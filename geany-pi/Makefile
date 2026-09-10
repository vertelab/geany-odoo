# Makefile for geany-pi - a Geany 2.x (GTK3) OpenAI-compatible plugin.
#
# Builds directly with pkg-config against:
#   - geany        (headers + API macros, from geany-common on Ubuntu)
#   - gtk+-3.0     (GTK3 widgets)
#   - json-glib-1.0(JSON)
#   - libcurl      (HTTP)
#
# Targets:
#   make            -> build src/geany-pi.so
#   make install    -> copy into xdg data dir for the current user
#                      (# ~/.local/share/geany/plugins is scanned by Geany 1.38+)
#                      Geany looks in $XDG_DATA_HOME/geany/plugins and
#                      $(prefix)/lib/geany. On Ubuntu the standard user path is
#                      ~/.local/lib/geany for API 224- (Geany <= 2).  We write
#                      to all the common per-user spots so any build works.
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g
PKGS     = geany gtk+-3.0 glib-2.0 json-glib-1.0

# pkg-config must resolve ALL of them.
GEMM_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
GEMM_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

ifeq ($(strip $(GEMM_CFLAGS)),)
$(error "Missing dev packages. On Ubuntu: sudo apt install geany-common libgtk-3-dev libjson-glib-dev libcurl4-openssl-dev")
endif

CFLAGS  += -fPIC -Wall -Wextra -std=c11 $(GEMM_CFLAGS) -Isrc
LDFLAGS += -shared

# --- optional embedded terminal (VTE) ------------------------------------
# Enables the "Pi" terminal tab when libvte-2.91-dev is installed:
#     sudo apt install libvte-2.91-dev
VTE_CFLAGS := $(shell pkg-config --cflags vte-2.91 2>/dev/null)
VTE_LIBS   := $(shell pkg-config --libs   vte-2.91 2>/dev/null)
ifneq ($(strip $(VTE_CFLAGS)),)
CFLAGS    += -DHAVE_VTE $(VTE_CFLAGS)
GEMM_LIBS += $(VTE_LIBS)
$(info geany-pi: VTE found -> embedded Pi terminal enabled)
else
$(info geany-pi: VTE not found -> no embedded terminal (apt install libvte-2.91-dev))
endif

SRCDIR   = src
OBJS     = $(SRCDIR)/geany-pi.o $(SRCDIR)/active.o \
           $(SRCDIR)/bridge.o $(SRCDIR)/pi_mcp.o $(SRCDIR)/terminal.o \
           $(SRCDIR)/pi_tab.o $(SRCDIR)/log.o $(SRCDIR)/prefs.o

PLUGIN   = geany-pi.so
SYS_PLUGINDIR = /usr/lib/x86_64-linux-gnu/geany

# ---- rules ---------------------------------------------------------------

all: $(PLUGIN)

$(PLUGIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(GEMM_LIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/geany-pi-common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Install into the standard Geany plugin lookup paths for user-space.
# Modern Geany reads XDG_DATA_HOME(the-application independent) first, then
# the "lib" dirs. We install a copy to the two conventional per-user spots
# and report both.
install: $(PLUGIN)
	@echo "Installing geany-pi (plugin + helpers)..."
	@# Geany loads plugins from the system plugin dir; NEVER install a second
	@# copy into user plugin dirs (two loaded copies crashed Geany).
	@if cp $(PLUGIN) $(SYS_PLUGINDIR)/$(PLUGIN) 2>/dev/null; then \
		echo "   $(SYS_PLUGINDIR)/$(PLUGIN)"; \
	else \
		echo "   !! could not write $(SYS_PLUGINDIR) - run:"; \
		echo "      sudo cp $(PWD)/$(PLUGIN) $(SYS_PLUGINDIR)/"; \
	fi
	@mkdir -p $(HOME)/.local/bin
	@for h in geany-pi-ctl geany-pi-git geany-pi-mcp-sync geany-pi-mcp-router; do \
		cp scripts/$$h $(HOME)/.local/bin/$$h && chmod +x $(HOME)/.local/bin/$$h; \
	done
	@echo "   $(HOME)/.local/bin/geany-pi-{ctl,git,mcp-sync,mcp-router}"
	@echo "Restart Geany to load the new build."

clean:
	rm -f $(OBJS) $(PLUGIN)

.PHONY: all clean install

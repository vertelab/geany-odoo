/*
 * geany-pi.c - plugin entry point, lifecycle.
 *
 * The plugin provides three independent things:
 *   - a "Pi" tab          (pi_tab.c)     one Pi coding-agent session
 *   - a "Multiterm" tab   (terminal.c)   per-directory $SHELL terminals
 *   - an agent surface    (bridge.c, pi_mcp.c)  UNIX-socket control bridge
 *                         and ai_pi_mcp (Odoo MCP) setup
 *
 * All configuration lives in Geany's Plugin Manager -> Preferences
 * (plugin_configure -> prefs.c).  No custom Tools menu items.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <string.h>

/* Geany fills geany_plugin in for legacy plugins (needed by
 * plugin_signal_connect); geany_data is set by us in plugin_init. */
GeanyData     *geany_data;
GeanyPlugin   *geany_plugin;

static gboolean g_inited   = FALSE;   /* init-without-cleanup guard */
static gboolean g_quitting = FALSE;   /* set by "geany-before-quit"  */

/* Geany tells us when it is really quitting (as opposed to the plugin just
 * being disabled in the Plugin Manager).  Mirrors the LSP plugin. */
static void on_geany_before_quit(G_GNUC_UNUSED GObject *obj,
                                 G_GNUC_UNUSED gpointer data)
{
    g_quitting = TRUE;
}

static gboolean geany_quitting(void)
{
    return g_quitting;
}

/* ---------------------------------------------------------------------- */
/* Metadata / ABI guard                                                    */
/* ---------------------------------------------------------------------- */
PLUGIN_VERSION_CHECK(224)

PLUGIN_SET_INFO("geany-pi",
                "Multiterm (terminal per document directory), a Pi agent tab, "
                "and an agent control bridge (UNIX socket + ai_pi_mcp setup)",
                "0.3.0",
                "Vertelab")

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* ---------------------------------------------------------------------- */
void plugin_init(GeanyData *data)
{
    /* Safety: if two copies of the plugin are somehow loaded, do not run
     * the whole init twice (duplicate tabs, duplicate bridges, crash). */
    if (g_inited)
    {
        gemm_logf("init: redan initierad - hoppar över (dubbel laddning?)");
        return;
    }
    g_inited = TRUE;
    geany_data = data;
    gemm_logf("init: start (quitting=%d)", (int)geany_quitting());

    /* Keep the module loaded while Geany runs.  Without this, disabling the
     * plugin can unload the .so while pending timeouts / child watches /
     * signal handlers still point into it -> segfault.  (Same reason the LSP
     * plugin calls this.) */
    if (geany_plugin)
        plugin_module_make_resident(geany_plugin);
    plugin_signal_connect(geany_plugin, NULL, "geany-before-quit", TRUE,
                          G_CALLBACK(on_geany_before_quit), NULL);

    gemm_prefs_load();

    /* UNIX-socket bridge for external agents (geany-pi-ctl / Pi). */
    gemm_logf("init: brygga...");
    if (!gemm_bridge_start())
        gemm_logf("init: bryggan kunde inte startas (socket upptagen?)");

    /* ai_pi_mcp: local entry + router + per-machine sync (agent tooling). */
    gemm_logf("init: pi_mcp_setup...");
    {
        gchar *msg = NULL;
        if (gemm_pi_mcp_setup(&msg))
            gemm_logf("pi_mcp: %s", msg ? msg : "ok");
        g_free(msg);
    }
    gemm_logf("init: router...");
    gemm_pi_mcp_router_ensure();
    gemm_pi_mcp_sync_schedule();

    /* The two tabs. */
    gemm_logf("init: flikar...");
    gemm_pi_tab_show();
    gemm_terminal_show();

    gemm_logf("init: klar (geany-pi 0.3.0)");
}

void plugin_cleanup(void)
{
    gemm_logf("cleanup: start");
    /* Tear down everything we created.  A disabled-then-re-enabled plugin
     * reuses these statics, so no dangling widget may survive. */
    gemm_pi_tab_shutdown();
    gemm_terminal_shutdown();
    gemm_pi_mcp_shutdown();
    gemm_bridge_stop();

    gemm_logf("cleanup: klar (quitting=%d)", (int)geany_quitting());
    geany_data = NULL;
    g_inited = FALSE;
}

/* ---------------------------------------------------------------------- */
/* Configuration (Plugin Manager -> Preferences)                          */
/* ---------------------------------------------------------------------- */
GtkWidget *plugin_configure(GtkDialog *dialog)
{
    GtkWidget *w = gemm_prefs_configure_widget(dialog);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(gemm_prefs_on_response), NULL);
    return w;
}

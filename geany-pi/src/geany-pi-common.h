/*
 * geany-pi-common.h - shared declarations for the geany-pi plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANY_LLM_COMMON_H
#define GEANY_LLM_COMMON_H

#include <geanyplugin.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <Scintilla.h>
#include <SciLexer.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

typedef enum
{
    GEMM_REQUEST_DOC = 0,    /* whole active document */
    GEMM_REQUEST_SELECTION   /* current selection     */
} GemmRequestKind;

/* globals from geany-pi.c (extern, real symbols in the plugin .so)      */
extern GeanyData   *geany_data;
/* Geany fills this in for legacy plugins - needed by plugin_signal_connect */
extern GeanyPlugin *geany_plugin;

/* ---- debug log (log.c) ------------------------------------------------- */
void gemm_logf(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---- active.c: document/editor helpers -------------------------------- */
/* doc == NULL means "the current document" (see gemm_active_resolve). */
GeanyDocument *gemm_active_resolve(GeanyDocument *doc);
gboolean gemm_active_has_text(GeanyDocument *doc);
gboolean gemm_active_has_selection(GeanyDocument *doc);
gchar   *gemm_active_get_text(GeanyDocument *doc, GemmRequestKind kind,
                              gint *start, gint *end);
void     gemm_editor_replace(GeanyDocument *doc, gint start, gint end,
                             const gchar *new_text);
/* convenience wrappers for the active document */
gboolean gemm_active_has_text_active(void);
gboolean gemm_active_has_selection_active(void);
gchar   *gemm_active_get_text_active(GemmRequestKind kind,
                                     gint *start, gint *end);
void     gemm_editor_replace_active(gint start, gint end,
                                    const gchar *new_text);
void     gemm_ui_status(const gchar *utf8_status);

/* ---- bridge.c: UNIX-socket control bridge for external agents ---------- */
gboolean     gemm_bridge_start(void);
void         gemm_bridge_stop(void);
const gchar *gemm_bridge_socket_path(void);

/* ---- pi_mcp.c: ai_pi_mcp (Odoo MCP) configuration + local router ------- */
gboolean gemm_pi_mcp_setup(gchar **message);
void     gemm_pi_mcp_sync(void);
void     gemm_pi_mcp_sync_schedule(void);
void     gemm_pi_mcp_router_ensure(void);
void     gemm_pi_mcp_shutdown(void);

/* ---- prefs.c: configuration (exposed via Geany's Plugin Manager) ------- */
void         gemm_prefs_load(void);
void         gemm_prefs_save(void);
gboolean     gemm_prefs_apply(GtkDialog *dlg);
void         gemm_prefs_on_response(GtkDialog *dlg, gint resp, gpointer data);
GtkWidget   *gemm_prefs_configure_widget(GtkDialog *dlg);
const gchar *gemm_prefs_term_program(void);
gint         gemm_prefs_term_max(void);
const gchar *gemm_prefs_pi_program(void);

/* ---- terminal.c: per-directory $SHELL terminals ------------------------ */
void gemm_terminal_show(void);
void gemm_terminal_shutdown(void);

/* ---- pi_tab.c: single Pi agent tab (separate from the terminal) -------- */
void gemm_pi_tab_show(void);
void gemm_pi_tab_shutdown(void);

#endif /* GEANY_LLM_COMMON_H */

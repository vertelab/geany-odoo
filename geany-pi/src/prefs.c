/*
 * prefs.c - plugin configuration.
 *
 * Exposed through Geany's Plugin Manager ("Preferences" button) via
 * plugin_configure(), NOT through custom menu items. Stored as a GKeyFile:
 *   ~/.config/geany/geany-pi/geany-pi.conf
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <glib/gstdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */
typedef struct
{
    /* Multiterm (per-directory terminals) */
    gchar *term_program;    /* "" -> $SHELL -l */
    gint   term_max;        /* max concurrent terminals */

    /* Pi tab */
    gchar *pi_program;      /* "pi" */

    /* agent surface */
    gchar *mcp_token;       /* shared ai_pi_mcp token */
    gchar *mcp_url_pattern; /* http://{host}:8069/mcp */
} Prefs;

static Prefs g_p;

static const gchar *DEFAULT_URL_PATTERN = "http://{host}:8069/mcp";

static gchar *conf_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "geany", "geany-pi",
                            "geany-pi.conf", NULL);
}

void gemm_prefs_load(void)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path = conf_path();

    g_free(g_p.term_program);
    g_free(g_p.pi_program);
    g_free(g_p.mcp_token);
    g_free(g_p.mcp_url_pattern);
    memset(&g_p, 0, sizeof(g_p));
    g_p.term_max = 6;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    {
        gchar *s;
        if ((s = g_key_file_get_string(kf, "multiterm", "program", NULL)))
            g_p.term_program = s;
        if (g_key_file_has_key(kf, "multiterm", "max_terminals", NULL))
            g_p.term_max = g_key_file_get_integer(kf, "multiterm",
                                                  "max_terminals", NULL);
        if ((s = g_key_file_get_string(kf, "pi", "program", NULL)))
            g_p.pi_program = s;
        if ((s = g_key_file_get_string(kf, "mcp", "token", NULL)))
            g_p.mcp_token = s;
        if ((s = g_key_file_get_string(kf, "mcp", "url_pattern", NULL)))
            g_p.mcp_url_pattern = s;
    }
    if (!g_p.term_program)    g_p.term_program = g_strdup("");
    if (!g_p.pi_program)      g_p.pi_program = g_strdup("pi");
    if (!g_p.mcp_token)       g_p.mcp_token = g_strdup("");
    if (!g_p.mcp_url_pattern) g_p.mcp_url_pattern = g_strdup(DEFAULT_URL_PATTERN);
    if (g_p.term_max < 1 || g_p.term_max > 24) g_p.term_max = 6;

    g_key_file_free(kf);
    g_free(path);
}

void gemm_prefs_save(void)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path = conf_path();
    gchar *dir = g_path_get_dirname(path);
    gchar *data;
    gsize len = 0;

    g_mkdir_with_parents(dir, 0700);
    g_key_file_set_string(kf, "multiterm", "program", g_p.term_program ? g_p.term_program : "");
    g_key_file_set_integer(kf, "multiterm", "max_terminals", g_p.term_max);
    g_key_file_set_string(kf, "pi", "program", g_p.pi_program ? g_p.pi_program : "pi");
    g_key_file_set_string(kf, "mcp", "token", g_p.mcp_token ? g_p.mcp_token : "");
    g_key_file_set_string(kf, "mcp", "url_pattern",
                          g_p.mcp_url_pattern ? g_p.mcp_url_pattern : DEFAULT_URL_PATTERN);
    data = g_key_file_to_data(kf, &len, NULL);
    if (data)
    {
        (void)g_file_set_contents(path, data, (gssize)len, NULL);
        (void)g_chmod(path, 0600);
        g_free(data);
    }
    gemm_logf("prefs: sparade %s", path);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* accessors used by the other modules */
const gchar *gemm_prefs_term_program(void) { return g_p.term_program; }
gint         gemm_prefs_term_max(void)     { return g_p.term_max; }
const gchar *gemm_prefs_pi_program(void)   { return g_p.pi_program; }

/* Write a mcp-hosts.json for the helpers (token + url pattern). */
static void write_mcp_hosts(void)
{
    gchar *dir = g_build_filename(g_get_user_config_dir(), "geany", "geany-pi", NULL);
    gchar *path = g_build_filename(dir, "mcp-hosts.json", NULL);
    gchar *json = g_strdup_printf(
        "{\n  \"default_token\": \"%s\",\n  \"default_url_pattern\": \"%s\"\n}\n",
        g_p.mcp_token ? g_p.mcp_token : "",
        g_p.mcp_url_pattern ? g_p.mcp_url_pattern : DEFAULT_URL_PATTERN);

    g_mkdir_with_parents(dir, 0700);
    (void)g_file_set_contents(path, json, -1, NULL);
    (void)g_chmod(path, 0600);
    gemm_logf("prefs: skrev %s", path);
    msgwin_status_add("[geany-pi] mcp-hosts.json uppdaterad");
    g_free(json);
    g_free(path);
    g_free(dir);
}

static void on_write_hosts(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    /* pull the widgets into the in-memory prefs first */
    gemm_prefs_save();          /* values are collected by the caller */
    write_mcp_hosts();
}

/* ------------------------------------------------------------------ */
/* Geany preferences widget                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    GtkWidget *term_prog;
    GtkWidget *term_max;
    GtkWidget *pi_prog;
    GtkWidget *mcp_token;
    GtkWidget *mcp_pattern;
} Widgets;

static void collect(GtkDialog *dlg, Prefs *p)
{
    Widgets *w = g_object_get_data(G_OBJECT(dlg), "gemm-prefs-widgets");
    const gchar *s;

    if (!w) return;
    g_free(p->term_program);
    s = gtk_entry_get_text(GTK_ENTRY(w->term_prog));
    p->term_program = g_strdup(s ? s : "");
    p->term_max = (gint)gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->term_max));
    g_free(p->pi_program);
    s = gtk_entry_get_text(GTK_ENTRY(w->pi_prog));
    p->pi_program = g_strdup((s && *s) ? s : "pi");
    g_free(p->mcp_token);
    s = gtk_entry_get_text(GTK_ENTRY(w->mcp_token));
    p->mcp_token = g_strdup(s ? s : "");
    g_free(p->mcp_url_pattern);
    s = gtk_entry_get_text(GTK_ENTRY(w->mcp_pattern));
    p->mcp_url_pattern = g_strdup((s && *s) ? s : DEFAULT_URL_PATTERN);
}

static void on_setup_local(GtkButton *b, gpointer d)
{
    gchar *msg = NULL;
    (void)b; (void)d;
    if (gemm_pi_mcp_setup(&msg))
        msgwin_status_add("[geany-pi] %s", msg ? msg : "Pi MCP klar");
    else
        msgwin_status_add("[geany-pi] Pi MCP: %s", msg ? msg : "misslyckades");
    g_free(msg);
}

static void on_sync_docs(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gemm_pi_mcp_sync();
    msgwin_status_add("[geany-pi] synkar Pi MCP för öppna dokument...");
}

static void on_write_hosts_clicked(GtkButton *b, gpointer dlgp)
{
    (void)b;
    collect(GTK_DIALOG(dlgp), &g_p);
    gemm_prefs_save();
    write_mcp_hosts();
}

GtkWidget *gemm_prefs_configure_widget(GtkDialog *dlg)
{
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *lbl, *hdr;
    Widgets *w = g_new0(Widgets, 1);
    gchar *hpath = g_build_filename(g_get_user_config_dir(), "geany",
                                    "geany-pi", "mcp-hosts.json", NULL);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    /* --- Multiterm ---------------------------------------------------- */
    hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Multiterm</b> (terminaler per katalog)");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), hdr, 0, 0, 2, 1);

    lbl = gtk_label_new("Program");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 1, 1, 1);
    w->term_prog = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->term_prog),
                                   "tomt = $SHELL -l");
    gtk_entry_set_text(GTK_ENTRY(w->term_prog), g_p.term_program ? g_p.term_program : "");
    gtk_widget_set_hexpand(w->term_prog, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->term_prog, 1, 1, 1, 1);

    lbl = gtk_label_new("Max samtidiga");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 2, 1, 1);
    w->term_max = gtk_spin_button_new_with_range(1, 24, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->term_max), g_p.term_max);
    gtk_widget_set_halign(w->term_max, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), w->term_max, 1, 2, 1, 1);

    /* --- Pi-fliken ---------------------------------------------------- */
    hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Pi</b> (agentfliken)");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr, 8);
    gtk_grid_attach(GTK_GRID(grid), hdr, 0, 3, 2, 1);

    lbl = gtk_label_new("Program");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 4, 1, 1);
    w->pi_prog = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->pi_prog), g_p.pi_program ? g_p.pi_program : "pi");
    gtk_widget_set_hexpand(w->pi_prog, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->pi_prog, 1, 4, 1, 1);

    /* --- MCP / agentyta ---------------------------------------------- */
    hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>MCP</b> (ai_pi_mcp = Odoo-verktyg)");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr, 8);
    gtk_grid_attach(GTK_GRID(grid), hdr, 0, 5, 2, 1);

    lbl = gtk_label_new("Delad token");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 6, 1, 1);
    w->mcp_token = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(w->mcp_token), FALSE);
    gtk_entry_set_text(GTK_ENTRY(w->mcp_token), g_p.mcp_token ? g_p.mcp_token : "");
    gtk_widget_set_hexpand(w->mcp_token, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->mcp_token, 1, 6, 1, 1);

    lbl = gtk_label_new("URL-mönster");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 7, 1, 1);
    w->mcp_pattern = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->mcp_pattern),
                       g_p.mcp_url_pattern ? g_p.mcp_url_pattern : DEFAULT_URL_PATTERN);
    gtk_widget_set_hexpand(w->mcp_pattern, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->mcp_pattern, 1, 7, 1, 1);

    lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "<small>Skrivs till <tt>~/.config/geany/geany-pi/mcp-hosts.json</tt>\n"
        "(Salt kan också hantera den filen centralt)</small>");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 8, 2, 1);

    {
        GtkWidget *box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
        GtkWidget *b1 = gtk_button_new_with_label("Skriv mcp-hosts.json");
        GtkWidget *b2 = gtk_button_new_with_label("Sätt upp Pi MCP (denna maskin)");
        GtkWidget *b3 = gtk_button_new_with_label("Synka Pi MCP för öppna dokument");
        gtk_button_box_set_layout(GTK_BUTTON_BOX(box), GTK_BUTTONBOX_START);
        gtk_box_pack_start(GTK_BOX(box), b1, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), b2, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), b3, FALSE, FALSE, 0);
        g_signal_connect(b1, "clicked", G_CALLBACK(on_write_hosts_clicked), dlg);
        g_signal_connect(b2, "clicked", G_CALLBACK(on_setup_local), NULL);
        g_signal_connect(b3, "clicked", G_CALLBACK(on_sync_docs), NULL);
        gtk_widget_set_margin_top(box, 6);
        gtk_grid_attach(GTK_GRID(grid), box, 0, 9, 2, 1);
    }

    g_object_set_data(G_OBJECT(dlg), "gemm-prefs-widgets", w);
    g_free(hpath);
    (void)on_write_hosts;
    return grid;
}

/* Called by Geany when the preferences dialog is accepted. */
gboolean gemm_prefs_apply(GtkDialog *dlg)
{
    collect(dlg, &g_p);
    gemm_prefs_save();
    return TRUE;
}

/* Signal handler bound to Geany's dialog: save on OK.  Geany owns and
 * destroys the dialog, we only read the values. */
void gemm_prefs_on_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_OK || resp == GTK_RESPONSE_APPLY)
        gemm_prefs_apply(dlg);
}

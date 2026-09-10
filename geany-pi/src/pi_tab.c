/*
 * pi_tab.c - the "Pi" tab: one Pi coding-agent session in Geany.
 *
 * This is deliberately separate from the Terminal tab (terminal.c), which
 * holds plain per-directory $SHELL terminals. This tab runs the Pi agent
 * once, in a single session, started by the user (menu "Pi").
 *
 * Working directory: the active document's directory when that document is
 * local, otherwise $HOME. The tab does not follow documents afterwards.
 *
 * Requires libvte-2.91-dev at build time (HAVE_VTE).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#ifdef HAVE_VTE

#include <vte/vte.h>
#include <string.h>

static GtkWidget *g_pi_page = NULL;

/* directory to start in: local document dir, else $HOME */
static gchar *pi_start_dir(void)
{
    GeanyDocument *d = document_get_current();
    const gchar *path = (d && d->real_path) ? d->real_path
                       : (d ? d->file_name : NULL);

    if (path && !strstr(path, "/gvfs/"))
        return g_path_get_dirname(path);
    return g_strdup(g_get_home_dir());
}

static void pi_spawn(GtkWidget *term, const gchar *cwd)
{
    const gchar *prog = gemm_prefs_pi_program();
    gchar **argv;

    if (prog && *prog)
    {
        argv = g_new0(gchar*, 2);
        argv[0] = (gchar*)prog;
    }
    else
    {
        const gchar *sh = g_getenv("SHELL");
        if (!sh || !*sh)
            sh = "/bin/bash";
        argv = g_new0(gchar*, 3);
        argv[0] = (gchar*)sh;
        argv[1] = (gchar*)"-l";
        msgwin_status_add("[geany-pi] tomt Pi-program - startar %s", sh);
    }

    vte_terminal_spawn_async(VTE_TERMINAL(term), VTE_PTY_DEFAULT,
                             (cwd && *cwd) ? cwd : NULL,
                             argv, NULL, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL, -1, NULL, NULL, NULL);
    g_free(argv);
}

void gemm_pi_tab_show(void)
{
    GtkWidget *nb, *sw, *term;
    gchar *cwd;

    if (!(geany && geany->main_widgets))
        return;

    nb = geany->main_widgets->message_window_notebook;

    if (g_pi_page)
    {
        gint n = gtk_notebook_page_num(GTK_NOTEBOOK(nb), g_pi_page);
        if (n >= 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), n);
        return;
    }

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    term = vte_terminal_new();
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(term), 10000);
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(term), TRUE);
    vte_terminal_set_mouse_autohide(VTE_TERMINAL(term), TRUE);
    if (geany->interface_prefs && geany->interface_prefs->msgwin_font)
    {
        PangoFontDescription *fd = pango_font_description_from_string(
            geany->interface_prefs->msgwin_font);
        vte_terminal_set_font(VTE_TERMINAL(term), fd);
        pango_font_description_free(fd);
    }
    gtk_container_add(GTK_CONTAINER(sw), term);

    cwd = pi_start_dir();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new("Pi"));
    gtk_widget_show_all(sw);          /* page created after show_all */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb),
        gtk_notebook_page_num(GTK_NOTEBOOK(nb), sw));
    gtk_widget_grab_focus(term);
    g_pi_page = sw;

    pi_spawn(term, cwd);
    gemm_logf("pi: tab skapad (cwd=%s)", cwd ? cwd : "?");
    g_free(cwd);
}

void gemm_pi_tab_shutdown(void)
{
    if (g_pi_page)
    {
        gtk_widget_destroy(g_pi_page);
        g_pi_page = NULL;
    }
    gemm_logf("pi: shutdown");
}

#else  /* !HAVE_VTE */

void gemm_pi_tab_show(void)
{
    msgwin_status_add("[geany-pi] inbäddad terminal saknas - installera "
                      "libvte-2.91-dev och bygg om");
}

void gemm_pi_tab_shutdown(void)
{
}

#endif /* HAVE_VTE */

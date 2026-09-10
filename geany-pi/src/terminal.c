/*
 * terminal.c - embedded terminal tab with one session per document directory.
 *
 * The "Terminal" tab in Geany's message window holds an inner notebook with
 * one terminal per *directory*:
 *
 *   - local documents   -> login shell with cwd = the document's directory
 *   - gvfs/sftp documents -> ssh -t <host> "cd <dir> && exec $SHELL -l"
 *
 * Sessions follow the active document (document-activate) and are closed when
 * the last document using that directory is closed (document-close). At most
 * term_max() sessions are kept; the least recently used one is closed when
 * a new directory needs a terminal.
 *
 * Requires libvte-2.91-dev at build time (Makefile sets HAVE_VTE).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#ifdef HAVE_VTE

#include <vte/vte.h>
#include <string.h>

#define GEMM_LABEL_MAX 42

typedef struct
{
    gchar     *key;      /* unique: "user@host:port:dir" or "local:dir" */
    gchar     *label;    /* tab label: "host:dir" (or the dir for local) */
    gchar     *tooltip;  /* full location */
    GtkWidget *page;     /* notebook page (scrolled window) */
    GtkWidget *term;     /* VteTerminal */
    guint64    used;     /* LRU stamp */
} TermSession;

static gint term_max(void);   /* forward: used by evict_if_needed */

static GtkWidget  *g_outer_page = NULL;
static GtkWidget  *g_nb         = NULL;
static GHashTable *g_sessions   = NULL;   /* key -> TermSession* */
static guint64     g_tick       = 0;
static gboolean    g_ready      = FALSE;

/* ------------------------------------------------------------------ */
/* document location                                                  */
/* ------------------------------------------------------------------ */
typedef struct
{
    gboolean remote;
    gchar   *scheme;     /* sftp, ftp, smb, dav ... */
    gchar   *host;
    gchar   *user;
    gchar   *port;
    gchar   *dir;        /* remote path (dir of the file) */
    gchar   *local_dir;
} DocLoc;

static void docloc_free(DocLoc *l)
{
    if (!l) return;
    g_free(l->scheme); g_free(l->host); g_free(l->user);
    g_free(l->port);   g_free(l->dir);  g_free(l->local_dir);
    g_free(l);
}

/* Parse a Geany path.  gvfs/sftp looks like:
 *   /run/user/1000/gvfs/sftp:host=192.168.11.145/usr/share/x/y.py
 *   /run/user/1000/gvfs/sftp:host=h,user=u,port=2222/usr/share/x/y.py
 * Anything else is treated as a local path.
 */
static DocLoc *docloc_from_doc(GeanyDocument *doc)
{
    DocLoc *l = g_new0(DocLoc, 1);
    const gchar *path = (doc && doc->real_path) ? doc->real_path
                       : (doc ? doc->file_name : NULL);
    const gchar *g;

    if (!path)
    {
        l->remote = FALSE;
        l->local_dir = g_strdup(g_get_home_dir());
        return l;
    }

    g = strstr(path, "/gvfs/");
    if (g)
    {
        const gchar *p = g + strlen("/gvfs/");
        const gchar *c = strchr(p, ':');

        if (c && g_str_has_prefix(c + 1, "host="))
        {
            const gchar *end;

            l->remote = TRUE;
            l->scheme = g_strndup(p, (gsize)(c - p));
            end = c + 6;                       /* after "host=" */
            {
                const gchar *hs = end;
                while (*end && *end != ',' && *end != '/')
                    end++;
                l->host = g_strndup(hs, (gsize)(end - hs));
            }
            while (*end == ',')
            {
                const gchar *ks = ++end;
                gsize klen;
                gchar *value = NULL;

                while (*end && *end != '=' && *end != ',' && *end != '/')
                    end++;
                klen = (gsize)(end - ks);
                if (*end == '=')
                {
                    const gchar *vs = ++end;
                    while (*end && *end != ',' && *end != '/')
                        end++;
                    value = g_strndup(vs, (gsize)(end - vs));
                }
                if (klen == 4 && !strncmp(ks, "user", 4))
                {
                    g_free(l->user); l->user = value;
                }
                else if (klen == 4 && !strncmp(ks, "port", 4))
                {
                    g_free(l->port); l->port = value;
                }
                else
                    g_free(value);
            }
            {
                gchar *full = g_strdup(*end ? end : "/");
                l->dir = g_path_get_dirname(full);
                g_free(full);
            }
            return l;
        }
    }

    l->remote = FALSE;
    l->local_dir = g_path_get_dirname(path);
    return l;
}

static gchar *loc_key(const DocLoc *l)
{
    if (!l->remote)
        return g_strdup_printf("local:%s", l->local_dir);
    return g_strdup_printf("%s@%s:%s:%s",
                           l->user ? l->user : "",
                           l->host ? l->host : "",
                           l->port ? l->port : "",
                           l->dir ? l->dir : "/");
}

/* host:dir, shortened in the middle if long */
static gchar *shorten(const gchar *s)
{
    gsize n = s ? strlen(s) : 0;
    if (n <= GEMM_LABEL_MAX)
        return g_strdup(s ? s : "");
    {
        gchar *out = g_malloc(GEMM_LABEL_MAX + 4);
        g_snprintf(out, GEMM_LABEL_MAX + 4, "…%s", s + (n - GEMM_LABEL_MAX));
        return out;
    }
}

static gchar *loc_label(const DocLoc *l)
{
    /* host:dir — host kept intact, only the directory is shortened */
    gchar *d = shorten(l->remote ? (l->dir ? l->dir : "/")
                                 : (l->local_dir ? l->local_dir : "/"));
    gchar *out;
    if (!l->remote)
        return d;
    out = g_strdup_printf("%s:%s", l->host ? l->host : "?", d);
    g_free(d);
    return out;
}

static gchar *loc_tooltip(const DocLoc *l)
{
    if (!l->remote)
        return g_strdup_printf("%s (lokal)", l->local_dir);
    return g_strdup_printf("%s://%s%s%s%s",
                           l->scheme ? l->scheme : "sftp",
                           l->user ? l->user : "",
                           l->user ? "@" : "",
                           l->host ? l->host : "",
                           l->dir ? l->dir : "/");
}

/* ------------------------------------------------------------------ */
/* sessions                                                           */
/* ------------------------------------------------------------------ */
static void session_free(gpointer data)
{
    TermSession *s = data;
    if (!s) return;
    if (s->page)
        gtk_widget_destroy(s->page);   /* VTE finalises -> child gets SIGHUP */
    g_free(s->key);
    g_free(s->label);
    g_free(s->tooltip);
    g_free(s);
}

/* keys used by currently open documents */
static GHashTable *open_keys(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint i;

    if (geany && geany->documents_array)
        for (i = 0; i < geany->documents_array->len; i++)
        {
            GeanyDocument *d = g_ptr_array_index(geany->documents_array, i);
            if (!d) continue;
            {
                DocLoc *l = docloc_from_doc(d);
                gchar *k = loc_key(l);
                g_hash_table_add(set, k);
                docloc_free(l);
            }
        }
    return set;
}

static void evict_if_needed(void)
{
    GHashTable *open;

    if (g_hash_table_size(g_sessions) < term_max())
        return;

    open = open_keys();
    while (g_hash_table_size(g_sessions) >= term_max())
    {
        GHashTableIter it;
        gpointer k, v;
        TermSession *victim = NULL;
        gchar *victim_key = NULL;

        g_hash_table_iter_init(&it, g_sessions);
        while (g_hash_table_iter_next(&it, &k, &v))
        {
            TermSession *s = v;
            if (g_hash_table_contains(open, s->key))
                continue;                     /* a document still uses it */
            if (!victim || s->used < victim->used)
                victim = s;
        }
        if (!victim)                          /* all in use -> LRU overall */
        {
            g_hash_table_iter_init(&it, g_sessions);
            while (g_hash_table_iter_next(&it, &k, &v))
            {
                TermSession *s = v;
                if (!victim || s->used < victim->used)
                    victim = s;
            }
        }
        if (!victim)
            break;
        victim_key = g_strdup(victim->key);
        msgwin_status_add("[geany-pi] stänger terminal (max %d): %s",
                          term_max(), victim->label);
        gemm_logf("terminal: evict %s", victim_key);
        g_hash_table_remove(g_sessions, victim_key);
        g_free(victim_key);
    }
    g_hash_table_destroy(open);
}

/* The terminal runs the user's login shell.  The terminal is a plain
 * terminal - it is deliberately independent of Pi and any agent tooling.
 * $GEANY_LLM_TERMINAL overrides the program (run without -l). */
static gint term_max(void)
{
    gint m = gemm_prefs_term_max();
    return (m >= 1 && m <= 24) ? m : 6;
}

static void term_program(gchar **prog_out, gboolean *login_out)
{
    const gchar *cfg = gemm_prefs_term_program();
    const gchar *env = g_getenv("GEANY_LLM_TERMINAL");
    const gchar *sh  = g_getenv("SHELL");

    if (!sh || !*sh)
        sh = "/bin/bash";
    if (cfg && *cfg)
    {
        *prog_out  = g_strdup(cfg);
        *login_out = FALSE;
    }
    else if (env && *env)
    {
        *prog_out  = g_strdup(env);
        *login_out = FALSE;
    }
    else
    {
        *prog_out  = g_strdup(sh);
        *login_out = TRUE;
    }
}

static void spawn_session(TermSession *s, const DocLoc *l)
{
    gchar   *prog = NULL;
    gboolean login = FALSE;

    term_program(&prog, &login);

    if (!l->remote)
    {
        gchar *argv[3];
        argv[0] = prog;
        argv[1] = login ? (gchar*)"-l" : NULL;
        argv[2] = NULL;
        vte_terminal_spawn_async(VTE_TERMINAL(s->term), VTE_PTY_DEFAULT,
                                 l->local_dir, argv, NULL, G_SPAWN_SEARCH_PATH,
                                 NULL, NULL, NULL, -1, NULL, NULL, NULL);
        g_free(prog);
        return;
    }

    {
        GPtrArray *a = g_ptr_array_new();
        gchar *qdir = g_shell_quote(l->dir ? l->dir : "/");
        gchar *qprog = login ? g_strdup("\"${SHELL:-/bin/bash}\" -l")
                             : g_shell_quote(prog);
        gchar *cmd  = g_strdup_printf(
            "cd %s 2>/dev/null || { echo 'geany-pi: katalogen finns inte'; "
            "cd \"$HOME\"; }; exec %s",
            qdir, qprog);
        gchar *target;

        g_ptr_array_add(a, (gpointer)"ssh");
        g_ptr_array_add(a, (gpointer)"-t");
        if (l->port && *l->port)
        {
            g_ptr_array_add(a, (gpointer)"-p");
            g_ptr_array_add(a, (gpointer)l->port);
        }
        target = l->user && *l->user
               ? g_strdup_printf("%s@%s", l->user, l->host)
               : g_strdup(l->host);
        g_ptr_array_add(a, target);
        g_ptr_array_add(a, cmd);
        g_ptr_array_add(a, NULL);

        vte_terminal_spawn_async(VTE_TERMINAL(s->term), VTE_PTY_DEFAULT,
                                 NULL, (char**)a->pdata, NULL,
                                 G_SPAWN_SEARCH_PATH,
                                 NULL, NULL, NULL, -1, NULL, NULL, NULL);
        g_free(qdir);
        g_free(qprog);
        g_free(cmd);
        g_free(target);
        g_free(prog);
        g_ptr_array_free(a, TRUE);
    }
}

static void session_close_key(const gchar *key)
{
    if (g_sessions && g_hash_table_contains(g_sessions, key))
        g_hash_table_remove(g_sessions, key);
}

static gboolean close_key_idle(gpointer data)
{
    gchar *key = data;
    session_close_key(key);
    g_free(key);
    return G_SOURCE_REMOVE;
}

static void on_tab_close(GtkButton *btn, gpointer data)
{
    TermSession *s = data;
    (void)btn;
    if (s && s->key)
        g_idle_add(close_key_idle, g_strdup(s->key));
}

static TermSession *session_new(const DocLoc *l)
{
    TermSession *s = g_new0(TermSession, 1);
    GtkWidget *sw, *tabbox, *lbl, *btn;

    s->key     = loc_key(l);
    s->label   = loc_label(l);
    s->tooltip = loc_tooltip(l);
    s->used    = ++g_tick;

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    s->term = vte_terminal_new();
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(s->term), 10000);
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(s->term), TRUE);
    vte_terminal_set_mouse_autohide(VTE_TERMINAL(s->term), TRUE);
    if (geany && geany->interface_prefs && geany->interface_prefs->msgwin_font)
    {
        PangoFontDescription *fd = pango_font_description_from_string(
            geany->interface_prefs->msgwin_font);
        vte_terminal_set_font(VTE_TERMINAL(s->term), fd);
        pango_font_description_free(fd);
    }

    gtk_container_add(GTK_CONTAINER(sw), s->term);
    s->page = sw;

    /* tab: label + close button */
    tabbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    lbl = gtk_label_new(s->label);
    btn = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_tooltip_text(btn, "Stäng denna terminal");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_close), s);
    gtk_box_pack_start(GTK_BOX(tabbox), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tabbox), btn, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(tabbox, s->tooltip);
    gtk_widget_show_all(tabbox);

    gtk_notebook_append_page(GTK_NOTEBOOK(g_nb), s->page, tabbox);
    gtk_widget_show_all(s->page);   /* sidan skapas efter show_all(vbox) */
    g_hash_table_insert(g_sessions, g_strdup(s->key), s);

    spawn_session(s, l);
    gemm_logf("terminal: ny session %s (%s)", s->label, s->tooltip);
    return s;
}

static gboolean focus_idle(gpointer data)
{
    TermSession *s = data;
    if (!s || !s->term || !gtk_widget_get_realized(s->term))
        return G_SOURCE_REMOVE;
    gemm_logf("terminal: %s realized=%d", s->label,
              (int)gtk_widget_get_realized(s->term));
    if (gtk_notebook_get_current_page(GTK_NOTEBOOK(g_nb)) ==
        gtk_notebook_page_num(GTK_NOTEBOOK(g_nb), s->page))
        gtk_widget_grab_focus(s->term);
    return G_SOURCE_REMOVE;
}

static void session_activate(TermSession *s)
{
    gint n;

    s->used = ++g_tick;
    n = gtk_notebook_page_num(GTK_NOTEBOOK(g_nb), s->page);
    if (n >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_nb), n);
    g_idle_add(focus_idle, s);
}

/* Switch to (creating if needed) the terminal for a document's directory. */
static void activate_for_doc(GeanyDocument *doc)
{
    DocLoc *l;
    gchar *key;
    TermSession *s;

    if (!g_ready || !doc || !g_nb)
        return;

    l = docloc_from_doc(doc);
    key = loc_key(l);
    s = g_hash_table_lookup(g_sessions, key);

    if (!s)
    {
        if (g_hash_table_size(g_sessions) >= term_max())
            evict_if_needed();
        s = session_new(l);
    }
    g_free(key);
    docloc_free(l);

    if (s)
        session_activate(s);
    else
        msgwin_status_add("[geany-pi] kunde inte skapa terminal");
}

/* ------------------------------------------------------------------ */
/* Geany signals                                                      */
/* ------------------------------------------------------------------ */
static void on_document_activate(GObject *obj, GeanyDocument *doc, gpointer data)
{
    (void)obj; (void)data;
    activate_for_doc(doc);
}

static gboolean close_if_unused_idle(gpointer data)
{
    gchar *key = data;
    GHashTable *open = open_keys();
    if (!g_hash_table_contains(open, key))
        session_close_key(key);
    g_hash_table_destroy(open);
    g_free(key);
    return G_SOURCE_REMOVE;
}

static void on_document_close(GObject *obj, GeanyDocument *doc, gpointer data)
{
    DocLoc *l;
    (void)obj; (void)data;

    if (!g_ready || !doc)
        return;
    l = docloc_from_doc(doc);
    /* defer: the document is still in documents_array during the signal */
    g_idle_add(close_if_unused_idle, loc_key(l));
    docloc_free(l);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
void gemm_terminal_show(void)
{
    GtkWidget *nb, *vbox, *hint;

    if (!(geany && geany->main_widgets))
        return;

    if (g_outer_page)
    {
        nb = geany->main_widgets->message_window_notebook;
        {
            gint n = gtk_notebook_page_num(GTK_NOTEBOOK(nb), g_outer_page);
            if (n >= 0)
                gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), n);
        }
        activate_for_doc(document_get_current());
        return;
    }

    nb = geany->main_widgets->message_window_notebook;
    g_sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       session_free);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_nb = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_nb), TRUE);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(g_nb), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(vbox), g_nb, TRUE, TRUE, 0);

    hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hint),
        "<small>Multiterm: terminaler per katalog – följer aktivt dokument. "
        "Fjärrdokument (sftp) öppnas med ssh till samma maskin.</small>");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_margin_start(hint, 6);
    gtk_box_pack_end(GTK_BOX(vbox), hint, FALSE, FALSE, 2);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox, gtk_label_new("Multiterm"));
    gtk_widget_show_all(vbox);
    g_outer_page = vbox;
    g_ready = TRUE;

    /* follow the active document */
    if (geany_plugin)
    {
        plugin_signal_connect(geany_plugin, NULL, "document-activate", TRUE,
                              G_CALLBACK(on_document_activate), NULL);
        plugin_signal_connect(geany_plugin, NULL, "document-close", TRUE,
                              G_CALLBACK(on_document_close), NULL);
    }
    else
        gemm_logf("terminal: geany_plugin NULL - kan inte följa dokument");

    {
        gint n = gtk_notebook_page_num(GTK_NOTEBOOK(nb), g_outer_page);
        if (n >= 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), n);
    }
    activate_for_doc(document_get_current());
    gemm_logf("terminal: tab skapad");
}

/* Tear down everything this tab created (called from plugin_cleanup). */
void gemm_terminal_shutdown(void)
{
    if (g_sessions)
    {
        g_hash_table_destroy(g_sessions);   /* free func destroys each page */
        g_sessions = NULL;
    }
    if (g_outer_page)
    {
        gtk_widget_destroy(g_outer_page);   /* removes it from the notebook */
        g_outer_page = NULL;
    }
    g_nb = NULL;
    g_ready = FALSE;
    gemm_logf("terminal: shutdown");
}

#else  /* !HAVE_VTE */

void gemm_terminal_show(void)
{
    msgwin_status_add("[geany-pi] inbäddad terminal saknas - installera "
                      "libvte-2.91-dev och bygg om");
}

void gemm_terminal_shutdown(void)
{
}

#endif /* HAVE_VTE */

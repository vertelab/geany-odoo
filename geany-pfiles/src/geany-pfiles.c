/*
 * geany-pfiles.c - p-file support for Geany.
 *
 * Odoo modules here are written version-agnostically as *p-files*
 * (x.p.py, x.p.xml, ...) and expanded per edition with
 *     preprocess -D VERSION=<v> -o <out> <in>
 *
 * This plugin:
 *   1. keeps a comma separated list of editions in its settings
 *      (e.g. "17.0,18.0,19.0");
 *   2. offers one item per edition in a toolbar drop-down and generates one
 *      item per edition in Geany's Build menu (via geany.conf's NF_xx slots,
 *      because build_set_group_count()/build_menu_update() are private to
 *      Geany);
 *   3. runs the expansion through the `geany-pfiles` helper, which performs
 *      it **on the machine that owns the document** (ssh for gvfs/sftp
 *      documents - same target resolution as the Multiterm tab);
 *   4. can install the git configuration the p-files need (.gitattributes +
 *      pre-commit hook + core.hooksPath) in the document's repository, again
 *      on the owning machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pfiles.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

GeanyData   *geany_data;
GeanyPlugin *geany_plugin;

static gboolean    g_inited   = FALSE;
static gboolean    g_quitting = FALSE;
static GtkToolItem *g_tb_item = NULL;

/* settings */
static gchar   *g_editions       = NULL;   /* "17.0,18.0,19.0" */
static gchar   *g_preprocess_cmd = NULL;   /* "preprocess" */
static gchar   *g_defines        = NULL;   /* extra -D args */
static gchar   *g_force_args     = NULL;   /* "-f": overwrite the output */
static gchar   *g_hooks_path     = NULL;   /* ".githooks" */
static gboolean g_precommit      = TRUE;

/* ---------------------------------------------------------------------- */
/* logging                                                                */
/* ---------------------------------------------------------------------- */
void pfiles_logf(const gchar *fmt, ...)
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "geany",
                                   "geany-pfiles", "log.txt", NULL);
    gchar *dir = g_path_get_dirname(path);
    gchar *msg, *ts, *line;
    GDateTime *now;
    va_list ap;
    gint fd;

    g_mkdir_with_parents(dir, 0700);
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    now = g_date_time_new_now_local();
    ts = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0)
    {
        line = g_strdup_printf("%s %s\n", ts, msg);
        (void)write(fd, line, strlen(line));
        (void)close(fd);
        g_free(line);
    }
    g_date_time_unref(now);
    g_free(ts);
    g_free(msg);
    g_free(dir);
    g_free(path);
}

static void status(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);
static void status(const gchar *fmt, ...)
{
    gchar *msg;
    va_list ap;
    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    msgwin_status_add("[geany-pfiles] %s", msg);
    pfiles_logf("status: %s", msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------- */
/* settings                                                               */
/* ---------------------------------------------------------------------- */
static gchar *conf_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "geany", "geany-pfiles",
                            "geany-pfiles.conf", NULL);
}

static void prefs_load(void)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path = conf_path();
    gchar *s;

    g_free(g_editions); g_free(g_preprocess_cmd);
    g_free(g_defines);  g_free(g_hooks_path); g_free(g_force_args);
    g_editions = g_strdup("17.0,18.0,19.0");
    g_preprocess_cmd = g_strdup("preprocess");
    g_defines = g_strdup("");
    g_force_args = g_strdup("-f");
    g_hooks_path = g_strdup(".githooks");
    g_precommit = TRUE;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    {
        if ((s = g_key_file_get_string(kf, "editions", "list", NULL)))
        { g_free(g_editions); g_editions = s; }
        if ((s = g_key_file_get_string(kf, "editions", "preprocess_cmd", NULL)))
        { g_free(g_preprocess_cmd); g_preprocess_cmd = s; }
        if ((s = g_key_file_get_string(kf, "editions", "defines", NULL)))
        { g_free(g_defines); g_defines = s; }
        if (g_key_file_has_key(kf, "editions", "force_args", NULL))
        {
            if ((s = g_key_file_get_string(kf, "editions", "force_args", NULL)))
            { g_free(g_force_args); g_force_args = s; }
        }
        if ((s = g_key_file_get_string(kf, "git", "hooks_path", NULL)))
        { g_free(g_hooks_path); g_hooks_path = s; }
        if (g_key_file_has_key(kf, "git", "precommit", NULL))
            g_precommit = g_key_file_get_boolean(kf, "git", "precommit", NULL);
    }
    g_key_file_free(kf);
    g_free(path);
}

static void prefs_save(void)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path = conf_path();
    gchar *dir = g_path_get_dirname(path);
    gchar *data;
    gsize len = 0;

    g_key_file_set_string(kf, "editions", "list", g_editions ? g_editions : "");
    g_key_file_set_string(kf, "editions", "preprocess_cmd",
                          g_preprocess_cmd ? g_preprocess_cmd : "preprocess");
    g_key_file_set_string(kf, "editions", "defines", g_defines ? g_defines : "");
    g_key_file_set_string(kf, "editions", "force_args",
                          g_force_args ? g_force_args : "");
    g_key_file_set_string(kf, "git", "hooks_path",
                          g_hooks_path ? g_hooks_path : ".githooks");
    g_key_file_set_boolean(kf, "git", "precommit", g_precommit);

    g_mkdir_with_parents(dir, 0700);
    data = g_key_file_to_data(kf, &len, NULL);
    if (data)
    {
        (void)g_file_set_contents(path, data, (gssize)len, NULL);
        (void)g_chmod(path, 0600);
        g_free(data);
    }
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* cleaned list of editions; caller frees with g_strfreev */
static gchar **editions_get(guint *n_out)
{
    gchar **raw = g_strsplit(g_editions ? g_editions : "", ",", -1);
    GPtrArray *a = g_ptr_array_new();
    guint i;

    for (i = 0; raw[i]; i++)
    {
        gchar *e = g_strstrip(g_strdup(raw[i]));
        if (*e)
            g_ptr_array_add(a, e);
        else
            g_free(e);
    }
    g_strfreev(raw);
    g_ptr_array_add(a, NULL);
    if (n_out)
        *n_out = a->len - 1;
    return (gchar **)g_ptr_array_free(a, FALSE);
}

/* ---------------------------------------------------------------------- */
/* process spawning (Geany/LSP pattern)                                   */
/* ---------------------------------------------------------------------- */
typedef struct { gchar *tag; GIOChannel *out, *err; } SpawnCtx;

static void spawn_drain(GIOChannel *ch, const gchar *tag)
{
    gchar *line = NULL;
    gsize len = 0;
    GError *e = NULL;
    GIOStatus st;

    if (!ch)
        return;
    while ((st = g_io_channel_read_line(ch, &line, &len, NULL, &e)) ==
           G_IO_STATUS_NORMAL)
    {
        g_strchomp(line);
        if (*line)
        {
            pfiles_logf("%s: %s", tag, line);
            msgwin_msg_add(COLOR_BLACK, -1, NULL, "%s", line);
        }
        g_free(line);
        line = NULL;
    }
    g_free(line);
    if (e)
        g_error_free(e);
}

static gboolean spawn_read_cb(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    SpawnCtx *ctx = data;
    spawn_drain(ch, ctx->tag);
    return (cond & (G_IO_HUP | G_IO_ERR)) ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void spawn_exit_cb(GPid pid, gint st, gpointer data)
{
    SpawnCtx *ctx = data;
    spawn_drain(ctx->out, ctx->tag);
    spawn_drain(ctx->err, ctx->tag);
    if (ctx->out) { g_io_channel_unref(ctx->out); ctx->out = NULL; }
    if (ctx->err) { g_io_channel_unref(ctx->err); ctx->err = NULL; }
    g_spawn_close_pid(pid);
    status("%s klar (status %d)", ctx->tag, st);
    g_free(ctx->tag);
    g_free(ctx);
}

static gboolean run_capture(const gchar *tag, const gchar *const *argv)
{
    GPid pid = 0;
    gint out_fd = -1, err_fd = -1;
    GError *err = NULL;
    SpawnCtx *ctx;

    if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, NULL, &out_fd, &err_fd, &err))
    {
        status("%s: kunde inte starta: %s", tag, err ? err->message : "?");
        if (err) g_error_free(err);
        return FALSE;
    }
    ctx = g_new0(SpawnCtx, 1);
    ctx->tag = g_strdup(tag);
    ctx->out = g_io_channel_unix_new(out_fd);
    ctx->err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(ctx->out, NULL, NULL);
    g_io_channel_set_encoding(ctx->err, NULL, NULL);
    g_io_add_watch(ctx->out, G_IO_IN | G_IO_HUP | G_IO_ERR, spawn_read_cb, ctx);
    g_io_add_watch(ctx->err, G_IO_IN | G_IO_HUP | G_IO_ERR, spawn_read_cb, ctx);
    g_child_watch_add(pid, spawn_exit_cb, ctx);
    return TRUE;
}

static gchar *helper_path(void)
{
    const gchar *dirs[] = { "/usr/local/bin", NULL };
    gchar *homebin = g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
    gchar *found = NULL;
    guint i;

    dirs[1] = homebin;
    for (i = 0; i < 2 && !found; i++)
    {
        gchar *cand = g_build_filename(dirs[i], "geany-pfiles", NULL);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE))
            found = cand;
        else
            g_free(cand);
    }
    g_free(homebin);
    return found;
}

static gchar *active_file(void)
{
    GeanyDocument *d = document_get_current();
    if (d == NULL || d->real_path == NULL)
        return NULL;
    return g_strdup(d->real_path);
}

/* `geany-pfiles <subcmd> [version] <file>` */
static gboolean run_helper(const gchar *subcmd, const gchar *version)
{
    gchar *helper = helper_path();
    gchar *file;
    GPtrArray *argv;
    gchar *tag;
    gboolean ok;

    if (!helper)
    {
        status("hjälparen 'geany-pfiles' saknas (installera via Salt: "
               "workstation.geany)");
        return FALSE;
    }
    file = active_file();
    if (!file)
    {
        status("inget aktivt dokument");
        g_free(helper);
        return FALSE;
    }

    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(helper));
    g_ptr_array_add(argv, g_strdup(subcmd));
    if (version)
        g_ptr_array_add(argv, g_strdup(version));
    g_ptr_array_add(argv, file);
    g_ptr_array_add(argv, NULL);

    tag = version ? g_strdup_printf("%s %s", subcmd, version)
                  : g_strdup(subcmd);
    ok = run_capture(tag, (const gchar *const *)argv->pdata);
    g_free(tag);
    g_ptr_array_free(argv, TRUE);
    g_free(helper);
    return ok;
}

static void on_edition_item(GtkWidget *w, gpointer data)
{
    const gchar *version = g_object_get_data(G_OBJECT(w), "version");
    (void)data;
    run_helper("preprocess", version);
}

static void on_git_config(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    run_helper("git-config", NULL);
}

/* ---------------------------------------------------------------------- */
/* Build menu: generate geany.conf's NF_xx slots + patch live slots        */
/* ---------------------------------------------------------------------- */
static gboolean key_is_ours(GKeyFile *kf, const gchar *key)
{
    gchar *v = NULL;
    gboolean ours = FALSE;

    if (!g_str_has_suffix(key, "_LB"))
        return FALSE;
    v = g_key_file_get_string(kf, "geany", key, NULL);
    if (v && (g_str_has_prefix(v, "P-files ") ||
              g_str_has_prefix(v, "Preprocess ")))
        ours = TRUE;
    g_free(v);
    return ours;
}

static void update_build_menu(void)
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "geany",
                                   "geany.conf", NULL);
    GKeyFile *kf = g_key_file_new();
    gchar **editions = editions_get(NULL);
    guint n = g_strv_length(editions);
    guint live;
    guint i;
    gboolean changed_file = FALSE;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL))
    {
        gsize klen = 0;
        gchar **keys = g_key_file_get_keys(kf, "geany", &klen, NULL);
        /* remove our own (and the legacy hand-made 'Preprocess <v>') slots */
        for (i = 0; keys && i < klen; i++)
            if (key_is_ours(kf, keys[i]))
            {
                gchar *base = g_strndup(keys[i], strlen(keys[i]) - 3);
                gchar *k;
                k = g_strdup_printf("%s_LB", base); g_key_file_remove_key(kf, "geany", k, NULL); g_free(k);
                k = g_strdup_printf("%s_CM", base); g_key_file_remove_key(kf, "geany", k, NULL); g_free(k);
                k = g_strdup_printf("%s_WD", base); g_key_file_remove_key(kf, "geany", k, NULL); g_free(k);
                g_free(base);
                changed_file = TRUE;
            }
        if (keys) g_strfreev(keys);
    }
    else
        changed_file = TRUE;

    for (i = 0; i < n; i++)
    {
        gchar *k  = g_strdup_printf("NF_%02u_LB", i);
        gchar *kv = g_strdup_printf("P-files %s", editions[i]);
        g_key_file_set_string(kf, "geany", k, kv);
        g_free(k);
        g_free(kv);
        k  = g_strdup_printf("NF_%02u_CM", i);
        kv = g_strdup_printf("geany-pfiles preprocess %s %%f", editions[i]);
        g_key_file_set_string(kf, "geany", k, kv);
        g_free(k);
        g_free(kv);
        k  = g_strdup_printf("NF_%02u_WD", i);
        g_key_file_set_string(kf, "geany", k, "");
        g_free(k);
        changed_file = TRUE;
    }

    if (changed_file)
    {
        gchar *data = g_key_file_to_data(kf, NULL, NULL);
        if (data)
        {
            (void)g_file_set_contents(path, data, -1, NULL);
            g_free(data);
            pfiles_logf("build-menu: skrev %u NF-poster till %s", n, path);
        }
    }
    g_key_file_free(kf);
    g_free(path);

    /* patch the slots that already exist in this session */
    live = build_get_group_count(GEANY_GBG_NON_FT);
    for (i = 0; i < n && i < live; i++)
    {
        gchar *label = g_strdup_printf("P-files %s", editions[i]);
        gchar *cmd   = g_strdup_printf("geany-pfiles preprocess %s %%f",
                                       editions[i]);
        build_set_menu_item(GEANY_BCS_PREF, GEANY_GBG_NON_FT, i,
                            GEANY_BC_LABEL, label);
        build_set_menu_item(GEANY_BCS_PREF, GEANY_GBG_NON_FT, i,
                            GEANY_BC_COMMAND, cmd);
        build_set_menu_item(GEANY_BCS_PREF, GEANY_GBG_NON_FT, i,
                            GEANY_BC_WORKING_DIR, "");
        g_free(label);
        g_free(cmd);
    }
    for (i = n; i < live; i++)
        build_remove_menu_item(GEANY_BCS_PREF, GEANY_GBG_NON_FT, i);

    pfiles_logf("build-menu: %u utgåvor, %u live-platser", n, live);
    if (n > live)
        status("Bygg-menyn har %u platser men %u utgåvor är konfigurerade - "
               "starta om Geany så syns alla", live, n);
    g_strfreev(editions);
}

/* ---------------------------------------------------------------------- */
/* toolbar                                                                */
/* ---------------------------------------------------------------------- */
static void toolbar_refresh(void)
{
    GtkWidget *menu;
    gchar **editions = editions_get(NULL);
    GtkWidget *item;
    guint i;

    if (!g_tb_item)
        return;
    menu = gtk_menu_new();

    for (i = 0; editions[i]; i++)
    {
        gchar *label = g_strdup_printf("Expandera för %s", editions[i]);
        item = gtk_menu_item_new_with_label(label);
        g_object_set_data_full(G_OBJECT(item), "version",
                               g_strdup(editions[i]), g_free);
        g_signal_connect(item, "activate", G_CALLBACK(on_edition_item), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        g_free(label);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label("Git-konfiguration för p-filer…");
    g_signal_connect(item, "activate", G_CALLBACK(on_git_config), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(g_tb_item), menu);
    status("menyn uppdaterad (%u utgåvor)", g_strv_length(editions));
    g_strfreev(editions);
}

/* ---------------------------------------------------------------------- */
/* preferences dialog                                                     */
/* ---------------------------------------------------------------------- */
typedef struct { GtkWidget *editions, *cmd, *defines, *force, *hooks, *precommit; }
PrefsW;

static void on_refresh_menu(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    update_build_menu();
    toolbar_refresh();
}

static void on_send_git(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    run_helper("git-config", NULL);
}

static void on_configure_response(GtkDialog *dlg, gint resp, gpointer data);

GtkWidget *plugin_configure(GtkDialog *dialog)
{
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *lbl, *hdr;
    PrefsW *w = g_new0(PrefsW, 1);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Utgåvor</b> (kommaseparerad lista)");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), hdr, 0, 0, 2, 1);

    lbl = gtk_label_new("Utgåvor");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 1, 1, 1);
    w->editions = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->editions), "17.0,18.0,19.0");
    gtk_entry_set_text(GTK_ENTRY(w->editions), g_editions ? g_editions : "");
    gtk_widget_set_hexpand(w->editions, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->editions, 1, 1, 1, 1);

    lbl = gtk_label_new("Preprocess-kommando");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 2, 1, 1);
    w->cmd = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->cmd), g_preprocess_cmd ? g_preprocess_cmd : "");
    gtk_widget_set_hexpand(w->cmd, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->cmd, 1, 2, 1, 1);

    lbl = gtk_label_new("Extra -D-argument");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 3, 1, 1);
    w->defines = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->defines), "-D REPO=min_modul");
    gtk_entry_set_text(GTK_ENTRY(w->defines), g_defines ? g_defines : "");
    gtk_widget_set_hexpand(w->defines, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->defines, 1, 3, 1, 1);

    lbl = gtk_label_new("Extra flaggor (force)");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 4, 1, 1);
    w->force = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->force), "-f");
    gtk_entry_set_text(GTK_ENTRY(w->force), g_force_args ? g_force_args : "");
    gtk_widget_set_hexpand(w->force, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->force, 1, 4, 1, 1);

    hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Git</b> (för p-filerna)");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr, 8);
    gtk_grid_attach(GTK_GRID(grid), hdr, 0, 5, 2, 1);

    lbl = gtk_label_new("hooksPath");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 6, 1, 1);
    w->hooks = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->hooks), g_hooks_path ? g_hooks_path : "");
    gtk_widget_set_hexpand(w->hooks, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->hooks, 1, 6, 1, 1);

    w->precommit = gtk_check_button_new_with_mnemonic(
        "Installera pre-commit-hook (expanderar staged p-filer per gren)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->precommit), g_precommit);
    gtk_grid_attach(GTK_GRID(grid), w->precommit, 0, 7, 2, 1);

    {
        GtkWidget *box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
        GtkWidget *b1 = gtk_button_new_with_label("Uppdatera Bygg-menyn nu");
        GtkWidget *b2 = gtk_button_new_with_label("Skicka ut git-konfiguration");
        gtk_box_pack_start(GTK_BOX(box), b1, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), b2, FALSE, FALSE, 0);
        g_signal_connect(b1, "clicked", G_CALLBACK(on_refresh_menu), NULL);
        g_signal_connect(b2, "clicked", G_CALLBACK(on_send_git), NULL);
        gtk_widget_set_margin_top(box, 8);
        gtk_grid_attach(GTK_GRID(grid), box, 0, 8, 2, 1);
    }

    g_object_set_data(G_OBJECT(dialog), "pfiles-w", w);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_configure_response), NULL);
    return grid;
}

static void prefs_collect(GtkDialog *dlg)
{
    PrefsW *w = g_object_get_data(G_OBJECT(dlg), "pfiles-w");
    const gchar *s;

    if (!w)
        return;
    g_free(g_editions);
    s = gtk_entry_get_text(GTK_ENTRY(w->editions));
    g_editions = g_strdup(s ? s : "");
    g_free(g_preprocess_cmd);
    s = gtk_entry_get_text(GTK_ENTRY(w->cmd));
    g_preprocess_cmd = g_strdup((s && *s) ? s : "preprocess");
    g_free(g_defines);
    s = gtk_entry_get_text(GTK_ENTRY(w->defines));
    g_defines = g_strdup(s ? s : "");
    g_free(g_force_args);
    s = gtk_entry_get_text(GTK_ENTRY(w->force));
    g_force_args = g_strdup(s ? s : "");
    g_free(g_hooks_path);
    s = gtk_entry_get_text(GTK_ENTRY(w->hooks));
    g_hooks_path = g_strdup((s && *s) ? s : ".githooks");
    g_precommit = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->precommit));
}

static void on_configure_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp != GTK_RESPONSE_OK && resp != GTK_RESPONSE_APPLY)
        return;
    prefs_collect(dlg);
    prefs_save();
    update_build_menu();
    toolbar_refresh();
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                              */
/* ---------------------------------------------------------------------- */
static void on_before_quit(GObject *o, gpointer d)
{
    (void)o; (void)d;
    g_quitting = TRUE;
}

static void build_toolbar(void)
{
    GtkWidget *menu;

    g_tb_item = gtk_menu_tool_button_new(NULL, "P-files");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g_tb_item),
        "P-filer: expandera det aktiva dokumentet per Odoo-utgåva "
        "(körs på dokumentets maskin)");
    menu = gtk_menu_new();
    gtk_menu_tool_button_set_menu(GTK_MENU_TOOL_BUTTON(g_tb_item), menu);
    plugin_add_toolbar_item(geany_plugin, g_tb_item);
}

void plugin_init(GeanyData *data)
{
    if (g_inited)
    {
        pfiles_logf("init: redan initierad - hoppar över");
        return;
    }
    g_inited = TRUE;
    geany_data = data;

    if (geany_plugin)
        plugin_module_make_resident(geany_plugin);
    plugin_signal_connect(geany_plugin, NULL, "geany-before-quit", TRUE,
                          G_CALLBACK(on_before_quit), NULL);

    prefs_load();
    pfiles_logf("init: utgåvor=%s cmd=%s slots: non-ft=%u ft=%u exec=%u",
                g_editions, g_preprocess_cmd,
                build_get_group_count(GEANY_GBG_NON_FT),
                build_get_group_count(GEANY_GBG_FT),
                build_get_group_count(GEANY_GBG_EXEC));

    build_toolbar();
    update_build_menu();
    toolbar_refresh();
    pfiles_logf("init: klar");
}

void plugin_cleanup(void)
{
    if (g_tb_item)
    {
        gtk_widget_destroy(GTK_WIDGET(g_tb_item));
        g_tb_item = NULL;
    }
    prefs_save();
    pfiles_logf("cleanup: klar (quitting=%d)", (int)g_quitting);
    g_inited = FALSE;
}

PLUGIN_VERSION_CHECK(224)
PLUGIN_SET_INFO("geany-pfiles",
                "P-file (preprocess) support: per-edition build items, "
                "expansion on the document's machine, git setup for p-files",
                "0.1.0",
                "Vertelab")

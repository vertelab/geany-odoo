/*
 * geany-mermaid.c - Mermaid diagram preview for Geany.
 *
 * Finds the ```mermaid (and @startmermaid / @endmermaid) blocks in the active
 * document, renders each one with mmdc (mermaid-cli) and shows the resulting
 * PNGs in a plugin window.  PNG + GdkPixbuf is deliberate: librsvg is not a
 * Geany dependency and is not assumed to be installed.
 *
 * The document text is already local (it lives in Geany's buffer), so the
 * render runs locally; mmdc comes from the workstation.md2pdf Salt state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-mermaid.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

GeanyData   *geany_data;
GeanyPlugin *geany_plugin;

static gboolean g_inited   = FALSE;
static gboolean g_quitting = FALSE;

/* settings */
static gchar   *g_mmdc_cmd   = NULL;
static gchar   *g_puppeteer  = NULL;
static gchar   *g_theme      = NULL;
static gchar   *g_background = NULL;
static gint     g_width      = 1400;
static gint     g_scale      = 2;
static gboolean g_on_save    = FALSE;
static gboolean g_on_activate = FALSE;

/* ui */
static GtkToolItem *g_tb_item = NULL;
static GtkWidget *g_win      = NULL;   /* GtkWindow */
static GtkWidget *g_images   = NULL;   /* GtkBox holding GtkImage:s */
static GtkWidget *g_status   = NULL;   /* GtkLabel */
static GtkWidget *g_zoom_lbl = NULL;
static gdouble    g_zoom     = 1.0;

/* jobs: parallel arrays, index == diagram number */
static GPtrArray *g_sources  = NULL;   /* gchar*: mermaid source */
static GPtrArray *g_pngs     = NULL;   /* gchar*: rendered png path */
static guint       g_job_i    = 0;
static gboolean    g_rendering = FALSE;
static gchar      *g_tmpdir   = NULL;

/* ---------------------------------------------------------------------- */
/* logging / status                                                       */
/* ---------------------------------------------------------------------- */
void mmd_logf(const gchar *fmt, ...)
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "geany",
                                   "geany-mermaid", "log.txt", NULL);
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
    msgwin_status_add("[mermaid] %s", msg);
    mmd_logf("status: %s", msg);
    if (g_status)
        gtk_label_set_text(GTK_LABEL(g_status), msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------- */
/* settings                                                               */
/* ---------------------------------------------------------------------- */
static gchar *conf_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "geany", "geany-mermaid",
                            "geany-mermaid.conf", NULL);
}

static void prefs_load(void)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path = conf_path();
    gchar *s;

    g_free(g_mmdc_cmd); g_free(g_puppeteer);
    g_free(g_theme);    g_free(g_background);
    g_mmdc_cmd   = g_strdup("mmdc");
    g_puppeteer  = g_strdup("/etc/md2pdf/puppeteer-config.json");
    g_theme      = g_strdup("");
    g_background = g_strdup("");
    g_width = 1400; g_scale = 2; g_on_save = FALSE; g_on_activate = FALSE;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
    {
        if ((s = g_key_file_get_string(kf, "mmdc", "command", NULL)))
        { g_free(g_mmdc_cmd); g_mmdc_cmd = s; }
        if ((s = g_key_file_get_string(kf, "mmdc", "puppeteer_config", NULL)))
        { g_free(g_puppeteer); g_puppeteer = s; }
        if ((s = g_key_file_get_string(kf, "render", "theme", NULL)))
        { g_free(g_theme); g_theme = s; }
        if ((s = g_key_file_get_string(kf, "render", "background", NULL)))
        { g_free(g_background); g_background = s; }
        if (g_key_file_has_key(kf, "render", "width", NULL))
            g_width = g_key_file_get_integer(kf, "render", "width", NULL);
        if (g_key_file_has_key(kf, "render", "scale", NULL))
            g_scale = g_key_file_get_integer(kf, "render", "scale", NULL);
        if (g_key_file_has_key(kf, "render", "on_save", NULL))
            g_on_save = g_key_file_get_boolean(kf, "render", "on_save", NULL);
        if (g_key_file_has_key(kf, "render", "on_activate", NULL))
            g_on_activate = g_key_file_get_boolean(kf, "render", "on_activate", NULL);
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

    g_key_file_set_string(kf, "mmdc", "command",
                          (g_mmdc_cmd && *g_mmdc_cmd) ? g_mmdc_cmd : "mmdc");
    g_key_file_set_string(kf, "mmdc", "puppeteer_config",
                          g_puppeteer ? g_puppeteer : "");
    g_key_file_set_string(kf, "render", "theme", g_theme ? g_theme : "");
    g_key_file_set_string(kf, "render", "background",
                          g_background ? g_background : "");
    g_key_file_set_integer(kf, "render", "width", g_width);
    g_key_file_set_integer(kf, "render", "scale", g_scale);
    g_key_file_set_boolean(kf, "render", "on_save", g_on_save);
    g_key_file_set_boolean(kf, "render", "on_activate", g_on_activate);

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

/* ---------------------------------------------------------------------- */
/* process spawning (Geany/LSP pattern - never g_spawn_sync here)          */
/* ---------------------------------------------------------------------- */
typedef struct { gchar *tag; GIOChannel *out, *err; } SpawnCtx;

static void spawn_drain(GIOChannel *ch, const gchar *tag)
{
    gchar *line = NULL;
    GError *e = NULL;
    GIOStatus st;

    if (!ch)
        return;
    while ((st = g_io_channel_read_line(ch, &line, NULL, NULL, &e)) ==
           G_IO_STATUS_NORMAL)
    {
        g_strchomp(line);
        if (*line)
            mmd_logf("%s: %s", tag, line);
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

static void render_next(void);

static void spawn_exit_cb(GPid pid, gint st, gpointer data)
{
    SpawnCtx *ctx = data;
    spawn_drain(ctx->out, ctx->tag);
    spawn_drain(ctx->err, ctx->tag);
    if (ctx->out) { g_io_channel_unref(ctx->out); ctx->out = NULL; }
    if (ctx->err) { g_io_channel_unref(ctx->err); ctx->err = NULL; }
    g_spawn_close_pid(pid);
    if (st != 0)
        status("%s misslyckades (status %d) - se ~/.config/geany/"
               "geany-mermaid/log.txt", ctx->tag, st);
    g_free(ctx->tag);
    g_free(ctx);
    render_next();
}

static gboolean spawn_mmdc(const gchar *tag, GPtrArray *argv)
{
    GPid pid = 0;
    gint out_fd = -1, err_fd = -1;
    GError *err = NULL;
    SpawnCtx *ctx;

    if (!g_spawn_async_with_pipes(NULL, (gchar **)argv->pdata, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, NULL, &out_fd, &err_fd, &err))
    {
        status("%s: kunde inte starta: %s", tag, err ? err->message : "?");
        if (err)
        {
            mmd_logf("%s: %s", tag, err->message);
            g_error_free(err);
        }
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

/* ---------------------------------------------------------------------- */
/* block extraction                                                       */
/* ---------------------------------------------------------------------- */
/* a fence line: at least three ` or ~, optionally followed by info text */
static gboolean is_fence(const gchar *line, const gchar **info)
{
    const gchar *p = line;
    gchar fence;
    guint n = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '`' && *p != '~')
        return FALSE;
    fence = *p;
    while (*p == fence) { p++; n++; }
    if (n < 3)
        return FALSE;
    while (*p == ' ' || *p == '\t')
        p++;
    if (info)
        *info = p;
    return TRUE;
}

static gboolean starts_with(const gchar *s, const gchar *pfx)
{
    return s && g_str_has_prefix(s, pfx);
}

/* GPtrArray of gchar* (mermaid sources); caller frees the array */
static GPtrArray *extract_blocks(const gchar *text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    guint i;

    for (i = 0; lines[i]; i++)
    {
        const gchar *info = NULL;
        GString *buf;
        gboolean tag_form;
        guint j;

        if (starts_with(g_strstrip(lines[i]), "@startmermaid"))
            tag_form = TRUE;
        else if (is_fence(lines[i], &info) &&
                 g_ascii_strncasecmp(g_strstrip((gchar *)info), "mermaid", 7) == 0)
            tag_form = FALSE;
        else
            continue;

        buf = g_string_new(NULL);
        if (tag_form)
        {
            for (j = i + 1; lines[j]; j++)
            {
                if (starts_with(g_strstrip(lines[j]), "@endmermaid"))
                    break;
                g_string_append(buf, lines[j]);
                g_string_append_c(buf, '\n');
            }
        }
        else
        {
            for (j = i + 1; lines[j]; j++)
            {
                const gchar *dummy = NULL;
                if (is_fence(lines[j], &dummy))
                    break;              /* closing fence */
                g_string_append(buf, lines[j]);
                g_string_append_c(buf, '\n');
            }
        }
        i = j;
        if (buf->len > 0)
            g_ptr_array_add(out, g_strdup(buf->str));
        g_string_free(buf, TRUE);
    }
    g_strfreev(lines);
    return out;
}

/* ---------------------------------------------------------------------- */
/* rendering                                                              */
/* ---------------------------------------------------------------------- */
static void rm_rf(const gchar *path)
{
    GDir *d;
    const gchar *name;

    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    if ((d = g_dir_open(path, 0, NULL)) != NULL)
    {
        while ((name = g_dir_read_name(d)) != NULL)
        {
            gchar *sub = g_build_filename(path, name, NULL);
            rm_rf(sub);
            g_free(sub);
        }
        g_dir_close(d);
        g_rmdir(path);
    }
    else
        g_remove(path);
}

static void destroy_child(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_destroy(w);
}

static void clear_images(void)
{
    if (g_images)
        gtk_container_foreach(GTK_CONTAINER(g_images),
                              (GtkCallback)destroy_child, NULL);
}

static void show_results(void)
{
    guint i, n_shown = 0;
    gboolean any = FALSE;

    if (!g_images || !g_pngs)
        return;
    for (i = 0; i < g_pngs->len; i++)
    {
        const gchar *png = g_ptr_array_index(g_pngs, i);
        GdkPixbuf *pb;
        GtkWidget *img;
        gint w = (gint)((g_width > 0 ? g_width : 1400) * g_zoom);

        if (!png || !g_file_test(png, G_FILE_TEST_EXISTS))
            continue;
        pb = gdk_pixbuf_new_from_file_at_scale(png, w, -1, TRUE, NULL);
        if (!pb)
            continue;
        img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(img, 6);
        gtk_widget_set_margin_bottom(img, 6);
        gtk_box_pack_start(GTK_BOX(g_images), img, FALSE, FALSE, 0);
        any = TRUE;
        n_shown++;
    }
    gtk_widget_show_all(g_images);
    mmd_logf("visar %u/%u bilder (zoom %d%%)", n_shown, g_pngs->len,
             (gint)(g_zoom * 100));
    if (!any)
        status("inget att visa");
}

static void render_next(void)
{
    gchar *in, *out, *tag;
    GPtrArray *argv;
    gboolean started;

    if (!g_sources || !g_pngs || g_job_i >= g_pngs->len)
    {
        g_rendering = FALSE;
        if (g_pngs && g_pngs->len)
            status("%u diagram klara", g_pngs->len);
        show_results();
        return;
    }

    in  = g_strdup_printf("%s/d%u.mmd", g_tmpdir, g_job_i);
    out = g_ptr_array_index(g_pngs, g_job_i);
    (void)g_file_set_contents(in, (const gchar *)g_ptr_array_index(g_sources, g_job_i),
                              -1, NULL);

    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup((g_mmdc_cmd && *g_mmdc_cmd) ? g_mmdc_cmd : "mmdc"));
    if (g_puppeteer && *g_puppeteer && g_file_test(g_puppeteer, G_FILE_TEST_EXISTS))
    {
        g_ptr_array_add(argv, g_strdup("-p"));
        g_ptr_array_add(argv, g_strdup(g_puppeteer));
    }
    if (g_theme && *g_theme)
    {
        g_ptr_array_add(argv, g_strdup("-t"));
        g_ptr_array_add(argv, g_strdup(g_theme));
    }
    if (g_background && *g_background)
    {
        g_ptr_array_add(argv, g_strdup("-b"));
        g_ptr_array_add(argv, g_strdup(g_background));
    }
    g_ptr_array_add(argv, g_strdup("-i")); g_ptr_array_add(argv, g_strdup(in));
    g_ptr_array_add(argv, g_strdup("-o")); g_ptr_array_add(argv, g_strdup(out));
    g_ptr_array_add(argv, g_strdup("-w"));
    g_ptr_array_add(argv, g_strdup_printf("%d", g_width > 0 ? g_width : 1400));
    g_ptr_array_add(argv, g_strdup("-s"));
    g_ptr_array_add(argv, g_strdup_printf("%d", g_scale > 0 ? g_scale : 1));
    g_ptr_array_add(argv, g_strdup("-q"));
    g_ptr_array_add(argv, NULL);

    tag = g_strdup_printf("mmdc %u/%u", g_job_i + 1, g_pngs->len);
    g_job_i++;
    started = spawn_mmdc(tag, argv);
    g_free(tag);
    g_ptr_array_free(argv, TRUE);
    g_free(in);

    if (!started)
    {
        status("mmdc kunde inte startas - installera med "
               "'salt <minion> state.apply workstation.md2pdf'");
        g_rendering = FALSE;
        show_results();
    }
}

static void render_document(void)
{
    GeanyDocument *doc = document_get_current();
    GPtrArray *blocks;
    gchar *text;
    guint i;

    if (!doc || !doc->editor || !doc->editor->sci || !doc->real_path)
    {
        status("inget aktivt dokument");
        return;
    }
    text = sci_get_contents(doc->editor->sci, -1);
    blocks = extract_blocks(text);
    mmd_logf("render: %u mermaid-block i %s", blocks->len,
             doc->file_name ? doc->file_name : "?");
    g_free(text);

    if (blocks->len == 0)
    {
        status("hittade inga ```mermaid-block i %s",
               doc->file_name ? doc->file_name : "dokumentet");
        g_ptr_array_free(blocks, TRUE);
        return;
    }

    clear_images();
    g_ptr_array_set_size(g_pngs, 0);
    g_ptr_array_set_size(g_sources, 0);
    rm_rf(g_tmpdir);
    g_free(g_tmpdir);
    g_tmpdir = g_dir_make_tmp("geany-mermaid-XXXXXX", NULL);
    if (!g_tmpdir)
    {
        status("kunde inte skapa temp-katalog");
        g_ptr_array_free(blocks, TRUE);
        return;
    }
    for (i = 0; i < blocks->len; i++)
    {
        g_ptr_array_add(g_sources, g_strdup(g_ptr_array_index(blocks, i)));
        g_ptr_array_add(g_pngs, g_strdup_printf("%s/d%u.png", g_tmpdir, i));
    }
    g_ptr_array_free(blocks, TRUE);

    g_job_i = 0;
    g_rendering = TRUE;
    if (g_win && !gtk_widget_get_visible(g_win))
        gtk_widget_show_all(g_win);
    status("renderar %u diagram…", g_pngs->len);
    render_next();
}

/* ---------------------------------------------------------------------- */
/* window                                                                 */
/* ---------------------------------------------------------------------- */
static void on_refresh(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    render_document();
}

static void on_zoom(GtkWidget *w, gpointer d)
{
    gdouble step = GPOINTER_TO_INT(d) > 0 ? 0.25 : -0.25;
    gchar *t;
    (void)w;

    g_zoom = CLAMP(g_zoom + step, 0.25, 4.0);
    t = g_strdup_printf("%d %%", (gint)(g_zoom * 100));
    if (g_zoom_lbl)
        gtk_label_set_text(GTK_LABEL(g_zoom_lbl), t);
    g_free(t);
    clear_images();
    show_results();
}

static void on_save_png(GtkWidget *w, gpointer d)
{
    GtkWidget *dlg;
    GeanyDocument *doc = document_get_current();
    gchar *base = g_strdup_printf("%s.png",
                    (doc && doc->file_name) ? doc->file_name : "mermaid");
    (void)w; (void)d;

    if (!g_pngs || !g_pngs->len)
    {
        status("inget diagram att spara");
        g_free(base);
        return;
    }
    dlg = gtk_file_chooser_dialog_new("Spara diagram som PNG",
              g_win ? GTK_WINDOW(g_win) : NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
              "_Avbryt", GTK_RESPONSE_CANCEL, "_Spara", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar *dest = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        const gchar *src = g_ptr_array_index(g_pngs, 0);
        gchar *data = NULL;
        gsize len = 0;
        GError *e = NULL;

        if (src && g_file_get_contents(src, &data, &len, &e))
        {
            if (g_file_set_contents(dest, data, (gssize)len, &e))
                status("sparade %s", dest);
            g_free(data);
        }
        if (e)
        {
            status("kunde inte spara: %s", e->message);
            g_error_free(e);
        }
        g_free(dest);
    }
    gtk_widget_destroy(dlg);
    g_free(base);
}

static gboolean on_win_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    gtk_widget_hide(w);
    return TRUE;
}

static void ensure_window(void)
{
    GtkWidget *box, *bar, *scroll, *btn;

    if (g_win)
        return;
    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), "Mermaid");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 900, 700);
    gtk_window_set_type_hint(GTK_WINDOW(g_win), GDK_WINDOW_TYPE_HINT_UTILITY);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);

    btn = gtk_button_new_with_label("Uppdatera");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("Spara PNG…");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_save_png), NULL);
    gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 0);

    btn = gtk_button_new_with_label("−");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_zoom), GINT_TO_POINTER(-1));
    gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 0);
    g_zoom_lbl = gtk_label_new("100 %");
    gtk_box_pack_start(GTK_BOX(bar), g_zoom_lbl, FALSE, FALSE, 0);
    btn = gtk_button_new_with_label("+");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_zoom), GINT_TO_POINTER(1));
    gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 0);

    g_status = gtk_label_new("Redo");
    gtk_widget_set_halign(g_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(bar), g_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    g_images = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(scroll), g_images);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(g_win), box);
    g_signal_connect(g_win, "delete-event", G_CALLBACK(on_win_delete), NULL);
    gtk_widget_show_all(g_win);
}

/* ---------------------------------------------------------------------- */
/* actions                                                                */
/* ---------------------------------------------------------------------- */
static void on_toolbar_click(GtkToolButton *b, gpointer d)
{
    (void)b; (void)d;
    ensure_window();
    render_document();
}

static void on_keybinding(guint id)
{
    if (id == 0)
    {
        ensure_window();
        render_document();
    }
}

static void on_document_activate(GObject *obj, GeanyDocument *doc, gpointer data)
{
    (void)obj; (void)data;
    if (g_on_activate && doc && !g_rendering)
    {
        ensure_window();
        render_document();
    }
}

static void on_document_save(GObject *obj, GeanyDocument *doc, gpointer data)
{
    (void)obj; (void)data;
    if (g_on_save && doc && doc == document_get_current() && g_win &&
        gtk_widget_get_visible(g_win) && !g_rendering)
        render_document();
}

/* ---------------------------------------------------------------------- */
/* preferences                                                            */
/* ---------------------------------------------------------------------- */
typedef struct { GtkWidget *cmd, *puppeteer, *theme, *bg, *width, *scale, *onsave,
                 *onactivate; }
PrefsW;

static void on_configure_response(GtkDialog *dlg, gint resp, gpointer data);

GtkWidget *plugin_configure(GtkDialog *dialog)
{
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *lbl;
    PrefsW *w = g_new0(PrefsW, 1);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    lbl = gtk_label_new("mmdc-kommando");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 0, 1, 1);
    w->cmd = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->cmd), g_mmdc_cmd ? g_mmdc_cmd : "mmdc");
    gtk_widget_set_hexpand(w->cmd, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->cmd, 1, 0, 1, 1);

    lbl = gtk_label_new("puppeteer-config");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 1, 1, 1);
    w->puppeteer = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w->puppeteer), g_puppeteer ? g_puppeteer : "");
    gtk_widget_set_hexpand(w->puppeteer, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w->puppeteer, 1, 1, 1, 1);

    lbl = gtk_label_new("Tema (tomt = mmdc-standard)");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 2, 1, 1);
    w->theme = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->theme), "default|dark|forest|neutral");
    gtk_entry_set_text(GTK_ENTRY(w->theme), g_theme ? g_theme : "");
    gtk_grid_attach(GTK_GRID(grid), w->theme, 1, 2, 1, 1);

    lbl = gtk_label_new("Bakgrund (tomt = mmdc-standard)");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 3, 1, 1);
    w->bg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->bg), "white|transparent");
    gtk_entry_set_text(GTK_ENTRY(w->bg), g_background ? g_background : "");
    gtk_grid_attach(GTK_GRID(grid), w->bg, 1, 3, 1, 1);

    lbl = gtk_label_new("Bredd");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 4, 1, 1);
    w->width = gtk_spin_button_new_with_range(200, 6000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->width), g_width);
    gtk_grid_attach(GTK_GRID(grid), w->width, 1, 4, 1, 1);

    lbl = gtk_label_new("Skala");
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, 5, 1, 1);
    w->scale = gtk_spin_button_new_with_range(1, 4, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->scale), g_scale);
    gtk_grid_attach(GTK_GRID(grid), w->scale, 1, 5, 1, 1);

    w->onsave = gtk_check_button_new_with_mnemonic(
        "Rendera om när dokumentet sparas (om fönstret är öppet)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->onsave), g_on_save);
    gtk_grid_attach(GTK_GRID(grid), w->onsave, 0, 6, 2, 1);

    w->onactivate = gtk_check_button_new_with_mnemonic(
        "Förhandsgranska automatiskt när ett dokument aktiveras");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->onactivate), g_on_activate);
    gtk_grid_attach(GTK_GRID(grid), w->onactivate, 0, 7, 2, 1);

    g_object_set_data(G_OBJECT(dialog), "mmd-w", w);
    g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), NULL);
    return grid;
}

static void on_configure_response(GtkDialog *dlg, gint resp, gpointer data)
{
    PrefsW *w = g_object_get_data(G_OBJECT(dlg), "mmd-w");
    const gchar *s;
    (void)data;

    if (!w || (resp != GTK_RESPONSE_OK && resp != GTK_RESPONSE_APPLY))
        return;
    g_free(g_mmdc_cmd);
    s = gtk_entry_get_text(GTK_ENTRY(w->cmd));
    g_mmdc_cmd = g_strdup((s && *s) ? s : "mmdc");
    g_free(g_puppeteer);
    g_puppeteer = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->puppeteer)));
    g_free(g_theme);
    g_theme = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->theme)));
    g_free(g_background);
    g_background = g_strdup(gtk_entry_get_text(GTK_ENTRY(w->bg)));
    g_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->width));
    g_scale = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->scale));
    g_on_save = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->onsave));
    g_on_activate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->onactivate));
    prefs_save();
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                              */
/* ---------------------------------------------------------------------- */
static void on_before_quit(GObject *o, gpointer d)
{
    (void)o; (void)d;
    g_quitting = TRUE;
}

void plugin_init(GeanyData *data)
{
    GeanyKeyGroup *group;

    if (g_inited)
    {
        mmd_logf("init: redan initierad - hoppar över");
        return;
    }
    g_inited = TRUE;
    geany_data = data;

    if (geany_plugin)
        plugin_module_make_resident(geany_plugin);
    plugin_signal_connect(geany_plugin, NULL, "geany-before-quit", TRUE,
                          G_CALLBACK(on_before_quit), NULL);
    plugin_signal_connect(geany_plugin, NULL, "document-save", TRUE,
                          G_CALLBACK(on_document_save), NULL);
    plugin_signal_connect(geany_plugin, NULL, "document-activate", TRUE,
                          G_CALLBACK(on_document_activate), NULL);

    prefs_load();
    g_sources = g_ptr_array_new_with_free_func(g_free);
    g_pngs    = g_ptr_array_new_with_free_func(g_free);
    mmd_logf("init: mmdc=%s width=%d scale=%d on_save=%d", g_mmdc_cmd, g_width,
             g_scale, (int)g_on_save);

    g_tb_item = gtk_tool_button_new(NULL, "Mermaid");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g_tb_item),
        "Rendera ```mermaid-blocken i det aktiva dokumentet (mmdc)");
    g_signal_connect(g_tb_item, "clicked", G_CALLBACK(on_toolbar_click), NULL);
    plugin_add_toolbar_item(geany_plugin, g_tb_item);

    group = plugin_set_key_group(geany_plugin, "geany-mermaid", 1, NULL);
    if (group)
        keybindings_set_item(group, 0, on_keybinding, GDK_KEY_m,
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                             "mermaid_preview", "Förhandsgranska Mermaid", NULL);

    mmd_logf("init: klar");
}

void plugin_cleanup(void)
{
    if (g_tb_item)
    {
        gtk_widget_destroy(GTK_WIDGET(g_tb_item));
        g_tb_item = NULL;
    }
    if (g_win)
    {
        gtk_widget_destroy(g_win);
        g_win = NULL;
    }
    g_images = NULL;
    g_status = NULL;
    g_zoom_lbl = NULL;
    if (g_sources) { g_ptr_array_free(g_sources, TRUE); g_sources = NULL; }
    if (g_pngs)    { g_ptr_array_free(g_pngs, TRUE);    g_pngs = NULL; }
    rm_rf(g_tmpdir);
    g_free(g_tmpdir);
    g_tmpdir = NULL;
    prefs_save();
    mmd_logf("cleanup: klar (quitting=%d)", (int)g_quitting);
    g_inited = FALSE;
}

PLUGIN_VERSION_CHECK(224)
PLUGIN_SET_INFO("geany-mermaid",
                "Mermaid diagram preview: renders ```mermaid blocks with mmdc",
                "0.1.0",
                "Vertelab")

/*
 * bridge.c - control bridge for external agents (Pi etc.)
 *
 * The plugin listens on a UNIX domain socket. External tooling (the
 * `geany-pi-ctl` CLI, and therefore any agent with shell access, e.g. Pi)
 * can query and modify the active Geany document.
 *
 * Socket:  $XDG_RUNTIME_DIR/geany-pi.sock   (mode 0600)
 * Protocol: one JSON object per line (newline terminated). Text is JSON
 *           escaped, so no literal newlines ever appear inside a frame.
 *
 * Requests (all have a "cmd"):
 *   {"cmd":"ping"}                                  -> {ok,plugin,version,pid}
 *   {"cmd":"info"}                                  -> {ok,file,language,has_selection,
 *                                                       selection_start,selection_end,length,
 *                                                       line,column,index,id}
 *   {"cmd":"get","scope":"document"|"selection"}    -> {ok,text,file,language,scope,index,id}
 *   {"cmd":"set","scope":"document"|"selection","text":".."} -> {ok,applied,scope,index,id}
 *   {"cmd":"insert","text":".."}                    -> {ok,at,index,id}
 *   {"cmd":"status","text":".."}                    -> {ok}          (Geany status tab)
 *   {"cmd":"docs"}                                  -> {ok,docs:[{index,id,file,current,
 *                                                                   changed,language}]}
 *   {"cmd":"open","file":"/abs/path"}               -> {ok,file,index,id}
 *
 * Document targeting (all document-scoped commands, optional):
 *   "id": N       stable Geany document id  (see "docs")
 *   "index": N    documents_array index     (see "docs")
 *   "file": ".."  UTF-8 file name           (see "docs")
 *   "activate":b  switch to the tab before acting (default: false)
 * When none of id/index/file is present the ACTIVE document is used,
 * which keeps every existing caller working unchanged.
 *
 * The handlers run on the GTK main thread (GSocketService + GDataInputStream
 * async read), so all Scintilla calls are safe.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <string.h>

#ifndef GEMM_VERSION
# define GEMM_VERSION "0.1.0"
#endif

static GSocketService *g_service  = NULL;
static gchar          *g_sockpath = NULL;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static JsonObject *error_obj(const gchar *msg);   /* defined below */

/* Bring a document's tab to the front using only public API.
 * document_show_tab() would be the direct call, but it lives behind
 * GEANY_PRIVATE; notebook page + main_widgets->notebook is public and
 * has the same effect. */
static void show_doc_tab(GeanyDocument *d)
{
    gint page;
    if (!DOC_VALID(d) || d == document_get_current())
        return;
    page = document_get_notebook_page(d);
    if (page >= 0 && geany->main_widgets && geany->main_widgets->notebook)
        gtk_notebook_set_current_page(
            GTK_NOTEBOOK(geany->main_widgets->notebook), page);
}

/* --- document targeting -------------------------------------------- */

/* Read an optional integer member; FALSE when absent. */
static gboolean json_int_opt(JsonObject *o, const gchar *key, gint64 *out)
{
    if (!o || !json_object_has_member(o, key))
        return FALSE;
    *out = json_object_get_int_member(o, key);
    return TRUE;
}

/* Optional boolean with a default for "absent". */
static gboolean json_bool_opt(JsonObject *o, const gchar *key, gboolean dflt)
{
    if (!o || !json_object_has_member(o, key))
        return dflt;
    return json_object_get_boolean_member(o, key);
}

/* Resolve the document a request targets.
 *   id > index > file > active documented.
 * When "activate" is true and the target is not already current, the tab
 * is brought to the front (show_doc_tab) so the user sees the edit. */
static GeanyDocument *resolve_doc(JsonObject *req, gboolean activate)
{
    GeanyDocument *d = NULL;
    gint64 v;

    if (json_int_opt(req, "id", &v))
        d = document_find_by_id((guint)v);
    else if (json_int_opt(req, "index", &v))
        d = document_index((gint)v);
    else if (req && json_object_has_member(req, "file"))
        d = document_find_by_filename(
                json_object_get_string_member(req, "file"));

    if (!DOC_VALID(d))
        return NULL;                    /* explicit miss => no fallback */

    if (activate && d != document_get_current())
        show_doc_tab(d);

    return d;
}

/* Resolve for a request, defaulting to the active document when no
 * targeting field was supplied at all.  Returns NULL if the request
 * named a document that does not exist/is not valid. */
static gboolean has_target(JsonObject *req)
{
    gint64 v;
    return json_int_opt(req, "id", &v)
        || json_int_opt(req, "index", &v)
        || (req && json_object_has_member(req, "file"));
}

static GeanyDocument *target_doc(JsonObject *req)
{
    if (!has_target(req))
        return document_get_current();
    return resolve_doc(req, json_bool_opt(req, "activate", FALSE));
}

/* Attach the identity of the document a response relates to. */
static void put_doc_identity(JsonObject *r, GeanyDocument *d)
{
    json_object_set_int_member(r, "id",    (gint64)(d ? d->id : 0));
    json_object_set_int_member(r, "index", (gint64)(d ? d->index : -1));
}

static gchar *json_object_to_string(JsonObject *obj)
{
    JsonNode *root = json_node_alloc();
    JsonGenerator *gen;
    gchar *s;

    json_node_take_object(root, obj);       /* takes ownership */
    gen = json_generator_new();
    json_generator_set_root(gen, root);
    s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(root);                    /* frees obj too */
    return s;
}

/* ------------------------------------------------------------------ */
/* command handlers                                                   */
/* ------------------------------------------------------------------ */
static JsonObject *cmd_ping(void)
{
    JsonObject *r = json_object_new();
    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_string_member(r, "plugin", "geany-pi");
    json_object_set_string_member(r, "version", GEMM_VERSION);
    json_object_set_int_member(r, "pid", (gint64)getpid());
    return r;
}

/* Where the document lives.  gvfs/sftp paths look like
 *   /run/user/1000/gvfs/sftp:host=192.168.11.145/usr/share/x/y.py
 * Agents that must run git / build tools need the host and directory. */
static void put_location(JsonObject *r, const gchar *path)
{
    const gchar *g, *c, *end;
    gboolean remote = FALSE;
    gchar *host = NULL, *user = NULL, *port = NULL, *dir = NULL;

    if (path && (g = strstr(path, "/gvfs/")) != NULL &&
        (c = strchr(g + 6, ':')) != NULL && g_str_has_prefix(c + 1, "host="))
    {
        end = c + 6;                              /* after "host=" */
        {
            const gchar *hs = end;
            while (*end && *end != ',' && *end != '/')
                end++;
            host = g_strndup(hs, (gsize)(end - hs));
        }
        while (*end == ',')
        {
            const gchar *ks = ++end;
            gsize klen;
            gchar *val = NULL;
            while (*end && *end != '=' && *end != ',' && *end != '/')
                end++;
            klen = (gsize)(end - ks);
            if (*end == '=')
            {
                const gchar *vs = ++end;
                while (*end && *end != ',' && *end != '/')
                    end++;
                val = g_strndup(vs, (gsize)(end - vs));
            }
            if (klen == 4 && !strncmp(ks, "user", 4))      { g_free(user); user = val; }
            else if (klen == 4 && !strncmp(ks, "port", 4)) { g_free(port); port = val; }
            else g_free(val);
        }
        {
            gchar *full = g_strdup(*end ? end : "/");
            dir = g_path_get_dirname(full);
            g_free(full);
        }
        remote = TRUE;
    }
    else if (path)
        dir = g_path_get_dirname(path);

    json_object_set_boolean_member(r, "remote", remote);
    json_object_set_string_member(r, "host", host ? host : "");
    json_object_set_string_member(r, "user", user ? user : "");
    json_object_set_string_member(r, "port", port ? port : "");
    json_object_set_string_member(r, "dir",  dir  ? dir  : "");
    g_free(host); g_free(user); g_free(port); g_free(dir);
}

static JsonObject *cmd_info(JsonObject *req)
{
    GeanyDocument *d   = target_doc(req);
    GeanyDocument *cur = document_get_current();
    ScintillaObject *sci = (d && d->editor) ? d->editor->sci : NULL;
    JsonObject *r = json_object_new();

    if (has_target(req) && d == NULL)
        return error_obj("document not found");

    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_string_member(r, "file",
        (d && d->file_name) ? d->file_name : "");
    put_location(r, (d && d->real_path) ? d->real_path
                                       : (d ? d->file_name : NULL));
    json_object_set_string_member(r, "language",
        (d && d->file_type && d->file_type->name) ? d->file_type->name : "");
    json_object_set_boolean_member(r, "is_active", d != NULL && d == cur);
    json_object_set_boolean_member(r, "changed", d ? d->changed : FALSE);
    put_doc_identity(r, d);
    json_object_set_boolean_member(r, "has_selection",
        gemm_active_has_selection(d));
    if (sci)
    {
        json_object_set_int_member(r, "selection_start",
            (gint64)sci_get_selection_start(sci));
        json_object_set_int_member(r, "selection_end",
            (gint64)sci_get_selection_end(sci));
        json_object_set_int_member(r, "length",
            (gint64)sci_get_length(sci));
        {
            gint pos = (gint)sci_get_current_position(sci);
            json_object_set_int_member(r, "cursor", (gint64)pos);
            json_object_set_int_member(r, "line",
                (gint64)sci_get_line_from_position(sci, pos) + 1);
        }
    }
    else
        json_object_set_int_member(r, "length", 0);
    return r;
}

static JsonObject *cmd_get(JsonObject *req)
{
    JsonObject *r = json_object_new();
    const gchar *scope = json_object_has_member(req, "scope")
        ? json_object_get_string_member(req, "scope") : "document";
    gint s = -1, e = -1;
    GemmRequestKind kind = (scope && strcmp(scope, "selection") == 0)
        ? GEMM_REQUEST_SELECTION : GEMM_REQUEST_DOC;
    GeanyDocument *d = target_doc(req);
    gchar *text;

    if (has_target(req) && d == NULL)
        return error_obj("document not found");

    text = gemm_active_get_text(d, kind, &s, &e);

    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_string_member(r, "scope",
        kind == GEMM_REQUEST_SELECTION ? "selection" : "document");
    json_object_set_string_member(r, "text", text ? text : "");
    json_object_set_int_member(r, "start", (gint64)s);
    json_object_set_int_member(r, "end",   (gint64)e);
    json_object_set_string_member(r, "file",
        (d && d->file_name) ? d->file_name : "");
    json_object_set_string_member(r, "language",
        (d && d->file_type && d->file_type->name) ? d->file_type->name : "");
    put_doc_identity(r, d);
    g_free(text);
    return r;
}

static JsonObject *cmd_set(JsonObject *req)
{
    JsonObject *r = json_object_new();
    const gchar *scope = json_object_has_member(req, "scope")
        ? json_object_get_string_member(req, "scope") : "document";
    const gchar *text  = json_object_has_member(req, "text")
        ? json_object_get_string_member(req, "text") : "";
    gboolean sel = (scope && strcmp(scope, "selection") == 0);
    /* Writes default to activating the target tab so a background edit is
     * visible; pass "activate":false to edit silently. */
    GeanyDocument *d = has_target(req)
        ? resolve_doc(req, json_bool_opt(req, "activate", TRUE))
        : document_get_current();
    ScintillaObject *sci = (d && d->editor) ? d->editor->sci : NULL;
    gint applied = 0;

    if (has_target(req) && d == NULL)
        return error_obj("document not found");

    if (sci == NULL)
        return error_obj("no active document");

    if (sel)
    {
        gint s = (gint)sci_get_selection_start(sci);
        gint e = (gint)sci_get_selection_end(sci);
        gemm_editor_replace(d, s, e, text);
        applied = e - s;
        json_object_set_string_member(r, "scope", "selection");
    }
    else
    {
        gemm_editor_replace(d, -1, -1, text);
        applied = (gint)strlen(text);
        json_object_set_string_member(r, "scope", "document");
    }

    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_int_member(r, "applied", (gint64)applied);
    put_doc_identity(r, d);
    msgwin_status_add("[geany-pi bridge] %s updated (%d bytes) in %s",
                      sel ? "selection" : "document",
                      (int)strlen(text),
                      (d && d->file_name) ? d->file_name : "?");
    gemm_logf("bridge: set %s (%zu bytes) doc id=%u", scope ? scope : "?",
              strlen(text), d ? d->id : 0);
    return r;
}

static JsonObject *cmd_insert(JsonObject *req)
{
    JsonObject *r = json_object_new();
    const gchar *text = json_object_has_member(req, "text")
        ? json_object_get_string_member(req, "text") : "";
    GeanyDocument *d = has_target(req)
        ? resolve_doc(req, json_bool_opt(req, "activate", TRUE))
        : document_get_current();
    ScintillaObject *sci = (d && d->editor) ? d->editor->sci : NULL;

    if (has_target(req) && d == NULL)
        return error_obj("document not found");
    if (sci == NULL)
        return error_obj("no active document");
    {
        gint pos = (gint)sci_get_current_position(sci);
        sci_insert_text(sci, pos, text);
        if (!d->changed)
            document_set_text_changed(d, TRUE);
        json_object_set_boolean_member(r, "ok", TRUE);
        json_object_set_int_member(r, "at", (gint64)pos);
        put_doc_identity(r, d);
    }
    return r;
}

static JsonObject *cmd_status(JsonObject *req)
{
    JsonObject *r = json_object_new();
    const gchar *text = json_object_has_member(req, "text")
        ? json_object_get_string_member(req, "text") : "";
    msgwin_status_add("[agent] %s", text);
    json_object_set_boolean_member(r, "ok", TRUE);
    return r;
}

static JsonObject *cmd_codes(JsonObject *req)
{
    JsonObject *r = json_object_new();
    GeanyDocument *d = target_doc(req);
    ScintillaObject *sci = (d && d->editor) ? d->editor->sci : NULL;
    gint start = 0, len = 32, i;
    JsonArray *arr;
    if (json_object_has_member(req, "start"))
        start = (gint)json_object_get_int_member(req, "start");
    if (json_object_has_member(req, "len"))
        len = (gint)json_object_get_int_member(req, "len");
    if (has_target(req) && d == NULL)
        return error_obj("document not found");
    if (sci == NULL)
        return error_obj("no active document");
    arr = json_array_new();
    for (i = 0; i < len; i++)
        json_array_add_int_element(arr,
            (gint64)(guchar)sci_get_char_at(sci, start + i));
    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_int_member(r, "length", (gint64)sci_get_length(sci));
    json_object_set_array_member(r, "codes", arr);
    put_doc_identity(r, d);
    return r;
}

static JsonObject *cmd_select(JsonObject *req)
{
    JsonObject *r = json_object_new();
    GeanyDocument *d = has_target(req)
        ? resolve_doc(req, json_bool_opt(req, "activate", TRUE))
        : document_get_current();
    ScintillaObject *sci = (d && d->editor) ? d->editor->sci : NULL;
    gint start, end;

    if (has_target(req) && d == NULL)
        return error_obj("document not found");
    if (sci == NULL)
        return error_obj("no active document");
    if (!json_object_has_member(req, "start") ||
        !json_object_has_member(req, "end"))
        return error_obj("missing 'start'/'end'");
    start = (gint)json_object_get_int_member(req, "start");
    end   = (gint)json_object_get_int_member(req, "end");
    sci_set_selection_start(sci, start);
    sci_set_selection_end(sci, end);
    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_int_member(r, "start", (gint64)start);
    json_object_set_int_member(r, "end", (gint64)end);
    put_doc_identity(r, d);
    return r;
}

static JsonObject *cmd_docs(void)
{
    JsonObject *r = json_object_new();
    JsonArray  *arr = json_array_new();
    guint i;
    GeanyDocument *cur = document_get_current();

    for (i = 0; i < geany->documents_array->len; i++)
    {
        GeanyDocument *d = g_ptr_array_index(geany->documents_array, i);
        if (!d) continue;
        {
            JsonObject *o = json_object_new();
            json_object_set_int_member(o, "index", (gint64)d->index);
            json_object_set_int_member(o, "id",    (gint64)d->id);
            json_object_set_string_member(o, "file",
                d->file_name ? d->file_name : "");
            json_object_set_boolean_member(o, "current", d == cur);
            json_object_set_boolean_member(o, "changed", d->changed);
            json_object_set_string_member(o, "language",
                (d->file_type && d->file_type->name) ? d->file_type->name : "");
            json_array_add_object_element(arr, o);  /* takes ownership */
        }
    }
    json_object_set_boolean_member(r, "ok", TRUE);
    json_object_set_array_member(r, "docs", arr);   /* takes ownership */
    return r;
}

static JsonObject *cmd_open(JsonObject *req)
{
    JsonObject *r = json_object_new();
    const gchar *file = json_object_has_member(req, "file")
        ? json_object_get_string_member(req, "file") : NULL;

    if (!file || !*file)
    {
        json_object_set_boolean_member(r, "ok", FALSE);
        json_object_set_string_member(r, "error", "missing 'file'");
        return r;
    }
    {
        GeanyDocument *d;
        document_open_file(file, FALSE, NULL, NULL);
        /* document_open_file() returns the doc when the file was newly
         * opened; when it was ALREADY open it may just focus the tab. Look
         * the document up afterwards so the response always carries the
         * real identity. */
        d = document_find_by_filename(file);
        if (!DOC_VALID(d))
            d = document_get_current();
        json_object_set_boolean_member(r, "ok", DOC_VALID(d));
        if (DOC_VALID(d))
            show_doc_tab(d);        /* make the tab active */
        json_object_set_string_member(r, "file",
            (DOC_VALID(d) && d->file_name) ? d->file_name : file);
        put_doc_identity(r, DOC_VALID(d) ? d : NULL);
        if (DOC_VALID(d))
            json_object_set_boolean_member(r, "current", TRUE);
    }
    return r;
}

static JsonObject *error_obj(const gchar *msg)
{
    JsonObject *r = json_object_new();
    json_object_set_boolean_member(r, "ok", FALSE);
    json_object_set_string_member(r, "error", msg ? msg : "error");
    return r;
}

static JsonObject *dispatch(JsonObject *req)
{
    const gchar *cmd;

    if (!req || !json_object_has_member(req, "cmd"))
        return error_obj("missing 'cmd'");
    cmd = json_object_get_string_member(req, "cmd");

    if (!cmd)                          return error_obj("bad 'cmd'");
    if (!strcmp(cmd, "ping"))          return cmd_ping();
    if (!strcmp(cmd, "info"))          return cmd_info(req);
    if (!strcmp(cmd, "get"))           return cmd_get(req);
    if (!strcmp(cmd, "set"))           return cmd_set(req);
    if (!strcmp(cmd, "insert"))        return cmd_insert(req);
    if (!strcmp(cmd, "status"))        return cmd_status(req);
    if (!strcmp(cmd, "docs"))          return cmd_docs();
    if (!strcmp(cmd, "codes"))         return cmd_codes(req);
    if (!strcmp(cmd, "select"))        return cmd_select(req);
    if (!strcmp(cmd, "open"))          return cmd_open(req);
    return error_obj("unknown command");
}

/* ------------------------------------------------------------------ */
/* connection plumbing (async so the UI never blocks)                 */
/* ------------------------------------------------------------------ */
typedef struct
{
    GSocketConnection *conn;
    GDataInputStream  *dis;
} ConnCtx;

static void conn_free(ConnCtx *ctx)
{
    if (!ctx) return;
    if (ctx->dis)  g_object_unref(ctx->dis);
    if (ctx->conn)
    {
        g_io_stream_close(G_IO_STREAM(ctx->conn), NULL, NULL);
        g_object_unref(ctx->conn);
    }
    g_free(ctx);
}

static void on_line_read(GObject *src, GAsyncResult *res, gpointer data)
{
    ConnCtx *ctx = data;
    GError  *err = NULL;
    gsize    len = 0;
    gchar   *line;

    line = g_data_input_stream_read_line_finish(
               G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (err)
    {
        gemm_logf("bridge: read error: %s", err->message);
        g_error_free(err);
        conn_free(ctx);
        return;
    }
    if (line == NULL)          /* EOF: peer closed without a frame */
    {
        conn_free(ctx);
        return;
    }

    {
        JsonParser *par = json_parser_new();
        JsonObject *resp;

        if (json_parser_load_from_data(par, line, (gssize)len, NULL))
        {
            JsonNode   *root = json_parser_get_root(par);
            JsonObject *req  = root ? json_node_get_object(root) : NULL;
            resp = dispatch(req);
        }
        else
            resp = error_obj("invalid JSON request");

        {
            gchar *out = json_object_to_string(resp);
            GError *werr = NULL;
            GOutputStream *os =
                g_io_stream_get_output_stream(G_IO_STREAM(ctx->conn));
            g_output_stream_write_all(os, out, strlen(out), NULL, NULL, &werr);
            g_output_stream_write_all(os, "\n", 1, NULL, NULL, &werr);
            g_output_stream_flush(os, NULL, NULL);
            if (werr)
            {
                gemm_logf("bridge: write error: %s", werr->message);
                g_error_free(werr);
            }
            g_free(out);
        }
        g_object_unref(par);   /* frees resp (owned by the parser root) */
    }
    g_free(line);
    conn_free(ctx);
}

static gboolean on_incoming(GSocketService *service,
                            GSocketConnection *conn,
                            GObject *source,
                            gpointer user_data)
{
    ConnCtx *ctx = g_new0(ConnCtx, 1);
    (void)service; (void)source; (void)user_data;

    ctx->conn = g_object_ref(conn);
    ctx->dis  = g_data_input_stream_new(
                    g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    g_data_input_stream_set_newline_type(ctx->dis, G_DATA_STREAM_NEWLINE_TYPE_LF);

    g_data_input_stream_read_line_async(ctx->dis, G_PRIORITY_DEFAULT, NULL,
                                        on_line_read, ctx);
    return TRUE;   /* we own the connection; do not let the service close it */
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */
const gchar *gemm_bridge_socket_path(void)
{
    return g_sockpath;
}

gboolean gemm_bridge_start(void)
{
    GSocketAddress *addr;
    GError *err = NULL;
    const gchar *dir;

    if (g_service)
        return TRUE;                  /* already running */

    dir = g_get_user_runtime_dir();
    g_sockpath = g_build_filename(dir, "geany-pi.sock", NULL);

    if (g_file_test(g_sockpath, G_FILE_TEST_EXISTS))
        (void)g_unlink(g_sockpath);   /* drop a stale socket */

    g_service = g_socket_service_new();
    addr = g_unix_socket_address_new(g_sockpath);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(g_service),
                                       addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, NULL, &err))
    {
        gemm_logf("bridge: cannot bind %s: %s", g_sockpath,
                  err ? err->message : "?");
        if (err) g_error_free(err);
        g_object_unref(addr);
        g_object_unref(g_service);
        g_service = NULL;
        g_clear_pointer(&g_sockpath, g_free);
        return FALSE;
    }
    g_object_unref(addr);
    (void)g_chmod(g_sockpath, 0600);

    g_signal_connect(g_service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(g_service);

    gemm_logf("bridge: listening on %s", g_sockpath);
    msgwin_status_add("[geany-pi] control bridge: %s", g_sockpath);
    return TRUE;
}

void gemm_bridge_stop(void)
{
    if (g_service)
    {
        g_socket_service_stop(g_service);
        g_object_unref(g_service);
        g_service = NULL;
    }
    if (g_sockpath)
    {
        (void)g_unlink(g_sockpath);
        g_free(g_sockpath);
        g_sockpath = NULL;
    }
}

/*
 * pi_mcp.c - configure Pi's MCP client for the local machine.
 *
 * Makes the `ai_pi_mcp` Odoo MCP server available to the Pi agent running on
 * the same machine as Geany (i.e. the machine that has the document open).
 *
 * It reads the Odoo connection Pi already uses (~/.pi/agent/odoo.json:
 * baseUrl + token), derives the MCP endpoint (<odoo-root>/mcp) and merges an
 * entry into ~/.pi/agent/mcp.json:
 *
 *   "ai_pi_mcp": {
 *       "transport": "streamable-http",
 *       "url": "https://<odoo-root>/mcp",
 *       "headers": { "Authorization": "Bearer <token>" },
 *       "lifecycle": "lazy"
 *   }
 *
 * The merge is idempotent and preserves every other server in the file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <string.h>

static void mcp_sync_child(GPid pid, gint status, gpointer data);
static guint g_sync_source = 0;

#define PI_MCP_NAME "ai_pi_mcp"

static gchar *pi_agent_file(const gchar *name)
{
    /* Pi's agent dir is ~/.pi/agent (NOT ~/.config/pi/agent) */
    return g_build_filename(g_get_home_dir(), ".pi", "agent", name, NULL);
}

/* Read a JSON object from a file; returns NULL on any problem. */
static JsonObject *load_json_object(const gchar *path)
{
    JsonParser *par;
    JsonNode   *root;
    JsonObject *obj = NULL;
    gchar      *data = NULL;

    if (!g_file_get_contents(path, &data, NULL, NULL))
        return NULL;
    par = json_parser_new();
    if (json_parser_load_from_data(par, data, -1, NULL))
    {
        root = json_parser_get_root(par);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            obj = json_object_ref(json_node_get_object(root));
    }
    g_object_unref(par);
    g_free(data);
    return obj;
}

static gboolean write_json_object(const gchar *path, JsonObject *obj)
{
    JsonNode *root = json_node_alloc();
    JsonGenerator *gen;
    gchar *data;
    gboolean ok;

    json_node_take_object(root, obj);   /* consumes the reference */
    gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_indent(gen, 2);
    json_generator_set_root(gen, root);
    data = json_generator_to_data(gen, NULL);
    ok = g_file_set_contents(path, data, -1, NULL);
    (void)g_chmod(path, 0600);
    g_free(data);
    g_object_unref(gen);
    json_node_free(root);
    return ok;
}

/*
 * Configure / refresh the ai_pi_mcp entry in Pi's MCP config.
 * Returns TRUE on success (or when nothing needed doing), FALSE on skip/error.
 * Sets *message to a short human-readable status (owned by caller).
 */
gboolean gemm_pi_mcp_setup(gchar **message)
{
    gchar *odoo_path = pi_agent_file("odoo.json");
    gchar *mcp_path  = pi_agent_file("mcp.json");
    JsonObject *odoo = NULL, *mcp = NULL, *servers = NULL, *entry = NULL;
    JsonObject *headers = NULL;
    gchar *base_url = NULL, *token = NULL, *root = NULL, *url = NULL;
    gboolean ok = FALSE;

    if (message) *message = NULL;

    odoo = load_json_object(odoo_path);
    if (!odoo)
    {
        if (message)
            *message = g_strdup_printf("no %s - configure Pi's Odoo "
                                       "connection first", odoo_path);
        goto out;
    }

    if (json_object_has_member(odoo, "baseUrl"))
    {
        const gchar *b = json_object_get_string_member(odoo, "baseUrl");
        if (b) base_url = g_strdup(b);
    }
    if (json_object_has_member(odoo, "token"))
    {
        const gchar *t = json_object_get_string_member(odoo, "token");
        if (t) token = g_strdup(t);
    }

    if (!base_url || !*base_url || !token || !*token)
    {
        if (message)
            *message = g_strdup("odoo.json is missing baseUrl or token");
        goto out;
    }

    /* root = baseUrl without the trailing "/ai/v1" */
    root = g_strdup(base_url);
    if (g_str_has_suffix(root, "/ai/v1"))
        root[strlen(root) - strlen("/ai/v1")] = '\0';
    while (*root && root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = '\0';

    /* explicit override (e.g. a locally reachable Odoo) wins; otherwise Pi
     * talks to the local router, which follows the active document and
     * forwards to the right machine with the right token. */
    {
        const gchar *env = g_getenv("GEANY_LLM_PI_MCP_URL");
        if (env && *env)
            url = g_strdup(env);
        else
        {
            const gchar *p = g_getenv("PI_MCP_ROUTER_PORT");
            url = g_strdup_printf("http://127.0.0.1:%s/mcp",
                                  (p && *p) ? p : "8765");
        }
    }

    mcp = load_json_object(mcp_path);
    if (!mcp)
    {
        /* create a fresh config; an unparsable existing file is left alone */
        if (g_file_test(mcp_path, G_FILE_TEST_EXISTS))
        {
            if (message)
                *message = g_strdup_printf("%s exists but is not valid JSON "
                                           "- left untouched", mcp_path);
            goto out;
        }
        mcp = json_object_new();
    }

    if (json_object_has_member(mcp, "mcpServers") &&
        json_node_get_node_type(json_object_get_member(mcp, "mcpServers")) ==
            JSON_NODE_OBJECT)
    {
        servers = json_object_get_object_member(mcp, "mcpServers");
    }
    else
    {
        servers = json_object_new();
        json_object_set_object_member(mcp, "mcpServers", servers);
    }

    headers = json_object_new();
    {
        gchar *bearer = g_strdup_printf("Bearer %s", token);
        json_object_set_string_member(headers, "Authorization", bearer);
        g_free(bearer);
    }
    entry = json_object_new();
    json_object_set_string_member(entry, "transport", "streamable-http");
    json_object_set_string_member(entry, "url", url);
    json_object_set_object_member(entry, "headers", headers);
    json_object_set_string_member(entry, "lifecycle", "lazy");

    json_object_set_object_member(servers, PI_MCP_NAME, entry);

    if (!write_json_object(mcp_path, mcp))
    {
        if (message)
            *message = g_strdup_printf("could not write %s", mcp_path);
        mcp = NULL;               /* consumed by write_json_object */
        goto out;
    }
    mcp = NULL;                   /* consumed */

    ok = TRUE;
    if (message)
        *message = g_strdup_printf("Pi MCP '%s' -> %s", PI_MCP_NAME, url);
    gemm_logf("pi_mcp: configured %s -> %s", PI_MCP_NAME, url);

out:
    if (odoo)     json_object_unref(odoo);
    if (base_url) g_free(base_url);
    if (token)    g_free(token);
    if (root)     g_free(root);
    if (url)      g_free(url);
    g_free(odoo_path);
    g_free(mcp_path);
    return ok;
}

/* Local MCP router: one stable endpoint that follows the active document. */
#define GEMM_ROUTER_PORT_DEFAULT 8765

/* ------------------------------------------------------------------ */
/* Process spawning - the Geany/LSP pattern.                          */
/*                                                                    */
/* g_spawn_async_with_pipes() + g_child_watch_source_new() +          */
/* GIOChannel reads.  NEVER g_spawn_sync()/waitpid: inside Geany's    */
/* main loop those hang (SIGCHLD race).  See lsp/src/spawn/spawn.c    */
/* ("stolen from Geany").  No shell is involved, so no quoting bugs.  */
/* ------------------------------------------------------------------ */
typedef struct
{
    gchar      *tag;
    GIOChannel *out;
    GIOChannel *err;
} SpawnCtx;

static gchar *find_helper(const gchar *name)
{
    const gchar *dirs[] = { "/usr/local/bin", NULL };
    gchar *homebin = g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
    gchar *found = NULL;
    guint i;

    dirs[1] = homebin;
    for (i = 0; i < 2; i++)
    {
        gchar *cand = g_build_filename(dirs[i], name, NULL);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE))
        {
            found = cand;
            break;
        }
        g_free(cand);
    }
    g_free(homebin);
    return found;
}

static void spawn_drain(GIOChannel *ch, const gchar *tag)
{
    gchar *line = NULL;
    gsize len = 0;
    GError *e = NULL;
    GIOStatus st;

    if (!ch)
        return;
    while ((st = g_io_channel_read_line(ch, &line, &len, NULL, &e)) == G_IO_STATUS_NORMAL)
    {
        g_strchomp(line);
        if (*line)
            gemm_logf("%s: %s", tag, line);
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
    if (cond & (G_IO_HUP | G_IO_ERR))
        return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

static void spawn_exit_cb(GPid pid, gint status, gpointer data)
{
    SpawnCtx *ctx = data;

    spawn_drain(ctx->out, ctx->tag);   /* catch anything still buffered */
    spawn_drain(ctx->err, ctx->tag);
    if (ctx->out) { g_io_channel_unref(ctx->out); ctx->out = NULL; }
    if (ctx->err) { g_io_channel_unref(ctx->err); ctx->err = NULL; }
    g_spawn_close_pid(pid);
    gemm_logf("%s: klar (status %d)", ctx->tag, status);
    g_free(ctx->tag);
    g_free(ctx);
}

/* Run a short-lived helper, logging its output into the plugin log. */
static gboolean spawn_capture(const gchar *tag, const gchar *const *argv)
{
    GPid  pid = 0;
    gint  out_fd = -1, err_fd = -1;
    GError *err = NULL;
    SpawnCtx *ctx;

    if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, NULL, &out_fd, &err_fd, &err))
    {
        gemm_logf("%s: kunde inte starta: %s", tag, err ? err->message : "?");
        if (err) g_error_free(err);
        return FALSE;
    }

    ctx = g_new0(SpawnCtx, 1);
    ctx->tag = g_strdup(tag);
    ctx->out = g_io_channel_unix_new(out_fd);
    ctx->err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(ctx->out, NULL, NULL);   /* raw, never block on UTF-8 */
    g_io_channel_set_encoding(ctx->err, NULL, NULL);
    g_io_add_watch(ctx->out, G_IO_IN | G_IO_HUP | G_IO_ERR, spawn_read_cb, ctx);
    g_io_add_watch(ctx->err, G_IO_IN | G_IO_HUP | G_IO_ERR, spawn_read_cb, ctx);
    g_child_watch_add(pid, spawn_exit_cb, ctx);
    return TRUE;
}

static gboolean router_reachable(void)
{
    GSocketClient *cl = g_socket_client_new();
    GSocketConnection *c;
    gboolean ok;

    g_socket_client_set_timeout(cl, 1);
    c = g_socket_client_connect_to_host(cl, "127.0.0.1",
            (guint)GEMM_ROUTER_PORT_DEFAULT, NULL, NULL);
    ok = (c != NULL);
    if (c) g_object_unref(c);
    g_object_unref(cl);
    return ok;
}

/* Start geany-pi-mcp-router (detached) if it is not already listening. */
void gemm_pi_mcp_router_ensure(void)
{
    gchar *exe;
    gchar *logdir, *logfile;
    gchar *cmd;
    gchar *argv[4];
    GPid pid = 0;
    GError *err = NULL;

    if (router_reachable())
    {
        gemm_logf("pi_mcp: router already listening on %d",
                  GEMM_ROUTER_PORT_DEFAULT);
        return;
    }

    exe = find_helper("geany-pi-mcp-router");
    if (!exe)
    {
        gemm_logf("pi_mcp: geany-pi-mcp-router saknas - startar ingen router");
        return;
    }

    logdir = g_build_filename(g_get_user_config_dir(), "geany", "geany-pi",
                              NULL);
    g_mkdir_with_parents(logdir, 0700);
    logfile = g_build_filename(logdir, "mcp-router.log", NULL);

    cmd = g_strdup_printf(
        "/usr/bin/python3 %s >> %s 2>&1", exe, logfile);   /* daemon: fil-logg */

    argv[0] = (gchar*)"/bin/sh";
    argv[1] = (gchar*)"-c";
    argv[2] = cmd;
    argv[3] = NULL;

    if (!g_spawn_async(NULL, argv, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &pid, &err))
    {
        gemm_logf("pi_mcp: router spawn failed: %s",
                  err ? err->message : "?");
        if (err) g_error_free(err);
    }
    else
    {
        g_child_watch_add(pid, mcp_sync_child, NULL);
        gemm_logf("pi_mcp: router started (pid %d)", (int)pid);
    }

    g_free(cmd);
    g_free(exe);
    g_free(logdir);
    g_free(logfile);
}

/* Per-machine registration.                                          */
/*                                                                    */
/* Geany can open documents from other machines over gvfs/sftp:        */
/*   /run/user/1000/gvfs/sftp:host=192.168.11.145/usr/share/.../x.py   */
/* For Odoo development the useful MCP server is that machine's own    */
/* ai_pi_mcp (http://<host>:8069/mcp), which needs a token valid in     */
/* that machine's database. The `geany-pi-mcp-sync` helper resolves hosts,    */
/* tokens and merges one entry per machine into Pi's mcp.json.          */
/* ------------------------------------------------------------------ */

static void collect_doc_hosts(GPtrArray *out)
{
    guint i;

    if (!geany || !geany->documents_array)
        return;
    for (i = 0; i < geany->documents_array->len; i++)
    {
        GeanyDocument *d = g_ptr_array_index(geany->documents_array, i);
        const gchar *p, *e;
        gchar *host;

        if (!d || !d->file_name)
            continue;
        p = strstr(d->file_name, "sftp:host=");
        if (!p)
            continue;
        p += strlen("sftp:host=");
        for (e = p; *e && *e != '/' && *e != ',' && *e != '\\'; e++)
            ;
        if (e == p)
            continue;
        host = g_strndup(p, (gsize)(e - p));
        if (!g_ptr_array_find_with_equal_func(out, host, g_str_equal, NULL))
            g_ptr_array_add(out, host);
        else
            g_free(host);
    }
}

static void mcp_sync_child(GPid pid, gint status, gpointer data)
{
    (void)status;
    (void)data;
    g_spawn_close_pid(pid);
    gemm_logf("pi_mcp: sync helper finished");
}

void gemm_pi_mcp_sync(void)
{
    gchar *exe = find_helper("geany-pi-mcp-sync");
    GPtrArray *hosts;
    GPtrArray *argv;
    guint i;

    if (!exe)
    {
        gemm_logf("pi_mcp: geany-pi-mcp-sync saknas (make install / Salt)");
        return;
    }

    hosts = g_ptr_array_new_with_free_func(g_free);
    collect_doc_hosts(hosts);

    /* Build argv directly - no shell, no quoting, no temp log file. */
    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("/usr/bin/python3"));
    g_ptr_array_add(argv, g_strdup(exe));
    g_ptr_array_add(argv, g_strdup("--no-local"));
    g_ptr_array_add(argv, g_strdup("--no-hosts"));
    for (i = 0; i < hosts->len; i++)
    {
        g_ptr_array_add(argv, g_strdup("--host"));
        g_ptr_array_add(argv, g_strdup(g_ptr_array_index(hosts, i)));
    }
    g_ptr_array_add(argv, NULL);

    gemm_logf("pi_mcp: synkar %u dokumentvärd(ar)", hosts->len);
    spawn_capture("mcp-sync", (const gchar *const *)argv->pdata);

    g_ptr_array_free(argv, TRUE);
    g_ptr_array_free(hosts, TRUE);
    g_free(exe);
}

/* Run a little after startup, when session documents are loaded. */
static gboolean mcp_sync_idle(gpointer data)
{
    (void)data;
    gemm_pi_mcp_sync();
    return G_SOURCE_REMOVE;
}

void gemm_pi_mcp_sync_schedule(void)
{
    if (g_sync_source)
        return;
    g_sync_source = g_timeout_add_seconds(2, mcp_sync_idle, NULL);
}

/* Remove pending callbacks - MUST run at plugin_cleanup: a queued timeout
 * whose code lives in an unloaded module segfaults (see LSP plugin, which
 * calls plugin_module_make_resident() for the same reason). */
void gemm_pi_mcp_shutdown(void)
{
    if (g_sync_source)
    {
        g_source_remove(g_sync_source);
        g_sync_source = 0;
    }
}

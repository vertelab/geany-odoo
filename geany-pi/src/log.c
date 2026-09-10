/*
 * log.c - tiny append-only debug log.
 *
 * Writes timestamped lines to ~/.config/geany/geany-pi/log.txt.
 * Best effort; never fails the caller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

void gemm_logf(const gchar *fmt, ...)
{
    const gchar *cfg = g_getenv("XDG_CONFIG_HOME");
    const gchar *dir = cfg ? cfg : g_get_user_config_dir();
    gchar *pdir, *path, *msg, *ts, *line;
    GDateTime *now;
    gint fd;
    va_list ap;

    if (dir == NULL)
        return;
    pdir = g_build_filename(dir, "geany", "geany-pi", NULL);
    g_mkdir_with_parents(pdir, 0700);
    path = g_build_filename(pdir, "log.txt", NULL);
    g_free(pdir);

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    now = g_date_time_new_now_local();
    ts  = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    fd  = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
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
    g_free(path);
}

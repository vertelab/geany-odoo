/*
 * geany-mermaid.h - shared declarations for the geany-mermaid plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANY_MERMAID_H
#define GEANY_MERMAID_H

#include <geanyplugin.h>
#include <gtk/gtk.h>

extern GeanyData   *geany_data;
extern GeanyPlugin *geany_plugin;

void mmd_logf(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);

#endif /* GEANY_MERMAID_H */

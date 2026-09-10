/*
 * geany-pfiles.h - shared declarations for the geany-pfiles plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANY_PFILES_H
#define GEANY_PFILES_H

#include <geanyplugin.h>
#include <gtk/gtk.h>

extern GeanyData   *geany_data;
extern GeanyPlugin *geany_plugin;

void pfiles_logf(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);

#endif /* GEANY_PFILES_H */

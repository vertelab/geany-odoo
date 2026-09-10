/*
 * active.c - document/editor helpers for the current Geany document.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "geany-pi-common.h"

#include <string.h>

/* Scintilla editor for an explicit document (NULL => active document). */
static ScintillaObject *sci_of(GeanyDocument *doc)
{
    GeanyDocument *d = DOC_VALID(doc) ? doc : document_get_current();
    if (d == NULL)
        return NULL;
    return d->editor ? d->editor->sci : NULL;
}

/* Public: expose the resolution rule so bridge.c can report which
 * document a request actually landed on. */
GeanyDocument *gemm_active_resolve(GeanyDocument *doc)
{
    GeanyDocument *d = DOC_VALID(doc) ? doc : document_get_current();
    return DOC_VALID(d) ? d : NULL;
}

gboolean gemm_active_has_text(GeanyDocument *doc)
{
    ScintillaObject *sci = sci_of(doc);
    return (sci != NULL && sci_get_length(sci) > 0);
}

gboolean gemm_active_has_selection(GeanyDocument *doc)
{
    ScintillaObject *sci = sci_of(doc);
    if (sci == NULL)
        return FALSE;
    {
        gint s = (gint)sci_get_selection_start(sci);
        gint e = (gint)sci_get_selection_end(sci);
        return (e > s);
    }
}

/* Fetch the text to send.  For DOC request start is -1 (= whole buffer). */
gchar *gemm_active_get_text(GeanyDocument *doc,
                            GemmRequestKind kind,
                            gint *out_start, gint *out_end)
{
    ScintillaObject *sci = sci_of(doc);
    gint   start = -1, end = -1;
    char  *raw = NULL;

    if (sci == NULL)
        return NULL;

    if (kind == GEMM_REQUEST_SELECTION)
    {
        start = (gint)sci_get_selection_start(sci);
        end   = (gint)sci_get_selection_end(sci);
        if (end <= start)
            return NULL;
        raw = sci_get_contents_range(sci, start, end);
        if (raw == NULL)
            return NULL;
    }
    else /* whole buffer */
    {
        gint buflen = (gint)sci_get_length(sci);
        /* range-based fetch is unambiguous about start/end (no off-by-one
         * and no reliance on the buffer_len convention of sci_get_contents). */
        raw = sci_get_contents_range(sci, 0, buflen);
        end = buflen;
    }

    if (out_start) *out_start = start;
    if (out_end)   *out_end   = end;
    return g_utf8_make_valid(raw, -1);
}

/* Single replace: for a DOC request start==-1 replaces the whole buffer. */
void gemm_editor_replace(GeanyDocument *doc, gint start, gint end,
                         const gchar *new_text)
{
    ScintillaObject *sci = sci_of(doc);
    GeanyDocument *d = gemm_active_resolve(doc);
    if (sci == NULL)
        return;

    if (start < 0)
    {
        start = 0;
        end   = (gint)sci_get_length(sci);
    }

    /* keep highlight so the user can see exactly what will change */
    sci_set_selection_start(sci, start);
    sci_set_selection_end(sci, end);

    /* This single SciReplaceSel is recorded in the Scintilla undo stack:  */
    /* a lone Ctrl+Z after apply returns the file to the pre-edit state.   */
    sci_replace_sel(sci, new_text);

    /* Writing through the Sci wrapper does not necessarily raise the
     * "changed" state that Geany's tab indicator and close-prompt use.
     * Set it explicitly so a background edit is never silently lost. */
    if (d != NULL && !d->changed)
        document_set_text_changed(d, TRUE);
}

/* ---- thin wrappers for the ACTIVE document (panel/request callers) ---- */

gboolean gemm_active_has_text_active(void)
{
    return gemm_active_has_text(NULL);
}

gboolean gemm_active_has_selection_active(void)
{
    return gemm_active_has_selection(NULL);
}

gchar *gemm_active_get_text_active(GemmRequestKind kind,
                                   gint *out_start, gint *out_end)
{
    return gemm_active_get_text(NULL, kind, out_start, out_end);
}

void gemm_editor_replace_active(gint start, gint end, const gchar *new_text)
{
    gemm_editor_replace(NULL, start, end, new_text);
}

/* Small helper to push a message into the Geany status bar (or fall back
 * to g_print if the main window is not ready).                           */
void gemm_ui_status(const gchar *utf8_status)
{
    if (geany != NULL && geany->main_widgets != NULL)
        ui_set_statusbar(FALSE, "%s", utf8_status);
    else
        g_print("geany-pi: %s\n", utf8_status);
}

/*
 * Phi Markdown PDF export dialog
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_MARKDOWN_EXPORT_H
#define PDFV_MARKDOWN_EXPORT_H

#include "markdown-editor.h"
#include "pdfv-workspace.h"

G_BEGIN_DECLS

typedef void (*PdfvMarkdownExportSavedFunc)(GtkWidget *parent,
                                            gpointer user_data);

void pdfv_markdown_export_present(
    GtkWidget *parent, PdfvMarkdownEditor *editor, PdfvWorkspace *workspace,
    gboolean multiple, gboolean allow_remote_images, gdouble font_size,
    PdfvMarkdownExportSavedFunc saved_callback, gpointer saved_data);

G_END_DECLS

#endif

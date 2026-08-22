/*
 * Phi Markdown editor - native GTK/WebKit host
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_MARKDOWN_EDITOR_H
#define PDFV_MARKDOWN_EDITOR_H

#include <adwaita.h>

G_BEGIN_DECLS

#define PDFV_TYPE_MARKDOWN_EDITOR (pdfv_markdown_editor_get_type())
G_DECLARE_FINAL_TYPE(PdfvMarkdownEditor, pdfv_markdown_editor, PDFV,
                     MARKDOWN_EDITOR, GtkBox)

PdfvMarkdownEditor *pdfv_markdown_editor_new(GFile *vault_root);

void pdfv_markdown_editor_open_file_async(
    PdfvMarkdownEditor *self, GFile *file, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean pdfv_markdown_editor_open_file_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error);

void pdfv_markdown_editor_flush_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean pdfv_markdown_editor_flush_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error);

void pdfv_markdown_editor_save_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean pdfv_markdown_editor_save_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error);

void pdfv_markdown_editor_reload_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);

void pdfv_markdown_editor_run_command(PdfvMarkdownEditor *self,
                                      const gchar *command);
void pdfv_markdown_editor_reveal_fragment(PdfvMarkdownEditor *self,
                                          const gchar *target);
void pdfv_markdown_editor_set_theme(PdfvMarkdownEditor *self,
                                    gboolean dark, gdouble font_scale);
void pdfv_markdown_editor_set_remote_images_allowed(
    PdfvMarkdownEditor *self, gboolean allowed);
void pdfv_markdown_editor_set_readable_line_width(
    PdfvMarkdownEditor *self, gboolean enabled);
void pdfv_markdown_editor_set_latex_conceal(
    PdfvMarkdownEditor *self, gboolean enabled);
void pdfv_markdown_editor_set_snippets(PdfvMarkdownEditor *self,
                                       const gchar *snippets);
void pdfv_markdown_editor_set_attachment_folder(
    PdfvMarkdownEditor *self, GFile *folder);
void pdfv_markdown_editor_focus(PdfvMarkdownEditor *self);

gboolean pdfv_markdown_editor_get_dirty(PdfvMarkdownEditor *self);
gboolean pdfv_markdown_editor_get_ready(PdfvMarkdownEditor *self);
GFile *pdfv_markdown_editor_get_file(PdfvMarkdownEditor *self);
GFile *pdfv_markdown_editor_get_vault_root(PdfvMarkdownEditor *self);
const gchar *pdfv_markdown_editor_get_relative_path(
    PdfvMarkdownEditor *self);
GFile *pdfv_markdown_editor_resolve_new_note(PdfvMarkdownEditor *self,
                                             const gchar *target,
                                             GError **error);

G_END_DECLS

#endif

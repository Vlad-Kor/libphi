/*
 * Phi Markdown editor - native vault filesystem boundary
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_MARKDOWN_VAULT_ADAPTER_H
#define PDFV_MARKDOWN_VAULT_ADAPTER_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define PDFV_TYPE_MARKDOWN_VAULT_ADAPTER (pdfv_markdown_vault_adapter_get_type())
G_DECLARE_FINAL_TYPE(PdfvMarkdownVaultAdapter, pdfv_markdown_vault_adapter,
                     PDFV, MARKDOWN_VAULT_ADAPTER, GObject)

PdfvMarkdownVaultAdapter *pdfv_markdown_vault_adapter_new(GFile *root);
GFile *pdfv_markdown_vault_adapter_get_root(PdfvMarkdownVaultAdapter *self);

GFile *pdfv_markdown_vault_adapter_resolve(PdfvMarkdownVaultAdapter *self,
                                           const gchar *relative_path,
                                           GError **error);
GFile *pdfv_markdown_vault_adapter_resolve_note(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, GError **error);
GFile *pdfv_markdown_vault_adapter_resolve_attachment(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, gboolean relative_to_note, GError **error);
GFile *pdfv_markdown_vault_adapter_resolve_attachment_fast(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, gboolean relative_to_note);
GFile *pdfv_markdown_vault_adapter_resolve_new_note(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, GError **error);
gchar *pdfv_markdown_vault_adapter_relative_path(
    PdfvMarkdownVaultAdapter *self, GFile *file);

gchar *pdfv_markdown_vault_adapter_read_text(
    PdfvMarkdownVaultAdapter *self, GFile *file, gchar **etag,
    GError **error);
gchar *pdfv_markdown_vault_adapter_read_embed(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, gchar **resolved_path, GError **error);
/* Preview resolution may scan the vault. Run it outside the GTK thread.
 * The adapter is immutable after construction and the task owns its inputs. */
typedef struct {
  gchar *path;
  gchar *text; /* NULL for attachments. */
  GFile *file; /* NULL for note embeds. */
} PdfvMarkdownPreview;
void pdfv_markdown_preview_free(PdfvMarkdownPreview *preview);
void pdfv_markdown_vault_adapter_preview_async(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, gboolean embed, gboolean relative_to_note,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
PdfvMarkdownPreview *pdfv_markdown_vault_adapter_preview_finish(
    PdfvMarkdownVaultAdapter *self, GAsyncResult *result, GError **error);

GBytes *pdfv_markdown_vault_adapter_read_bytes(
    PdfvMarkdownVaultAdapter *self, const gchar *relative_path,
    gchar **content_type, GError **error);

GPtrArray *pdfv_markdown_vault_adapter_list_notes(
    PdfvMarkdownVaultAdapter *self, const gchar *query, GError **error);
GPtrArray *pdfv_markdown_vault_adapter_get_headings(
    PdfvMarkdownVaultAdapter *self, GFile *file, GError **error);
GPtrArray *pdfv_markdown_vault_adapter_get_blocks(
    PdfvMarkdownVaultAdapter *self, GFile *file, GError **error);

G_END_DECLS

#endif

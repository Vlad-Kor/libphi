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
GFile *pdfv_markdown_vault_adapter_resolve_new_note(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, GError **error);
gchar *pdfv_markdown_vault_adapter_relative_path(
    PdfvMarkdownVaultAdapter *self, GFile *file);

gchar *pdfv_markdown_vault_adapter_read_text(
    PdfvMarkdownVaultAdapter *self, GFile *file, gchar **etag,
    GError **error);
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

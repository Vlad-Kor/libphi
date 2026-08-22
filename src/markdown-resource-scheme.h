/*
 * Phi Markdown editor - WebKit app:// and vault:// resources
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_MARKDOWN_RESOURCE_SCHEME_H
#define PDFV_MARKDOWN_RESOURCE_SCHEME_H

#include "markdown-vault-adapter.h"

#include <webkit/webkit.h>

G_BEGIN_DECLS

#define PDFV_TYPE_MARKDOWN_RESOURCE_SCHEME (pdfv_markdown_resource_scheme_get_type())
G_DECLARE_FINAL_TYPE(PdfvMarkdownResourceScheme, pdfv_markdown_resource_scheme,
                     PDFV, MARKDOWN_RESOURCE_SCHEME, GObject)

PdfvMarkdownResourceScheme *pdfv_markdown_resource_scheme_new(
    PdfvMarkdownVaultAdapter *vault);
WebKitWebContext *pdfv_markdown_resource_scheme_get_context(
    PdfvMarkdownResourceScheme *self);
void pdfv_markdown_resource_scheme_bind_web_view(
    PdfvMarkdownResourceScheme *self, WebKitWebView *web_view);
gchar *pdfv_markdown_resource_scheme_load_default_snippets(GError **error);

G_END_DECLS

#endif

/*
 * Phi Markdown editor - versioned native/WebKit JSON bridge
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_MARKDOWN_EDITOR_BRIDGE_H
#define PDFV_MARKDOWN_EDITOR_BRIDGE_H

#include <json-glib/json-glib.h>
#include <webkit/webkit.h>

G_BEGIN_DECLS

#define PDFV_TYPE_MARKDOWN_EDITOR_BRIDGE (pdfv_markdown_editor_bridge_get_type())
G_DECLARE_FINAL_TYPE(PdfvMarkdownEditorBridge, pdfv_markdown_editor_bridge,
                     PDFV, MARKDOWN_EDITOR_BRIDGE, GObject)

PdfvMarkdownEditorBridge *pdfv_markdown_editor_bridge_new(
    WebKitWebView *web_view, WebKitUserContentManager *content_manager);

void pdfv_markdown_editor_bridge_send(PdfvMarkdownEditorBridge *self,
                                      const gchar *type, const gchar *id,
                                      JsonObject *payload);

G_END_DECLS

#endif

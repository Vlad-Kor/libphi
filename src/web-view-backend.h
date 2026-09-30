/*
 * Phi - contract between PdfvWebView and its platform backends
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A backend is a subclass of PdfvWebView that shows the page inside its own
 * allocation and implements the class methods below. It must:
 *
 *   - install window.phiHost (see web-view.h) before any page script runs, and
 *     report messages with pdfv_web_view_emit_script_message(),
 *   - serve every scheme from pdfv_web_view_get_uri_schemes() by passing a
 *     PdfvWebSchemeRequest to pdfv_web_view_handle_scheme_request(),
 *   - ask pdfv_web_view_emit_decide_navigation() before navigating any frame,
 *     and never open new windows,
 *   - offer its context menu to pdfv_web_view_emit_context_menu(),
 *   - deny permission requests,
 *   - pass keyboard focus between the page and GTK when either grabs it.
 *
 * Each platform provides pdfv_web_view_backend_get_type(), which
 * pdfv_web_view_new() instantiates.
 */

#ifndef PDFV_WEB_VIEW_BACKEND_H
#define PDFV_WEB_VIEW_BACKEND_H

#include "web-view.h"

G_BEGIN_DECLS

struct _PdfvWebViewClass {
  GtkWidgetClass parent_class;

  /* @related is NULL or another view; called once, right after construction. */
  void (*setup)(PdfvWebView *self, PdfvWebView *related);
  void (*load_uri)(PdfvWebView *self, const gchar *uri);
  void (*set_background_color)(PdfvWebView *self, const GdkRGBA *color);
  void (*set_developer_extras_enabled)(PdfvWebView *self, gboolean enabled);
  /* These complete @task with a boolean or an error. */
  void (*run_javascript)(PdfvWebView *self, const gchar *script,
                         const gchar *source_uri, GTask *task);
  void (*print_to_pdf)(PdfvWebView *self, const gchar *filename,
                       GtkPageSetup *page_setup, GTask *task);
};

GType pdfv_web_view_backend_get_type(void);

/* Scheme handling. */
const gchar *const *pdfv_web_view_get_uri_schemes(void);

typedef void (*PdfvWebSchemeRequestFinishFunc)(gpointer backend_data,
                                               GInputStream *stream,
                                               gint64 length,
                                               const gchar *content_type,
                                               const GError *error);

/* @finish is called exactly once, on the main thread, with either a stream
 * or an error. A request dropped without an answer finishes with an error. */
PdfvWebSchemeRequest *pdfv_web_scheme_request_new(
    PdfvWebView *web_view, const gchar *uri,
    PdfvWebSchemeRequestFinishFunc finish, gpointer backend_data,
    GDestroyNotify backend_data_destroy);
void pdfv_web_view_handle_scheme_request(PdfvWebSchemeRequest *request);

/* Signals. */
void pdfv_web_view_emit_script_message(PdfvWebView *self,
                                       const gchar *channel,
                                       const gchar *message);
gboolean pdfv_web_view_emit_decide_navigation(PdfvWebView *self,
                                              const gchar *uri,
                                              gboolean user_gesture,
                                              gboolean new_window);
/* Returns TRUE when no menu should be shown. */
gboolean pdfv_web_view_emit_context_menu(PdfvWebView *self,
                                         PdfvWebContextMenu *menu);

/* Context menu model. Items taken from an engine menu keep an engine handle;
 * items made by the application have a GAction or a stock action only. */
PdfvWebContextMenu *pdfv_web_context_menu_new(const gchar *image_uri);
PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_native(
    PdfvWebContextMenuAction action, gpointer handle, GDestroyNotify destroy);
gpointer pdfv_web_context_menu_item_get_native(PdfvWebContextMenuItem *self);
GAction *pdfv_web_context_menu_item_get_gaction(PdfvWebContextMenuItem *self);
GVariant *pdfv_web_context_menu_item_get_target(PdfvWebContextMenuItem *self);
const gchar *pdfv_web_context_menu_item_get_label(
    PdfvWebContextMenuItem *self);

G_END_DECLS

#endif

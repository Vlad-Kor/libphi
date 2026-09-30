/*
 * Phi - platform independent embedded web view
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * PdfvWebView hosts the Markdown editor and the PDF export preview. It exposes
 * only what these pages need from a browser engine, so each platform provides
 * a small backend instead of the application depending on one engine:
 *
 *   - WebKitGTK (web-view-webkit.c) on Linux and other Unix systems,
 *   - WebView2 (web-view-webview2.cpp) on Windows.
 *
 * web-view-backend.h describes what a new backend has to implement.
 *
 * Pages talk to the application through window.phiHost, which every backend
 * installs before any page script runs:
 *
 *   window.phiHost.postMessage(channel, message)
 *
 * posts the string @message to the "script-message::channel" signal. The
 * application answers with pdfv_web_view_run_javascript().
 *
 *   window.phiHost.toolkitScrollbars
 *
 * is false if the engine draws its platform's scrollbars rather than ones
 * that look like the toolkit's, so that the page draws its own.
 */

#ifndef PDFV_WEB_VIEW_H
#define PDFV_WEB_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Scheme requests --------------------------------------------------------- */

#define PDFV_TYPE_WEB_SCHEME_REQUEST (pdfv_web_scheme_request_get_type())
G_DECLARE_FINAL_TYPE(PdfvWebSchemeRequest, pdfv_web_scheme_request, PDFV,
                     WEB_SCHEME_REQUEST, GObject)

typedef struct _PdfvWebView PdfvWebView;
typedef struct _PdfvWebViewClass PdfvWebViewClass;

/* Called on the main thread. The request may be finished later, from any
 * main-loop callback, but must be finished exactly once. */
typedef void (*PdfvWebSchemeRequestFunc)(PdfvWebSchemeRequest *request,
                                         gpointer user_data);

const gchar *pdfv_web_scheme_request_get_uri(PdfvWebSchemeRequest *self);
PdfvWebView *pdfv_web_scheme_request_get_web_view(
    PdfvWebSchemeRequest *self);
void pdfv_web_scheme_request_finish(PdfvWebSchemeRequest *self,
                                    GInputStream *stream, gint64 length,
                                    const gchar *content_type);
void pdfv_web_scheme_request_finish_error(PdfvWebSchemeRequest *self,
                                          GError *error);

/* Context menus ----------------------------------------------------------- */

typedef enum {
  PDFV_WEB_CONTEXT_MENU_ACTION_NONE,
  PDFV_WEB_CONTEXT_MENU_ACTION_CUSTOM,
  PDFV_WEB_CONTEXT_MENU_ACTION_GO_BACK,
  PDFV_WEB_CONTEXT_MENU_ACTION_GO_FORWARD,
  PDFV_WEB_CONTEXT_MENU_ACTION_STOP,
  PDFV_WEB_CONTEXT_MENU_ACTION_RELOAD,
  PDFV_WEB_CONTEXT_MENU_ACTION_CUT,
  PDFV_WEB_CONTEXT_MENU_ACTION_COPY,
  PDFV_WEB_CONTEXT_MENU_ACTION_PASTE,
  PDFV_WEB_CONTEXT_MENU_ACTION_DELETE,
  PDFV_WEB_CONTEXT_MENU_ACTION_SELECT_ALL,
  PDFV_WEB_CONTEXT_MENU_ACTION_INSERT_EMOJI,
  PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD,
  PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD,
  PDFV_WEB_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW,
  PDFV_WEB_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK,
  /* Engine specific entries without an equivalent above. */
  PDFV_WEB_CONTEXT_MENU_ACTION_OTHER,
} PdfvWebContextMenuAction;

#define PDFV_TYPE_WEB_CONTEXT_MENU_ITEM (pdfv_web_context_menu_item_get_type())
G_DECLARE_FINAL_TYPE(PdfvWebContextMenuItem, pdfv_web_context_menu_item, PDFV,
                     WEB_CONTEXT_MENU_ITEM, GObject)

PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_from_gaction(
    GAction *action, const gchar *label, GVariant *target);
PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_from_stock_action(
    PdfvWebContextMenuAction action);
PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_separator(void);
PdfvWebContextMenuAction pdfv_web_context_menu_item_get_stock_action(
    PdfvWebContextMenuItem *self);
gboolean pdfv_web_context_menu_item_is_separator(
    PdfvWebContextMenuItem *self);

#define PDFV_TYPE_WEB_CONTEXT_MENU (pdfv_web_context_menu_get_type())
G_DECLARE_FINAL_TYPE(PdfvWebContextMenu, pdfv_web_context_menu, PDFV,
                     WEB_CONTEXT_MENU, GObject)

/* The menu takes ownership of floating items and adds a reference to others. */
GList *pdfv_web_context_menu_get_items(PdfvWebContextMenu *self);
void pdfv_web_context_menu_append(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item);
void pdfv_web_context_menu_insert(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item,
                                  gint position);
void pdfv_web_context_menu_remove(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item);
void pdfv_web_context_menu_remove_all(PdfvWebContextMenu *self);
/* The address of the image under the pointer, or NULL. */
const gchar *pdfv_web_context_menu_get_image_uri(PdfvWebContextMenu *self);

/* Web view ---------------------------------------------------------------- */

/* Signals:
 *
 *   script-message::channel (const gchar *message)
 *     The page called window.phiHost.postMessage(channel, message).
 *
 *   gboolean decide-navigation (const gchar *uri, gboolean user_gesture,
 *                               gboolean new_window)
 *     A frame is about to navigate, or the page asked for a new window.
 *     Return TRUE to block the navigation. New windows are never opened.
 *
 *   gboolean context-menu (PdfvWebContextMenu *menu)
 *     Edit @menu before it is shown, or return TRUE to show no menu.
 */
#define PDFV_TYPE_WEB_VIEW (pdfv_web_view_get_type())
G_DECLARE_DERIVABLE_TYPE(PdfvWebView, pdfv_web_view, PDFV, WEB_VIEW, GtkWidget)

/* Serves @scheme URIs in every web view. Register schemes before creating the
 * first view. Registered schemes are secure, local and may be used with CORS
 * by pages served from registered schemes. */
void pdfv_web_view_register_uri_scheme(const gchar *scheme,
                                       PdfvWebSchemeRequestFunc callback,
                                       gpointer user_data);

/* Views with a @related view share its web process where the engine supports
 * that. Every view denies permission requests and never opens new windows. */
PdfvWebView *pdfv_web_view_new(PdfvWebView *related);

void pdfv_web_view_load_uri(PdfvWebView *self, const gchar *uri);
void pdfv_web_view_set_background_color(PdfvWebView *self,
                                        const GdkRGBA *color);
void pdfv_web_view_set_developer_extras_enabled(PdfvWebView *self,
                                                gboolean enabled);

/* Messages to the page. Scripts run in order; the result is not returned. */
void pdfv_web_view_run_javascript(PdfvWebView *self, const gchar *script,
                                  const gchar *source_uri,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
gboolean pdfv_web_view_run_javascript_finish(PdfvWebView *self,
                                             GAsyncResult *result,
                                             GError **error);

/* Prints the page to a PDF file at @filename using the paper size and
 * margins of @page_setup. Backgrounds are printed; no headers or footers. */
void pdfv_web_view_print_to_pdf(PdfvWebView *self, const gchar *filename,
                                GtkPageSetup *page_setup,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data);
gboolean pdfv_web_view_print_to_pdf_finish(PdfvWebView *self,
                                           GAsyncResult *result,
                                           GError **error);

G_END_DECLS

#endif

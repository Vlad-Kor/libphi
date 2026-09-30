/*
 * Phi - platform independent embedded web view
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phi-web-view"

#include "web-view-backend.h"

/* Scheme registry --------------------------------------------------------- */

typedef struct {
  PdfvWebSchemeRequestFunc callback;
  gpointer user_data;
} SchemeHandler;

static GHashTable *scheme_handlers; /* scheme -> SchemeHandler */
static GPtrArray *scheme_names;     /* NULL-terminated */

void pdfv_web_view_register_uri_scheme(const gchar *scheme,
                                       PdfvWebSchemeRequestFunc callback,
                                       gpointer user_data) {
  g_return_if_fail(scheme && *scheme);
  g_return_if_fail(callback != NULL);
  if (!scheme_handlers) {
    scheme_handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);
    scheme_names = g_ptr_array_new();
    g_ptr_array_add(scheme_names, NULL);
  }
  SchemeHandler *handler = g_new0(SchemeHandler, 1);
  handler->callback = callback;
  handler->user_data = user_data;
  if (!g_hash_table_contains(scheme_handlers, scheme)) {
    g_ptr_array_insert(scheme_names, (gint)scheme_names->len - 1,
                       g_strdup(scheme));
  }
  g_hash_table_insert(scheme_handlers, g_strdup(scheme), handler);
}

const gchar *const *pdfv_web_view_get_uri_schemes(void) {
  static const gchar *const none[] = {NULL};
  return scheme_names ? (const gchar *const *)scheme_names->pdata : none;
}

/* Scheme requests --------------------------------------------------------- */

struct _PdfvWebSchemeRequest {
  GObject parent_instance;
  GWeakRef web_view;
  gchar *uri;
  PdfvWebSchemeRequestFinishFunc finish;
  gpointer backend_data;
  GDestroyNotify backend_data_destroy;
  gboolean finished;
};

G_DEFINE_FINAL_TYPE(PdfvWebSchemeRequest, pdfv_web_scheme_request,
                    G_TYPE_OBJECT)

static void scheme_request_complete(PdfvWebSchemeRequest *self,
                                    GInputStream *stream, gint64 length,
                                    const gchar *content_type,
                                    const GError *error) {
  if (self->finished) {
    g_warning("Scheme request for %s was finished twice", self->uri);
    return;
  }
  self->finished = TRUE;
  self->finish(self->backend_data, stream, length, content_type, error);
}

static void pdfv_web_scheme_request_finalize(GObject *object) {
  PdfvWebSchemeRequest *self = PDFV_WEB_SCHEME_REQUEST(object);
  if (!self->finished) {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Request for %s was not answered", self->uri);
    scheme_request_complete(self, NULL, -1, NULL, error);
    g_error_free(error);
  }
  if (self->backend_data_destroy)
    self->backend_data_destroy(self->backend_data);
  g_weak_ref_clear(&self->web_view);
  g_free(self->uri);
  G_OBJECT_CLASS(pdfv_web_scheme_request_parent_class)->finalize(object);
}

static void pdfv_web_scheme_request_class_init(
    PdfvWebSchemeRequestClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_web_scheme_request_finalize;
}

static void pdfv_web_scheme_request_init(PdfvWebSchemeRequest *self) {
  g_weak_ref_init(&self->web_view, NULL);
}

PdfvWebSchemeRequest *pdfv_web_scheme_request_new(
    PdfvWebView *web_view, const gchar *uri,
    PdfvWebSchemeRequestFinishFunc finish, gpointer backend_data,
    GDestroyNotify backend_data_destroy) {
  g_return_val_if_fail(uri != NULL, NULL);
  g_return_val_if_fail(finish != NULL, NULL);
  PdfvWebSchemeRequest *self = g_object_new(PDFV_TYPE_WEB_SCHEME_REQUEST,
                                            NULL);
  g_weak_ref_set(&self->web_view, web_view);
  self->uri = g_strdup(uri);
  self->finish = finish;
  self->backend_data = backend_data;
  self->backend_data_destroy = backend_data_destroy;
  return self;
}

const gchar *pdfv_web_scheme_request_get_uri(PdfvWebSchemeRequest *self) {
  g_return_val_if_fail(PDFV_IS_WEB_SCHEME_REQUEST(self), NULL);
  return self->uri;
}

PdfvWebView *pdfv_web_scheme_request_get_web_view(
    PdfvWebSchemeRequest *self) {
  g_return_val_if_fail(PDFV_IS_WEB_SCHEME_REQUEST(self), NULL);
  PdfvWebView *web_view = g_weak_ref_get(&self->web_view);
  /* The request keeps no strong reference; views outlive their requests
   * while they are handled on the main thread. */
  if (web_view)
    g_object_unref(web_view);
  return web_view;
}

void pdfv_web_scheme_request_finish(PdfvWebSchemeRequest *self,
                                    GInputStream *stream, gint64 length,
                                    const gchar *content_type) {
  g_return_if_fail(PDFV_IS_WEB_SCHEME_REQUEST(self));
  g_return_if_fail(G_IS_INPUT_STREAM(stream));
  scheme_request_complete(self, stream, length, content_type, NULL);
}

void pdfv_web_scheme_request_finish_error(PdfvWebSchemeRequest *self,
                                          GError *error) {
  g_return_if_fail(PDFV_IS_WEB_SCHEME_REQUEST(self));
  g_return_if_fail(error != NULL);
  scheme_request_complete(self, NULL, -1, NULL, error);
}

void pdfv_web_view_handle_scheme_request(PdfvWebSchemeRequest *request) {
  g_return_if_fail(PDFV_IS_WEB_SCHEME_REQUEST(request));
  gchar *scheme = g_uri_parse_scheme(request->uri);
  SchemeHandler *handler = scheme && scheme_handlers
      ? g_hash_table_lookup(scheme_handlers, scheme) : NULL;
  if (handler) {
    handler->callback(request, handler->user_data);
  } else {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Unsupported scheme in %s", request->uri);
    pdfv_web_scheme_request_finish_error(request, error);
    g_error_free(error);
  }
  g_free(scheme);
}

/* Context menus ----------------------------------------------------------- */

struct _PdfvWebContextMenuItem {
  GInitiallyUnowned parent_instance;
  PdfvWebContextMenuAction action;
  gboolean separator;
  gpointer native;
  GDestroyNotify native_destroy;
  GAction *gaction;
  GVariant *target;
  gchar *label;
};

G_DEFINE_FINAL_TYPE(PdfvWebContextMenuItem, pdfv_web_context_menu_item,
                    G_TYPE_INITIALLY_UNOWNED)

static void pdfv_web_context_menu_item_finalize(GObject *object) {
  PdfvWebContextMenuItem *self = PDFV_WEB_CONTEXT_MENU_ITEM(object);
  if (self->native_destroy && self->native)
    self->native_destroy(self->native);
  g_clear_object(&self->gaction);
  g_clear_pointer(&self->target, g_variant_unref);
  g_free(self->label);
  G_OBJECT_CLASS(pdfv_web_context_menu_item_parent_class)->finalize(object);
}

static void pdfv_web_context_menu_item_class_init(
    PdfvWebContextMenuItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_web_context_menu_item_finalize;
}

static void pdfv_web_context_menu_item_init(PdfvWebContextMenuItem *self) {
  (void)self;
}

PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_from_gaction(
    GAction *action, const gchar *label, GVariant *target) {
  g_return_val_if_fail(G_IS_ACTION(action), NULL);
  PdfvWebContextMenuItem *self = g_object_new(
      PDFV_TYPE_WEB_CONTEXT_MENU_ITEM, NULL);
  self->action = PDFV_WEB_CONTEXT_MENU_ACTION_CUSTOM;
  self->gaction = g_object_ref(action);
  self->target = target ? g_variant_ref_sink(target) : NULL;
  self->label = g_strdup(label);
  return self;
}

PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_from_stock_action(
    PdfvWebContextMenuAction action) {
  g_return_val_if_fail(action > PDFV_WEB_CONTEXT_MENU_ACTION_CUSTOM &&
                       action < PDFV_WEB_CONTEXT_MENU_ACTION_OTHER, NULL);
  PdfvWebContextMenuItem *self = g_object_new(
      PDFV_TYPE_WEB_CONTEXT_MENU_ITEM, NULL);
  self->action = action;
  return self;
}

PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_separator(void) {
  PdfvWebContextMenuItem *self = g_object_new(
      PDFV_TYPE_WEB_CONTEXT_MENU_ITEM, NULL);
  self->separator = TRUE;
  return self;
}

PdfvWebContextMenuItem *pdfv_web_context_menu_item_new_native(
    PdfvWebContextMenuAction action, gpointer handle, GDestroyNotify destroy) {
  PdfvWebContextMenuItem *self = g_object_new(
      PDFV_TYPE_WEB_CONTEXT_MENU_ITEM, NULL);
  self->action = action;
  self->native = handle;
  self->native_destroy = destroy;
  return self;
}

PdfvWebContextMenuAction pdfv_web_context_menu_item_get_stock_action(
    PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self),
                       PDFV_WEB_CONTEXT_MENU_ACTION_NONE);
  return self->action;
}

gboolean pdfv_web_context_menu_item_is_separator(
    PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self), FALSE);
  return self->separator;
}

gpointer pdfv_web_context_menu_item_get_native(PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self), NULL);
  return self->native;
}

GAction *pdfv_web_context_menu_item_get_gaction(PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self), NULL);
  return self->gaction;
}

GVariant *pdfv_web_context_menu_item_get_target(PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self), NULL);
  return self->target;
}

const gchar *pdfv_web_context_menu_item_get_label(
    PdfvWebContextMenuItem *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(self), NULL);
  return self->label;
}

struct _PdfvWebContextMenu {
  GObject parent_instance;
  GList *items;
  gchar *image_uri;
};

G_DEFINE_FINAL_TYPE(PdfvWebContextMenu, pdfv_web_context_menu, G_TYPE_OBJECT)

static void pdfv_web_context_menu_finalize(GObject *object) {
  PdfvWebContextMenu *self = PDFV_WEB_CONTEXT_MENU(object);
  g_list_free_full(self->items, g_object_unref);
  g_free(self->image_uri);
  G_OBJECT_CLASS(pdfv_web_context_menu_parent_class)->finalize(object);
}

static void pdfv_web_context_menu_class_init(PdfvWebContextMenuClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_web_context_menu_finalize;
}

static void pdfv_web_context_menu_init(PdfvWebContextMenu *self) {
  (void)self;
}

PdfvWebContextMenu *pdfv_web_context_menu_new(const gchar *image_uri) {
  PdfvWebContextMenu *self = g_object_new(PDFV_TYPE_WEB_CONTEXT_MENU, NULL);
  self->image_uri = g_strdup(image_uri);
  return self;
}

GList *pdfv_web_context_menu_get_items(PdfvWebContextMenu *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU(self), NULL);
  return self->items;
}

void pdfv_web_context_menu_insert(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item,
                                  gint position) {
  g_return_if_fail(PDFV_IS_WEB_CONTEXT_MENU(self));
  g_return_if_fail(PDFV_IS_WEB_CONTEXT_MENU_ITEM(item));
  self->items = g_list_insert(self->items, g_object_ref_sink(item), position);
}

void pdfv_web_context_menu_append(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item) {
  pdfv_web_context_menu_insert(self, item, -1);
}

void pdfv_web_context_menu_remove(PdfvWebContextMenu *self,
                                  PdfvWebContextMenuItem *item) {
  g_return_if_fail(PDFV_IS_WEB_CONTEXT_MENU(self));
  GList *link = g_list_find(self->items, item);
  if (!link)
    return;
  self->items = g_list_delete_link(self->items, link);
  g_object_unref(item);
}

void pdfv_web_context_menu_remove_all(PdfvWebContextMenu *self) {
  g_return_if_fail(PDFV_IS_WEB_CONTEXT_MENU(self));
  g_list_free_full(g_steal_pointer(&self->items), g_object_unref);
}

const gchar *pdfv_web_context_menu_get_image_uri(PdfvWebContextMenu *self) {
  g_return_val_if_fail(PDFV_IS_WEB_CONTEXT_MENU(self), NULL);
  return self->image_uri;
}

/* Web view ---------------------------------------------------------------- */

enum {
  SIGNAL_SCRIPT_MESSAGE,
  SIGNAL_DECIDE_NAVIGATION,
  SIGNAL_CONTEXT_MENU,
  N_SIGNALS,
};

static guint web_view_signals[N_SIGNALS];

G_DEFINE_ABSTRACT_TYPE(PdfvWebView, pdfv_web_view, GTK_TYPE_WIDGET)

static void pdfv_web_view_class_init(PdfvWebViewClass *klass) {
  web_view_signals[SIGNAL_SCRIPT_MESSAGE] = g_signal_new(
      "script-message", G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
  web_view_signals[SIGNAL_DECIDE_NAVIGATION] = g_signal_new(
      "decide-navigation", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
      g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 3,
      G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  web_view_signals[SIGNAL_CONTEXT_MENU] = g_signal_new(
      "context-menu", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
      g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 1,
      PDFV_TYPE_WEB_CONTEXT_MENU);
}

static void pdfv_web_view_init(PdfvWebView *self) {
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

void pdfv_web_view_emit_script_message(PdfvWebView *self,
                                       const gchar *channel,
                                       const gchar *message) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  g_signal_emit(self, web_view_signals[SIGNAL_SCRIPT_MESSAGE],
                g_quark_from_string(channel), message);
}

gboolean pdfv_web_view_emit_decide_navigation(PdfvWebView *self,
                                              const gchar *uri,
                                              gboolean user_gesture,
                                              gboolean new_window) {
  g_return_val_if_fail(PDFV_IS_WEB_VIEW(self), TRUE);
  gboolean blocked = FALSE;
  g_signal_emit(self, web_view_signals[SIGNAL_DECIDE_NAVIGATION], 0, uri,
                user_gesture, new_window, &blocked);
  return blocked;
}

gboolean pdfv_web_view_emit_context_menu(PdfvWebView *self,
                                         PdfvWebContextMenu *menu) {
  g_return_val_if_fail(PDFV_IS_WEB_VIEW(self), TRUE);
  gboolean suppressed = FALSE;
  g_signal_emit(self, web_view_signals[SIGNAL_CONTEXT_MENU], 0, menu,
                &suppressed);
  return suppressed;
}

PdfvWebView *pdfv_web_view_new(PdfvWebView *related) {
  g_return_val_if_fail(!related || PDFV_IS_WEB_VIEW(related), NULL);
  PdfvWebView *self = g_object_new(pdfv_web_view_backend_get_type(), NULL);
  PDFV_WEB_VIEW_GET_CLASS(self)->setup(self, related);
  return self;
}

void pdfv_web_view_load_uri(PdfvWebView *self, const gchar *uri) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  g_return_if_fail(uri != NULL);
  PDFV_WEB_VIEW_GET_CLASS(self)->load_uri(self, uri);
}

void pdfv_web_view_set_background_color(PdfvWebView *self,
                                        const GdkRGBA *color) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  g_return_if_fail(color != NULL);
  PDFV_WEB_VIEW_GET_CLASS(self)->set_background_color(self, color);
}

void pdfv_web_view_set_developer_extras_enabled(PdfvWebView *self,
                                                gboolean enabled) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  PDFV_WEB_VIEW_GET_CLASS(self)->set_developer_extras_enabled(self, enabled);
}

void pdfv_web_view_run_javascript(PdfvWebView *self, const gchar *script,
                                  const gchar *source_uri,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  g_return_if_fail(script != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_web_view_run_javascript);
  PDFV_WEB_VIEW_GET_CLASS(self)->run_javascript(self, script, source_uri,
                                                 task);
}

gboolean pdfv_web_view_run_javascript_finish(PdfvWebView *self,
                                             GAsyncResult *result,
                                             GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void pdfv_web_view_print_to_pdf(PdfvWebView *self, const gchar *filename,
                                GtkPageSetup *page_setup,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
  g_return_if_fail(PDFV_IS_WEB_VIEW(self));
  g_return_if_fail(filename != NULL);
  g_return_if_fail(GTK_IS_PAGE_SETUP(page_setup));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_web_view_print_to_pdf);
  PDFV_WEB_VIEW_GET_CLASS(self)->print_to_pdf(self, filename, page_setup,
                                               task);
}

gboolean pdfv_web_view_print_to_pdf_finish(PdfvWebView *self,
                                           GAsyncResult *result,
                                           GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

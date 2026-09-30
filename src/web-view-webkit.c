/*
 * Phi - WebKitGTK backend of PdfvWebView
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phi-web-view"

#include "web-view-backend.h"

#include <webkit/webkit.h>

#define PDFV_TYPE_WEBKIT_VIEW (pdfv_webkit_view_get_type())
G_DECLARE_FINAL_TYPE(PdfvWebKitView, pdfv_webkit_view, PDFV, WEBKIT_VIEW,
                     PdfvWebView)

struct _PdfvWebKitView {
  PdfvWebView parent_instance;
  WebKitWebView *web_view;
  WebKitUserContentManager *content_manager;
};

G_DEFINE_FINAL_TYPE(PdfvWebKitView, pdfv_webkit_view, PDFV_TYPE_WEB_VIEW)

GType pdfv_web_view_backend_get_type(void) {
  return PDFV_TYPE_WEBKIT_VIEW;
}

/* The channel of every message is registered as a script message handler. */
#define HOST_CHANNEL "native"

static const gchar host_script[] =
    "window.phiHost = Object.freeze({"
    "  postMessage(channel, message) {"
    "    window.webkit.messageHandlers[channel].postMessage(String(message));"
    "  }"
    "});";

/* All views share a context so WebKit can reuse its web process, compiled
 * JavaScript and resource cache. */
static WebKitWebContext *shared_context;

static void finish_scheme_request(gpointer backend_data, GInputStream *stream,
                                  gint64 length, const gchar *content_type,
                                  const GError *error) {
  WebKitURISchemeRequest *request = backend_data;
  if (stream)
    webkit_uri_scheme_request_finish(request, stream, length, content_type);
  else
    webkit_uri_scheme_request_finish_error(request, (GError *)error);
}

static void on_scheme_request(WebKitURISchemeRequest *request,
                              gpointer user_data) {
  (void)user_data;
  WebKitWebView *web_view = webkit_uri_scheme_request_get_web_view(request);
  PdfvWebView *view = web_view
      ? g_object_get_data(G_OBJECT(web_view), "phi-web-view") : NULL;
  PdfvWebSchemeRequest *wrapped = pdfv_web_scheme_request_new(
      view, webkit_uri_scheme_request_get_uri(request), finish_scheme_request,
      g_object_ref(request), g_object_unref);
  pdfv_web_view_handle_scheme_request(wrapped);
  g_object_unref(wrapped);
}

static WebKitWebContext *get_shared_context(void) {
  if (shared_context)
    return shared_context;
  shared_context = webkit_web_context_new();
  webkit_web_context_set_cache_model(shared_context,
                                     WEBKIT_CACHE_MODEL_WEB_BROWSER);
  WebKitSecurityManager *security =
      webkit_web_context_get_security_manager(shared_context);
  for (const gchar *const *scheme = pdfv_web_view_get_uri_schemes(); *scheme;
       scheme++) {
    webkit_web_context_register_uri_scheme(shared_context, *scheme,
                                           on_scheme_request, NULL, NULL);
    webkit_security_manager_register_uri_scheme_as_local(security, *scheme);
    webkit_security_manager_register_uri_scheme_as_secure(security, *scheme);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                                 *scheme);
  }
  return shared_context;
}

static void on_script_message(WebKitUserContentManager *manager,
                              JSCValue *value, PdfvWebKitView *self) {
  (void)manager;
  gchar *message = jsc_value_is_string(value) ? jsc_value_to_string(value)
                                              : jsc_value_to_json(value, 0);
  pdfv_web_view_emit_script_message(PDFV_WEB_VIEW(self), HOST_CHANNEL,
                                    message ? message : "");
  g_free(message);
}

static gboolean on_decide_policy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 PdfvWebKitView *self) {
  (void)web_view;
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;
  WebKitNavigationAction *action =
      webkit_navigation_policy_decision_get_navigation_action(
          WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const gchar *uri = request ? webkit_uri_request_get_uri(request) : NULL;
  if (!pdfv_web_view_emit_decide_navigation(
          PDFV_WEB_VIEW(self), uri ? uri : "",
          webkit_navigation_action_is_user_gesture(action),
          type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION))
    return FALSE;
  webkit_policy_decision_ignore(decision);
  return TRUE;
}

static gboolean on_permission_request(WebKitWebView *web_view,
                                      WebKitPermissionRequest *request,
                                      PdfvWebKitView *self) {
  (void)web_view;
  (void)self;
  webkit_permission_request_deny(request);
  return TRUE;
}

static const struct {
  WebKitContextMenuAction webkit;
  PdfvWebContextMenuAction action;
} context_actions[] = {
    {WEBKIT_CONTEXT_MENU_ACTION_GO_BACK, PDFV_WEB_CONTEXT_MENU_ACTION_GO_BACK},
    {WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD,
     PDFV_WEB_CONTEXT_MENU_ACTION_GO_FORWARD},
    {WEBKIT_CONTEXT_MENU_ACTION_STOP, PDFV_WEB_CONTEXT_MENU_ACTION_STOP},
    {WEBKIT_CONTEXT_MENU_ACTION_RELOAD, PDFV_WEB_CONTEXT_MENU_ACTION_RELOAD},
    {WEBKIT_CONTEXT_MENU_ACTION_CUT, PDFV_WEB_CONTEXT_MENU_ACTION_CUT},
    {WEBKIT_CONTEXT_MENU_ACTION_COPY, PDFV_WEB_CONTEXT_MENU_ACTION_COPY},
    {WEBKIT_CONTEXT_MENU_ACTION_PASTE, PDFV_WEB_CONTEXT_MENU_ACTION_PASTE},
    {WEBKIT_CONTEXT_MENU_ACTION_DELETE, PDFV_WEB_CONTEXT_MENU_ACTION_DELETE},
    {WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL,
     PDFV_WEB_CONTEXT_MENU_ACTION_SELECT_ALL},
    {WEBKIT_CONTEXT_MENU_ACTION_INSERT_EMOJI,
     PDFV_WEB_CONTEXT_MENU_ACTION_INSERT_EMOJI},
    {WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD,
     PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD},
    {WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD,
     PDFV_WEB_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD},
    {WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW,
     PDFV_WEB_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW},
    {WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK,
     PDFV_WEB_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK},
};

static PdfvWebContextMenuAction action_from_webkit(
    WebKitContextMenuAction webkit) {
  if (webkit == WEBKIT_CONTEXT_MENU_ACTION_CUSTOM)
    return PDFV_WEB_CONTEXT_MENU_ACTION_CUSTOM;
  if (webkit == WEBKIT_CONTEXT_MENU_ACTION_NO_ACTION)
    return PDFV_WEB_CONTEXT_MENU_ACTION_NONE;
  for (guint i = 0; i < G_N_ELEMENTS(context_actions); i++) {
    if (context_actions[i].webkit == webkit)
      return context_actions[i].action;
  }
  return PDFV_WEB_CONTEXT_MENU_ACTION_OTHER;
}

/* Returns a full reference to the WebKit item that shows @item. */
static WebKitContextMenuItem *webkit_item_for(PdfvWebContextMenuItem *item) {
  WebKitContextMenuItem *native = pdfv_web_context_menu_item_get_native(item);
  if (native)
    return g_object_ref(native);
  if (pdfv_web_context_menu_item_is_separator(item))
    return g_object_ref_sink(webkit_context_menu_item_new_separator());
  GAction *action = pdfv_web_context_menu_item_get_gaction(item);
  if (action) {
    return g_object_ref_sink(webkit_context_menu_item_new_from_gaction(
        action, pdfv_web_context_menu_item_get_label(item),
        pdfv_web_context_menu_item_get_target(item)));
  }
  PdfvWebContextMenuAction stock =
      pdfv_web_context_menu_item_get_stock_action(item);
  for (guint i = 0; i < G_N_ELEMENTS(context_actions); i++) {
    if (context_actions[i].action == stock)
      return g_object_ref_sink(webkit_context_menu_item_new_from_stock_action(
          context_actions[i].webkit));
  }
  return NULL;
}

static gboolean on_context_menu(WebKitWebView *web_view,
                                WebKitContextMenu *native_menu,
                                WebKitHitTestResult *hit,
                                PdfvWebKitView *self) {
  (void)web_view;
  PdfvWebContextMenu *menu = pdfv_web_context_menu_new(
      webkit_hit_test_result_context_is_image(hit)
          ? webkit_hit_test_result_get_image_uri(hit)
          : NULL);
  for (GList *at = webkit_context_menu_get_items(native_menu); at;
       at = at->next) {
    WebKitContextMenuItem *native = at->data;
    PdfvWebContextMenuItem *item = pdfv_web_context_menu_item_new_native(
        webkit_context_menu_item_is_separator(native)
            ? PDFV_WEB_CONTEXT_MENU_ACTION_NONE
            : action_from_webkit(
                  webkit_context_menu_item_get_stock_action(native)),
        g_object_ref(native), g_object_unref);
    pdfv_web_context_menu_append(menu, item);
  }
  gboolean suppressed = pdfv_web_view_emit_context_menu(PDFV_WEB_VIEW(self),
                                                        menu);
  if (!suppressed) {
    /* Build the replacement before clearing the menu, which releases the
     * items that are about to be reused. */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (GList *at = pdfv_web_context_menu_get_items(menu); at; at = at->next) {
      WebKitContextMenuItem *native = webkit_item_for(at->data);
      if (native)
        g_ptr_array_add(items, native);
    }
    webkit_context_menu_remove_all(native_menu);
    for (guint i = 0; i < items->len; i++)
      webkit_context_menu_append(native_menu, g_ptr_array_index(items, i));
    g_ptr_array_unref(items);
  }
  g_object_unref(menu);
  return suppressed;
}

static void pdfv_webkit_view_setup(PdfvWebView *view, PdfvWebView *related) {
  PdfvWebKitView *self = PDFV_WEBKIT_VIEW(view);
  self->content_manager = webkit_user_content_manager_new();
  WebKitUserScript *script = webkit_user_script_new(
      host_script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
  webkit_user_content_manager_add_script(self->content_manager, script);
  webkit_user_script_unref(script);
  g_signal_connect(self->content_manager,
                   "script-message-received::" HOST_CHANNEL,
                   G_CALLBACK(on_script_message), self);
  if (!webkit_user_content_manager_register_script_message_handler(
          self->content_manager, HOST_CHANNEL, NULL))
    g_warning("Could not register the web view message handler");

  if (related) {
    self->web_view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "related-view",
        PDFV_WEBKIT_VIEW(related)->web_view, "user-content-manager",
        self->content_manager, NULL));
  } else {
    self->web_view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "web-context", get_shared_context(),
        "user-content-manager", self->content_manager, NULL));
  }
  g_object_set_data(G_OBJECT(self->web_view), "phi-web-view", self);
  gtk_widget_set_parent(GTK_WIDGET(self->web_view), GTK_WIDGET(self));

  WebKitSettings *settings = webkit_web_view_get_settings(self->web_view);
  g_object_set(settings, "enable-html5-database", FALSE,
               "enable-html5-local-storage", FALSE,
               "enable-page-cache", FALSE, NULL);
  g_signal_connect(self->web_view, "decide-policy",
                   G_CALLBACK(on_decide_policy), self);
  g_signal_connect(self->web_view, "permission-request",
                   G_CALLBACK(on_permission_request), self);
  g_signal_connect(self->web_view, "context-menu",
                   G_CALLBACK(on_context_menu), self);
}

static void pdfv_webkit_view_load_uri(PdfvWebView *view, const gchar *uri) {
  webkit_web_view_load_uri(PDFV_WEBKIT_VIEW(view)->web_view, uri);
}

static void pdfv_webkit_view_set_background_color(PdfvWebView *view,
                                                  const GdkRGBA *color) {
  webkit_web_view_set_background_color(PDFV_WEBKIT_VIEW(view)->web_view,
                                       color);
}

static void pdfv_webkit_view_set_developer_extras_enabled(PdfvWebView *view,
                                                          gboolean enabled) {
  WebKitSettings *settings =
      webkit_web_view_get_settings(PDFV_WEBKIT_VIEW(view)->web_view);
  g_object_set(settings, "enable-developer-extras", enabled, NULL);
}

static void on_javascript_finished(GObject *source, GAsyncResult *result,
                                   gpointer user_data) {
  GTask *task = G_TASK(user_data);
  GError *error = NULL;
  JSCValue *value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (!value && error)
    g_task_return_error(task, error);
  else
    g_task_return_boolean(task, TRUE);
  g_clear_object(&value);
  g_object_unref(task);
}

static void pdfv_webkit_view_run_javascript(PdfvWebView *view,
                                            const gchar *script,
                                            const gchar *source_uri,
                                            GTask *task) {
  webkit_web_view_evaluate_javascript(
      PDFV_WEBKIT_VIEW(view)->web_view, script, -1, NULL, source_uri,
      g_task_get_cancellable(task), on_javascript_finished, task);
}

static void on_print_failed(WebKitPrintOperation *operation, GError *error,
                            GTask *task) {
  (void)operation;
  if (!g_task_get_task_data(task)) {
    g_task_set_task_data(task,
                         g_error_copy(error ? error
                                            : &(GError){G_IO_ERROR,
                                                        G_IO_ERROR_FAILED,
                                                        "Could not print"}),
                         (GDestroyNotify)g_error_free);
  }
}

static void on_print_finished(WebKitPrintOperation *operation, GTask *task) {
  g_signal_handlers_disconnect_by_data(operation, task);
  GError *error = g_task_get_task_data(task);
  if (error)
    g_task_return_error(task, g_error_copy(error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(operation);
  g_object_unref(task);
}

static void pdfv_webkit_view_print_to_pdf(PdfvWebView *view,
                                          const gchar *filename,
                                          GtkPageSetup *page_setup,
                                          GTask *task) {
  GError *error = NULL;
  gchar *uri = g_filename_to_uri(filename, NULL, &error);
  if (!uri) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  WebKitPrintOperation *operation =
      webkit_print_operation_new(PDFV_WEBKIT_VIEW(view)->web_view);
  GtkPrintSettings *settings = gtk_print_settings_new();
  gtk_print_settings_set_printer(settings, "Print to File");
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT,
                         "pdf");
  gtk_print_settings_set_orientation(
      settings, gtk_page_setup_get_orientation(page_setup));
  webkit_print_operation_set_print_settings(operation, settings);
  webkit_print_operation_set_page_setup(operation, page_setup);
  g_object_unref(settings);
  g_free(uri);
  g_signal_connect(operation, "failed", G_CALLBACK(on_print_failed), task);
  g_signal_connect(operation, "finished", G_CALLBACK(on_print_finished),
                   task);
  webkit_print_operation_print(operation);
}

static gboolean pdfv_webkit_view_grab_focus(GtkWidget *widget) {
  PdfvWebKitView *self = PDFV_WEBKIT_VIEW(widget);
  return self->web_view &&
      gtk_widget_grab_focus(GTK_WIDGET(self->web_view));
}

static void pdfv_webkit_view_dispose(GObject *object) {
  PdfvWebKitView *self = PDFV_WEBKIT_VIEW(object);
  if (self->content_manager) {
    webkit_user_content_manager_unregister_script_message_handler(
        self->content_manager, HOST_CHANNEL, NULL);
    g_signal_handlers_disconnect_by_data(self->content_manager, self);
  }
  if (self->web_view) {
    g_signal_handlers_disconnect_by_data(self->web_view, self);
    g_object_set_data(G_OBJECT(self->web_view), "phi-web-view", NULL);
    gtk_widget_unparent(GTK_WIDGET(self->web_view));
    self->web_view = NULL;
  }
  g_clear_object(&self->content_manager);
  G_OBJECT_CLASS(pdfv_webkit_view_parent_class)->dispose(object);
}

static void pdfv_webkit_view_class_init(PdfvWebKitViewClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  PdfvWebViewClass *view_class = PDFV_WEB_VIEW_CLASS(klass);
  object_class->dispose = pdfv_webkit_view_dispose;
  widget_class->grab_focus = pdfv_webkit_view_grab_focus;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  view_class->setup = pdfv_webkit_view_setup;
  view_class->load_uri = pdfv_webkit_view_load_uri;
  view_class->set_background_color = pdfv_webkit_view_set_background_color;
  view_class->set_developer_extras_enabled =
      pdfv_webkit_view_set_developer_extras_enabled;
  view_class->run_javascript = pdfv_webkit_view_run_javascript;
  view_class->print_to_pdf = pdfv_webkit_view_print_to_pdf;
}

static void pdfv_webkit_view_init(PdfvWebKitView *self) {
  (void)self;
}

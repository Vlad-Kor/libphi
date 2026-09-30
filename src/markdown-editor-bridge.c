/*
 * Phi Markdown editor - versioned native/web JSON bridge
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "markdown-editor-bridge.h"

struct _PdfvMarkdownEditorBridge {
  GObject parent_instance;
  PdfvWebView *web_view;
};

enum {
  SIGNAL_MESSAGE,
  SIGNAL_ERROR,
  N_SIGNALS,
};

static guint bridge_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PdfvMarkdownEditorBridge, pdfv_markdown_editor_bridge,
                    G_TYPE_OBJECT)

static void on_javascript_finished(GObject *source, GAsyncResult *result,
                                   gpointer user_data) {
  PdfvMarkdownEditorBridge *self = PDFV_MARKDOWN_EDITOR_BRIDGE(user_data);
  GError *error = NULL;
  if (!pdfv_web_view_run_javascript_finish(PDFV_WEB_VIEW(source), result,
                                           &error))
    g_signal_emit(self, bridge_signals[SIGNAL_ERROR], 0, error->message);
  g_clear_error(&error);
  g_object_unref(self);
}

void pdfv_markdown_editor_bridge_send(PdfvMarkdownEditorBridge *self,
                                      const gchar *type, const gchar *id,
                                      JsonObject *payload) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR_BRIDGE(self));
  g_return_if_fail(type && *type);
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "protocol");
  json_builder_add_int_value(builder, 1);
  if (id && *id) {
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);
  }
  json_builder_set_member_name(builder, "type");
  json_builder_add_string_value(builder, type);
  json_builder_set_member_name(builder, "payload");
  if (payload) {
    JsonNode *payload_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(payload_node, payload);
    json_builder_add_value(builder, payload_node);
  } else {
    json_builder_begin_object(builder);
    json_builder_end_object(builder);
  }
  json_builder_end_object(builder);
  JsonNode *root = json_builder_get_root(builder);
  JsonGenerator *generator = json_generator_new();
  json_generator_set_root(generator, root);
  gchar *message = json_generator_to_data(generator, NULL);

  JsonNode *quoted_node = json_node_new(JSON_NODE_VALUE);
  json_node_set_string(quoted_node, message);
  json_generator_set_root(generator, quoted_node);
  gchar *quoted = json_generator_to_data(generator, NULL);
  gchar *script = g_strdup_printf(
      "window.nativeEditorReceive && window.nativeEditorReceive(JSON.parse(%s));",
      quoted);
  pdfv_web_view_run_javascript(
      self->web_view, script, "app://editor/native-bridge.js", NULL,
      on_javascript_finished, g_object_ref(self));

  g_free(script);
  g_free(quoted);
  json_node_unref(quoted_node);
  g_free(message);
  g_object_unref(generator);
  json_node_unref(root);
  g_object_unref(builder);
}

static void on_script_message(PdfvWebView *web_view, const gchar *source,
                              PdfvMarkdownEditorBridge *self) {
  (void)web_view;
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if (!source || !json_parser_load_from_data(parser, source, -1, &error)) {
    g_signal_emit(self, bridge_signals[SIGNAL_ERROR], 0,
                  error ? error->message : "Bridge message was not JSON");
    g_clear_error(&error);
    g_object_unref(parser);
    return;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_signal_emit(self, bridge_signals[SIGNAL_ERROR], 0,
                  "Bridge message root must be an object");
    g_object_unref(parser);
    return;
  }
  JsonObject *message = json_node_get_object(root);
  gint64 protocol = json_object_get_int_member_with_default(message,
                                                             "protocol", 0);
  const gchar *type = json_object_get_string_member_with_default(
      message, "type", NULL);
  if (protocol != 1 || !type || !*type) {
    g_signal_emit(self, bridge_signals[SIGNAL_ERROR], 0,
                  "Unsupported or incomplete bridge message");
    g_object_unref(parser);
    return;
  }
  const gchar *id = json_object_get_string_member_with_default(message, "id",
                                                                NULL);
  JsonObject *payload = json_object_has_member(message, "payload") &&
                                JSON_NODE_HOLDS_OBJECT(
                                    json_object_get_member(message, "payload"))
                            ? json_object_get_object_member(message, "payload")
                            : NULL;
  g_signal_emit(self, bridge_signals[SIGNAL_MESSAGE], 0, type, id, payload);
  g_object_unref(parser);
}

static void pdfv_markdown_editor_bridge_dispose(GObject *object) {
  PdfvMarkdownEditorBridge *self = PDFV_MARKDOWN_EDITOR_BRIDGE(object);
  if (self->web_view)
    g_signal_handlers_disconnect_by_data(self->web_view, self);
  g_clear_object(&self->web_view);
  G_OBJECT_CLASS(pdfv_markdown_editor_bridge_parent_class)->dispose(object);
}

static void pdfv_markdown_editor_bridge_class_init(
    PdfvMarkdownEditorBridgeClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pdfv_markdown_editor_bridge_dispose;
  bridge_signals[SIGNAL_MESSAGE] = g_signal_new(
      "message", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, JSON_TYPE_OBJECT);
  bridge_signals[SIGNAL_ERROR] = g_signal_new(
      "bridge-error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void pdfv_markdown_editor_bridge_init(
    PdfvMarkdownEditorBridge *self) {
  (void)self;
}

PdfvMarkdownEditorBridge *pdfv_markdown_editor_bridge_new(
    PdfvWebView *web_view) {
  g_return_val_if_fail(PDFV_IS_WEB_VIEW(web_view), NULL);
  PdfvMarkdownEditorBridge *self =
      g_object_new(PDFV_TYPE_MARKDOWN_EDITOR_BRIDGE, NULL);
  self->web_view = g_object_ref(web_view);
  g_signal_connect(web_view, "script-message::native",
                   G_CALLBACK(on_script_message), self);
  return self;
}

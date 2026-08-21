/*
 * Phi Markdown editor - native GTK/WebKit host
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phi-markdown"

#include "markdown-editor.h"

#include "markdown-editor-bridge.h"
#include "markdown-resource-scheme.h"
#include "markdown-vault-adapter.h"

#include <json-glib/json-glib.h>
#include <webkit/webkit.h>

#include <string.h>

#define MAX_ATTACHMENT_BYTES (20 * 1024 * 1024)
#define MAX_EMBED_DEPTH 4

typedef struct {
  GTask *task;
  guint timeout_id;
} FlushPending;

struct _PdfvMarkdownEditor {
  GtkBox parent_instance;
  WebKitWebView *web_view;
  WebKitUserContentManager *content_manager;
  PdfvMarkdownVaultAdapter *vault;
  PdfvMarkdownResourceScheme *resources;
  PdfvMarkdownEditorBridge *bridge;

  GFile *file;
  GFileMonitor *monitor;
  GFileMonitor *preamble_monitor;
  gchar *document_id;
  gchar *relative_path;
  gchar *current_text;
  gchar *persisted_text;
  gchar *etag;
  guint64 revision;
  guint64 editor_revision;
  gboolean ready;
  gboolean dirty;
  gboolean saving;
  gboolean theme_dark;
  gboolean theme_set;
  gboolean settings_set;
  gboolean allow_remote_images;
  gdouble font_scale;
  guint request_sequence;
  GHashTable *flush_tasks; /* request ID -> FlushPending */
};

enum {
  SIGNAL_DIRTY_CHANGED,
  SIGNAL_OPEN_FILE,
  SIGNAL_CREATE_LINK,
  SIGNAL_OPEN_EXTERNAL_URI,
  SIGNAL_CONFLICT,
  SIGNAL_EDITOR_ERROR,
  N_SIGNALS,
};

static guint editor_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PdfvMarkdownEditor, pdfv_markdown_editor, GTK_TYPE_BOX)

static JsonObject *json_object_new_owned(void) { return json_object_new(); }

static void flush_pending_free(FlushPending *pending) {
  if (pending->timeout_id)
    g_source_remove(pending->timeout_id);
  g_clear_object(&pending->task);
  g_free(pending);
}

static void emit_error(PdfvMarkdownEditor *self, const gchar *message) {
  g_signal_emit(self, editor_signals[SIGNAL_EDITOR_ERROR], 0,
                message ? message : "Unknown Markdown editor error");
}

static void set_dirty(PdfvMarkdownEditor *self, gboolean dirty) {
  if (self->dirty == dirty)
    return;
  self->dirty = dirty;
  g_signal_emit(self, editor_signals[SIGNAL_DIRTY_CHANGED], 0, dirty);
}

static const gchar *line_ending_for_text(const gchar *text) {
  return text && strstr(text, "\r\n") ? "CRLF" : "LF";
}

static gchar *read_preamble(PdfvMarkdownEditor *self) {
  GFile *root = pdfv_markdown_vault_adapter_get_root(self->vault);
  GFile *preamble = g_file_get_child(root, "preamble.sty");
  gchar *contents = NULL;
  if (!g_file_load_contents(preamble, NULL, &contents, NULL, NULL, NULL))
    contents = g_strdup("");
  g_object_unref(preamble);
  return contents;
}

static void send_open_document(PdfvMarkdownEditor *self,
                               const gchar *message_type) {
  if (!self->ready || !self->file || !self->current_text)
    return;
  JsonObject *payload = json_object_new_owned();
  json_object_set_string_member(payload, "documentId", self->document_id);
  json_object_set_string_member(payload, "path", self->relative_path);
  json_object_set_string_member(payload, "text", self->current_text);
  json_object_set_int_member(payload, "revision", (gint64)self->revision);
  json_object_set_string_member(payload, "lineEnding",
                                line_ending_for_text(self->current_text));
  gchar *preamble = read_preamble(self);
  json_object_set_string_member(payload, "preamble", preamble);
  pdfv_markdown_editor_bridge_send(self->bridge, message_type, NULL, payload);
  g_free(preamble);
  json_object_unref(payload);
}

static void send_theme(PdfvMarkdownEditor *self) {
  if (!self->ready || !self->theme_set)
    return;
  JsonObject *payload = json_object_new_owned();
  json_object_set_boolean_member(payload, "dark", self->theme_dark);
  json_object_set_double_member(payload, "fontScale", self->font_scale);
  pdfv_markdown_editor_bridge_send(self->bridge, "theme/update", NULL,
                                   payload);
  json_object_unref(payload);
}

static void send_settings(PdfvMarkdownEditor *self) {
  if (!self->ready || !self->settings_set)
    return;
  JsonObject *payload = json_object_new_owned();
  json_object_set_boolean_member(payload, "allowRemoteImages",
                                 self->allow_remote_images);
  pdfv_markdown_editor_bridge_send(self->bridge, "settings/update", NULL,
                                   payload);
  json_object_unref(payload);
}

static void update_snapshot(PdfvMarkdownEditor *self, JsonObject *payload) {
  if (!payload)
    return;
  const gchar *document_id = json_object_get_string_member_with_default(
      payload, "documentId", NULL);
  if (document_id && g_strcmp0(document_id, self->document_id) != 0)
    return;
  const gchar *text = json_object_get_string_member_with_default(payload,
                                                                  "text", NULL);
  if (text) {
    g_free(self->current_text);
    self->current_text = g_strdup(text);
  }
  self->editor_revision = (guint64)json_object_get_int_member_with_default(
      payload, "editorRevision", (gint64)self->editor_revision);
}

static JsonNode *string_array_node(GPtrArray *values) {
  JsonArray *array = json_array_new();
  for (guint i = 0; values && i < values->len; i++)
    json_array_add_string_element(array, g_ptr_array_index(values, i));
  JsonNode *node = json_node_new(JSON_NODE_ARRAY);
  json_node_take_array(node, array);
  return node;
}

static void send_response_node(PdfvMarkdownEditor *self, const gchar *id,
                               JsonNode *result, const gchar *error) {
  if (!id || !*id) {
    g_clear_pointer(&result, json_node_unref);
    return;
  }
  JsonObject *payload = json_object_new_owned();
  if (error)
    json_object_set_string_member(payload, "error", error);
  else if (result)
    json_object_set_member(payload, "result", result);
  else
    json_object_set_null_member(payload, "result");
  pdfv_markdown_editor_bridge_send(self->bridge, "request/response", id,
                                   payload);
  json_object_unref(payload);
}

static void send_response_error(PdfvMarkdownEditor *self, const gchar *id,
                                GError *error) {
  send_response_node(self, id, NULL,
                     error ? error->message : "Native request failed");
}

static GFile *resolve_target_note(PdfvMarkdownEditor *self,
                                  const gchar *target, GError **error) {
  if (!target || !*target || g_str_has_prefix(target, "#"))
    return self->file ? g_object_ref(self->file) : NULL;
  return pdfv_markdown_vault_adapter_resolve_note(
      self->vault, self->relative_path, target, error);
}

static void handle_completion(PdfvMarkdownEditor *self, const gchar *type,
                              const gchar *id, JsonObject *payload) {
  const gchar *query = payload ? json_object_get_string_member_with_default(
                                      payload, "query", "")
                               : "";
  const gchar *target = payload ? json_object_get_string_member_with_default(
                                       payload, "target", "")
                                : "";
  GError *error = NULL;
  GPtrArray *values = NULL;
  if (g_str_equal(type, "completion/files")) {
    values = pdfv_markdown_vault_adapter_list_notes(self->vault, query,
                                                     &error);
  } else {
    GFile *file = resolve_target_note(self, target, &error);
    if (file) {
      values = g_str_equal(type, "completion/headings")
                   ? pdfv_markdown_vault_adapter_get_headings(self->vault,
                                                               file, &error)
                   : pdfv_markdown_vault_adapter_get_blocks(self->vault, file,
                                                             &error);
      g_object_unref(file);
    }
  }
  if (!values) {
    send_response_error(self, id, error);
  } else {
    if (values->len > 100)
      g_ptr_array_set_size(values, 100);
    send_response_node(self, id, string_array_node(values), NULL);
  }
  g_clear_pointer(&values, g_ptr_array_unref);
  g_clear_error(&error);
}

static void handle_link(PdfvMarkdownEditor *self, const gchar *type,
                        const gchar *id, JsonObject *payload) {
  const gchar *target = payload ? json_object_get_string_member_with_default(
                                      payload, "target", "")
                               : "";
  GError *error = NULL;
  GFile *file = resolve_target_note(self, target, &error);
  if (g_str_equal(type, "link/resolve")) {
    JsonObject *value = json_object_new_owned();
    json_object_set_boolean_member(value, "exists", file != NULL);
    if (file) {
      gchar *relative = pdfv_markdown_vault_adapter_relative_path(self->vault,
                                                                  file);
      json_object_set_string_member(value, "path", relative);
      g_free(relative);
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, value);
    send_response_node(self, id, node, NULL);
  } else if (file) {
    g_object_set_data(G_OBJECT(file), "markdown-link-target",
                      (gpointer)target);
    g_signal_emit(self, editor_signals[SIGNAL_OPEN_FILE], 0, file);
    g_object_set_data(G_OBJECT(file), "markdown-link-target", NULL);
  } else {
    g_signal_emit(self, editor_signals[SIGNAL_CREATE_LINK], 0, target);
  }
  g_clear_object(&file);
  g_clear_error(&error);
}

static guint markdown_heading_level(const gchar *line, gchar **title) {
  guint level = 0;
  while (level < 6 && line[level] == '#')
    level++;
  if (level == 0 || (line[level] != ' ' && line[level] != '\t'))
    return 0;
  gchar *value = g_strdup(line + level);
  g_strstrip(value);
  gsize length = strlen(value);
  while (length && value[length - 1] == '#')
    value[--length] = '\0';
  g_strchomp(value);
  if (title)
    *title = value;
  else
    g_free(value);
  return level;
}

static gchar *join_markdown_lines(gchar **lines, guint start, guint end) {
  GString *result = g_string_new(NULL);
  for (guint i = start; i < end; i++) {
    g_string_append(result, lines[i]);
    if (i + 1 < end)
      g_string_append_c(result, '\n');
  }
  return g_string_free(result, FALSE);
}

static gboolean markdown_line_blank(const gchar *line) {
  for (const guchar *at = (const guchar *)line; *at; at++)
    if (!g_ascii_isspace(*at))
      return FALSE;
  return TRUE;
}

static gchar *extract_embed_fragment(const gchar *text, const gchar *target,
                                     GError **error) {
  const gchar *hash = target ? strchr(target, '#') : NULL;
  if (!hash || !hash[1])
    return g_strdup(text);
  gchar *fragment = g_uri_unescape_string(hash + 1, NULL);
  if (!fragment) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Invalid embed fragment");
    return NULL;
  }
  gchar **lines = g_strsplit(text, "\n", -1);
  guint count = g_strv_length(lines);
  gchar *result = NULL;

  if (fragment[0] == '^') {
    const gchar *id = fragment + 1;
    for (guint i = 0; i < count; i++) {
      gchar *trimmed = g_strdup(lines[i]);
      g_strstrip(trimmed);
      gchar *marker = g_strconcat("^", id, NULL);
      gboolean match = g_str_has_suffix(trimmed, marker);
      gsize prefix_length = match ? strlen(trimmed) - strlen(marker) : 0;
      match = match &&
              (prefix_length == 0 ||
               g_ascii_isspace((guchar)trimmed[prefix_length - 1]));
      g_free(marker);
      g_free(trimmed);
      if (!match)
        continue;
      guint start = i;
      guint end = i + 1;
      while (start > 0 && !markdown_line_blank(lines[start - 1]))
        start--;
      if (start == i && i > 1) {
        guint previous = i - 1;
        while (previous > 0 && markdown_line_blank(lines[previous]))
          previous--;
        start = previous;
        while (start > 0 && !markdown_line_blank(lines[start - 1]))
          start--;
      }
      while (end < count && !markdown_line_blank(lines[end]))
        end++;
      result = join_markdown_lines(lines, start, end);
      break;
    }
  } else {
    gchar *wanted = g_utf8_casefold(fragment, -1);
    for (guint i = 0; i < count; i++) {
      gchar *title = NULL;
      guint level = markdown_heading_level(lines[i], &title);
      gchar *folded = title ? g_utf8_casefold(title, -1) : NULL;
      gboolean match = level && g_strcmp0(folded, wanted) == 0;
      g_free(folded);
      g_free(title);
      if (!match)
        continue;
      guint end = i + 1;
      for (; end < count; end++) {
        guint next_level = markdown_heading_level(lines[end], NULL);
        if (next_level && next_level <= level)
          break;
      }
      result = join_markdown_lines(lines, i, end);
      break;
    }
    g_free(wanted);
  }
  g_strfreev(lines);
  if (!result)
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Embed section '%s' was not found", fragment);
  g_free(fragment);
  return result;
}

static void handle_embed_read(PdfvMarkdownEditor *self, const gchar *id,
                              JsonObject *payload) {
  const gchar *target = payload ? json_object_get_string_member_with_default(
                                      payload, "target", "")
                               : "";
  gint64 depth = payload ? json_object_get_int_member_with_default(
                               payload, "depth", 0)
                         : 0;
  if (depth > MAX_EMBED_DEPTH) {
    send_response_node(self, id, NULL, "Maximum embed depth exceeded");
    return;
  }
  GError *error = NULL;
  GFile *file = resolve_target_note(self, target, &error);
  gchar *text = file ? pdfv_markdown_vault_adapter_read_text(
                           self->vault, file, NULL, &error)
                     : NULL;
  if (text) {
    gchar *fragment = extract_embed_fragment(text, target, &error);
    g_free(text);
    text = fragment;
  }
  if (!text) {
    send_response_error(self, id, error);
  } else {
    JsonObject *value = json_object_new_owned();
    json_object_set_string_member(value, "text", text);
    gchar *relative = pdfv_markdown_vault_adapter_relative_path(self->vault,
                                                                file);
    json_object_set_string_member(value, "path", relative);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, value);
    send_response_node(self, id, node, NULL);
    g_free(relative);
  }
  g_free(text);
  g_clear_object(&file);
  g_clear_error(&error);
}

static const gchar *extension_for_mime(const gchar *mime) {
  if (g_strcmp0(mime, "image/jpeg") == 0)
    return "jpg";
  if (g_strcmp0(mime, "image/webp") == 0)
    return "webp";
  if (g_strcmp0(mime, "image/gif") == 0)
    return "gif";
  if (g_strcmp0(mime, "image/svg+xml") == 0)
    return "svg";
  return "png";
}

static void handle_attachment_create(PdfvMarkdownEditor *self,
                                     const gchar *id, JsonObject *payload) {
  const gchar *encoded = payload ? json_object_get_string_member_with_default(
                                       payload, "data", NULL)
                                 : NULL;
  const gchar *mime = payload ? json_object_get_string_member_with_default(
                                    payload, "mimeType", "image/png")
                              : "image/png";
  gsize length = 0;
  guchar *data = encoded ? g_base64_decode(encoded, &length) : NULL;
  if (!data || length == 0 || length > MAX_ATTACHMENT_BYTES) {
    g_free(data);
    send_response_node(self, id, NULL, "Invalid or oversized attachment");
    return;
  }
  GDateTime *now = g_date_time_new_now_local();
  gchar *stamp = g_date_time_format(now, "%Y-%m-%d %H%M%S");
  gchar *name = g_strdup_printf("Pasted image %s.%s", stamp,
                                extension_for_mime(mime));
  gchar *relative = g_build_filename("attachments", name, NULL);
  GError *error = NULL;
  GFile *file = pdfv_markdown_vault_adapter_resolve(self->vault, relative,
                                                    &error);
  GFile *parent = file ? g_file_get_parent(file) : NULL;
  if (parent)
    g_file_make_directory_with_parents(parent, NULL, NULL);
  gboolean saved = file && g_file_replace_contents(
                                file, (const gchar *)data, length, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL,
                                &error);
  if (!saved) {
    send_response_error(self, id, error);
  } else {
    JsonObject *value = json_object_new_owned();
    json_object_set_string_member(value, "path", relative);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, value);
    send_response_node(self, id, node, NULL);
  }
  g_clear_object(&parent);
  g_clear_object(&file);
  g_clear_error(&error);
  g_free(relative);
  g_free(name);
  g_free(stamp);
  g_date_time_unref(now);
  g_free(data);
}

static GFile *resolve_vault_attachment(PdfvMarkdownEditor *self,
                                       const gchar *target,
                                       gboolean relative_to_note,
                                       GError **error) {
  gchar *decoded = g_uri_unescape_string(target ? target : "", NULL);
  if (!decoded || !*decoded || g_path_is_absolute(decoded) ||
      strchr(decoded, '\\')) {
    g_free(decoded);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Unsafe attachment path");
    return NULL;
  }
  gchar *suffix = strpbrk(decoded, "?#");
  if (suffix)
    *suffix = '\0';

  GFile *file = NULL;
  if (relative_to_note && self->file) {
    GFile *parent = g_file_get_parent(self->file);
    GFile *candidate = parent
                           ? g_file_resolve_relative_path(parent, decoded)
                           : NULL;
    gchar *relative = candidate
                          ? g_file_get_relative_path(
                                pdfv_markdown_vault_adapter_get_root(
                                    self->vault), candidate)
                          : NULL;
    if (relative)
      file = pdfv_markdown_vault_adapter_resolve(self->vault, relative,
                                                  error);
    else
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                  "Attachment path escapes the vault");
    g_free(relative);
    g_clear_object(&candidate);
    g_clear_object(&parent);
  } else {
    file = pdfv_markdown_vault_adapter_resolve(self->vault, decoded, error);
  }
  g_free(decoded);
  if (file && !g_file_query_exists(file, NULL)) {
    g_clear_object(&file);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Attachment was not found");
  }
  return file;
}

static void handle_attachment_action(PdfvMarkdownEditor *self,
                                     const gchar *type, const gchar *id,
                                     JsonObject *payload) {
  const gchar *target = payload ? json_object_get_string_member_with_default(
                                      payload, "target", "")
                                : "";
  gboolean relative = payload && json_object_get_boolean_member_with_default(
                                     payload, "relative", FALSE);
  GError *error = NULL;
  GFile *file = resolve_vault_attachment(self, target, relative, &error);
  if (!file) {
    if (id && *id)
      send_response_error(self, id, error);
    else
      emit_error(self, error ? error->message : "Attachment was not found");
  } else if (g_str_equal(type, "attachment/open")) {
    g_object_set_data(G_OBJECT(file), "markdown-link-target",
                      (gpointer)target);
    g_signal_emit(self, editor_signals[SIGNAL_OPEN_FILE], 0, file);
    g_object_set_data(G_OBJECT(file), "markdown-link-target", NULL);
  } else {
    gchar *relative_path = pdfv_markdown_vault_adapter_relative_path(
        self->vault, file);
    JsonObject *value = json_object_new_owned();
    json_object_set_string_member(value, "path", relative_path);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, value);
    send_response_node(self, id, node, NULL);
    g_free(relative_path);
  }
  g_clear_error(&error);
  g_clear_object(&file);
}

static void complete_flush(PdfvMarkdownEditor *self, const gchar *id,
                           JsonObject *payload) {
  if (!id)
    return;
  FlushPending *pending = g_hash_table_lookup(self->flush_tasks, id);
  if (!pending)
    return;
  update_snapshot(self, payload);
  GTask *task = g_object_ref(pending->task);
  g_source_remove(pending->timeout_id);
  pending->timeout_id = 0;
  g_hash_table_remove(self->flush_tasks, id);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

typedef struct {
  PdfvMarkdownEditor *editor;
  gchar *id;
} FlushTimeoutData;

static void flush_timeout_data_free(FlushTimeoutData *data) {
  g_clear_object(&data->editor);
  g_free(data->id);
  g_free(data);
}

static gboolean on_flush_timeout(gpointer user_data) {
  FlushTimeoutData *data = user_data;
  FlushPending *pending = g_hash_table_lookup(data->editor->flush_tasks,
                                               data->id);
  if (!pending)
    return G_SOURCE_REMOVE;
  GTask *task = g_object_ref(pending->task);
  pending->timeout_id = 0;
  g_hash_table_remove(data->editor->flush_tasks, data->id);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                          "Timed out waiting for the Markdown editor");
  g_object_unref(task);
  return G_SOURCE_REMOVE;
}

static void save_requested_done(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  if (!pdfv_markdown_editor_save_finish(PDFV_MARKDOWN_EDITOR(source), result,
                                        &error)) {
    emit_error(PDFV_MARKDOWN_EDITOR(source), error->message);
  }
  g_clear_error(&error);
}

static void on_bridge_message(PdfvMarkdownEditorBridge *bridge,
                              const gchar *type, const gchar *id,
                              JsonObject *payload,
                              PdfvMarkdownEditor *self) {
  (void)bridge;
  if (g_str_equal(type, "editor/ready")) {
    self->ready = TRUE;
    send_theme(self);
    send_settings(self);
    send_open_document(self, "document/open");
  } else if (g_str_equal(type, "document/changed")) {
    update_snapshot(self, payload);
    set_dirty(self, TRUE);
  } else if (g_str_equal(type, "document/save")) {
    update_snapshot(self, payload);
    set_dirty(self, TRUE);
    pdfv_markdown_editor_save_async(self, NULL, save_requested_done, NULL);
  } else if (g_str_equal(type, "document/flush")) {
    complete_flush(self, id, payload);
  } else if (g_str_equal(type, "document/state")) {
    if (payload && json_object_get_boolean_member_with_default(
                       payload, "conflict", FALSE))
      g_signal_emit(self, editor_signals[SIGNAL_CONFLICT], 0);
  } else if (g_str_has_prefix(type, "completion/")) {
    handle_completion(self, type, id, payload);
  } else if (g_str_has_prefix(type, "link/")) {
    handle_link(self, type, id, payload);
  } else if (g_str_equal(type, "embed/read") ||
             g_str_equal(type, "embed/resolve")) {
    handle_embed_read(self, id, payload);
  } else if (g_str_equal(type, "attachment/create")) {
    handle_attachment_create(self, id, payload);
  } else if (g_str_equal(type, "attachment/resolve") ||
             g_str_equal(type, "attachment/open")) {
    handle_attachment_action(self, type, id, payload);
  } else if (g_str_equal(type, "url/open")) {
    const gchar *uri = payload ? json_object_get_string_member_with_default(
                                     payload, "uri", "")
                               : "";
    g_signal_emit(self, editor_signals[SIGNAL_OPEN_EXTERNAL_URI], 0, uri);
  } else if (g_str_equal(type, "log/error")) {
    const gchar *message = payload
                               ? json_object_get_string_member_with_default(
                                     payload, "message", "Editor error")
                               : "Editor error";
    const gchar *component = payload
                                 ? json_object_get_string_member_with_default(
                                       payload, "component", "web")
                                 : "web";
    g_debug("Markdown editor [%s]: %s", component, message);
  }
}

static void on_bridge_error(PdfvMarkdownEditorBridge *bridge,
                            const gchar *message,
                            PdfvMarkdownEditor *self) {
  (void)bridge;
  emit_error(self, message);
}

static gboolean on_decide_policy(WebKitWebView *view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 PdfvMarkdownEditor *self) {
  (void)view;
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;
  WebKitNavigationAction *action =
      webkit_navigation_policy_decision_get_navigation_action(
          WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const gchar *uri = webkit_uri_request_get_uri(request);
  if ((uri && g_str_has_prefix(uri, "app://editor/")) ||
      g_strcmp0(uri, "about:blank") == 0)
    return FALSE;
  webkit_policy_decision_ignore(decision);
  if (uri && *uri)
    g_signal_emit(self, editor_signals[SIGNAL_OPEN_EXTERNAL_URI], 0, uri);
  return TRUE;
}

static gboolean on_permission_request(WebKitWebView *view,
                                      WebKitPermissionRequest *request,
                                      PdfvMarkdownEditor *self) {
  (void)view;
  (void)self;
  webkit_permission_request_deny(request);
  return TRUE;
}

static void monitor_external_loaded(GObject *source, GAsyncResult *result,
                                    gpointer user_data) {
  PdfvMarkdownEditor *self = PDFV_MARKDOWN_EDITOR(user_data);
  gchar *contents = NULL;
  gchar *etag = NULL;
  gsize length = 0;
  GError *error = NULL;
  if (g_file_load_contents_finish(G_FILE(source), result, &contents, &length,
                                  &etag, &error) &&
      g_strcmp0(contents, self->persisted_text) != 0) {
    self->revision++;
    if (self->dirty) {
      JsonObject *payload = json_object_new_owned();
      json_object_set_string_member(payload, "documentId", self->document_id);
      json_object_set_string_member(payload, "path", self->relative_path);
      json_object_set_string_member(payload, "text", contents);
      json_object_set_int_member(payload, "revision", (gint64)self->revision);
      json_object_set_string_member(payload, "lineEnding",
                                    line_ending_for_text(contents));
      pdfv_markdown_editor_bridge_send(self->bridge,
                                       "document/external-update", NULL,
                                       payload);
      json_object_unref(payload);
    } else {
      g_free(self->etag);
      self->etag = etag;
      etag = NULL;
      g_free(self->current_text);
      self->current_text = g_strdup(contents);
      g_free(self->persisted_text);
      self->persisted_text = g_strdup(contents);
      send_open_document(self, "document/external-update");
    }
  }
  if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    emit_error(self, error->message);
  g_clear_error(&error);
  g_free(etag);
  g_free(contents);
  g_object_unref(self);
}

static void on_file_changed(GFileMonitor *monitor, GFile *file,
                            GFile *other_file, GFileMonitorEvent event,
                            PdfvMarkdownEditor *self) {
  (void)monitor;
  (void)other_file;
  if (self->saving)
    return;
  if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event == G_FILE_MONITOR_EVENT_CREATED ||
      event == G_FILE_MONITOR_EVENT_MOVED_IN) {
    g_file_load_contents_async(file, NULL, monitor_external_loaded,
                               g_object_ref(self));
  }
}

static void on_preamble_changed(GFileMonitor *monitor, GFile *file,
                                GFile *other_file, GFileMonitorEvent event,
                                PdfvMarkdownEditor *self) {
  (void)monitor;
  (void)file;
  (void)other_file;
  if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      event != G_FILE_MONITOR_EVENT_CREATED &&
      event != G_FILE_MONITOR_EVENT_DELETED)
    return;
  gchar *preamble = read_preamble(self);
  JsonObject *payload = json_object_new_owned();
  json_object_set_string_member(payload, "preamble", preamble);
  pdfv_markdown_editor_bridge_send(self->bridge, "preamble/update", NULL,
                                   payload);
  json_object_unref(payload);
  g_free(preamble);
}

static void setup_monitors(PdfvMarkdownEditor *self) {
  g_clear_object(&self->monitor);
  g_clear_object(&self->preamble_monitor);
  if (self->file) {
    self->monitor = g_file_monitor_file(self->file, G_FILE_MONITOR_NONE, NULL,
                                        NULL);
    if (self->monitor)
      g_signal_connect(self->monitor, "changed", G_CALLBACK(on_file_changed),
                       self);
  }
  GFile *preamble = g_file_get_child(
      pdfv_markdown_vault_adapter_get_root(self->vault), "preamble.sty");
  self->preamble_monitor = g_file_monitor_file(
      preamble, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (self->preamble_monitor)
    g_signal_connect(self->preamble_monitor, "changed",
                     G_CALLBACK(on_preamble_changed), self);
  g_object_unref(preamble);
}

static void open_file_loaded(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  GTask *task = G_TASK(user_data);
  PdfvMarkdownEditor *self = g_task_get_source_object(task);
  gchar *contents = NULL;
  gchar *etag = NULL;
  gsize length = 0;
  GError *error = NULL;
  if (!g_file_load_contents_finish(G_FILE(source), result, &contents, &length,
                                   &etag, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  if (!g_utf8_validate(contents, length, NULL) ||
      memchr(contents, '\0', length) != NULL) {
    g_free(contents);
    g_free(etag);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Markdown file is not valid UTF-8 text");
    g_object_unref(task);
    return;
  }
  g_set_object(&self->file, G_FILE(source));
  g_free(self->relative_path);
  self->relative_path =
      pdfv_markdown_vault_adapter_relative_path(self->vault, self->file);
  if (!self->relative_path)
    self->relative_path = g_file_get_basename(self->file);
  g_free(self->document_id);
  self->document_id = g_file_get_uri(self->file);
  g_free(self->current_text);
  self->current_text = g_strdup(contents);
  g_free(self->persisted_text);
  self->persisted_text = contents;
  g_free(self->etag);
  self->etag = etag;
  self->revision++;
  self->editor_revision = 0;
  set_dirty(self, FALSE);
  setup_monitors(self);
  send_open_document(self, "document/open");
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

void pdfv_markdown_editor_open_file_async(
    PdfvMarkdownEditor *self, GFile *file, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  g_return_if_fail(G_IS_FILE(file));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_markdown_editor_open_file_async);
  g_file_load_contents_async(file, cancellable, open_file_loaded, task);
}

gboolean pdfv_markdown_editor_open_file_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void pdfv_markdown_editor_reload_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  g_return_if_fail(self->file != NULL);
  pdfv_markdown_editor_open_file_async(self, self->file, cancellable, callback,
                                       user_data);
}

void pdfv_markdown_editor_flush_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_markdown_editor_flush_async);
  if (!self->ready) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "Markdown editor is not ready");
    g_object_unref(task);
    return;
  }
  gchar *id = g_strdup_printf("native-flush-%u", ++self->request_sequence);
  FlushPending *pending = g_new0(FlushPending, 1);
  pending->task = task;
  FlushTimeoutData *timeout = g_new0(FlushTimeoutData, 1);
  timeout->editor = g_object_ref(self);
  timeout->id = g_strdup(id);
  pending->timeout_id = g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, 8, on_flush_timeout, timeout,
      (GDestroyNotify)flush_timeout_data_free);
  g_hash_table_insert(self->flush_tasks, g_strdup(id), pending);
  pdfv_markdown_editor_bridge_send(self->bridge, "document/flush", id, NULL);
  g_free(id);
}

gboolean pdfv_markdown_editor_flush_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void save_file_done(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  GTask *task = G_TASK(user_data);
  PdfvMarkdownEditor *self = g_task_get_source_object(task);
  gchar *new_etag = NULL;
  GError *error = NULL;
  self->saving = FALSE;
  if (!g_file_replace_contents_finish(G_FILE(source), result, &new_etag,
                                      &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  g_free(self->etag);
  self->etag = new_etag;
  g_free(self->persisted_text);
  self->persisted_text = g_strdup(self->current_text);
  self->revision++;
  set_dirty(self, FALSE);
  JsonObject *payload = json_object_new_owned();
  json_object_set_int_member(payload, "revision", (gint64)self->revision);
  pdfv_markdown_editor_bridge_send(self->bridge, "document/saved", NULL,
                                   payload);
  json_object_unref(payload);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void save_after_flush(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  PdfvMarkdownEditor *self = PDFV_MARKDOWN_EDITOR(source);
  GTask *task = G_TASK(user_data);
  GError *error = NULL;
  if (!pdfv_markdown_editor_flush_finish(self, result, &error)) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }
  self->saving = TRUE;
  g_file_replace_contents_async(
      self->file, self->current_text ? self->current_text : "",
      self->current_text ? strlen(self->current_text) : 0, self->etag, FALSE,
      G_FILE_CREATE_REPLACE_DESTINATION, g_task_get_cancellable(task),
      save_file_done, task);
}

void pdfv_markdown_editor_save_async(
    PdfvMarkdownEditor *self, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_markdown_editor_save_async);
  if (!self->file) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "No Markdown document is open");
    g_object_unref(task);
    return;
  }
  pdfv_markdown_editor_flush_async(self, cancellable, save_after_flush, task);
}

gboolean pdfv_markdown_editor_save_finish(
    PdfvMarkdownEditor *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

void pdfv_markdown_editor_run_command(PdfvMarkdownEditor *self,
                                      const gchar *command) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  JsonObject *payload = json_object_new_owned();
  json_object_set_string_member(payload, "command", command);
  pdfv_markdown_editor_bridge_send(self->bridge, "command/run", NULL,
                                   payload);
  json_object_unref(payload);
}

void pdfv_markdown_editor_reveal_fragment(PdfvMarkdownEditor *self,
                                          const gchar *target) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  JsonObject *payload = json_object_new_owned();
  json_object_set_string_member(payload, "target", target ? target : "");
  pdfv_markdown_editor_bridge_send(self->bridge, "navigation/reveal", NULL,
                                   payload);
  json_object_unref(payload);
}

void pdfv_markdown_editor_set_theme(PdfvMarkdownEditor *self,
                                    gboolean dark, gdouble font_scale) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  self->theme_dark = dark;
  self->font_scale = font_scale;
  self->theme_set = TRUE;
  send_theme(self);
}

void pdfv_markdown_editor_set_remote_images_allowed(
    PdfvMarkdownEditor *self, gboolean allowed) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  self->allow_remote_images = allowed;
  self->settings_set = TRUE;
  send_settings(self);
}

void pdfv_markdown_editor_focus(PdfvMarkdownEditor *self) {
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(self));
  gtk_widget_grab_focus(GTK_WIDGET(self->web_view));
}

gboolean pdfv_markdown_editor_get_dirty(PdfvMarkdownEditor *self) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_EDITOR(self), FALSE);
  return self->dirty;
}

GFile *pdfv_markdown_editor_get_file(PdfvMarkdownEditor *self) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_EDITOR(self), NULL);
  return self->file;
}

const gchar *pdfv_markdown_editor_get_relative_path(
    PdfvMarkdownEditor *self) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_EDITOR(self), NULL);
  return self->relative_path;
}

GFile *pdfv_markdown_editor_resolve_new_note(PdfvMarkdownEditor *self,
                                             const gchar *target,
                                             GError **error) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_EDITOR(self), NULL);
  return pdfv_markdown_vault_adapter_resolve_new_note(
      self->vault, self->relative_path, target, error);
}

static void pdfv_markdown_editor_dispose(GObject *object) {
  PdfvMarkdownEditor *self = PDFV_MARKDOWN_EDITOR(object);
  if (self->monitor)
    g_file_monitor_cancel(self->monitor);
  if (self->preamble_monitor)
    g_file_monitor_cancel(self->preamble_monitor);
  g_clear_object(&self->monitor);
  g_clear_object(&self->preamble_monitor);
  g_clear_pointer(&self->flush_tasks, g_hash_table_unref);
  g_clear_object(&self->bridge);
  g_clear_object(&self->resources);
  g_clear_object(&self->vault);
  g_clear_object(&self->content_manager);
  g_clear_object(&self->file);
  self->web_view = NULL;
  G_OBJECT_CLASS(pdfv_markdown_editor_parent_class)->dispose(object);
}

static void pdfv_markdown_editor_finalize(GObject *object) {
  PdfvMarkdownEditor *self = PDFV_MARKDOWN_EDITOR(object);
  g_free(self->document_id);
  g_free(self->relative_path);
  g_free(self->current_text);
  g_free(self->persisted_text);
  g_free(self->etag);
  G_OBJECT_CLASS(pdfv_markdown_editor_parent_class)->finalize(object);
}

static void pdfv_markdown_editor_class_init(PdfvMarkdownEditorClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = pdfv_markdown_editor_dispose;
  object_class->finalize = pdfv_markdown_editor_finalize;
  editor_signals[SIGNAL_DIRTY_CHANGED] = g_signal_new(
      "dirty-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  editor_signals[SIGNAL_OPEN_FILE] = g_signal_new(
      "open-file", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 1, G_TYPE_FILE);
  editor_signals[SIGNAL_CREATE_LINK] = g_signal_new(
      "create-link", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  editor_signals[SIGNAL_OPEN_EXTERNAL_URI] = g_signal_new(
      "open-external-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  editor_signals[SIGNAL_CONFLICT] = g_signal_new(
      "conflict", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 0);
  editor_signals[SIGNAL_EDITOR_ERROR] = g_signal_new(
      "editor-error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
      NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void pdfv_markdown_editor_init(PdfvMarkdownEditor *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_VERTICAL);
  self->revision = 0;
  self->flush_tasks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)flush_pending_free);
}

PdfvMarkdownEditor *pdfv_markdown_editor_new(GFile *vault_root) {
  g_return_val_if_fail(G_IS_FILE(vault_root), NULL);
  PdfvMarkdownEditor *self =
      g_object_new(PDFV_TYPE_MARKDOWN_EDITOR, NULL);
  self->vault = pdfv_markdown_vault_adapter_new(vault_root);
  self->resources = pdfv_markdown_resource_scheme_new(self->vault);
  self->content_manager = webkit_user_content_manager_new();
  self->web_view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW, "web-context",
      pdfv_markdown_resource_scheme_get_context(self->resources),
      "user-content-manager", self->content_manager, NULL));
  gtk_widget_set_hexpand(GTK_WIDGET(self->web_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self->web_view), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->web_view));

  WebKitSettings *settings = webkit_web_view_get_settings(self->web_view);
  g_object_set(settings, "enable-developer-extras", FALSE,
               "enable-html5-database", FALSE,
               "enable-html5-local-storage", FALSE,
               "enable-page-cache", FALSE, NULL);
  self->bridge = pdfv_markdown_editor_bridge_new(self->web_view,
                                                  self->content_manager);
  g_signal_connect(self->bridge, "message", G_CALLBACK(on_bridge_message),
                   self);
  g_signal_connect(self->bridge, "bridge-error",
                   G_CALLBACK(on_bridge_error), self);
  g_signal_connect(self->web_view, "decide-policy",
                   G_CALLBACK(on_decide_policy), self);
  g_signal_connect(self->web_view, "permission-request",
                   G_CALLBACK(on_permission_request), self);
  webkit_web_view_load_uri(self->web_view, "app://editor/index.html");
  return self;
}

/*
 * Phi Markdown PDF export dialogs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define G_LOG_DOMAIN "phi-markdown-export"

#include "markdown-export.h"

#include "markdown-editor-bridge.h"
#include "markdown-resource-scheme.h"
#include "markdown-vault-adapter.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <webkit/webkit.h>

#include <string.h>

#define MAX_EXPORT_NOTES 256
#define MAX_EXPORT_NOTE_BYTES (20 * 1024 * 1024)
#define MAX_EXPORT_EMBED_DEPTH 4
#define MIN_EXPORT_SCALE 50.0
#define MAX_EXPORT_SCALE 200.0

typedef struct {
  gchar *path;
  gchar *name;
  gchar *text;
} ExportNote;

typedef struct _PdfvMarkdownExport PdfvMarkdownExport;
typedef struct _PdfvMarkdownExportClass PdfvMarkdownExportClass;

struct _PdfvMarkdownExport {
  GObject parent_instance;
  GWeakRef parent;
  AdwDialog *dialog;
  AdwDialog *picker_dialog;
  WebKitWebView *web_view;
  WebKitUserContentManager *content_manager;
  PdfvMarkdownVaultAdapter *vault;
  PdfvMarkdownResourceScheme *resources;
  PdfvMarkdownEditorBridge *bridge;
  PdfvWorkspace *workspace;
  gchar *current_path;
  gchar *preamble;
  GPtrArray *notes;
  GHashTable *picker_selection;
  GtkTreeListModel *picker_tree;
  GtkButton *picker_done_button;
  GtkLabel *picker_count_label;
  AdwEntryRow *title_row;
  AdwEntryRow *author_row;
  AdwSpinRow *scale_row;
  AdwSwitchRow *cover_row;
  GtkWidget *title_required_icon;
  GtkListBox *order_list;
  GtkButton *export_button;
  GtkLabel *status_label;
  GtkWidget *preview_spinner;
  GtkWidget *busy_overlay;
  GtkLabel *busy_label;
  gboolean multiple;
  gboolean allow_remote_images;
  gdouble font_size;
  gdouble export_scale;
  gboolean web_ready;
  gboolean source_ready;
  gboolean preview_ready;
  gboolean preview_generation_active;
  gboolean preview_generation_pending;
  gboolean export_requested;
  gboolean busy;
  guint64 preview_revision;
  guint64 source_ready_revision;
  guint64 preview_generation_revision;
  guint64 cached_preview_revision;
  guint64 export_requested_revision;
  WebKitPrintOperation *print_operation;
  guint print_idle_id;
  gchar *print_error;
  gchar *print_source_filename;
  gchar *temporary_filename;
  gchar *suggested_filename;
  gchar *metadata_title;
  gchar *metadata_author;
  PdfvMarkdownExportSavedFunc saved_callback;
  gpointer saved_data;
};

struct _PdfvMarkdownExportClass {
  GObjectClass parent_class;
};

G_DEFINE_FINAL_TYPE(PdfvMarkdownExport, pdfv_markdown_export, G_TYPE_OBJECT)

static void send_preview_state(PdfvMarkdownExport *self, const gchar *type);
static void refresh_order_list(PdfvMarkdownExport *self);
static void update_export_enabled(PdfvMarkdownExport *self);
static void request_exact_preview(PdfvMarkdownExport *self, guint64 revision);

static gboolean note_filename_supported(const gchar *name) {
  const gchar *dot = name ? strrchr(name, '.') : NULL;
  return dot && (g_ascii_strcasecmp(dot, ".md") == 0 ||
                 g_ascii_strcasecmp(dot, ".markdown") == 0 ||
                 g_ascii_strcasecmp(dot, ".txt") == 0);
}

static ExportNote *export_note_new(const gchar *path, const gchar *text) {
  ExportNote *note = g_new0(ExportNote, 1);
  note->path = g_strdup(path);
  note->name = g_path_get_basename(path);
  note->text = g_strdup(text ? text : "");
  return note;
}

static ExportNote *export_note_copy(const ExportNote *note) {
  return export_note_new(note->path, note->text);
}

static void export_note_free(ExportNote *note) {
  if (!note)
    return;
  g_free(note->path);
  g_free(note->name);
  g_free(note->text);
  g_free(note);
}

static gchar *note_title(const gchar *path) {
  gchar *title = g_path_get_basename(path && *path ? path : "Document");
  gchar *dot = strrchr(title, '.');
  if (dot && note_filename_supported(title))
    *dot = '\0';
  return title;
}

static JsonNode *object_node(JsonObject *object) {
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, object);
  return node;
}

static void send_response(PdfvMarkdownExport *self, const gchar *id,
                          JsonNode *result, const gchar *error) {
  if (!id || !*id) {
    g_clear_pointer(&result, json_node_unref);
    return;
  }
  JsonObject *payload = json_object_new();
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

static JsonObject *document_object(const ExportNote *note) {
  JsonObject *document = json_object_new();
  json_object_set_string_member(document, "path", note->path);
  json_object_set_string_member(document, "name", note->name);
  json_object_set_string_member(document, "text", note->text);
  return document;
}

static const gchar *entry_text(AdwEntryRow *row) {
  return row ? gtk_editable_get_text(GTK_EDITABLE(row)) : "";
}

static gboolean entry_has_text(AdwEntryRow *row) {
  gchar *value = g_strdup(entry_text(row));
  g_strstrip(value);
  gboolean has_text = *value != '\0';
  g_free(value);
  return has_text;
}

static void set_status(PdfvMarkdownExport *self, const gchar *message,
                       gboolean error) {
  if (!self->status_label)
    return;
  gtk_label_set_text(self->status_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), message && *message);
  gtk_widget_remove_css_class(GTK_WIDGET(self->status_label), "error");
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "error");
}

static void set_preview_loading(PdfvMarkdownExport *self,
                                const gchar *message) {
  set_status(self, message, FALSE);
}

static void set_busy(PdfvMarkdownExport *self, gboolean busy,
                     const gchar *message) {
  self->busy = busy;
  if (self->busy_overlay)
    gtk_widget_set_visible(self->busy_overlay, busy);
  if (self->busy_label && message)
    gtk_label_set_text(self->busy_label, message);
  if (busy) {
    if (self->status_label)
      gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  }
  update_export_enabled(self);
}

static void update_export_enabled(PdfvMarkdownExport *self) {
  if (!self->export_button)
    return;
  gboolean valid_title = !self->multiple || entry_has_text(self->title_row);
  gboolean enabled = !self->busy && self->notes &&
      self->notes->len > 0 && valid_title;
  gtk_widget_set_sensitive(GTK_WIDGET(self->export_button), enabled);
  if (self->multiple && self->title_row) {
    if (valid_title)
      gtk_widget_remove_css_class(GTK_WIDGET(self->title_row), "error");
    else
      gtk_widget_add_css_class(GTK_WIDGET(self->title_row), "error");
    if (self->title_required_icon)
      gtk_widget_set_visible(self->title_required_icon, !valid_title);
  }
  if (self->multiple && !valid_title)
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->export_button),
                                "Enter a document title first");
  else
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->export_button), NULL);
}

static void send_preview_state(PdfvMarkdownExport *self, const gchar *type) {
  if (!self->bridge || !self->web_ready)
    return;
  self->preview_ready = FALSE;
  self->source_ready = FALSE;
  self->preview_revision++;
  if (self->export_requested)
    self->export_requested_revision = self->preview_revision;
  if (!self->busy)
    set_preview_loading(self, "Updating preview…");
  update_export_enabled(self);

  JsonObject *payload = json_object_new();
  json_object_set_boolean_member(payload, "multiple", self->multiple);
  json_object_set_string_member(payload, "title", entry_text(self->title_row));
  json_object_set_string_member(payload, "author", entry_text(self->author_row));
  json_object_set_double_member(payload, "fontSize", self->font_size);
  json_object_set_double_member(payload, "scale",
      self->scale_row ? adw_spin_row_get_value(self->scale_row) : 100.0);
  json_object_set_boolean_member(payload, "coverPage",
      self->cover_row ? adw_switch_row_get_active(self->cover_row) : TRUE);
  json_object_set_int_member(payload, "revision",
                             (gint64)self->preview_revision);

  if (g_str_equal(type, "export/initialize") ||
      g_str_equal(type, "export/documents")) {
    JsonArray *documents = json_array_new();
    for (guint i = 0; self->notes && i < self->notes->len; i++)
      json_array_add_object_element(
          documents, document_object(g_ptr_array_index(self->notes, i)));
    json_object_set_array_member(payload, "documents", documents);
    JsonObject *settings = json_object_new();
    json_object_set_boolean_member(settings, "allowRemoteImages",
                                   self->allow_remote_images);
    json_object_set_boolean_member(settings, "workspaceMode",
                                   self->workspace != NULL);
    json_object_set_object_member(payload, "settings", settings);
    json_object_set_string_member(payload, "preamble",
                                  self->preamble ? self->preamble : "");
  }

  pdfv_markdown_editor_bridge_send(self->bridge, type, NULL, payload);
  json_object_unref(payload);
}

static void on_metadata_changed(GtkEditable *editable,
                                PdfvMarkdownExport *self) {
  (void)editable;
  send_preview_state(self, "export/metadata");
  update_export_enabled(self);
}

static void on_scale_changed(AdwSpinRow *row, GParamSpec *pspec,
                             PdfvMarkdownExport *self) {
  (void)row;
  (void)pspec;
  send_preview_state(self, "export/metadata");
}

static void on_cover_changed(AdwSwitchRow *row, GParamSpec *pspec,
                             PdfvMarkdownExport *self) {
  (void)row;
  (void)pspec;
  send_preview_state(self, "export/metadata");
}

static void handle_embed_read(PdfvMarkdownExport *self, const gchar *id,
                              JsonObject *payload) {
  const gchar *target = payload
      ? json_object_get_string_member_with_default(payload, "target", "")
      : "";
  const gchar *source_path = payload
      ? json_object_get_string_member_with_default(
            payload, "sourcePath", self->current_path)
      : self->current_path;
  gint64 depth = payload
      ? json_object_get_int_member_with_default(payload, "depth", 0) : 0;
  if (depth > MAX_EXPORT_EMBED_DEPTH) {
    send_response(self, id, NULL, "Maximum embed depth exceeded");
    return;
  }
  GError *error = NULL;
  gchar *resolved_path = NULL;
  gchar *text = pdfv_markdown_vault_adapter_read_embed(
      self->vault, source_path, target, &resolved_path, &error);
  if (!text) {
    send_response(self, id, NULL,
                  error ? error->message : "Embedded note was not found");
  } else {
    JsonObject *value = json_object_new();
    json_object_set_string_member(value, "text", text);
    json_object_set_string_member(value, "path", resolved_path);
    send_response(self, id, object_node(value), NULL);
  }
  g_free(resolved_path);
  g_free(text);
  g_clear_error(&error);
}

static void handle_attachment_resolve(PdfvMarkdownExport *self,
                                      const gchar *id,
                                      JsonObject *payload) {
  const gchar *target = payload
      ? json_object_get_string_member_with_default(payload, "target", "")
      : "";
  const gchar *source_path = payload
      ? json_object_get_string_member_with_default(
            payload, "sourcePath", self->current_path)
      : self->current_path;
  gboolean relative = payload &&
      json_object_get_boolean_member_with_default(payload, "relative", FALSE);
  GError *error = NULL;
  GFile *file = pdfv_markdown_vault_adapter_resolve_attachment(
      self->vault, source_path, target, relative, &error);
  if (!file) {
    send_response(self, id, NULL,
                  error ? error->message : "Attachment was not found");
  } else {
    JsonObject *value = json_object_new();
    gchar *path = pdfv_markdown_vault_adapter_relative_path(self->vault, file);
    json_object_set_string_member(value, "path", path);
    gchar *filename = g_file_get_path(file);
    if (filename) {
      gint width = 0;
      gint height = 0;
      if (gdk_pixbuf_get_file_info(filename, &width, &height) && width > 0 &&
          height > 0) {
        json_object_set_int_member(value, "width", width);
        json_object_set_int_member(value, "height", height);
      }
    }
    send_response(self, id, object_node(value), NULL);
    g_free(filename);
    g_free(path);
  }
  g_clear_object(&file);
  g_clear_error(&error);
}

static gchar *safe_suggested_filename(const gchar *requested) {
  gchar *basename = g_path_get_basename(
      requested && *requested ? requested : "Document.pdf");
  for (gchar *at = basename; *at; at++) {
    if (g_ascii_iscntrl((guchar)*at) || *at == '/' || *at == '\\' ||
        *at == ':' || *at == '*' ||
        *at == '?' || *at == '"' || *at == '<' || *at == '>' || *at == '|')
      *at = '-';
  }
  g_strstrip(basename);
  if (!*basename) {
    g_free(basename);
    basename = g_strdup("Document.pdf");
  } else if (!g_str_has_suffix(basename, ".pdf") &&
             !g_str_has_suffix(basename, ".PDF")) {
    gchar *with_extension = g_strconcat(basename, ".pdf", NULL);
    g_free(basename);
    basename = with_extension;
  }
  return basename;
}

static void temporary_file_remove(PdfvMarkdownExport *self) {
  if (self->temporary_filename)
    g_unlink(self->temporary_filename);
  g_clear_pointer(&self->temporary_filename, g_free);
}

static void print_source_file_remove(PdfvMarkdownExport *self) {
  if (self->print_source_filename)
    g_unlink(self->print_source_filename);
  g_clear_pointer(&self->print_source_filename, g_free);
}

static void finish_preview_generation(PdfvMarkdownExport *self) {
  self->preview_generation_active = FALSE;
  gboolean refresh = self->preview_generation_pending;
  self->preview_generation_pending = FALSE;
  if (refresh && self->dialog)
    request_exact_preview(self, self->preview_revision);
}

static void cancel_pending_export(PdfvMarkdownExport *self) {
  if (!self->export_requested)
    return;
  self->export_requested = FALSE;
  if (self->dialog)
    adw_dialog_set_can_close(self->dialog, TRUE);
  set_busy(self, FALSE, NULL);
}

static void report_preview_generation_error(PdfvMarkdownExport *self,
                                            const gchar *message) {
  if (self->preview_generation_revision != self->preview_revision)
    return;
  if (self->export_requested_revision == self->preview_revision)
    cancel_pending_export(self);
  if (self->preview_spinner)
    gtk_widget_set_visible(self->preview_spinner, FALSE);
  set_status(self, message, TRUE);
}

static void on_pdf_copied(GObject *source, GAsyncResult *result,
                          gpointer user_data) {
  PdfvMarkdownExport *self = user_data;
  GError *error = NULL;
  gboolean copied = g_file_copy_finish(G_FILE(source), result, &error);
  if (self->dialog)
    adw_dialog_set_can_close(self->dialog, TRUE);
  set_busy(self, FALSE, NULL);
  if (!copied) {
    set_status(self, error ? error->message : "Could not save PDF", TRUE);
  } else {
    temporary_file_remove(self);
    GObject *parent_object = g_weak_ref_get(&self->parent);
    if (self->dialog)
      adw_dialog_close(self->dialog);
    if (parent_object && GTK_IS_WIDGET(parent_object) && self->saved_callback)
      self->saved_callback(GTK_WIDGET(parent_object), self->saved_data);
    g_clear_object(&parent_object);
  }
  g_clear_error(&error);
  g_object_unref(self);
}

static void on_save_file_selected(GObject *source, GAsyncResult *result,
                                  gpointer user_data) {
  PdfvMarkdownExport *self = user_data;
  GError *error = NULL;
  GFile *destination = gtk_file_dialog_save_finish(
      GTK_FILE_DIALOG(source), result, &error);
  if (!destination) {
    if (self->dialog)
      adw_dialog_set_can_close(self->dialog, TRUE);
    set_busy(self, FALSE, NULL);
    if (error && !g_error_matches(error, GTK_DIALOG_ERROR,
                                   GTK_DIALOG_ERROR_DISMISSED))
      set_status(self, error->message, TRUE);
    else
      set_status(self, NULL, FALSE);
    g_clear_error(&error);
    g_object_unref(self);
    return;
  }

  set_busy(self, TRUE, "Saving PDF…");
  GFile *temporary = g_file_new_for_path(self->temporary_filename);
  g_file_copy_async(temporary, destination, G_FILE_COPY_OVERWRITE,
                    G_PRIORITY_DEFAULT, NULL, NULL, NULL, on_pdf_copied,
                    g_object_ref(self));
  g_object_unref(temporary);
  g_object_unref(destination);
  g_object_unref(self);
}

static void choose_export_destination(PdfvMarkdownExport *self) {
  GObject *parent_object = g_weak_ref_get(&self->parent);
  GtkWindow *parent = parent_object && GTK_IS_WINDOW(parent_object)
      ? GTK_WINDOW(parent_object) : NULL;
  if (!parent) {
    temporary_file_remove(self);
    if (self->dialog)
      adw_dialog_set_can_close(self->dialog, TRUE);
    set_busy(self, FALSE, NULL);
    set_status(self, "The export window is no longer available", TRUE);
    g_clear_object(&parent_object);
    return;
  }

  set_busy(self, TRUE, "Choose where to save the generated PDF…");
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Save PDF");
  gtk_file_dialog_set_accept_label(dialog, "Save");
  gtk_file_dialog_set_initial_name(
      dialog, self->suggested_filename ? self->suggested_filename
                                       : "Document.pdf");
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "PDF Documents");
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_filter_add_pattern(filter, "*.pdf");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, filter);
  gtk_file_dialog_save(dialog, parent, NULL, on_save_file_selected,
                       g_object_ref(self));
  g_object_unref(filters);
  g_object_unref(filter);
  g_object_unref(dialog);
  g_object_unref(parent_object);
}

static void on_print_failed(WebKitPrintOperation *operation, GError *error,
                            PdfvMarkdownExport *self) {
  (void)operation;
  g_free(self->print_error);
  self->print_error = g_strdup(error ? error->message
                                     : "Could not generate PDF");
}

typedef struct {
  gchar *source;
  gchar *output;
  gchar *title;
  gchar *author;
  gdouble scale;
} PdfFinalizeData;

typedef struct {
  gchar *filename;
  GPtrArray *page_images;
} PdfFinalizeResult;

static void pdf_finalize_data_free(PdfFinalizeData *data) {
  if (!data)
    return;
  if (data->output)
    g_unlink(data->output);
  g_free(data->source);
  g_free(data->output);
  g_free(data->title);
  g_free(data->author);
  g_free(data);
}

static void pdf_finalize_result_free(PdfFinalizeResult *result) {
  if (!result)
    return;
  if (result->filename)
    g_unlink(result->filename);
  g_free(result->filename);
  g_clear_pointer(&result->page_images, g_ptr_array_unref);
  g_free(result);
}

static gboolean pdf_page_is_blank(fz_context *context, pdf_document *document,
                                  gint page_number) {
  fz_pixmap *pixmap = NULL;
  gboolean blank = TRUE;
  fz_var(pixmap);
  fz_try(context) {
    pixmap = fz_new_pixmap_from_page_number(
        context, (fz_document *)document, page_number,
        fz_scale(0.2f, 0.2f), fz_device_rgb(context), 0);
    gint width = fz_pixmap_width(context, pixmap);
    gint height = fz_pixmap_height(context, pixmap);
    gint components = fz_pixmap_components(context, pixmap);
    gint stride = fz_pixmap_stride(context, pixmap);
    guchar *samples = fz_pixmap_samples(context, pixmap);
    for (gint y = 0; blank && y < height; y++) {
      guchar *row = samples + y * stride;
      for (gint x = 0; blank && x < width; x++) {
        guchar *pixel = row + x * components;
        for (gint component = 0; component < MIN(components, 3);
             component++) {
          if (pixel[component] < 250) {
            blank = FALSE;
            break;
          }
        }
      }
    }
  }
  fz_always(context) {
    fz_drop_pixmap(context, pixmap);
  }
  fz_catch(context) {
    fz_rethrow(context);
  }
  return blank;
}

static pdf_obj *pdf_new_content_stream(fz_context *context,
                                       pdf_document *document,
                                       const gchar *contents) {
  fz_buffer *buffer = NULL;
  pdf_obj *stream = NULL;
  fz_var(buffer);
  fz_try(context) {
    buffer = fz_new_buffer_from_copied_data(
        context, (const unsigned char *)contents, strlen(contents));
    stream = pdf_add_stream(context, document, buffer, NULL, 0);
  }
  fz_always(context) {
    fz_drop_buffer(context, buffer);
  }
  fz_catch(context) {
    fz_rethrow(context);
  }
  return stream;
}

static void pdf_scale_annotation_rectangles(fz_context *context,
                                            pdf_obj *page,
                                            gdouble scale) {
  const gdouble left = 48.18898;
  const gdouble bottom = 56.69291;
  pdf_obj *annotations = pdf_dict_get(context, page, PDF_NAME(Annots));
  if (!pdf_is_array(context, annotations))
    return;
  gint count = pdf_array_len(context, annotations);
  for (gint index = 0; index < count; index++) {
    pdf_obj *annotation = pdf_array_get(context, annotations, index);
    pdf_obj *rectangle = pdf_dict_get(context, annotation, PDF_NAME(Rect));
    if (!pdf_is_array(context, rectangle) ||
        pdf_array_len(context, rectangle) != 4)
      continue;
    fz_rect value = pdf_to_rect(context, rectangle);
    value.x0 = value.x0 * scale + left;
    value.y0 = value.y0 * scale + bottom;
    value.x1 = value.x1 * scale + left;
    value.y1 = value.y1 * scale + bottom;
    pdf_dict_put_rect(context, annotation, PDF_NAME(Rect), value);
  }
}

static void pdf_scale_page_to_a4(fz_context *context,
                                 pdf_document *document, gint page_number,
                                 gdouble scale) {
  pdf_obj *prefix = NULL;
  pdf_obj *suffix = NULL;
  pdf_obj *wrapped = NULL;
  fz_var(prefix);
  fz_var(suffix);
  fz_var(wrapped);
  fz_try(context) {
    gchar prefix_commands[G_ASCII_DTOSTR_BUF_SIZE * 2 + 48];
    gchar horizontal[G_ASCII_DTOSTR_BUF_SIZE];
    gchar vertical[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(horizontal, sizeof(horizontal), scale);
    g_ascii_dtostr(vertical, sizeof(vertical), scale);
    g_snprintf(prefix_commands, sizeof(prefix_commands),
               "q\n%s 0 0 %s 48.18898 56.69291 cm\n",
               horizontal, vertical);
    prefix = pdf_new_content_stream(context, document, prefix_commands);
    suffix = pdf_new_content_stream(context, document, "Q\n");

    pdf_obj *page = pdf_lookup_page_obj(context, document, page_number);
    pdf_obj *contents = pdf_dict_get(context, page, PDF_NAME(Contents));
    gint original_count = pdf_is_array(context, contents)
        ? pdf_array_len(context, contents) : (contents ? 1 : 0);
    wrapped = pdf_new_array(context, document, original_count + 2);
    pdf_array_push(context, wrapped, prefix);
    if (pdf_is_array(context, contents)) {
      for (gint index = 0; index < original_count; index++)
        pdf_array_push(context, wrapped,
                       pdf_array_get(context, contents, index));
    } else if (contents) {
      pdf_array_push(context, wrapped, contents);
    }
    pdf_array_push(context, wrapped, suffix);
    pdf_dict_put(context, page, PDF_NAME(Contents), wrapped);

    const fz_rect a4 = fz_make_rect(0, 0, 595.2756f, 841.8898f);
    pdf_dict_put_rect(context, page, PDF_NAME(MediaBox), a4);
    pdf_dict_put_rect(context, page, PDF_NAME(CropBox), a4);
    if (pdf_dict_get(context, page, PDF_NAME(BleedBox)))
      pdf_dict_put_rect(context, page, PDF_NAME(BleedBox), a4);
    if (pdf_dict_get(context, page, PDF_NAME(TrimBox)))
      pdf_dict_put_rect(context, page, PDF_NAME(TrimBox), a4);
    if (pdf_dict_get(context, page, PDF_NAME(ArtBox)))
      pdf_dict_put_rect(context, page, PDF_NAME(ArtBox), a4);
    pdf_scale_annotation_rectangles(context, page, scale);
  }
  fz_always(context) {
    pdf_drop_obj(context, wrapped);
    pdf_drop_obj(context, suffix);
    pdf_drop_obj(context, prefix);
  }
  fz_catch(context) {
    fz_rethrow(context);
  }
}

static gboolean finalize_pdf(const PdfFinalizeData *data, GError **error) {
  fz_context *context = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
  if (!context) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not initialize PDF metadata support");
    return FALSE;
  }

  pdf_document *document = NULL;
  gchar *failure = NULL;
  fz_var(document);
  fz_try(context) {
    fz_register_document_handlers(context);
    document = pdf_open_document(context, data->source);
    pdf_obj *trailer = pdf_trailer(context, document);
    pdf_obj *info = pdf_dict_get(context, trailer, PDF_NAME(Info));
    if (!pdf_is_dict(context, info)) {
      pdf_obj *reference = pdf_add_new_dict(context, document, 4);
      pdf_dict_put_drop(context, trailer, PDF_NAME(Info), reference);
      info = pdf_dict_get(context, trailer, PDF_NAME(Info));
    }
    pdf_dict_put_text_string(context, info, PDF_NAME(Title), data->title);
    pdf_dict_put_text_string(context, info, PDF_NAME(Author), data->author);
    pdf_dict_put_text_string(context, info, PDF_NAME(Creator),
                             "Phi Document Viewer");

    gint page_count = pdf_count_pages(context, document);
    for (gint page = 0; page < page_count; page++)
      pdf_scale_page_to_a4(context, document, page, data->scale);
    for (gint page = page_count - 1; page >= 0 && page_count > 1; page--) {
      if (pdf_page_is_blank(context, document, page)) {
        pdf_delete_page(context, document, page);
        page_count--;
      }
    }
    pdf_write_options options = pdf_default_write_options;
    pdf_save_document(context, document, data->output, &options);
  }
  fz_catch(context) {
    failure = g_strdup(fz_caught_message(context));
  }
  pdf_drop_document(context, document);
  fz_drop_context(context);

  if (failure) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Could not finalize the PDF: %s", failure);
    g_free(failure);
    return FALSE;
  }
  return TRUE;
}

static GPtrArray *render_pdf_preview_pages(const gchar *filename,
                                           GError **error) {
  fz_context *context = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
  if (!context) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not initialize PDF preview rendering");
    return NULL;
  }

  pdf_document *document = NULL;
  GPtrArray *pages = g_ptr_array_new_with_free_func(g_free);
  gchar *failure = NULL;
  fz_var(document);
  fz_try(context) {
    fz_register_document_handlers(context);
    document = pdf_open_document(context, filename);
    gint page_count = pdf_count_pages(context, document);
    if (page_count < 1)
      fz_throw(context, FZ_ERROR_FORMAT, "Finalized PDF has no pages");
    for (gint page = 0; page < page_count; page++) {
      fz_pixmap *pixmap = NULL;
      fz_buffer *png = NULL;
      fz_var(pixmap);
      fz_var(png);
      fz_try(context) {
        const gfloat raster_scale = 96.0f / 72.0f;
        pixmap = fz_new_pixmap_from_page_number(
            context, (fz_document *)document, page,
            fz_scale(raster_scale, raster_scale), fz_device_rgb(context), 0);
        png = fz_new_buffer_from_pixmap_as_png(
            context, pixmap, fz_default_color_params);
        unsigned char *bytes = NULL;
        size_t length = fz_buffer_storage(context, png, &bytes);
        gchar *encoded = g_base64_encode(bytes, length);
        g_ptr_array_add(pages,
                        g_strconcat("data:image/png;base64,", encoded, NULL));
        g_free(encoded);
      }
      fz_always(context) {
        fz_drop_buffer(context, png);
        fz_drop_pixmap(context, pixmap);
      }
      fz_catch(context) {
        fz_rethrow(context);
      }
    }
  }
  fz_catch(context) {
    failure = g_strdup(fz_caught_message(context));
  }
  pdf_drop_document(context, document);
  fz_drop_context(context);

  if (failure) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Could not render the PDF preview: %s", failure);
    g_free(failure);
    g_ptr_array_unref(pages);
    return NULL;
  }
  return pages;
}

static void finalize_pdf_thread(GTask *task, gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;
  PdfFinalizeData *data = task_data;
  GError *error = NULL;
  if (!finalize_pdf(data, &error)) {
    g_task_return_error(task, error);
    return;
  }
  GPtrArray *pages = render_pdf_preview_pages(data->output, &error);
  if (!pages) {
    g_task_return_error(task, error);
    return;
  }
  PdfFinalizeResult *result = g_new0(PdfFinalizeResult, 1);
  result->filename = g_steal_pointer(&data->output);
  result->page_images = pages;
  g_task_return_pointer(task, result,
                        (GDestroyNotify)pdf_finalize_result_free);
}

static void on_pdf_finalized(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  (void)user_data;
  PdfvMarkdownExport *self = (PdfvMarkdownExport *)source;
  GError *error = NULL;
  PdfFinalizeResult *finalized = g_task_propagate_pointer(G_TASK(result),
                                                          &error);
  print_source_file_remove(self);
  if (!finalized) {
    report_preview_generation_error(
        self, error ? error->message : "Could not finalize PDF");
  } else if (self->preview_generation_revision == self->preview_revision &&
             self->dialog) {
    temporary_file_remove(self);
    self->temporary_filename = g_steal_pointer(&finalized->filename);
    self->cached_preview_revision = self->preview_generation_revision;

    if (self->bridge) {
      JsonObject *payload = json_object_new();
      json_object_set_int_member(payload, "revision",
                                 (gint64)self->cached_preview_revision);
      JsonArray *pages = json_array_new();
      for (guint index = 0; index < finalized->page_images->len; index++)
        json_array_add_string_element(
            pages, g_ptr_array_index(finalized->page_images, index));
      json_object_set_array_member(payload, "pages", pages);
      pdfv_markdown_editor_bridge_send(self->bridge, "export/pdf-preview",
                                       NULL, payload);
      json_object_unref(payload);
    }

    gboolean begin_save = self->export_requested &&
        self->export_requested_revision == self->cached_preview_revision;
    if (begin_save) {
      self->export_requested = FALSE;
      choose_export_destination(self);
    } else if (!self->busy && self->bridge) {
      set_preview_loading(self, "Displaying preview…");
    }
    update_export_enabled(self);
  }
  pdf_finalize_result_free(finalized);
  g_clear_error(&error);
  finish_preview_generation(self);
}

static void begin_pdf_finalization(PdfvMarkdownExport *self) {
  GError *error = NULL;
  gchar *output = NULL;
  gint descriptor = g_file_open_tmp("phi-export-final-XXXXXX.pdf", &output,
                                    &error);
  if (descriptor < 0) {
    report_preview_generation_error(
        self, error ? error->message : "Could not prepare PDF metadata");
    g_clear_error(&error);
    g_free(output);
    print_source_file_remove(self);
    finish_preview_generation(self);
    return;
  }
  g_close(descriptor, NULL);
  g_unlink(output);

  PdfFinalizeData *data = g_new0(PdfFinalizeData, 1);
  data->source = g_strdup(self->print_source_filename);
  data->output = output;
  data->title = g_strdup(self->metadata_title ? self->metadata_title : "");
  data->author = g_strdup(self->metadata_author ? self->metadata_author : "");
  data->scale = CLAMP(self->export_scale, MIN_EXPORT_SCALE,
                      MAX_EXPORT_SCALE) / 100.0;
  GTask *task = g_task_new(self, NULL, on_pdf_finalized, NULL);
  g_task_set_task_data(task, data, (GDestroyNotify)pdf_finalize_data_free);
  if (self->preview_generation_revision == self->preview_revision)
    set_preview_loading(self, "Finalizing preview…");
  g_task_run_in_thread(task, finalize_pdf_thread);
  g_object_unref(task);
}

static void on_print_finished(WebKitPrintOperation *operation,
                              PdfvMarkdownExport *self) {
  (void)operation;
  g_clear_object(&self->print_operation);
  if (self->print_error) {
    report_preview_generation_error(self, self->print_error);
    print_source_file_remove(self);
    g_clear_pointer(&self->print_error, g_free);
    finish_preview_generation(self);
    return;
  }
  GStatBuf info;
  if (!self->print_source_filename ||
      g_stat(self->print_source_filename, &info) != 0 || info.st_size <= 0) {
    report_preview_generation_error(self,
                                    "WebKit did not produce a PDF file");
    print_source_file_remove(self);
    finish_preview_generation(self);
    return;
  }
  begin_pdf_finalization(self);
}

static void start_preview_print(PdfvMarkdownExport *self) {
  if (self->print_operation)
    return;
  print_source_file_remove(self);
  g_clear_pointer(&self->print_error, g_free);

  GError *error = NULL;
  gint descriptor = g_file_open_tmp("phi-export-XXXXXX.pdf",
                                    &self->print_source_filename, &error);
  if (descriptor < 0) {
    report_preview_generation_error(
        self, error ? error->message : "Could not create a temporary PDF");
    g_clear_error(&error);
    finish_preview_generation(self);
    return;
  }
  g_close(descriptor, NULL);
  g_unlink(self->print_source_filename);

  gchar *uri = g_filename_to_uri(self->print_source_filename, NULL, &error);
  if (!uri) {
    report_preview_generation_error(self, error->message);
    g_clear_error(&error);
    print_source_file_remove(self);
    finish_preview_generation(self);
    return;
  }

  self->print_operation = webkit_print_operation_new(self->web_view);
  GtkPrintSettings *settings = gtk_print_settings_new();
  gtk_print_settings_set_printer(settings, "Print to File");
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT,
                         "pdf");
  gtk_print_settings_set_orientation(settings,
                                     GTK_PAGE_ORIENTATION_PORTRAIT);
  webkit_print_operation_set_print_settings(self->print_operation, settings);
  g_object_unref(settings);

  gdouble scale = CLAMP(self->export_scale, MIN_EXPORT_SCALE,
                        MAX_EXPORT_SCALE) / 100.0;
  GtkPageSetup *setup = gtk_page_setup_new();
  GtkPaperSize *paper = gtk_paper_size_new_custom(
      "phi-export-page", "Phi export page", 176.0 / scale, 259.0 / scale,
      GTK_UNIT_MM);
  gtk_page_setup_set_paper_size_and_default_margins(setup, paper);
  gtk_page_setup_set_top_margin(setup, 0, GTK_UNIT_MM);
  gtk_page_setup_set_bottom_margin(setup, 0, GTK_UNIT_MM);
  gtk_page_setup_set_left_margin(setup, 0, GTK_UNIT_MM);
  gtk_page_setup_set_right_margin(setup, 0, GTK_UNIT_MM);
  gtk_page_setup_set_orientation(setup, GTK_PAGE_ORIENTATION_PORTRAIT);
  webkit_print_operation_set_page_setup(self->print_operation, setup);
  gtk_paper_size_free(paper);
  g_object_unref(setup);

  g_signal_connect(self->print_operation, "failed",
                   G_CALLBACK(on_print_failed), self);
  g_signal_connect(self->print_operation, "finished",
                   G_CALLBACK(on_print_finished), self);
  if (self->preview_generation_revision == self->preview_revision)
    set_preview_loading(self, "Rendering preview…");
  webkit_print_operation_print(self->print_operation);
  g_free(uri);
}

typedef struct {
  PdfvMarkdownExport *export;
} PrintIdleRequest;

static void print_idle_request_free(PrintIdleRequest *request) {
  g_object_unref(request->export);
  g_free(request);
}

static gboolean start_print_idle(gpointer user_data) {
  PrintIdleRequest *request = user_data;
  request->export->print_idle_id = 0;
  start_preview_print(request->export);
  return G_SOURCE_REMOVE;
}

static void schedule_preview_print(PdfvMarkdownExport *self) {
  if (self->print_operation || self->print_idle_id)
    return;
  PrintIdleRequest *request = g_new0(PrintIdleRequest, 1);
  request->export = g_object_ref(self);
  self->print_idle_id = g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE, start_print_idle, request,
      (GDestroyNotify)print_idle_request_free);
}

static void capture_preview_metadata(PdfvMarkdownExport *self) {
  gchar *entered = g_strdup(entry_text(self->title_row));
  g_strstrip(entered);
  ExportNote *first = self->notes && self->notes->len
      ? g_ptr_array_index(self->notes, 0) : NULL;
  gchar *fallback = first ? note_title(first->name) : g_strdup("Document");
  g_free(self->metadata_title);
  self->metadata_title = g_strdup(
      self->multiple ? entered : (*entered ? entered : fallback));
  g_free(self->metadata_author);
  self->metadata_author = g_strdup(entry_text(self->author_row));
  g_strstrip(self->metadata_author);
  self->export_scale = CLAMP(adw_spin_row_get_value(self->scale_row),
                             MIN_EXPORT_SCALE, MAX_EXPORT_SCALE);
  g_free(fallback);
  g_free(entered);
}

static void request_exact_preview(PdfvMarkdownExport *self, guint64 revision) {
  if (!self->dialog || revision != self->preview_revision)
    return;
  if (self->temporary_filename && self->cached_preview_revision == revision)
    return;
  if (self->preview_generation_active || self->print_operation ||
      self->print_idle_id) {
    if (!self->preview_generation_active ||
        self->preview_generation_revision != revision)
      self->preview_generation_pending = TRUE;
    return;
  }
  capture_preview_metadata(self);
  self->preview_generation_active = TRUE;
  self->preview_generation_pending = FALSE;
  self->preview_generation_revision = revision;
  schedule_preview_print(self);
}

static void on_export_clicked(GtkButton *button,
                              PdfvMarkdownExport *self) {
  (void)button;
  if (self->busy || !self->notes->len ||
      (self->multiple && !entry_has_text(self->title_row)))
    return;
  gchar *entered = g_strdup(entry_text(self->title_row));
  g_strstrip(entered);
  ExportNote *first = g_ptr_array_index(self->notes, 0);
  gchar *fallback = note_title(first->name);
  const gchar *base = *entered ? entered : fallback;
  gchar *suggested = g_strconcat(base, ".pdf", NULL);
  g_free(fallback);
  g_free(self->suggested_filename);
  self->suggested_filename = safe_suggested_filename(suggested);
  if (self->dialog)
    adw_dialog_set_can_close(self->dialog, FALSE);
  if (self->temporary_filename &&
      self->cached_preview_revision == self->preview_revision) {
    set_busy(self, TRUE, "Preparing PDF…");
    choose_export_destination(self);
  } else {
    self->export_requested = TRUE;
    self->export_requested_revision = self->preview_revision;
    set_busy(self, TRUE, "Generating PDF…");
    if (self->source_ready &&
        self->source_ready_revision == self->preview_revision)
      request_exact_preview(self, self->preview_revision);
  }
  g_free(suggested);
  g_free(entered);
}

static gint note_index_for_path(PdfvMarkdownExport *self,
                                const gchar *path) {
  for (guint i = 0; self->notes && i < self->notes->len; i++) {
    ExportNote *note = g_ptr_array_index(self->notes, i);
    if (g_str_equal(note->path, path))
      return (gint)i;
  }
  return -1;
}

static GdkContentProvider *on_order_drag_prepare(
    GtkDragSource *source, gdouble x, gdouble y, PdfvMarkdownExport *self) {
  (void)x;
  (void)y;
  (void)self;
  GtkWidget *widget = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(source));
  const gchar *path = g_object_get_data(G_OBJECT(widget), "export-path");
  if (!path)
    return NULL;
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_STRING);
  g_value_set_string(&value, path);
  GdkContentProvider *provider = gdk_content_provider_new_for_value(&value);
  g_value_unset(&value);
  return provider;
}

static void move_note(PdfvMarkdownExport *self, guint source,
                      guint destination) {
  if (source >= self->notes->len || destination >= self->notes->len ||
      source == destination)
    return;
  ExportNote *note = g_ptr_array_steal_index(self->notes, source);
  g_ptr_array_insert(self->notes, destination, note);
  refresh_order_list(self);
  send_preview_state(self, "export/documents");
}

static gboolean on_order_drop(GtkDropTarget *target, const GValue *value,
                              gdouble x, gdouble y,
                              PdfvMarkdownExport *self) {
  (void)x;
  if (!G_VALUE_HOLDS_STRING(value))
    return FALSE;
  const gchar *source_path = g_value_get_string(value);
  GtkWidget *widget = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(target));
  const gchar *target_path = g_object_get_data(G_OBJECT(widget), "export-path");
  gint source = note_index_for_path(self, source_path);
  gint destination = note_index_for_path(self, target_path);
  if (source < 0 || destination < 0)
    return FALSE;
  gint insertion = destination +
      (y > gtk_widget_get_height(widget) / 2.0 ? 1 : 0);
  if (source < insertion)
    insertion--;
  destination = CLAMP(insertion, 0, (gint)self->notes->len - 1);
  move_note(self, (guint)source, (guint)destination);
  return TRUE;
}

static void on_order_move_clicked(GtkButton *button,
                                  PdfvMarkdownExport *self) {
  const gchar *path = g_object_get_data(G_OBJECT(button), "export-path");
  gint delta = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(button), "export-delta"));
  gint source = note_index_for_path(self, path);
  gint destination = source + delta;
  if (source >= 0 && destination >= 0 &&
      destination < (gint)self->notes->len)
    move_note(self, (guint)source, (guint)destination);
}

static void on_order_remove_clicked(GtkButton *button,
                                    PdfvMarkdownExport *self) {
  const gchar *path = g_object_get_data(G_OBJECT(button), "export-path");
  gint index = note_index_for_path(self, path);
  if (index < 0)
    return;
  g_ptr_array_remove_index(self->notes, (guint)index);
  refresh_order_list(self);
  send_preview_state(self, "export/documents");
}

static GtkWidget *icon_button(const gchar *icon, const gchar *tooltip) {
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_tooltip_text(button, tooltip);
  return button;
}

static void refresh_order_list(PdfvMarkdownExport *self) {
  if (!self->order_list)
    return;
  gtk_list_box_remove_all(self->order_list);
  for (guint i = 0; i < self->notes->len; i++) {
    ExportNote *note = g_ptr_array_index(self->notes, i);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 6);
    GtkWidget *handle = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
    gtk_widget_add_css_class(handle, "dim-label");
    gtk_widget_set_tooltip_text(handle, "Drag to reorder");
    GtkWidget *label = gtk_label_new(note->name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(label, note->path);
    GtkWidget *up = icon_button("go-up-symbolic", "Move up");
    GtkWidget *down = icon_button("go-down-symbolic", "Move down");
    GtkWidget *remove = icon_button("edit-delete-symbolic", "Remove");
    gtk_widget_set_sensitive(up, i > 0);
    gtk_widget_set_sensitive(down, i + 1 < self->notes->len);
    g_object_set_data_full(G_OBJECT(up), "export-path", g_strdup(note->path),
                           g_free);
    g_object_set_data(G_OBJECT(up), "export-delta", GINT_TO_POINTER(-1));
    g_object_set_data_full(G_OBJECT(down), "export-path",
                           g_strdup(note->path), g_free);
    g_object_set_data(G_OBJECT(down), "export-delta", GINT_TO_POINTER(1));
    g_object_set_data_full(G_OBJECT(remove), "export-path",
                           g_strdup(note->path), g_free);
    g_signal_connect(up, "clicked", G_CALLBACK(on_order_move_clicked), self);
    g_signal_connect(down, "clicked", G_CALLBACK(on_order_move_clicked), self);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_order_remove_clicked),
                     self);
    gtk_box_append(GTK_BOX(box), handle);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), up);
    gtk_box_append(GTK_BOX(box), down);
    gtk_box_append(GTK_BOX(box), remove);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "export-path", g_strdup(note->path),
                           g_free);

    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_order_drag_prepare), self);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drag));
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop", G_CALLBACK(on_order_drop), self);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
    gtk_list_box_append(self->order_list, row);
  }
  update_export_enabled(self);
}

static gboolean picker_item_visible(gpointer item, gpointer user_data) {
  (void)user_data;
  PdfvWorkspaceItem *workspace_item = PDFV_WORKSPACE_ITEM(item);
  return pdfv_workspace_item_is_folder(workspace_item) ||
      note_filename_supported(pdfv_workspace_item_get_name(workspace_item));
}

static GListModel *filtered_workspace_model(GListModel *model) {
  GtkCustomFilter *filter = gtk_custom_filter_new(picker_item_visible, NULL,
                                                   NULL);
  return G_LIST_MODEL(gtk_filter_list_model_new(g_object_ref(model),
                                                 GTK_FILTER(filter)));
}

static GListModel *picker_create_children(gpointer item,
                                          gpointer user_data) {
  (void)user_data;
  PdfvWorkspaceItem *workspace_item = PDFV_WORKSPACE_ITEM(item);
  if (!pdfv_workspace_item_is_folder(workspace_item))
    return NULL;
  return filtered_workspace_model(
      pdfv_workspace_item_get_children(workspace_item));
}

static void update_picker_count(PdfvMarkdownExport *self) {
  guint count = self->picker_selection
      ? g_hash_table_size(self->picker_selection) : 0;
  if (self->picker_count_label) {
    gchar *message = g_strdup_printf("%u note%s selected", count,
                                     count == 1 ? "" : "s");
    gtk_label_set_text(self->picker_count_label, message);
    g_free(message);
  }
  if (self->picker_done_button)
    gtk_widget_set_sensitive(GTK_WIDGET(self->picker_done_button),
                             count > 0 && count <= MAX_EXPORT_NOTES);
}

static void on_picker_check_toggled(GtkCheckButton *button,
                                    PdfvMarkdownExport *self) {
  if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "picker-binding")))
    return;
  const gchar *path = g_object_get_data(G_OBJECT(button), "picker-path");
  if (!path || !self->picker_selection)
    return;
  if (gtk_check_button_get_active(button))
    g_hash_table_add(self->picker_selection, g_strdup(path));
  else
    g_hash_table_remove(self->picker_selection, path);
  update_picker_count(self);
}

static void on_picker_row_pressed(GtkGestureClick *gesture, gint n_press,
                                  gdouble x, gdouble y,
                                  PdfvMarkdownExport *self) {
  (void)n_press;
  (void)self;
  GtkWidget *box = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(gesture));
  GtkCheckButton *check = g_object_get_data(G_OBJECT(box), "picker-check");
  if (!check || !gtk_widget_get_visible(GTK_WIDGET(check)))
    return;
  GtkWidget *picked = gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT);
  if (picked == GTK_WIDGET(check) ||
      (picked && gtk_widget_is_ancestor(picked, GTK_WIDGET(check))))
    return;
  gtk_check_button_set_active(check, !gtk_check_button_get_active(check));
}

static void picker_factory_setup(GtkSignalListItemFactory *factory,
                                 GtkListItem *list_item,
                                 PdfvMarkdownExport *self) {
  (void)factory;
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  GtkWidget *check = gtk_check_button_new();
  GtkWidget *icon = gtk_image_new();
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), check);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  g_object_set_data(G_OBJECT(list_item), "picker-check", check);
  g_object_set_data(G_OBJECT(list_item), "picker-icon", icon);
  g_object_set_data(G_OBJECT(list_item), "picker-label", label);
  g_object_set_data(G_OBJECT(box), "picker-check", check);
  g_signal_connect(check, "toggled", G_CALLBACK(on_picker_check_toggled),
                   self);
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_picker_row_pressed), self);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  gtk_list_item_set_child(list_item, expander);
}

static void picker_factory_bind(GtkSignalListItemFactory *factory,
                                GtkListItem *list_item,
                                PdfvMarkdownExport *self) {
  (void)factory;
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(list_item));
  GtkTreeExpander *expander = GTK_TREE_EXPANDER(
      gtk_list_item_get_child(list_item));
  gtk_tree_expander_set_list_row(expander, row);
  PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
  gboolean folder = pdfv_workspace_item_is_folder(item);
  const gchar *path = pdfv_workspace_item_get_relative_path(item);
  GtkCheckButton *check = GTK_CHECK_BUTTON(
      g_object_get_data(G_OBJECT(list_item), "picker-check"));
  GtkImage *icon = GTK_IMAGE(
      g_object_get_data(G_OBJECT(list_item), "picker-icon"));
  GtkLabel *label = GTK_LABEL(
      g_object_get_data(G_OBJECT(list_item), "picker-label"));
  gtk_widget_set_visible(GTK_WIDGET(check), !folder);
  gtk_image_set_from_icon_name(icon, folder ? "folder-symbolic"
                                           : "document-edit-symbolic");
  gtk_label_set_text(label, pdfv_workspace_item_get_name(item));
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), path);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL,
      pdfv_workspace_item_get_name(item), -1);
  g_object_set_data(G_OBJECT(check), "picker-binding", GINT_TO_POINTER(TRUE));
  g_object_set_data_full(G_OBJECT(check), "picker-path",
                         folder ? NULL : g_strdup(path), g_free);
  gtk_check_button_set_active(check, !folder &&
      g_hash_table_contains(self->picker_selection, path));
  g_object_set_data(G_OBJECT(check), "picker-binding", NULL);
  g_object_unref(item);
}

static void picker_factory_unbind(GtkSignalListItemFactory *factory,
                                  GtkListItem *list_item,
                                  PdfvMarkdownExport *self) {
  (void)factory;
  (void)self;
  GtkTreeExpander *expander = GTK_TREE_EXPANDER(
      gtk_list_item_get_child(list_item));
  GtkCheckButton *check = GTK_CHECK_BUTTON(
      g_object_get_data(G_OBJECT(list_item), "picker-check"));
  g_object_set_data(G_OBJECT(check), "picker-path", NULL);
  gtk_tree_expander_set_list_row(expander, NULL);
}

static gboolean selected_below_folder(PdfvMarkdownExport *self,
                                      const gchar *folder) {
  if (!self->picker_selection)
    return FALSE;
  gsize length = strlen(folder);
  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, self->picker_selection);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    const gchar *path = key;
    if (g_str_has_prefix(path, folder) && path[length] == G_DIR_SEPARATOR)
      return TRUE;
  }
  return FALSE;
}

static void expand_picker_selection(PdfvMarkdownExport *self) {
  if (!self->picker_tree)
    return;
  for (guint position = 0;
       position < g_list_model_get_n_items(G_LIST_MODEL(self->picker_tree));
       position++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(self->picker_tree,
                                                       position);
    if (!row)
      continue;
    PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
    if (pdfv_workspace_item_is_folder(item) &&
        selected_below_folder(
            self, pdfv_workspace_item_get_relative_path(item)))
      gtk_tree_list_row_set_expanded(row, TRUE);
    g_object_unref(item);
    g_object_unref(row);
  }
}

static ExportNote *load_export_note(PdfvMarkdownExport *self,
                                    const gchar *path, GError **error) {
  gchar *basename = path ? g_path_get_basename(path) : NULL;
  if (!path || !basename || !note_filename_supported(basename)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The selection contains an unsupported note");
    g_free(basename);
    return NULL;
  }
  GFile *file = pdfv_markdown_vault_adapter_resolve(self->vault, path, error);
  GFileInfo *info = file ? g_file_query_info(
      file, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
            G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error) : NULL;
  if (info && (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR ||
               g_file_info_get_size(info) > MAX_EXPORT_NOTE_BYTES))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "A selected note is not a supported text file");
  gchar *text = info && !*error
      ? pdfv_markdown_vault_adapter_read_text(self->vault, file, NULL, error)
      : NULL;
  ExportNote *note = text ? export_note_new(path, text) : NULL;
  g_free(text);
  g_clear_object(&info);
  g_clear_object(&file);
  g_free(basename);
  return note;
}

static gboolean append_selected_notes(PdfvMarkdownExport *self,
                                      GListModel *model,
                                      GHashTable *added,
                                      GPtrArray *notes,
                                      GError **error) {
  for (guint i = 0; model && i < g_list_model_get_n_items(model); i++) {
    PdfvWorkspaceItem *item = g_list_model_get_item(model, i);
    const gchar *path = pdfv_workspace_item_get_relative_path(item);
    if (pdfv_workspace_item_is_folder(item)) {
      gboolean ok = append_selected_notes(
          self, pdfv_workspace_item_get_children(item), added, notes, error);
      g_object_unref(item);
      if (!ok)
        return FALSE;
      continue;
    }
    if (g_hash_table_contains(self->picker_selection, path) &&
        !g_hash_table_contains(added, path)) {
      ExportNote *note = load_export_note(self, path, error);
      if (!note) {
        g_object_unref(item);
        return FALSE;
      }
      g_ptr_array_add(notes, note);
      g_hash_table_add(added, g_strdup(path));
    }
    g_object_unref(item);
  }
  return TRUE;
}

static void present_native_error(PdfvMarkdownExport *self,
                                 const gchar *title,
                                 const gchar *message) {
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, message));
  adw_alert_dialog_add_response(alert, "ok", "OK");
  adw_alert_dialog_set_default_response(alert, "ok");
  GtkWidget *parent = self->picker_dialog
      ? GTK_WIDGET(self->picker_dialog) : GTK_WIDGET(self->dialog);
  adw_dialog_present(ADW_DIALOG(alert), parent);
}

static void close_picker(GtkButton *button, PdfvMarkdownExport *self) {
  (void)button;
  if (self->picker_dialog)
    adw_dialog_close(self->picker_dialog);
}

static void on_picker_done(GtkButton *button,
                           PdfvMarkdownExport *self) {
  (void)button;
  guint count = g_hash_table_size(self->picker_selection);
  if (count == 0 || count > MAX_EXPORT_NOTES)
    return;

  GPtrArray *notes = g_ptr_array_new_with_free_func(
      (GDestroyNotify)export_note_free);
  GHashTable *added = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            NULL);
  for (guint i = 0; i < self->notes->len; i++) {
    ExportNote *note = g_ptr_array_index(self->notes, i);
    if (g_hash_table_contains(self->picker_selection, note->path)) {
      g_ptr_array_add(notes, export_note_copy(note));
      g_hash_table_add(added, g_strdup(note->path));
    }
  }

  GError *error = NULL;
  gboolean loaded = append_selected_notes(
      self, pdfv_workspace_get_items(self->workspace), added, notes, &error);
  g_hash_table_unref(added);
  if (!loaded) {
    present_native_error(self, "Could Not Add Notes",
                         error ? error->message : "A note could not be read");
    g_clear_error(&error);
    g_ptr_array_unref(notes);
    return;
  }

  g_ptr_array_unref(self->notes);
  self->notes = notes;
  refresh_order_list(self);
  send_preview_state(self, "export/documents");
  close_picker(NULL, self);
}

static void on_picker_closed(AdwDialog *dialog,
                             PdfvMarkdownExport *self) {
  self->picker_dialog = NULL;
  self->picker_done_button = NULL;
  self->picker_count_label = NULL;
  g_clear_object(&self->picker_tree);
  g_clear_pointer(&self->picker_selection, g_hash_table_unref);
  g_object_set_data(G_OBJECT(dialog), "phi-markdown-export-picker", NULL);
}

static void on_picker_row_activated(GtkListView *list, guint position,
                                    PdfvMarkdownExport *self) {
  (void)list;
  if (!self->picker_tree || position == GTK_INVALID_LIST_POSITION)
    return;
  GtkTreeListRow *row = gtk_tree_list_model_get_row(self->picker_tree,
                                                     position);
  if (!row)
    return;
  PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
  if (pdfv_workspace_item_is_folder(item))
    gtk_tree_list_row_set_expanded(row,
                                  !gtk_tree_list_row_get_expanded(row));
  g_object_unref(item);
  g_object_unref(row);
}

static void present_file_picker(GtkButton *button,
                                PdfvMarkdownExport *self) {
  (void)button;
  if (self->picker_dialog) {
    adw_dialog_present(self->picker_dialog, GTK_WIDGET(self->dialog));
    return;
  }
  self->picker_selection = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
  for (guint i = 0; i < self->notes->len; i++) {
    ExportNote *note = g_ptr_array_index(self->notes, i);
    g_hash_table_add(self->picker_selection, g_strdup(note->path));
  }

  self->picker_dialog = adw_dialog_new();
  adw_dialog_set_title(self->picker_dialog, "Add Notes");
  adw_dialog_set_content_width(self->picker_dialog, 620);
  adw_dialog_set_content_height(self->picker_dialog, 680);
  adw_dialog_set_presentation_mode(self->picker_dialog, ADW_DIALOG_FLOATING);
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_show_start_title_buttons(header, FALSE);
  adw_header_bar_set_show_end_title_buttons(header, FALSE);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  self->picker_done_button = GTK_BUTTON(gtk_button_new_with_label("Done"));
  gtk_widget_add_css_class(GTK_WIDGET(self->picker_done_button),
                           "suggested-action");
  adw_header_bar_pack_start(header, cancel);
  adw_header_bar_pack_end(header, GTK_WIDGET(self->picker_done_button));
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  GListModel *root = filtered_workspace_model(
      pdfv_workspace_get_items(self->workspace));
  self->picker_tree = gtk_tree_list_model_new(
      root, FALSE, FALSE, picker_create_children, self, NULL);
  GtkNoSelection *selection = gtk_no_selection_new(
      g_object_ref(G_LIST_MODEL(self->picker_tree)));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(picker_factory_setup), self);
  g_signal_connect(factory, "bind", G_CALLBACK(picker_factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(picker_factory_unbind), self);
  GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list), TRUE);
  gtk_widget_add_css_class(list, "navigation-sidebar");
  g_signal_connect(list, "activate", G_CALLBACK(on_picker_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_box_append(GTK_BOX(content), scroll);
  self->picker_count_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->picker_count_label, 0.0f);
  gtk_widget_add_css_class(GTK_WIDGET(self->picker_count_label), "dim-label");
  gtk_widget_set_margin_top(GTK_WIDGET(self->picker_count_label), 10);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->picker_count_label), 10);
  gtk_widget_set_margin_start(GTK_WIDGET(self->picker_count_label), 16);
  gtk_widget_set_margin_end(GTK_WIDGET(self->picker_count_label), 16);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->picker_count_label));
  adw_toolbar_view_set_content(toolbar, content);
  adw_dialog_set_child(self->picker_dialog, GTK_WIDGET(toolbar));
  g_signal_connect(cancel, "clicked", G_CALLBACK(close_picker), self);
  g_signal_connect(self->picker_done_button, "clicked",
                   G_CALLBACK(on_picker_done), self);
  g_signal_connect(self->picker_dialog, "closed",
                   G_CALLBACK(on_picker_closed), self);
  g_object_set_data_full(G_OBJECT(self->picker_dialog),
                         "phi-markdown-export-picker", g_object_ref(self),
                         g_object_unref);
  update_picker_count(self);
  expand_picker_selection(self);
  adw_dialog_present(self->picker_dialog, GTK_WIDGET(self->dialog));
}

static void on_bridge_message(PdfvMarkdownEditorBridge *bridge,
                              const gchar *type, const gchar *id,
                              JsonObject *payload,
                              PdfvMarkdownExport *self) {
  (void)bridge;
  if (g_str_equal(type, "export/ready")) {
    self->web_ready = TRUE;
    send_preview_state(self, "export/initialize");
  } else if (g_str_equal(type, "export/preview-ready")) {
    gint64 revision = payload
        ? json_object_get_int_member_with_default(payload, "revision", -1) : -1;
    if (revision == (gint64)self->preview_revision) {
      self->source_ready = TRUE;
      self->source_ready_revision = (guint64)revision;
      self->preview_ready = FALSE;
      update_export_enabled(self);
      request_exact_preview(self, (guint64)revision);
    }
  } else if (g_str_equal(type, "export/pdf-preview-hidden")) {
    gint64 revision = payload
        ? json_object_get_int_member_with_default(payload, "revision", -1) : -1;
    if (revision == (gint64)self->preview_revision && self->preview_spinner)
      gtk_widget_set_visible(self->preview_spinner, TRUE);
  } else if (g_str_equal(type, "export/pdf-preview-ready")) {
    gint64 revision = payload
        ? json_object_get_int_member_with_default(payload, "revision", -1) : -1;
    if (revision == (gint64)self->preview_revision &&
        revision == (gint64)self->cached_preview_revision &&
        self->temporary_filename) {
      self->preview_ready = TRUE;
      if (self->preview_spinner)
        gtk_widget_set_visible(self->preview_spinner, FALSE);
      if (!self->busy)
        set_status(self, NULL, FALSE);
      update_export_enabled(self);
    }
  } else if (g_str_equal(type, "embed/read")) {
    handle_embed_read(self, id, payload);
  } else if (g_str_equal(type, "attachment/resolve")) {
    handle_attachment_resolve(self, id, payload);
  } else if (g_str_equal(type, "log/error")) {
    const gchar *message = payload
        ? json_object_get_string_member_with_default(
              payload, "message", "PDF preview failed")
        : "PDF preview failed";
    g_warning("PDF export web process: %s", message);
    if (!self->busy)
      set_status(self, message, TRUE);
  }
}

static void on_bridge_error(PdfvMarkdownEditorBridge *bridge,
                            const gchar *message,
                            PdfvMarkdownExport *self) {
  (void)bridge;
  g_warning("PDF export bridge: %s", message);
  cancel_pending_export(self);
  if (self->preview_spinner)
    gtk_widget_set_visible(self->preview_spinner, FALSE);
  set_status(self, message, TRUE);
}

static gboolean on_decide_policy(WebKitWebView *web_view,
                                 WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type,
                                 PdfvMarkdownExport *self) {
  (void)web_view;
  (void)self;
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;
  WebKitNavigationAction *action =
      webkit_navigation_policy_decision_get_navigation_action(
          WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const gchar *uri = request ? webkit_uri_request_get_uri(request) : NULL;
  if (uri && g_str_has_prefix(uri, "app://editor/export-preview.html"))
    return FALSE;
  webkit_policy_decision_ignore(decision);
  return TRUE;
}

static gboolean on_permission_request(WebKitWebView *web_view,
                                      WebKitPermissionRequest *request,
                                      PdfvMarkdownExport *self) {
  (void)web_view;
  (void)self;
  webkit_permission_request_deny(request);
  return TRUE;
}

static gboolean on_context_menu(WebKitWebView *web_view,
                                WebKitContextMenu *menu,
                                WebKitHitTestResult *hit,
                                PdfvMarkdownExport *self) {
  (void)web_view;
  (void)menu;
  (void)hit;
  (void)self;
  return TRUE;
}

static void on_dialog_closed(AdwDialog *dialog,
                             PdfvMarkdownExport *self) {
  self->dialog = NULL;
  self->export_requested = FALSE;
  if (self->picker_dialog)
    adw_dialog_close(self->picker_dialog);
  g_object_set_data(G_OBJECT(dialog), "phi-markdown-export", NULL);
}

static GtkWidget *create_controls(PdfvMarkdownExport *self) {
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroll, 350, -1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  AdwPreferencesGroup *metadata = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(metadata, "Document");
  self->title_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->title_row),
                                self->multiple ? "Title (required)" : "Title");
  if (self->multiple) {
    self->title_required_icon = gtk_image_new_from_icon_name(
        "dialog-warning-symbolic");
    gtk_widget_add_css_class(self->title_required_icon, "error");
    gtk_widget_set_tooltip_text(self->title_required_icon,
                                "A title is required");
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(self->title_required_icon),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "A title is required", -1);
    adw_entry_row_add_suffix(self->title_row, self->title_required_icon);
  }
  self->author_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->author_row),
                                "Author");
  self->scale_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(
      MIN_EXPORT_SCALE, MAX_EXPORT_SCALE, 5));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->scale_row),
                                "Scale");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(self->scale_row), "Percent");
  adw_preferences_group_add(metadata, GTK_WIDGET(self->title_row));
  adw_preferences_group_add(metadata, GTK_WIDGET(self->author_row));
  adw_preferences_group_add(metadata, GTK_WIDGET(self->scale_row));
  if (self->multiple) {
    self->cover_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->cover_row),
                                  "Title page");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->cover_row),
                                "Add a separate title and author page");
    adw_switch_row_set_active(self->cover_row, TRUE);
    adw_preferences_group_add(metadata, GTK_WIDGET(self->cover_row));
  }
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(metadata));

  if (self->multiple) {
    GtkWidget *order_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *heading = gtk_label_new("Notes");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(order_box), heading);
    self->order_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->order_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->order_list), "boxed-list");
    gtk_box_append(GTK_BOX(order_box), GTK_WIDGET(self->order_list));
    GtkWidget *add = gtk_button_new_with_label("Add Files…");
    gtk_widget_add_css_class(add, "pill");
    gtk_widget_set_halign(add, GTK_ALIGN_START);
    g_signal_connect(add, "clicked", G_CALLBACK(present_file_picker), self);
    gtk_box_append(GTK_BOX(order_box), add);
    gtk_box_append(GTK_BOX(box), order_box);
  }

  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(box), spacer);
  self->export_button = GTK_BUTTON(gtk_button_new_with_label("Export PDF"));
  gtk_widget_add_css_class(GTK_WIDGET(self->export_button),
                           "suggested-action");
  gtk_widget_set_size_request(GTK_WIDGET(self->export_button), -1, 44);
  g_signal_connect(self->export_button, "clicked",
                   G_CALLBACK(on_export_clicked), self);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->export_button));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  return scroll;
}

static GtkWidget *create_busy_overlay(PdfvMarkdownExport *self) {
  GtkWidget *backdrop = gtk_center_box_new();
  gtk_widget_set_halign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_set_valign(backdrop, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(backdrop, TRUE);
  gtk_widget_set_vexpand(backdrop, TRUE);
  gtk_widget_add_css_class(backdrop, "view");

  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(card, 24);
  gtk_widget_set_margin_bottom(card, 24);
  gtk_widget_set_margin_start(card, 24);
  gtk_widget_set_margin_end(card, 24);
  gtk_widget_add_css_class(card, "card");
  GtkWidget *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 28, 28);
  gtk_widget_set_margin_top(spinner, 20);
  gtk_widget_set_margin_bottom(spinner, 20);
  gtk_widget_set_margin_start(spinner, 20);
  self->busy_label = GTK_LABEL(gtk_label_new("Generating PDF…"));
  gtk_widget_add_css_class(GTK_WIDGET(self->busy_label), "heading");
  gtk_widget_set_margin_end(GTK_WIDGET(self->busy_label), 20);
  gtk_box_append(GTK_BOX(card), spinner);
  gtk_box_append(GTK_BOX(card), GTK_WIDGET(self->busy_label));
  gtk_center_box_set_center_widget(GTK_CENTER_BOX(backdrop), card);
  gtk_widget_set_visible(backdrop, FALSE);
  self->busy_overlay = backdrop;
  return backdrop;
}

static void pdfv_markdown_export_dispose(GObject *object) {
  PdfvMarkdownExport *self = (PdfvMarkdownExport *)object;
  if (self->print_idle_id) {
    g_source_remove(self->print_idle_id);
    self->print_idle_id = 0;
  }
  if (self->print_operation)
    g_signal_handlers_disconnect_by_data(self->print_operation, self);
  g_clear_object(&self->print_operation);
  g_clear_object(&self->picker_tree);
  if (self->bridge)
    g_signal_handlers_disconnect_by_data(self->bridge, self);
  if (self->web_view)
    g_signal_handlers_disconnect_by_data(self->web_view, self);
  g_clear_object(&self->bridge);
  /* GtkPaned owns the floating GtkWidget reference. The bridge's explicit
   * WebKitWebView reference was released above; leave widget destruction to
   * the dialog hierarchy. */
  self->web_view = NULL;
  g_clear_object(&self->content_manager);
  g_clear_object(&self->resources);
  g_clear_object(&self->vault);
  g_clear_object(&self->workspace);
  G_OBJECT_CLASS(pdfv_markdown_export_parent_class)->dispose(object);
}

static void pdfv_markdown_export_finalize(GObject *object) {
  PdfvMarkdownExport *self = (PdfvMarkdownExport *)object;
  print_source_file_remove(self);
  temporary_file_remove(self);
  g_weak_ref_clear(&self->parent);
  g_clear_pointer(&self->notes, g_ptr_array_unref);
  g_clear_pointer(&self->picker_selection, g_hash_table_unref);
  g_free(self->current_path);
  g_free(self->preamble);
  g_free(self->print_error);
  g_free(self->suggested_filename);
  g_free(self->metadata_title);
  g_free(self->metadata_author);
  G_OBJECT_CLASS(pdfv_markdown_export_parent_class)->finalize(object);
}

static void pdfv_markdown_export_class_init(PdfvMarkdownExportClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = pdfv_markdown_export_dispose;
  object_class->finalize = pdfv_markdown_export_finalize;
}

static void pdfv_markdown_export_init(PdfvMarkdownExport *self) {
  g_weak_ref_init(&self->parent, NULL);
  self->notes = g_ptr_array_new_with_free_func(
      (GDestroyNotify)export_note_free);
}

void pdfv_markdown_export_present(
    GtkWidget *parent, PdfvMarkdownEditor *editor, PdfvWorkspace *workspace,
    gboolean multiple, gboolean allow_remote_images, gdouble font_size,
    PdfvMarkdownExportSavedFunc saved_callback, gpointer saved_data) {
  g_return_if_fail(GTK_IS_WIDGET(parent));
  g_return_if_fail(PDFV_IS_MARKDOWN_EDITOR(editor));
  g_return_if_fail(!multiple || PDFV_IS_WORKSPACE(workspace));
  GFile *file = pdfv_markdown_editor_get_file(editor);
  GFile *vault_root = pdfv_markdown_editor_get_vault_root(editor);
  const gchar *relative = pdfv_markdown_editor_get_relative_path(editor);
  if (!file || !vault_root || !relative)
    return;

  PdfvMarkdownExport *self = g_object_new(
      pdfv_markdown_export_get_type(), NULL);
  GtkRoot *root = gtk_widget_get_root(parent);
  GObject *parent_object = root && GTK_IS_WINDOW(root)
      ? G_OBJECT(root) : G_OBJECT(parent);
  g_weak_ref_set(&self->parent, parent_object);
  self->workspace = workspace ? g_object_ref(workspace) : NULL;
  self->current_path = g_strdup(relative);
  self->preamble = pdfv_markdown_editor_dup_preamble(editor);
  self->multiple = multiple;
  self->allow_remote_images = allow_remote_images;
  self->font_size = font_size > 0.0 ? font_size : 16.0;
  self->saved_callback = saved_callback;
  self->saved_data = saved_data;
  gchar *current_text = pdfv_markdown_editor_dup_text(editor);
  g_ptr_array_add(self->notes, export_note_new(relative, current_text));
  g_free(current_text);
  self->vault = pdfv_markdown_vault_adapter_new(vault_root);
  self->resources = pdfv_markdown_resource_scheme_new(self->vault);
  self->content_manager = webkit_user_content_manager_new();
  self->web_view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW,
      "web-context", pdfv_markdown_resource_scheme_get_context(self->resources),
      "user-content-manager", self->content_manager, NULL));
  const GdkRGBA transparent = {0, 0, 0, 0};
  webkit_web_view_set_background_color(self->web_view, &transparent);
  pdfv_markdown_resource_scheme_bind_web_view(self->resources,
                                               self->web_view);
  WebKitSettings *settings = webkit_web_view_get_settings(self->web_view);
  g_object_set(settings, "enable-html5-database", FALSE,
               "enable-html5-local-storage", FALSE, "enable-page-cache", FALSE,
               NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(self->web_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self->web_view), TRUE);
  self->bridge = pdfv_markdown_editor_bridge_new(
      self->web_view, self->content_manager);
  g_signal_connect(self->bridge, "message", G_CALLBACK(on_bridge_message),
                   self);
  g_signal_connect(self->bridge, "bridge-error",
                   G_CALLBACK(on_bridge_error), self);
  g_signal_connect(self->web_view, "decide-policy",
                   G_CALLBACK(on_decide_policy), self);
  g_signal_connect(self->web_view, "permission-request",
                   G_CALLBACK(on_permission_request), self);
  g_signal_connect(self->web_view, "context-menu",
                   G_CALLBACK(on_context_menu), self);

  self->dialog = adw_dialog_new();
  adw_dialog_set_title(self->dialog,
      multiple ? "Export multiple files to PDF" : "Export to PDF");
  adw_dialog_set_content_width(self->dialog, 1180);
  adw_dialog_set_content_height(self->dialog, 780);
  adw_dialog_set_presentation_mode(self->dialog, ADW_DIALOG_FLOATING);
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(paned, "pdf-export-paned");
  gtk_paned_set_position(GTK_PANED(paned), 370);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  GtkWidget *controls = create_controls(self);
  gtk_paned_set_start_child(GTK_PANED(paned), controls);
  GtkWidget *preview_overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(preview_overlay),
                        GTK_WIDGET(self->web_view));
  GtkWidget *preview_status = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_halign(preview_status, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(preview_status, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(preview_status, FALSE);
  self->preview_spinner = adw_spinner_new();
  gtk_widget_set_size_request(self->preview_spinner, 36, 36);
  gtk_widget_set_halign(self->preview_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(self->preview_spinner, FALSE);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(self->preview_spinner), GTK_ACCESSIBLE_PROPERTY_LABEL,
      "Loading PDF preview", -1);
  self->status_label = GTK_LABEL(gtk_label_new("Loading preview…"));
  gtk_label_set_justify(self->status_label, GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap(self->status_label, TRUE);
  gtk_label_set_max_width_chars(self->status_label, 42);
  gtk_widget_set_halign(GTK_WIDGET(self->status_label), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "dim-label");
  gtk_box_append(GTK_BOX(preview_status), self->preview_spinner);
  gtk_box_append(GTK_BOX(preview_status), GTK_WIDGET(self->status_label));
  gtk_overlay_add_overlay(GTK_OVERLAY(preview_overlay),
                          preview_status);
  gtk_paned_set_end_child(GTK_PANED(paned), preview_overlay);
  GtkWidget *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), paned);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_busy_overlay(self));
  adw_toolbar_view_set_content(toolbar, overlay);
  adw_dialog_set_child(self->dialog, GTK_WIDGET(toolbar));

  gchar *title = note_title(relative);
  gtk_editable_set_text(GTK_EDITABLE(self->title_row), multiple ? "" : title);
  g_free(title);
  adw_spin_row_set_value(self->scale_row, 100.0);
  g_signal_connect(self->title_row, "changed",
                   G_CALLBACK(on_metadata_changed), self);
  g_signal_connect(self->author_row, "changed",
                   G_CALLBACK(on_metadata_changed), self);
  g_signal_connect(self->scale_row, "notify::value",
                   G_CALLBACK(on_scale_changed), self);
  if (self->cover_row)
    g_signal_connect(self->cover_row, "notify::active",
                     G_CALLBACK(on_cover_changed), self);
  refresh_order_list(self);
  update_export_enabled(self);
  g_signal_connect(self->dialog, "closed", G_CALLBACK(on_dialog_closed), self);
  g_object_set_data_full(G_OBJECT(self->dialog), "phi-markdown-export",
                         g_object_ref(self), g_object_unref);
  webkit_web_view_load_uri(self->web_view,
                           "app://editor/export-preview.html");
  adw_dialog_present(self->dialog, parent);
  g_object_unref(self);
}

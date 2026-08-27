/* Bounded global document-position history for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "document-history.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>

#define HISTORY_LIMIT 4096
#define HISTORY_SAVE_DELAY_MS 750

typedef struct {
  gchar *uri;
  PdfvDocumentPosition position;
  gint64 updated_at;
  guint64 update_order;
} HistoryEntry;

struct _PdfvDocumentHistory {
  GObject parent_instance;
  GFile *storage_file;
  GHashTable *entries; /* URI -> HistoryEntry */
  GQueue order;        /* HistoryEntry pointers, oldest first */
  guint save_timeout_id;
  gboolean enabled;
  gboolean loaded;
  gboolean dirty;
  guint64 next_update_order;
};

G_DEFINE_FINAL_TYPE(PdfvDocumentHistory, pdfv_document_history,
                    G_TYPE_OBJECT)

static void history_entry_free(HistoryEntry *entry) {
  g_free(entry->uri);
  g_free(entry);
}

static gboolean position_is_valid(
    const PdfvDocumentPosition *position) {
  if (!position || position->anchor < 0 ||
      !isfinite(position->offset) || position->offset < 0 ||
      !isfinite(position->horizontal))
    return FALSE;
  if (position->kind == PDFV_DOCUMENT_POSITION_PDF)
    return position->anchor <= G_MAXINT;
  if (position->kind == PDFV_DOCUMENT_POSITION_MARKDOWN)
    return position->horizontal >= 0;
  return FALSE;
}

static gchar *default_history_filename(void) {
  return g_build_filename(g_get_user_state_dir(), "phi-pdf-viewer",
                          "document-positions", NULL);
}

static gint history_entry_compare(gconstpointer left, gconstpointer right) {
  const HistoryEntry *a = *(HistoryEntry *const *)left;
  const HistoryEntry *b = *(HistoryEntry *const *)right;
  if (a->updated_at < b->updated_at)
    return -1;
  if (a->updated_at > b->updated_at)
    return 1;
  return g_strcmp0(a->uri, b->uri);
}

static void prune_history(PdfvDocumentHistory *self) {
  while (g_queue_get_length(&self->order) > HISTORY_LIMIT) {
    HistoryEntry *oldest = g_queue_pop_head(&self->order);
    g_hash_table_remove(self->entries, oldest->uri);
  }
}

static void load_history(PdfvDocumentHistory *self) {
  if (self->loaded)
    return;
  self->loaded = TRUE;

  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_load_contents(self->storage_file, NULL, &contents, &length,
                            NULL, NULL))
    return;

  GPtrArray *loaded = g_ptr_array_new();
  gchar **lines = g_strsplit(contents, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (!*lines[i] || lines[i][0] == '#')
      continue;
    gchar **fields = g_strsplit(lines[i], "\t", 6);
    if (g_strv_length(fields) != 6 ||
        (g_strcmp0(fields[1], "p") != 0 &&
         g_strcmp0(fields[1], "m") != 0) ||
        !g_uri_peek_scheme(fields[5])) {
      g_strfreev(fields);
      continue;
    }

    gchar *end = NULL;
    gint64 updated_at = g_ascii_strtoll(fields[0], &end, 10);
    if (!end || *end) {
      g_strfreev(fields);
      continue;
    }
    gint64 anchor = g_ascii_strtoll(fields[2], &end, 10);
    if (!end || *end) {
      g_strfreev(fields);
      continue;
    }
    gdouble offset = g_ascii_strtod(fields[3], &end);
    if (!end || *end) {
      g_strfreev(fields);
      continue;
    }
    gdouble horizontal = g_ascii_strtod(fields[4], &end);
    if (!end || *end || !isfinite(offset) || !isfinite(horizontal)) {
      g_strfreev(fields);
      continue;
    }

    HistoryEntry *entry = g_new0(HistoryEntry, 1);
    entry->uri = g_strdup(fields[5]);
    entry->updated_at = updated_at;
    entry->position.kind = g_str_equal(fields[1], "p")
        ? PDFV_DOCUMENT_POSITION_PDF : PDFV_DOCUMENT_POSITION_MARKDOWN;
    entry->position.anchor = anchor;
    entry->position.offset = offset;
    entry->position.horizontal = horizontal;
    if (!position_is_valid(&entry->position)) {
      history_entry_free(entry);
      g_strfreev(fields);
      continue;
    }
    HistoryEntry *previous = g_hash_table_lookup(self->entries, entry->uri);
    if (!previous || previous->updated_at < entry->updated_at) {
      if (previous) {
        g_ptr_array_remove_fast(loaded, previous);
        g_hash_table_remove(self->entries, previous->uri);
      }
      g_hash_table_insert(self->entries, entry->uri, entry);
      g_ptr_array_add(loaded, entry);
    } else {
      history_entry_free(entry);
    }
    g_strfreev(fields);
  }
  g_strfreev(lines);
  g_free(contents);

  g_ptr_array_sort(loaded, history_entry_compare);
  for (guint i = 0; i < loaded->len; i++)
    g_queue_push_tail(&self->order, g_ptr_array_index(loaded, i));
  g_ptr_array_unref(loaded);
  prune_history(self);
}

static gboolean save_timeout(gpointer user_data) {
  PdfvDocumentHistory *self = PDFV_DOCUMENT_HISTORY(user_data);
  self->save_timeout_id = 0;
  GError *error = NULL;
  if (!pdfv_document_history_flush(self, &error)) {
    g_warning("Could not save document positions: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_save(PdfvDocumentHistory *self) {
  self->dirty = TRUE;
  if (self->save_timeout_id)
    return;
  self->save_timeout_id = g_timeout_add_full(
      G_PRIORITY_LOW, HISTORY_SAVE_DELAY_MS, save_timeout,
      g_object_ref(self), g_object_unref);
}

static void pdfv_document_history_dispose(GObject *object) {
  PdfvDocumentHistory *self = PDFV_DOCUMENT_HISTORY(object);
  if (self->save_timeout_id) {
    g_source_remove(self->save_timeout_id);
    self->save_timeout_id = 0;
  }
  if (self->enabled && self->dirty) {
    GError *error = NULL;
    if (!pdfv_document_history_flush(self, &error)) {
      g_warning("Could not save document positions: %s",
                error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  }
  g_clear_object(&self->storage_file);
  G_OBJECT_CLASS(pdfv_document_history_parent_class)->dispose(object);
}

static void pdfv_document_history_finalize(GObject *object) {
  PdfvDocumentHistory *self = PDFV_DOCUMENT_HISTORY(object);
  g_queue_clear(&self->order);
  g_clear_pointer(&self->entries, g_hash_table_unref);
  G_OBJECT_CLASS(pdfv_document_history_parent_class)->finalize(object);
}

static void pdfv_document_history_class_init(
    PdfvDocumentHistoryClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = pdfv_document_history_dispose;
  object_class->finalize = pdfv_document_history_finalize;
}

static void pdfv_document_history_init(PdfvDocumentHistory *self) {
  self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                        (GDestroyNotify)history_entry_free);
  g_queue_init(&self->order);
}

PdfvDocumentHistory *pdfv_document_history_new(GFile *storage_file) {
  g_return_val_if_fail(G_IS_FILE(storage_file), NULL);
  PdfvDocumentHistory *self = g_object_new(PDFV_TYPE_DOCUMENT_HISTORY, NULL);
  self->storage_file = g_object_ref(storage_file);
  return self;
}

PdfvDocumentHistory *pdfv_document_history_get_default(void) {
  static GWeakRef default_ref;
  static gsize initialized = 0;
  if (g_once_init_enter(&initialized)) {
    g_weak_ref_init(&default_ref, NULL);
    g_once_init_leave(&initialized, 1);
  }
  PdfvDocumentHistory *history = g_weak_ref_get(&default_ref);
  if (history)
    return history;
  gchar *filename = default_history_filename();
  GFile *file = g_file_new_for_path(filename);
  history = pdfv_document_history_new(file);
  g_weak_ref_set(&default_ref, history);
  g_object_unref(file);
  g_free(filename);
  return history;
}

void pdfv_document_history_set_enabled(PdfvDocumentHistory *self,
                                       gboolean enabled) {
  g_return_if_fail(PDFV_IS_DOCUMENT_HISTORY(self));
  enabled = !!enabled;
  if (!enabled) {
    self->enabled = FALSE;
    pdfv_document_history_clear(self);
    return;
  }
  if (self->enabled)
    return;
  self->enabled = enabled;
  load_history(self);
}

gboolean pdfv_document_history_get_enabled(PdfvDocumentHistory *self) {
  g_return_val_if_fail(PDFV_IS_DOCUMENT_HISTORY(self), FALSE);
  return self->enabled;
}

gboolean pdfv_document_history_lookup(PdfvDocumentHistory *self, GFile *file,
                                      PdfvDocumentPositionKind kind,
                                      PdfvDocumentPosition *position) {
  g_return_val_if_fail(PDFV_IS_DOCUMENT_HISTORY(self), FALSE);
  g_return_val_if_fail(G_IS_FILE(file), FALSE);
  if (!self->enabled)
    return FALSE;
  load_history(self);
  gchar *uri = g_file_get_uri(file);
  HistoryEntry *entry = g_hash_table_lookup(self->entries, uri);
  gboolean found = entry && entry->position.kind == kind;
  if (found && position)
    *position = entry->position;
  g_free(uri);
  return found;
}

void pdfv_document_history_remember(PdfvDocumentHistory *self, GFile *file,
                                    const PdfvDocumentPosition *position) {
  guint64 update_order = pdfv_document_history_reserve_update(self);
  pdfv_document_history_remember_ordered(self, file, position, update_order);
}

guint64 pdfv_document_history_reserve_update(
    PdfvDocumentHistory *self) {
  g_return_val_if_fail(PDFV_IS_DOCUMENT_HISTORY(self), 0);
  if (!self->enabled)
    return 0;
  return ++self->next_update_order;
}

void pdfv_document_history_remember_ordered(
    PdfvDocumentHistory *self, GFile *file,
    const PdfvDocumentPosition *position, guint64 update_order) {
  g_return_if_fail(PDFV_IS_DOCUMENT_HISTORY(self));
  g_return_if_fail(G_IS_FILE(file));
  g_return_if_fail(position != NULL);
  if (!self->enabled || update_order == 0 ||
      !position_is_valid(position))
    return;
  load_history(self);
  gchar *uri = g_file_get_uri(file);
  HistoryEntry *entry = g_hash_table_lookup(self->entries, uri);
  if (entry && entry->update_order > update_order) {
    g_free(uri);
    return;
  }
  if (!entry) {
    entry = g_new0(HistoryEntry, 1);
    entry->uri = g_strdup(uri);
    g_hash_table_insert(self->entries, entry->uri, entry);
  } else {
    g_queue_remove(&self->order, entry);
  }
  entry->position = *position;
  entry->updated_at = g_get_real_time();
  entry->update_order = update_order;
  g_queue_push_tail(&self->order, entry);
  prune_history(self);
  schedule_save(self);
  g_free(uri);
}

void pdfv_document_history_clear(PdfvDocumentHistory *self) {
  g_return_if_fail(PDFV_IS_DOCUMENT_HISTORY(self));
  if (self->save_timeout_id) {
    g_source_remove(self->save_timeout_id);
    self->save_timeout_id = 0;
  }
  g_queue_clear(&self->order);
  g_hash_table_remove_all(self->entries);
  self->loaded = TRUE;
  self->dirty = FALSE;
  gchar *path = g_file_get_path(self->storage_file);
  if (path && g_remove(path) != 0 && errno != ENOENT)
    g_warning("Could not remove document positions at %s: %s", path,
              g_strerror(errno));
  g_free(path);
}

gboolean pdfv_document_history_flush(PdfvDocumentHistory *self,
                                     GError **error) {
  g_return_val_if_fail(PDFV_IS_DOCUMENT_HISTORY(self), FALSE);
  if (!self->enabled || !self->dirty)
    return TRUE;
  if (self->save_timeout_id) {
    g_source_remove(self->save_timeout_id);
    self->save_timeout_id = 0;
  }

  GString *contents = g_string_new("# Phi document positions v1\n");
  for (GList *at = self->order.head; at; at = at->next) {
    HistoryEntry *entry = at->data;
    gchar offset[G_ASCII_DTOSTR_BUF_SIZE];
    gchar horizontal[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(offset, sizeof(offset), entry->position.offset);
    g_ascii_dtostr(horizontal, sizeof(horizontal),
                   entry->position.horizontal);
    g_string_append_printf(
        contents, "%" G_GINT64_FORMAT "\t%c\t%" G_GINT64_FORMAT
                  "\t%s\t%s\t%s\n",
        entry->updated_at,
        entry->position.kind == PDFV_DOCUMENT_POSITION_PDF ? 'p' : 'm',
        entry->position.anchor, offset, horizontal, entry->uri);
  }

  gchar *path = g_file_get_path(self->storage_file);
  gchar *directory = path ? g_path_get_dirname(path) : NULL;
  gboolean saved = path && directory &&
      g_mkdir_with_parents(directory, 0700) == 0 &&
      g_file_set_contents(path, contents->str, contents->len, error);
  if (!saved && path && directory && error && !*error)
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not create the document-position directory");
  if (saved) {
    g_chmod(path, 0600);
    self->dirty = FALSE;
  }
  g_free(directory);
  g_free(path);
  g_string_free(contents, TRUE);
  return saved;
}

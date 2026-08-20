/*
 * Phi PDF Viewer - workspace discovery and in-memory PDF indexing
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-workspace.h"

#include <phi/phipage.h>

#define INDEX_BATCH_PAGES 6

struct _PdfvWorkspaceItem {
  GObject parent_instance;
  GFile *file;
  gchar *name;
  gchar *relative_path;
  gboolean folder;
  GListStore *children;
};

G_DEFINE_FINAL_TYPE(PdfvWorkspaceItem, pdfv_workspace_item, G_TYPE_OBJECT)

static void pdfv_workspace_item_finalize(GObject *object) {
  PdfvWorkspaceItem *self = PDFV_WORKSPACE_ITEM(object);
  g_clear_object(&self->file);
  g_clear_object(&self->children);
  g_free(self->name);
  g_free(self->relative_path);
  G_OBJECT_CLASS(pdfv_workspace_item_parent_class)->finalize(object);
}

static void pdfv_workspace_item_class_init(PdfvWorkspaceItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_workspace_item_finalize;
}

static void pdfv_workspace_item_init(PdfvWorkspaceItem *self) {
  self->children = g_list_store_new(PDFV_TYPE_WORKSPACE_ITEM);
}

const gchar *pdfv_workspace_item_get_name(PdfvWorkspaceItem *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE_ITEM(self), NULL);
  return self->name;
}

const gchar *pdfv_workspace_item_get_relative_path(PdfvWorkspaceItem *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE_ITEM(self), NULL);
  return self->relative_path;
}

GFile *pdfv_workspace_item_get_file(PdfvWorkspaceItem *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE_ITEM(self), NULL);
  return self->file;
}

gboolean pdfv_workspace_item_is_folder(PdfvWorkspaceItem *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE_ITEM(self), FALSE);
  return self->folder;
}

GListModel *pdfv_workspace_item_get_children(PdfvWorkspaceItem *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE_ITEM(self), NULL);
  return G_LIST_MODEL(self->children);
}

typedef struct {
  GFile *file;
  gchar *name;
  gchar *relative_path;
  gboolean folder;
  GPtrArray *children;
} ScanItem;

typedef struct {
  GPtrArray *roots;
  GPtrArray *pdf_files;
  GPtrArray *pdf_paths;
} ScanResult;

typedef struct {
  gint ref_count;
  gchar *text;
  gchar *folded;
} IndexedPage;

typedef struct {
  GFile *file;
  gchar *relative_path;
  GPtrArray *pages; /* IndexedPage*; sparse while indexing */
  gboolean complete;
} IndexedDocument;

struct _PdfvWorkspace {
  GObject parent_instance;
  GFile *folder;
  gchar *name;
  GListStore *items;
  GHashTable *index; /* URI -> IndexedDocument */
  guint pdf_count;
  guint indexed_count;
  gint generation;
};

enum {
  SIGNAL_INDEX_UPDATED,
  N_SIGNALS,
};

static guint workspace_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(PdfvWorkspace, pdfv_workspace, G_TYPE_OBJECT)

static void scan_item_free(ScanItem *item) {
  if (!item)
    return;
  g_clear_object(&item->file);
  g_free(item->name);
  g_free(item->relative_path);
  g_clear_pointer(&item->children, g_ptr_array_unref);
  g_free(item);
}

static void scan_result_free(ScanResult *result) {
  if (!result)
    return;
  g_clear_pointer(&result->roots, g_ptr_array_unref);
  g_clear_pointer(&result->pdf_files, g_ptr_array_unref);
  g_clear_pointer(&result->pdf_paths, g_ptr_array_unref);
  g_free(result);
}

static IndexedPage *indexed_page_new(gchar *text) {
  IndexedPage *page = g_new0(IndexedPage, 1);
  page->ref_count = 1;
  page->text = text ? text : g_strdup("");
  page->folded = g_utf8_casefold(page->text, -1);
  return page;
}

static IndexedPage *indexed_page_ref(IndexedPage *page) {
  g_atomic_int_inc(&page->ref_count);
  return page;
}

static void indexed_page_unref(IndexedPage *page) {
  if (!page || !g_atomic_int_dec_and_test(&page->ref_count))
    return;
  g_free(page->text);
  g_free(page->folded);
  g_free(page);
}

static void indexed_document_free(IndexedDocument *document) {
  if (!document)
    return;
  g_clear_object(&document->file);
  g_free(document->relative_path);
  g_clear_pointer(&document->pages, g_ptr_array_unref);
  g_free(document);
}

static void pdfv_workspace_finalize(GObject *object) {
  PdfvWorkspace *self = PDFV_WORKSPACE(object);
  pdfv_workspace_cancel(self);
  g_clear_object(&self->folder);
  g_clear_object(&self->items);
  g_clear_pointer(&self->index, g_hash_table_unref);
  g_free(self->name);
  G_OBJECT_CLASS(pdfv_workspace_parent_class)->finalize(object);
}

static void pdfv_workspace_class_init(PdfvWorkspaceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = pdfv_workspace_finalize;

  workspace_signals[SIGNAL_INDEX_UPDATED] =
      g_signal_new("index-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pdfv_workspace_init(PdfvWorkspace *self) {
  self->items = g_list_store_new(PDFV_TYPE_WORKSPACE_ITEM);
  self->index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)indexed_document_free);
  self->generation = 1;
}

PdfvWorkspace *pdfv_workspace_new(GFile *folder) {
  g_return_val_if_fail(G_IS_FILE(folder), NULL);
  PdfvWorkspace *self = g_object_new(PDFV_TYPE_WORKSPACE, NULL);
  self->folder = g_object_ref(folder);
  self->name = g_file_get_basename(folder);
  return self;
}

GFile *pdfv_workspace_get_folder(PdfvWorkspace *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE(self), NULL);
  return self->folder;
}

const gchar *pdfv_workspace_get_name(PdfvWorkspace *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE(self), NULL);
  return self->name;
}

GListModel *pdfv_workspace_get_items(PdfvWorkspace *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE(self), NULL);
  return G_LIST_MODEL(self->items);
}

guint pdfv_workspace_get_pdf_count(PdfvWorkspace *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE(self), 0);
  return self->pdf_count;
}

guint pdfv_workspace_get_indexed_count(PdfvWorkspace *self) {
  g_return_val_if_fail(PDFV_IS_WORKSPACE(self), 0);
  return self->indexed_count;
}

void pdfv_workspace_cancel(PdfvWorkspace *self) {
  g_return_if_fail(PDFV_IS_WORKSPACE(self));
  g_atomic_int_inc(&self->generation);
}

static gboolean filename_is_pdf(const gchar *name) {
  const gchar *dot = strrchr(name, '.');
  return dot && g_ascii_strcasecmp(dot, ".pdf") == 0;
}

static gint scan_item_compare(gconstpointer a, gconstpointer b) {
  const ScanItem *left = *(ScanItem *const *)a;
  const ScanItem *right = *(ScanItem *const *)b;
  if (left->folder != right->folder)
    return left->folder ? -1 : 1;
  gchar *left_key = g_utf8_collate_key_for_filename(left->name, -1);
  gchar *right_key = g_utf8_collate_key_for_filename(right->name, -1);
  gint result = strcmp(left_key, right_key);
  g_free(left_key);
  g_free(right_key);
  return result;
}

static GPtrArray *scan_folder(GFile *folder, const gchar *parent_path,
                              ScanResult *result, GCancellable *cancellable,
                              guint *pdf_count, GError **error) {
  guint descendant_pdf_count = 0;
  GFileEnumerator *enumerator = g_file_enumerate_children(
      folder, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
  if (!enumerator)
    return NULL;

  GPtrArray *children =
      g_ptr_array_new_with_free_func((GDestroyNotify)scan_item_free);
  for (;;) {
    GFileInfo *info = g_file_enumerator_next_file(enumerator, cancellable, error);
    if (!info)
      break;

    const gchar *name = g_file_info_get_name(info);
    GFileType type = g_file_info_get_file_type(info);
    GFile *child_file = g_file_get_child(folder, name);
    gchar *relative = parent_path && *parent_path
                          ? g_build_filename(parent_path, name, NULL)
                          : g_strdup(name);

    if (type == G_FILE_TYPE_DIRECTORY) {
      GError *child_error = NULL;
      guint nested_pdf_count = 0;
      GPtrArray *nested = scan_folder(child_file, relative, result, cancellable,
                                      &nested_pdf_count, &child_error);
      if (nested && nested_pdf_count > 0) {
        ScanItem *item = g_new0(ScanItem, 1);
        item->file = g_object_ref(child_file);
        item->name = g_strdup(name);
        item->relative_path = g_strdup(relative);
        item->folder = TRUE;
        item->children = nested;
        g_ptr_array_add(children, item);
        descendant_pdf_count += nested_pdf_count;
      } else if (nested) {
        g_ptr_array_unref(nested);
      }
      if (child_error && !g_error_matches(child_error, G_IO_ERROR,
                                           G_IO_ERROR_PERMISSION_DENIED)) {
        g_propagate_error(error, child_error);
        g_free(relative);
        g_object_unref(child_file);
        g_object_unref(info);
        break;
      }
      g_clear_error(&child_error);
    } else if (type == G_FILE_TYPE_REGULAR && filename_is_pdf(name)) {
      ScanItem *item = g_new0(ScanItem, 1);
      item->file = g_object_ref(child_file);
      item->name = g_strdup(name);
      item->relative_path = g_strdup(relative);
      item->children =
          g_ptr_array_new_with_free_func((GDestroyNotify)scan_item_free);
      g_ptr_array_add(children, item);
      g_ptr_array_add(result->pdf_files, g_object_ref(child_file));
      g_ptr_array_add(result->pdf_paths, g_strdup(relative));
      descendant_pdf_count++;
    }

    g_free(relative);
    g_object_unref(child_file);
    g_object_unref(info);
    if (error && *error)
      break;
  }

  g_object_unref(enumerator);
  if (error && *error) {
    g_ptr_array_unref(children);
    return NULL;
  }
  if (pdf_count)
    *pdf_count = descendant_pdf_count;
  g_ptr_array_sort(children, scan_item_compare);
  return children;
}

static void scan_worker(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable) {
  (void)task_data;
  PdfvWorkspace *self = PDFV_WORKSPACE(source_object);
  ScanResult *result = g_new0(ScanResult, 1);
  result->pdf_files = g_ptr_array_new_with_free_func(g_object_unref);
  result->pdf_paths = g_ptr_array_new_with_free_func(g_free);
  GError *error = NULL;
  guint pdf_count = 0;
  result->roots =
      scan_folder(self->folder, "", result, cancellable, &pdf_count, &error);
  if (!result->roots) {
    scan_result_free(result);
    g_task_return_error(task, error);
    return;
  }
  g_task_return_pointer(task, result, (GDestroyNotify)scan_result_free);
}

void pdfv_workspace_load_async(PdfvWorkspace *self, GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data) {
  g_return_if_fail(PDFV_IS_WORKSPACE(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_workspace_load_async);
  g_task_run_in_thread(task, scan_worker);
  g_object_unref(task);
}

static PdfvWorkspaceItem *workspace_item_from_scan(ScanItem *scan) {
  PdfvWorkspaceItem *item = g_object_new(PDFV_TYPE_WORKSPACE_ITEM, NULL);
  item->file = g_object_ref(scan->file);
  item->name = g_strdup(scan->name);
  item->relative_path = g_strdup(scan->relative_path);
  item->folder = scan->folder;
  for (guint i = 0; i < scan->children->len; i++) {
    PdfvWorkspaceItem *child =
        workspace_item_from_scan(g_ptr_array_index(scan->children, i));
    g_list_store_append(item->children, child);
    g_object_unref(child);
  }
  return item;
}

typedef enum {
  PDF_JOB_LOAD,
  PDF_JOB_INDEX,
} PdfJobType;

typedef struct {
  PdfJobType type;
  gint priority;
  guint64 sequence;
  GTask *task;
  GFile *file;
  PhiDocument *document;
  gint next_page;
  gint n_pages;
  gint target_page;
  PdfvWorkspace *workspace;
  gint generation;
  gchar *relative_path;
} PdfJob;

typedef struct {
  PdfvWorkspace *workspace;
  gint generation;
  GFile *file;
  gchar *relative_path;
  gint start_page;
  gint n_pages;
  GPtrArray *pages; /* IndexedPage* */
  gboolean complete;
  GError *error;
} IndexBatch;

static void pdf_job_worker(gpointer user_data, gpointer pool_data);

static gint pdf_job_compare(gconstpointer a, gconstpointer b,
                            gpointer user_data) {
  (void)user_data;
  const PdfJob *left = a;
  const PdfJob *right = b;
  if (left->priority != right->priority)
    return left->priority - right->priority;
  return left->sequence < right->sequence ? -1 : left->sequence > right->sequence;
}

static gpointer pdf_pool_init(gpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GThreadPool *pool = g_thread_pool_new(pdf_job_worker, NULL, 1, TRUE, &error);
  if (!pool) {
    g_warning("Could not create PDF worker: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }
  g_thread_pool_set_sort_function(pool, pdf_job_compare, NULL);
  return pool;
}

static GThreadPool *pdf_get_pool(void) {
  static GOnce once = G_ONCE_INIT;
  return g_once(&once, pdf_pool_init, NULL);
}

static guint64 next_job_sequence(void) {
  static gint sequence = 0;
  return (guint64)g_atomic_int_add(&sequence, 1);
}

static void pdf_job_free(PdfJob *job) {
  if (!job)
    return;
  g_clear_object(&job->task);
  g_clear_object(&job->file);
  g_clear_object(&job->document);
  g_clear_object(&job->workspace);
  g_free(job->relative_path);
  g_free(job);
}

static gboolean queue_pdf_job(PdfJob *job) {
  GThreadPool *pool = pdf_get_pool();
  if (!pool)
    return FALSE;
  job->sequence = next_job_sequence();
  GError *error = NULL;
  if (!g_thread_pool_push(pool, job, &error)) {
    g_warning("Could not queue PDF work: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    return FALSE;
  }
  return TRUE;
}

static gboolean index_batch_complete(gpointer user_data) {
  IndexBatch *batch = user_data;
  PdfvWorkspace *self = batch->workspace;
  if (g_atomic_int_get(&self->generation) == batch->generation) {
    gchar *uri = g_file_get_uri(batch->file);
    IndexedDocument *document = g_hash_table_lookup(self->index, uri);
    if (!document) {
      document = g_new0(IndexedDocument, 1);
      document->file = g_object_ref(batch->file);
      document->relative_path = g_strdup(batch->relative_path);
      document->pages = g_ptr_array_new_with_free_func(
          (GDestroyNotify)indexed_page_unref);
      g_ptr_array_set_size(document->pages, batch->n_pages);
      g_hash_table_insert(self->index, g_strdup(uri), document);
    }
    for (guint i = 0; i < batch->pages->len; i++) {
      guint page_number = batch->start_page + i;
      if (page_number < document->pages->len) {
        IndexedPage *old = g_ptr_array_index(document->pages, page_number);
        if (old)
          indexed_page_unref(old);
        g_ptr_array_index(document->pages, page_number) =
            indexed_page_ref(g_ptr_array_index(batch->pages, i));
      }
    }
    if (batch->complete && !document->complete) {
      document->complete = TRUE;
      self->indexed_count++;
    }
    g_free(uri);
    g_signal_emit(self, workspace_signals[SIGNAL_INDEX_UPDATED], 0);
  }

  if (batch->error)
    g_debug("Could not index %s: %s", batch->relative_path,
            batch->error->message);
  g_clear_error(&batch->error);
  g_ptr_array_unref(batch->pages);
  g_object_unref(batch->file);
  g_object_unref(batch->workspace);
  g_free(batch->relative_path);
  g_free(batch);
  return G_SOURCE_REMOVE;
}

static void return_load_error(PdfJob *job, GError *error) {
  if (!error)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Could not load PDF");
  g_task_return_error(job->task, error);
  pdf_job_free(job);
}

static void pdf_job_worker(gpointer user_data, gpointer pool_data) {
  (void)pool_data;
  PdfJob *job = user_data;

  if (job->type == PDF_JOB_LOAD) {
    GCancellable *cancellable = g_task_get_cancellable(job->task);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      return_load_error(job,
                        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                            "Document load was cancelled"));
      return;
    }

    GError *error = NULL;
    if (!job->document) {
      job->document = phi_document_new_from_file(job->file, &error);
      if (!job->document) {
        return_load_error(job, error);
        return;
      }
      job->n_pages = phi_document_get_n_pages(job->document);
    }

    gint pages_to_prepare[2] = {0, CLAMP(job->target_page, 0,
                                        MAX(0, job->n_pages - 1))};
    for (guint i = 0; i < G_N_ELEMENTS(pages_to_prepare); i++) {
      if (job->n_pages == 0 || (i == 1 && pages_to_prepare[1] == 0))
        continue;
      if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        return_load_error(job,
                          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                              "Document load was cancelled"));
        return;
      }
      PhiPage *page = phi_document_get_page(job->document,
                                            pages_to_prepare[i], &error);
      if (!page) {
        return_load_error(job, error);
        return;
      }
      /* Cache bounds on the worker too; setting the document in the GTK view
       * then performs only cheap array math, even for very long PDFs. */
      phi_page_get_size(page, NULL, NULL);
    }

    PhiDocument *document = g_steal_pointer(&job->document);
    g_task_return_pointer(job->task, document, g_object_unref);
    pdf_job_free(job);
    return;
  }

  if (g_atomic_int_get(&job->workspace->generation) != job->generation) {
    pdf_job_free(job);
    return;
  }

  GError *error = NULL;
  if (!job->document) {
    job->document = phi_document_new_from_file(job->file, &error);
    if (!job->document) {
      IndexBatch *batch = g_new0(IndexBatch, 1);
      batch->workspace = g_object_ref(job->workspace);
      batch->generation = job->generation;
      batch->file = g_object_ref(job->file);
      batch->relative_path = g_strdup(job->relative_path);
      batch->pages = g_ptr_array_new_with_free_func(
          (GDestroyNotify)indexed_page_unref);
      batch->complete = TRUE;
      batch->error = error;
      g_main_context_invoke(NULL, index_batch_complete, batch);
      pdf_job_free(job);
      return;
    }
    job->n_pages = phi_document_get_n_pages(job->document);
  }

  IndexBatch *batch = g_new0(IndexBatch, 1);
  batch->workspace = g_object_ref(job->workspace);
  batch->generation = job->generation;
  batch->file = g_object_ref(job->file);
  batch->relative_path = g_strdup(job->relative_path);
  batch->start_page = job->next_page;
  batch->n_pages = job->n_pages;
  batch->pages =
      g_ptr_array_new_with_free_func((GDestroyNotify)indexed_page_unref);

  gint end = MIN(job->next_page + INDEX_BATCH_PAGES, job->n_pages);
  for (; job->next_page < end; job->next_page++) {
    PhiPage *page = phi_document_get_page(job->document, job->next_page, &error);
    if (!page)
      break;
    g_ptr_array_add(batch->pages, indexed_page_new(phi_page_get_text(page)));
  }
  batch->complete = error != NULL || job->next_page >= job->n_pages;
  batch->error = error;
  gboolean job_complete = batch->complete;
  g_main_context_invoke(NULL, index_batch_complete, batch);

  if (error || job_complete ||
      g_atomic_int_get(&job->workspace->generation) != job->generation) {
    pdf_job_free(job);
  } else if (!queue_pdf_job(job)) {
    pdf_job_free(job);
  }
}

static void queue_index_document(PdfvWorkspace *self, GFile *file,
                                 const gchar *relative_path) {
  PdfJob *job = g_new0(PdfJob, 1);
  job->type = PDF_JOB_INDEX;
  job->priority = 10;
  job->workspace = g_object_ref(self);
  job->generation = g_atomic_int_get(&self->generation);
  job->file = g_object_ref(file);
  job->relative_path = g_strdup(relative_path);
  if (!queue_pdf_job(job))
    pdf_job_free(job);
}

gboolean pdfv_workspace_load_finish(PdfvWorkspace *self, GAsyncResult *result,
                                    GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  ScanResult *scan = g_task_propagate_pointer(G_TASK(result), error);
  if (!scan)
    return FALSE;

  g_list_store_remove_all(self->items);
  g_hash_table_remove_all(self->index);
  self->indexed_count = 0;
  self->pdf_count = scan->pdf_files->len;
  for (guint i = 0; i < scan->roots->len; i++) {
    PdfvWorkspaceItem *item =
        workspace_item_from_scan(g_ptr_array_index(scan->roots, i));
    g_list_store_append(self->items, item);
    g_object_unref(item);
  }
  for (guint i = 0; i < scan->pdf_files->len; i++)
    queue_index_document(self, g_ptr_array_index(scan->pdf_files, i),
                         g_ptr_array_index(scan->pdf_paths, i));

  scan_result_free(scan);
  return TRUE;
}

typedef struct {
  GFile *file;
  gchar *relative_path;
  GPtrArray *pages; /* IndexedPage* */
  guint proximity;
} SearchDocument;

typedef struct {
  gchar *query;
  GPtrArray *documents;
} SearchTaskData;

static void search_document_free(SearchDocument *document) {
  g_clear_object(&document->file);
  g_free(document->relative_path);
  g_clear_pointer(&document->pages, g_ptr_array_unref);
  g_free(document);
}

static void search_task_data_free(SearchTaskData *data) {
  g_free(data->query);
  g_ptr_array_unref(data->documents);
  g_free(data);
}

static gint search_document_compare(gconstpointer a, gconstpointer b) {
  const SearchDocument *left = *(SearchDocument *const *)a;
  const SearchDocument *right = *(SearchDocument *const *)b;
  if (left->proximity < right->proximity)
    return -1;
  if (left->proximity > right->proximity)
    return 1;
  return g_utf8_collate(left->relative_path, right->relative_path);
}

static guint directory_component_count(gchar **components) {
  guint count = 0;
  while (components[count])
    count++;
  return count;
}

static guint search_path_proximity(const gchar *near_path,
                                   const gchar *candidate_path) {
  if (!near_path)
    return 0;
  if (g_str_equal(near_path, candidate_path))
    return 0;

  gchar *near_directory = g_path_get_dirname(near_path);
  gchar *candidate_directory = g_path_get_dirname(candidate_path);
  gchar **near_components = g_str_equal(near_directory, ".")
                                ? g_new0(gchar *, 1)
                                : g_strsplit(near_directory,
                                             G_DIR_SEPARATOR_S, -1);
  gchar **candidate_components =
      g_str_equal(candidate_directory, ".")
          ? g_new0(gchar *, 1)
          : g_strsplit(candidate_directory, G_DIR_SEPARATOR_S, -1);
  guint near_count = directory_component_count(near_components);
  guint candidate_count = directory_component_count(candidate_components);
  guint common = 0;
  while (common < near_count && common < candidate_count &&
         g_str_equal(near_components[common], candidate_components[common]))
    common++;

  /* Keep the current file ahead of other files in the same directory, then
   * rank by the number of directory edges between the two PDFs. */
  guint proximity = 1 + near_count - common + candidate_count - common;
  g_strfreev(near_components);
  g_strfreev(candidate_components);
  g_free(near_directory);
  g_free(candidate_directory);
  return proximity;
}

static void workspace_match_free(PdfvWorkspaceMatch *match) {
  g_free(match->snippet);
  g_free(match);
}

void pdfv_workspace_result_group_free(PdfvWorkspaceResultGroup *group) {
  if (!group)
    return;
  g_clear_object(&group->file);
  g_free(group->relative_path);
  g_clear_pointer(&group->matches, g_ptr_array_unref);
  g_free(group);
}

static gchar *make_snippet(const gchar *text, const gchar *folded,
                           const gchar *found) {
  glong character = found ? g_utf8_strlen(folded, found - folded) : 0;
  glong length = g_utf8_strlen(text, -1);
  glong start = MAX(0, character - 45);
  glong end = MIN(length, character + 85);
  const gchar *start_ptr = g_utf8_offset_to_pointer(text, start);
  const gchar *end_ptr = g_utf8_offset_to_pointer(text, end);
  gchar *slice = g_strndup(start_ptr, end_ptr - start_ptr);
  GString *clean = g_string_sized_new(strlen(slice) + 8);
  gboolean spacing = FALSE;
  for (const gchar *p = slice; *p;) {
    gunichar ch = g_utf8_get_char(p);
    if (g_unichar_isspace(ch)) {
      spacing = clean->len > 0;
    } else {
      if (spacing)
        g_string_append_c(clean, ' ');
      gchar utf8[7] = {0};
      gint bytes = g_unichar_to_utf8(ch, utf8);
      g_string_append_len(clean, utf8, bytes);
      spacing = FALSE;
    }
    p = g_utf8_next_char(p);
  }
  g_free(slice);
  if (start > 0)
    g_string_prepend(clean, "…");
  if (end < length)
    g_string_append(clean, "…");
  return g_string_free(clean, FALSE);
}

static void search_worker(GTask *task, gpointer source_object,
                          gpointer task_data, GCancellable *cancellable) {
  (void)source_object;
  SearchTaskData *data = task_data;
  gchar *needle = g_utf8_casefold(data->query, -1);
  GPtrArray *groups = g_ptr_array_new_with_free_func(
      (GDestroyNotify)pdfv_workspace_result_group_free);

  for (guint d = 0; d < data->documents->len; d++) {
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
      g_free(needle);
      g_ptr_array_unref(groups);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "Workspace search was cancelled");
      return;
    }
    SearchDocument *document = g_ptr_array_index(data->documents, d);
    PdfvWorkspaceResultGroup *group = NULL;
    for (guint p = 0; p < document->pages->len; p++) {
      IndexedPage *page = g_ptr_array_index(document->pages, p);
      if (!page)
        continue;
      const gchar *found = strstr(page->folded, needle);
      if (!found)
        continue;
      if (!group) {
        group = g_new0(PdfvWorkspaceResultGroup, 1);
        group->file = g_object_ref(document->file);
        group->relative_path = g_strdup(document->relative_path);
        group->matches = g_ptr_array_new_with_free_func(
            (GDestroyNotify)workspace_match_free);
      }
      /* One concise result per matching page keeps the result model useful
       * without allowing repeated words on one page to drown out a PDF. */
      PdfvWorkspaceMatch *match = g_new0(PdfvWorkspaceMatch, 1);
      match->page = p;
      match->snippet = make_snippet(page->text, page->folded, found);
      g_ptr_array_add(group->matches, match);
    }
    if (group)
      g_ptr_array_add(groups, group);
  }

  g_free(needle);
  g_task_return_pointer(task, groups, (GDestroyNotify)g_ptr_array_unref);
}

void pdfv_workspace_search_async(PdfvWorkspace *self, const gchar *query,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  pdfv_workspace_search_near_async(self, query, NULL, cancellable, callback,
                                   user_data);
}

void pdfv_workspace_search_near_async(PdfvWorkspace *self,
                                      const gchar *query, GFile *near_file,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  g_return_if_fail(PDFV_IS_WORKSPACE(self));
  g_return_if_fail(query != NULL);

  SearchTaskData *data = g_new0(SearchTaskData, 1);
  data->query = g_utf8_normalize(query, -1, G_NORMALIZE_DEFAULT_COMPOSE);
  data->documents =
      g_ptr_array_new_with_free_func((GDestroyNotify)search_document_free);
  gchar *near_path = near_file
                         ? g_file_get_relative_path(self->folder, near_file)
                         : NULL;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->index);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    IndexedDocument *indexed = value;
    SearchDocument *document = g_new0(SearchDocument, 1);
    document->file = g_object_ref(indexed->file);
    document->relative_path = g_strdup(indexed->relative_path);
    document->proximity =
        search_path_proximity(near_path, document->relative_path);
    document->pages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)indexed_page_unref);
    g_ptr_array_set_size(document->pages, indexed->pages->len);
    for (guint i = 0; i < indexed->pages->len; i++) {
      IndexedPage *page = g_ptr_array_index(indexed->pages, i);
      if (page)
        g_ptr_array_index(document->pages, i) = indexed_page_ref(page);
    }
    g_ptr_array_add(data->documents, document);
  }
  g_free(near_path);
  g_ptr_array_sort(data->documents, search_document_compare);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, data, (GDestroyNotify)search_task_data_free);
  g_task_set_source_tag(task, pdfv_workspace_search_near_async);
  g_task_run_in_thread(task, search_worker);
  g_object_unref(task);
}

GPtrArray *pdfv_workspace_search_finish(PdfvWorkspace *self,
                                        GAsyncResult *result,
                                        GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void pdfv_workspace_load_document_async(GFile *file, gint target_page,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(G_IS_FILE(file));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, pdfv_workspace_load_document_async);
  PdfJob *job = g_new0(PdfJob, 1);
  job->type = PDF_JOB_LOAD;
  job->priority = 0;
  job->task = task;
  job->file = g_object_ref(file);
  job->target_page = target_page;
  if (!queue_pdf_job(job))
    return_load_error(job, NULL);
}

PhiDocument *pdfv_workspace_load_document_finish(GAsyncResult *result,
                                                  GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

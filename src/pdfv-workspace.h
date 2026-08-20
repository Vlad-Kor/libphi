/*
 * Phi PDF Viewer - workspace discovery and cached PDF indexing
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef PDFV_WORKSPACE_H
#define PDFV_WORKSPACE_H

#include <gio/gio.h>
#include <phi/phidocument.h>

G_BEGIN_DECLS

#define PDFV_TYPE_WORKSPACE_ITEM (pdfv_workspace_item_get_type())
G_DECLARE_FINAL_TYPE(PdfvWorkspaceItem, pdfv_workspace_item, PDFV,
                     WORKSPACE_ITEM, GObject)

const gchar *pdfv_workspace_item_get_name(PdfvWorkspaceItem *self);
const gchar *pdfv_workspace_item_get_relative_path(PdfvWorkspaceItem *self);
GFile *pdfv_workspace_item_get_file(PdfvWorkspaceItem *self);
gboolean pdfv_workspace_item_is_folder(PdfvWorkspaceItem *self);
GListModel *pdfv_workspace_item_get_children(PdfvWorkspaceItem *self);

#define PDFV_TYPE_WORKSPACE (pdfv_workspace_get_type())
G_DECLARE_FINAL_TYPE(PdfvWorkspace, pdfv_workspace, PDFV, WORKSPACE, GObject)

PdfvWorkspace *pdfv_workspace_new(GFile *folder);
GFile *pdfv_workspace_get_folder(PdfvWorkspace *self);
const gchar *pdfv_workspace_get_name(PdfvWorkspace *self);
GListModel *pdfv_workspace_get_items(PdfvWorkspace *self);

void pdfv_workspace_load_async(PdfvWorkspace *self, GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
gboolean pdfv_workspace_load_finish(PdfvWorkspace *self, GAsyncResult *result,
                                    GError **error);
void pdfv_workspace_cancel(PdfvWorkspace *self);

guint pdfv_workspace_get_pdf_count(PdfvWorkspace *self);
guint pdfv_workspace_get_indexed_count(PdfvWorkspace *self);
guint pdfv_workspace_get_cache_hit_count(PdfvWorkspace *self);

typedef struct {
  gint page;
  gchar *snippet;
} PdfvWorkspaceMatch;

typedef struct {
  GFile *file;
  gchar *relative_path;
  GPtrArray *matches; /* PdfvWorkspaceMatch* */
} PdfvWorkspaceResultGroup;

void pdfv_workspace_result_group_free(PdfvWorkspaceResultGroup *group);

void pdfv_workspace_search_async(PdfvWorkspace *self, const gchar *query,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
void pdfv_workspace_search_near_async(PdfvWorkspace *self,
                                      const gchar *query, GFile *near_file,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);
GPtrArray *pdfv_workspace_search_finish(PdfvWorkspace *self,
                                        GAsyncResult *result,
                                        GError **error);

/* Document creation, layout preparation, and indexing all use the same
 * exclusive worker. Preview loads have priority over background indexing. */
void pdfv_workspace_load_document_async(GFile *file, gint target_page,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);
PhiDocument *pdfv_workspace_load_document_finish(GAsyncResult *result,
                                                  GError **error);

G_END_DECLS

#endif

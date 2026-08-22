/*
 * Phi workspace filesystem operations
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_WORKSPACE_FILE_OPS_H
#define PDFV_WORKSPACE_FILE_OPS_H

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean pdfv_workspace_file_is_within(GFile *root, GFile *file);

GFile *pdfv_workspace_creation_parent(GFile *root, GFile *selected,
                                      gboolean selected_is_folder,
                                      GFile *active_file);

GFile *pdfv_workspace_create_note(GFile *root, GFile *parent,
                                  const gchar *name, GError **error);
GFile *pdfv_workspace_create_folder(GFile *root, GFile *parent,
                                    const gchar *name, GError **error);

GFile *pdfv_workspace_move_item(GFile *root, GFile *source,
                                GFile *destination_folder, GError **error);

G_END_DECLS

#endif

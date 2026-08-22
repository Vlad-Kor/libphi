/*
 * Phi workspace filesystem operations
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "workspace-file-ops.h"

#include <string.h>

gboolean pdfv_workspace_file_is_within(GFile *root, GFile *file) {
  g_return_val_if_fail(G_IS_FILE(root), FALSE);
  g_return_val_if_fail(G_IS_FILE(file), FALSE);
  if (g_file_equal(root, file))
    return TRUE;
  gchar *relative = g_file_get_relative_path(root, file);
  gboolean within = relative && *relative;
  g_free(relative);
  return within;
}

static gboolean file_is_directory(GFile *file) {
  return file && g_file_query_file_type(
      file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) ==
      G_FILE_TYPE_DIRECTORY;
}

GFile *pdfv_workspace_creation_parent(GFile *root, GFile *selected,
                                      gboolean selected_is_folder,
                                      GFile *active_file) {
  g_return_val_if_fail(G_IS_FILE(root), NULL);
  GFile *parent = NULL;
  if (selected && pdfv_workspace_file_is_within(root, selected)) {
    parent = selected_is_folder ? g_object_ref(selected)
                                : g_file_get_parent(selected);
  } else if (active_file &&
             pdfv_workspace_file_is_within(root, active_file)) {
    parent = g_file_get_parent(active_file);
  }
  if (!parent || !pdfv_workspace_file_is_within(root, parent) ||
      !file_is_directory(parent)) {
    g_clear_object(&parent);
    parent = g_object_ref(root);
  }
  return parent;
}

static gchar *validated_name(const gchar *name, gboolean markdown,
                             GError **error) {
  gchar *clean = g_strdup(name ? name : "");
  g_strstrip(clean);
  if (!*clean || g_str_equal(clean, ".") || g_str_equal(clean, "..") ||
      strchr(clean, G_DIR_SEPARATOR) || !g_utf8_validate(clean, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Use a valid name without path separators");
    g_free(clean);
    return NULL;
  }
  for (const gchar *at = clean; *at; at = g_utf8_next_char(at)) {
    if (g_unichar_iscntrl(g_utf8_get_char(at))) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "Names cannot contain control characters");
      g_free(clean);
      return NULL;
    }
  }
  if (markdown) {
    gsize length = strlen(clean);
    gboolean has_extension = length >= 3 &&
        g_ascii_strcasecmp(clean + length - 3, ".md") == 0;
    if (!has_extension) {
      gchar *with_extension = g_strconcat(clean, ".md", NULL);
      g_free(clean);
      clean = with_extension;
    }
  }
  return clean;
}

static gboolean validate_parent(GFile *root, GFile *parent, GError **error) {
  if (!pdfv_workspace_file_is_within(root, parent)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "The destination must be inside the workspace");
    return FALSE;
  }
  if (!file_is_directory(parent)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                        "The destination folder does not exist");
    return FALSE;
  }
  return TRUE;
}

GFile *pdfv_workspace_create_note(GFile *root, GFile *parent,
                                  const gchar *name, GError **error) {
  g_return_val_if_fail(G_IS_FILE(root), NULL);
  g_return_val_if_fail(G_IS_FILE(parent), NULL);
  if (!validate_parent(root, parent, error))
    return NULL;
  gchar *clean = validated_name(name, TRUE, error);
  if (!clean)
    return NULL;
  GFile *file = g_file_get_child(parent, clean);
  g_free(clean);
  GFileOutputStream *stream = g_file_create(
      file, G_FILE_CREATE_NONE, NULL, error);
  if (!stream) {
    g_object_unref(file);
    return NULL;
  }
  if (!g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, error)) {
    g_file_delete(file, NULL, NULL);
    g_object_unref(stream);
    g_object_unref(file);
    return NULL;
  }
  g_object_unref(stream);
  return file;
}

GFile *pdfv_workspace_create_folder(GFile *root, GFile *parent,
                                    const gchar *name, GError **error) {
  g_return_val_if_fail(G_IS_FILE(root), NULL);
  g_return_val_if_fail(G_IS_FILE(parent), NULL);
  if (!validate_parent(root, parent, error))
    return NULL;
  gchar *clean = validated_name(name, FALSE, error);
  if (!clean)
    return NULL;
  GFile *folder = g_file_get_child(parent, clean);
  g_free(clean);
  if (!g_file_make_directory(folder, NULL, error)) {
    g_object_unref(folder);
    return NULL;
  }
  return folder;
}

GFile *pdfv_workspace_move_item(GFile *root, GFile *source,
                                GFile *destination_folder, GError **error) {
  g_return_val_if_fail(G_IS_FILE(root), NULL);
  g_return_val_if_fail(G_IS_FILE(source), NULL);
  g_return_val_if_fail(G_IS_FILE(destination_folder), NULL);
  if (!pdfv_workspace_file_is_within(root, source) ||
      g_file_equal(root, source)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Only items inside the workspace can be moved");
    return NULL;
  }
  if (!validate_parent(root, destination_folder, error))
    return NULL;
  GFileType source_type = g_file_query_file_type(
      source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
  if (source_type != G_FILE_TYPE_REGULAR &&
      source_type != G_FILE_TYPE_DIRECTORY) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "The item no longer exists");
    return NULL;
  }
  if (source_type == G_FILE_TYPE_DIRECTORY &&
      pdfv_workspace_file_is_within(source, destination_folder)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                        "A folder cannot be moved into itself");
    return NULL;
  }
  GFile *source_parent = g_file_get_parent(source);
  if (source_parent && g_file_equal(source_parent, destination_folder)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "The item is already in that folder");
    g_object_unref(source_parent);
    return NULL;
  }
  g_clear_object(&source_parent);
  gchar *basename = g_file_get_basename(source);
  GFile *destination = g_file_get_child(destination_folder, basename);
  g_free(basename);
  if (g_file_query_exists(destination, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "An item with that name already exists there");
    g_object_unref(destination);
    return NULL;
  }
  if (!g_file_move(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                   NULL, NULL, NULL, error)) {
    g_object_unref(destination);
    return NULL;
  }
  return destination;
}

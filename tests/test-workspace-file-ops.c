/*
 * Phi workspace filesystem operation tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "workspace-file-ops.h"

#include <glib/gstdio.h>

static void delete_file(GFile *file) {
  GError *error = NULL;
  g_assert_true(g_file_delete(file, NULL, &error));
  g_assert_no_error(error);
}

static void test_workspace_file_operations(void) {
  GError *error = NULL;
  gchar *path = g_dir_make_tmp("phi-file-ops-XXXXXX", &error);
  g_assert_no_error(error);
  GFile *root = g_file_new_for_path(path);
  GFile *outside = g_file_new_for_path("/tmp");

  GFile *folder = pdfv_workspace_create_folder(
      root, root, "Vorlesung", &error);
  g_assert_no_error(error);
  g_assert_nonnull(folder);
  GFile *note = pdfv_workspace_create_note(
      root, folder, "Grüße", &error);
  g_assert_no_error(error);
  g_assert_nonnull(note);
  gchar *basename = g_file_get_basename(note);
  g_assert_cmpstr(basename, ==, "Grüße.md");
  g_free(basename);

  GFile *duplicate = pdfv_workspace_create_note(
      root, folder, "Grüße.md", &error);
  g_assert_null(duplicate);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error(&error);

  GFile *invalid = pdfv_workspace_create_folder(
      root, root, "../outside", &error);
  g_assert_null(invalid);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  GFile *chosen = pdfv_workspace_creation_parent(
      root, note, FALSE, NULL);
  g_assert_true(g_file_equal(chosen, folder));
  g_object_unref(chosen);
  chosen = pdfv_workspace_creation_parent(root, NULL, FALSE, note);
  g_assert_true(g_file_equal(chosen, folder));
  g_object_unref(chosen);
  chosen = pdfv_workspace_creation_parent(root, outside, TRUE, NULL);
  g_assert_true(g_file_equal(chosen, root));
  g_object_unref(chosen);
  GFile *trash = pdfv_workspace_create_folder(
      root, root, ".trash", &error);
  g_assert_no_error(error);
  chosen = pdfv_workspace_creation_parent(root, trash, TRUE, note);
  g_assert_true(g_file_equal(chosen, folder));
  g_object_unref(chosen);

  GFile *destination_folder = pdfv_workspace_create_folder(
      root, root, "Archive", &error);
  g_assert_no_error(error);
  GFile *moved = pdfv_workspace_move_item(
      root, note, destination_folder, &error);
  g_assert_no_error(error);
  g_assert_nonnull(moved);
  g_assert_false(g_file_query_exists(note, NULL));
  g_assert_true(g_file_query_exists(moved, NULL));

  GFile *collision = pdfv_workspace_create_note(
      root, folder, "Grüße.md", &error);
  g_assert_no_error(error);
  GFile *colliding_move = pdfv_workspace_move_item(
      root, collision, destination_folder, &error);
  g_assert_null(colliding_move);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error(&error);
  g_assert_true(g_file_query_exists(collision, NULL));
  g_assert_true(g_file_query_exists(moved, NULL));

  GFile *nested = pdfv_workspace_create_folder(
      root, folder, "Nested", &error);
  g_assert_no_error(error);
  GFile *recursive = pdfv_workspace_move_item(root, folder, nested, &error);
  g_assert_null(recursive);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE);
  g_clear_error(&error);

  GFile *escaped = pdfv_workspace_move_item(
      root, moved, outside, &error);
  g_assert_null(escaped);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);

  delete_file(moved);
  delete_file(collision);
  delete_file(nested);
  delete_file(folder);
  delete_file(destination_folder);
  delete_file(trash);
  delete_file(root);
  g_object_unref(moved);
  g_object_unref(collision);
  g_object_unref(nested);
  g_object_unref(destination_folder);
  g_object_unref(trash);
  g_object_unref(note);
  g_object_unref(folder);
  g_object_unref(outside);
  g_object_unref(root);
  g_free(path);
}

static void test_workspace_rename(void) {
  GError *error = NULL;
  gchar *path = g_dir_make_tmp("phi-rename-XXXXXX", &error);
  g_assert_no_error(error);
  GFile *root = g_file_new_for_path(path);
  GFile *folder = pdfv_workspace_create_folder(root, root, "Kurs", &error);
  g_assert_no_error(error);
  GFile *note = pdfv_workspace_create_note(root, folder, "Alt", &error);
  g_assert_no_error(error);
  GFile *other = pdfv_workspace_create_note(root, folder, "Andere", &error);
  g_assert_no_error(error);

  /* The extension is kept when the new name omits it. */
  GFile *renamed = pdfv_workspace_rename_item(root, note, " Neu ", &error);
  g_assert_no_error(error);
  gchar *basename = g_file_get_basename(renamed);
  g_assert_cmpstr(basename, ==, "Neu.md");
  g_free(basename);
  g_assert_false(g_file_query_exists(note, NULL));
  g_assert_true(g_file_query_exists(renamed, NULL));

  GFile *explicit_extension = pdfv_workspace_rename_item(
      root, renamed, "Neuer.MD", &error);
  g_assert_no_error(error);
  basename = g_file_get_basename(explicit_extension);
  g_assert_cmpstr(basename, ==, "Neuer.MD");
  g_free(basename);

  GFile *collision = pdfv_workspace_rename_item(
      root, explicit_extension, "Andere.md", &error);
  g_assert_null(collision);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error(&error);
  g_assert_true(g_file_query_exists(explicit_extension, NULL));

  GFile *unchanged = pdfv_workspace_rename_item(
      root, explicit_extension, "Neuer.MD", &error);
  g_assert_null(unchanged);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error(&error);

  GFile *invalid = pdfv_workspace_rename_item(
      root, explicit_extension, "../Neu", &error);
  g_assert_null(invalid);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  GFile *workspace_root = pdfv_workspace_rename_item(root, root, "x", &error);
  g_assert_null(workspace_root);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);

  /* Folders take the name verbatim and keep their contents. */
  GFile *renamed_folder = pdfv_workspace_rename_item(
      root, folder, "Kurs.2026", &error);
  g_assert_no_error(error);
  basename = g_file_get_basename(renamed_folder);
  g_assert_cmpstr(basename, ==, "Kurs.2026");
  g_free(basename);
  GFile *moved_note = g_file_get_child(renamed_folder, "Neuer.MD");
  GFile *moved_other = g_file_get_child(renamed_folder, "Andere.md");
  g_assert_true(g_file_query_exists(moved_note, NULL));

  delete_file(moved_note);
  delete_file(moved_other);
  delete_file(renamed_folder);
  delete_file(root);
  g_object_unref(moved_note);
  g_object_unref(moved_other);
  g_object_unref(renamed_folder);
  g_object_unref(explicit_extension);
  g_object_unref(renamed);
  g_object_unref(other);
  g_object_unref(note);
  g_object_unref(folder);
  g_object_unref(root);
  g_free(path);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/workspace/file-operations",
                  test_workspace_file_operations);
  g_test_add_func("/workspace/rename", test_workspace_rename);
  return g_test_run();
}

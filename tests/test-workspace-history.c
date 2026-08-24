/* Phi workspace history tests.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "workspace-history.h"

#include <glib/gstdio.h>

static gchar *test_config_dir;

static GFile *test_folder(const gchar *name) {
  gchar *path = g_build_filename(test_config_dir, name, NULL);
  GFile *file = g_file_new_for_path(path);
  g_free(path);
  return file;
}

static void assert_file_at(GPtrArray *files, guint index, GFile *expected) {
  g_assert_cmpuint(index, <, files->len);
  g_assert_true(g_file_equal(g_ptr_array_index(files, index), expected));
}

static void test_recent_workspaces(void) {
  GFile *a = test_folder("workspace-a");
  GFile *b = test_folder("workspace-b");
  GFile *c = test_folder("workspace-c");
  GFile *d = test_folder("workspace-d");
  GFile *e = test_folder("workspace-e");
  GFile *f = test_folder("workspace-f");

  pdfv_workspace_history_remember(a);
  pdfv_workspace_history_remember(b);
  pdfv_workspace_history_remember(a);

  GFile *current = pdfv_workspace_history_dup_current();
  g_assert_true(g_file_equal(current, a));
  g_object_unref(current);
  GPtrArray *recent = pdfv_workspace_history_list();
  g_assert_cmpuint(recent->len, ==, 2);
  assert_file_at(recent, 0, a);
  assert_file_at(recent, 1, b);
  g_ptr_array_unref(recent);

  pdfv_workspace_history_clear_current();
  g_assert_null(pdfv_workspace_history_dup_current());
  recent = pdfv_workspace_history_list();
  g_assert_cmpuint(recent->len, ==, 2);
  g_ptr_array_unref(recent);

  pdfv_workspace_history_remember(c);
  pdfv_workspace_history_remember(d);
  pdfv_workspace_history_remember(e);
  pdfv_workspace_history_remember(f);
  recent = pdfv_workspace_history_list();
  g_assert_cmpuint(recent->len, ==, PDFV_WORKSPACE_HISTORY_LIMIT);
  assert_file_at(recent, 0, f);
  assert_file_at(recent, 1, e);
  assert_file_at(recent, 2, d);
  assert_file_at(recent, 3, c);
  assert_file_at(recent, 4, a);
  g_ptr_array_unref(recent);

  pdfv_workspace_history_forget(d);
  recent = pdfv_workspace_history_list();
  g_assert_cmpuint(recent->len, ==, 4);
  assert_file_at(recent, 0, f);
  assert_file_at(recent, 1, e);
  assert_file_at(recent, 2, c);
  assert_file_at(recent, 3, a);
  g_ptr_array_unref(recent);

  g_object_unref(a);
  g_object_unref(b);
  g_object_unref(c);
  g_object_unref(d);
  g_object_unref(e);
  g_object_unref(f);
}

int main(int argc, char **argv) {
  GError *error = NULL;
  test_config_dir = g_dir_make_tmp("phi-workspace-history-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(test_config_dir);
  g_setenv("XDG_CONFIG_HOME", test_config_dir, TRUE);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/workspace-history/recent", test_recent_workspaces);
  gint result = g_test_run();

  gchar *history_dir = g_build_filename(test_config_dir, "phi-pdf-viewer",
                                        NULL);
  gchar *state_file = g_build_filename(history_dir, "state.ini", NULL);
  g_remove(state_file);
  g_rmdir(history_dir);
  g_rmdir(test_config_dir);
  g_free(state_file);
  g_free(history_dir);
  g_free(test_config_dir);
  return result;
}

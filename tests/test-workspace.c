/*
 * Phi PDF Viewer - workspace integration tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 */

#include "pdfv-workspace.h"

typedef struct {
  GMainLoop *loop;
  PdfvWorkspace *workspace;
  GFile *root;
  GFile *nested;
  GFile *empty;
  GFile *deep_empty;
  GFile *first;
  GFile *second;
  gboolean finished;
  guint timeout_id;
} TestState;

static gboolean test_timeout(gpointer user_data) {
  TestState *state = user_data;
  if (!state->finished) {
    g_test_message("Timed out waiting for workspace indexing");
    g_main_loop_quit(state->loop);
  }
  return G_SOURCE_REMOVE;
}

static void search_finished(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
  TestState *state = user_data;
  GError *error = NULL;
  GPtrArray *groups = pdfv_workspace_search_finish(
      PDFV_WORKSPACE(source), result, &error);
  g_assert_no_error(error);
  g_assert_nonnull(groups);
  g_assert_cmpuint(groups->len, ==, 2);
  for (guint i = 0; i < groups->len; i++) {
    PdfvWorkspaceResultGroup *group = g_ptr_array_index(groups, i);
    g_assert_cmpuint(group->matches->len, >, 0);
    PdfvWorkspaceMatch *match = g_ptr_array_index(group->matches, 0);
    g_assert_nonnull(g_strstr_len(match->snippet, -1, "präsentiert"));
  }
  g_ptr_array_unref(groups);
  state->finished = TRUE;
  if (state->timeout_id) {
    g_source_remove(state->timeout_id);
    state->timeout_id = 0;
  }
  g_main_loop_quit(state->loop);
}

static gboolean wait_for_index(gpointer user_data) {
  TestState *state = user_data;
  if (pdfv_workspace_get_indexed_count(state->workspace) < 2)
    return G_SOURCE_CONTINUE;
  pdfv_workspace_search_async(state->workspace, "präsentiert", NULL,
                              search_finished, state);
  return G_SOURCE_REMOVE;
}

static void workspace_loaded(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  TestState *state = user_data;
  GError *error = NULL;
  g_assert_true(pdfv_workspace_load_finish(PDFV_WORKSPACE(source), result,
                                           &error));
  g_assert_no_error(error);
  g_assert_cmpuint(pdfv_workspace_get_pdf_count(state->workspace), ==, 2);

  GListModel *roots = pdfv_workspace_get_items(state->workspace);
  g_assert_cmpuint(g_list_model_get_n_items(roots), ==, 2);
  guint folder_count = 0;
  for (guint i = 0; i < g_list_model_get_n_items(roots); i++) {
    PdfvWorkspaceItem *item = g_list_model_get_item(roots, i);
    if (pdfv_workspace_item_is_folder(item)) {
      folder_count++;
      g_assert_cmpuint(g_list_model_get_n_items(
                           pdfv_workspace_item_get_children(item)),
                       ==, 1);
    }
    g_object_unref(item);
  }
  g_assert_cmpuint(folder_count, ==, 1);
  g_timeout_add(10, wait_for_index, state);
}

static void test_workspace_search(void) {
  GError *error = NULL;
  gchar *root_path = g_dir_make_tmp("pdfv-workspace-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(root_path);

  TestState state = {0};
  state.loop = g_main_loop_new(NULL, FALSE);
  state.root = g_file_new_for_path(root_path);
  state.nested = g_file_get_child(state.root, "Nested");
  g_assert_true(g_file_make_directory(state.nested, NULL, &error));
  g_assert_no_error(error);
  state.empty = g_file_get_child(state.root, "Empty");
  g_assert_true(g_file_make_directory(state.empty, NULL, &error));
  g_assert_no_error(error);
  state.deep_empty = g_file_get_child(state.empty, "Still Empty");
  g_assert_true(g_file_make_directory(state.deep_empty, NULL, &error));
  g_assert_no_error(error);
  state.first = g_file_get_child(state.root, "first.pdf");
  state.second = g_file_get_child(state.nested, "second.PDF");
  GFile *fixture =
      g_file_new_for_path(TEST_DATA_DIR "/separate-diacritic.pdf");
  g_assert_true(g_file_copy(fixture, state.first, G_FILE_COPY_NONE, NULL, NULL,
                            NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_copy(fixture, state.second, G_FILE_COPY_NONE, NULL, NULL,
                            NULL, &error));
  g_assert_no_error(error);
  g_object_unref(fixture);

  state.workspace = pdfv_workspace_new(state.root);
  pdfv_workspace_load_async(state.workspace, NULL, workspace_loaded, &state);
  state.timeout_id = g_timeout_add_seconds(15, test_timeout, &state);
  g_main_loop_run(state.loop);
  g_assert_true(state.finished);

  pdfv_workspace_cancel(state.workspace);
  g_object_unref(state.workspace);
  g_main_loop_unref(state.loop);
  g_assert_true(g_file_delete(state.second, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.first, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.nested, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.deep_empty, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.empty, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.root, NULL, &error));
  g_assert_no_error(error);
  g_object_unref(state.second);
  g_object_unref(state.first);
  g_object_unref(state.nested);
  g_object_unref(state.deep_empty);
  g_object_unref(state.empty);
  g_object_unref(state.root);
  g_free(root_path);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/workspace/recursive-unicode-search",
                  test_workspace_search);
  return g_test_run();
}

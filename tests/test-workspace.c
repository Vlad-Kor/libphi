/*
 * Phi PDF Viewer - workspace integration tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 */

#include "pdfv-workspace.h"

#include <glib/gstdio.h>

typedef struct {
  GMainLoop *loop;
  PdfvWorkspace *workspace;
  GFile *root;
  GFile *nested;
  GFile *empty;
  GFile *deep_empty;
  GFile *first;
  GFile *second;
  GFile *note;
  gboolean finished;
  guint timeout_id;
  guint expected_cache_hits;
} TestState;

static gchar *test_cache_home;

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
  PdfvWorkspaceResultGroup *nearest = g_ptr_array_index(groups, 0);
  g_assert_true(g_file_equal(nearest->file, state->second));
  for (guint i = 0; i < groups->len; i++) {
    PdfvWorkspaceResultGroup *group = g_ptr_array_index(groups, i);
    g_assert_cmpuint(group->matches->len, >, 0);
    PdfvWorkspaceMatch *match = g_ptr_array_index(group->matches, 0);
    g_assert_nonnull(g_strstr_len(match->snippet, -1, "präsentiert"));
  }
  g_assert_cmpuint(pdfv_workspace_get_cache_hit_count(state->workspace), ==,
                   state->expected_cache_hits);
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
  pdfv_workspace_search_near_async(state->workspace, "präsentiert",
                                   state->second, NULL, search_finished,
                                   state);
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
  g_assert_cmpuint(g_list_model_get_n_items(roots), ==, 3);
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
  g_assert_cmpuint(folder_count, ==, 2);
  g_timeout_add(10, wait_for_index, state);
}

static void load_search_and_wait(TestState *state, guint expected_cache_hits) {
  state->finished = FALSE;
  state->expected_cache_hits = expected_cache_hits;
  state->workspace = pdfv_workspace_new(state->root);
  pdfv_workspace_load_async(state->workspace, NULL, workspace_loaded, state);
  state->timeout_id = g_timeout_add_seconds(15, test_timeout, state);
  g_main_loop_run(state->loop);
  g_assert_true(state->finished);
}

static gchar *workspace_cache_directory(GFile *root) {
  gchar *uri = g_file_get_uri(root);
  gchar *key =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
  gchar *directory =
      g_build_filename(g_get_user_cache_dir(), "phi-pdf-viewer",
                       "workspace-index-v1", key, NULL);
  g_free(key);
  g_free(uri);
  return directory;
}

static gchar *document_cache_filename(const gchar *directory,
                                      const gchar *relative_path) {
  gchar *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, relative_path,
                                             -1);
  gchar *basename = g_strconcat(key, ".idx", NULL);
  gchar *filename = g_build_filename(directory, basename, NULL);
  g_free(basename);
  g_free(key);
  return filename;
}

static guint count_cache_entries(const gchar *directory) {
  GDir *dir = g_dir_open(directory, 0, NULL);
  g_assert_nonnull(dir);
  guint count = 0;
  const gchar *name = NULL;
  while ((name = g_dir_read_name(dir))) {
    if (g_str_has_suffix(name, ".idx"))
      count++;
  }
  g_dir_close(dir);
  return count;
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
  state.note = g_file_get_child(state.empty, "lecture.md");
  gchar *note_path = g_file_get_path(state.note);
  g_assert_true(g_file_set_contents(note_path, "# Lecture\n", -1, &error));
  g_assert_no_error(error);
  g_free(note_path);
  GFile *fixture =
      g_file_new_for_path(TEST_DATA_DIR "/separate-diacritic.pdf");
  g_assert_true(g_file_copy(fixture, state.first, G_FILE_COPY_NONE, NULL, NULL,
                            NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_copy(fixture, state.second, G_FILE_COPY_NONE, NULL, NULL,
                            NULL, &error));
  g_assert_no_error(error);
  g_object_unref(fixture);

  /* The first run extracts both PDFs and creates entries outside the
   * workspace. The second run must load both documents from those entries. */
  load_search_and_wait(&state, 0);
  g_object_unref(state.workspace);
  state.workspace = NULL;

  gchar *cache_directory = workspace_cache_directory(state.root);
  g_assert_false(g_str_has_prefix(cache_directory, root_path));
  g_assert_cmpuint(count_cache_entries(cache_directory), ==, 2);

  load_search_and_wait(&state, 2);
  g_object_unref(state.workspace);
  state.workspace = NULL;

  /* A damaged entry is ignored and repaired without affecting other PDFs. */
  gchar *first_cache =
      document_cache_filename(cache_directory, "first.pdf");
  g_assert_true(g_file_set_contents(first_cache, "damaged", -1, &error));
  g_assert_no_error(error);
  load_search_and_wait(&state, 1);
  g_object_unref(state.workspace);
  state.workspace = NULL;
  g_free(first_cache);

  /* Changing one PDF invalidates only that entry; the other remains a hit. */
  GFileOutputStream *stream = g_file_append_to(
      state.first, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(stream);
  gsize written = 0;
  g_assert_true(g_output_stream_write_all(G_OUTPUT_STREAM(stream), "\n", 1,
                                          &written, NULL, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(written, ==, 1);
  g_assert_true(g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, &error));
  g_assert_no_error(error);
  g_object_unref(stream);

  load_search_and_wait(&state, 1);

  pdfv_workspace_cancel(state.workspace);
  g_object_unref(state.workspace);
  g_main_loop_unref(state.loop);
  g_assert_true(g_file_delete(state.second, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.first, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(state.note, NULL, &error));
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
  g_object_unref(state.note);
  g_object_unref(state.nested);
  g_object_unref(state.deep_empty);
  g_object_unref(state.empty);
  g_object_unref(state.root);
  GDir *cache = g_dir_open(cache_directory, 0, NULL);
  g_assert_nonnull(cache);
  const gchar *entry = NULL;
  while ((entry = g_dir_read_name(cache))) {
    gchar *filename = g_build_filename(cache_directory, entry, NULL);
    g_assert_cmpint(g_remove(filename), ==, 0);
    g_free(filename);
  }
  g_dir_close(cache);
  g_assert_cmpint(g_rmdir(cache_directory), ==, 0);
  g_free(cache_directory);
  g_free(root_path);
}

int main(int argc, char **argv) {
  GError *error = NULL;
  test_cache_home =
      g_dir_make_tmp("pdfv-workspace-cache-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(test_cache_home);
  g_setenv("XDG_CACHE_HOME", test_cache_home, TRUE);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/workspace/recursive-unicode-search",
                  test_workspace_search);
  gint status = g_test_run();
  gchar *version_directory =
      g_build_filename(test_cache_home, "phi-pdf-viewer",
                       "workspace-index-v1", NULL);
  gchar *application_directory =
      g_build_filename(test_cache_home, "phi-pdf-viewer", NULL);
  g_assert_cmpint(g_rmdir(version_directory), ==, 0);
  g_assert_cmpint(g_rmdir(application_directory), ==, 0);
  g_assert_cmpint(g_rmdir(test_cache_home), ==, 0);
  g_free(application_directory);
  g_free(version_directory);
  g_free(test_cache_home);
  return status;
}

/*
 * Phi PDF Viewer - Persistent recent workspace history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "workspace-history.h"

static gchar *workspace_history_file(void) {
  return g_build_filename(g_get_user_config_dir(), "phi-pdf-viewer",
                          "state.ini", NULL);
}

static GKeyFile *workspace_history_load(gchar **filename) {
  *filename = workspace_history_file();
  GKeyFile *state = g_key_file_new();
  g_key_file_load_from_file(state, *filename, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return state;
}

static void workspace_history_save(GKeyFile *state, const gchar *filename) {
  gchar *directory = g_path_get_dirname(filename);
  gsize length = 0;
  gchar *contents = g_key_file_to_data(state, &length, NULL);
  if (g_mkdir_with_parents(directory, 0700) != 0 ||
      !g_file_set_contents(filename, contents, length, NULL))
    g_debug("Could not save workspace history at %s", filename);
  g_free(contents);
  g_free(directory);
}

static gchar **workspace_history_get_recent(GKeyFile *state,
                                            gsize *length) {
  GError *error = NULL;
  gchar **recent = g_key_file_get_string_list(
      state, "workspace", "recent", length, &error);
  if (error) {
    g_clear_error(&error);
    *length = 0;
  }
  return recent;
}

static void workspace_history_set_recent(GKeyFile *state,
                                         GPtrArray *uris) {
  if (uris->len == 0) {
    g_key_file_remove_key(state, "workspace", "recent", NULL);
    return;
  }
  g_key_file_set_string_list(state, "workspace", "recent",
                             (const gchar *const *)uris->pdata, uris->len);
}

void pdfv_workspace_history_remember(GFile *folder) {
  g_return_if_fail(G_IS_FILE(folder));

  gchar *filename = NULL;
  GKeyFile *state = workspace_history_load(&filename);
  gchar *uri = g_file_get_uri(folder);
  g_key_file_set_string(state, "workspace", "uri", uri);

  gsize old_length = 0;
  gchar **old_recent = workspace_history_get_recent(state, &old_length);
  GPtrArray *recent = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
  g_ptr_array_add(recent, g_strdup(uri));
  g_hash_table_add(seen, g_strdup(uri));
  for (gsize i = 0;
       old_recent && i < old_length &&
           recent->len < PDFV_WORKSPACE_HISTORY_LIMIT;
       i++) {
    if (old_recent[i] && *old_recent[i] &&
        !g_hash_table_contains(seen, old_recent[i])) {
      g_ptr_array_add(recent, g_strdup(old_recent[i]));
      g_hash_table_add(seen, g_strdup(old_recent[i]));
    }
  }
  workspace_history_set_recent(state, recent);
  workspace_history_save(state, filename);

  g_hash_table_unref(seen);
  g_ptr_array_unref(recent);
  g_strfreev(old_recent);
  g_free(uri);
  g_key_file_unref(state);
  g_free(filename);
}

void pdfv_workspace_history_clear_current(void) {
  gchar *filename = NULL;
  GKeyFile *state = workspace_history_load(&filename);
  g_key_file_remove_key(state, "workspace", "uri", NULL);
  workspace_history_save(state, filename);
  g_key_file_unref(state);
  g_free(filename);
}

GFile *pdfv_workspace_history_dup_current(void) {
  gchar *filename = NULL;
  GKeyFile *state = workspace_history_load(&filename);
  gchar *uri = g_key_file_get_string(state, "workspace", "uri", NULL);
  GFile *folder = uri && *uri ? g_file_new_for_uri(uri) : NULL;
  g_free(uri);
  g_key_file_unref(state);
  g_free(filename);
  return folder;
}

GPtrArray *pdfv_workspace_history_list(void) {
  gchar *filename = NULL;
  GKeyFile *state = workspace_history_load(&filename);
  gsize length = 0;
  gchar **recent = workspace_history_get_recent(state, &length);
  GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
  for (gsize i = 0;
       recent && i < length && files->len < PDFV_WORKSPACE_HISTORY_LIMIT;
       i++) {
    if (!recent[i] || !*recent[i] || g_hash_table_contains(seen, recent[i]))
      continue;
    g_hash_table_add(seen, g_strdup(recent[i]));
    g_ptr_array_add(files, g_file_new_for_uri(recent[i]));
  }
  g_hash_table_unref(seen);
  g_strfreev(recent);
  g_key_file_unref(state);
  g_free(filename);
  return files;
}

void pdfv_workspace_history_forget(GFile *folder) {
  g_return_if_fail(G_IS_FILE(folder));

  gchar *filename = NULL;
  GKeyFile *state = workspace_history_load(&filename);
  gchar *uri = g_file_get_uri(folder);
  gchar *current = g_key_file_get_string(state, "workspace", "uri", NULL);
  if (g_strcmp0(uri, current) == 0)
    g_key_file_remove_key(state, "workspace", "uri", NULL);

  gsize old_length = 0;
  gchar **old_recent = workspace_history_get_recent(state, &old_length);
  GPtrArray *recent = g_ptr_array_new_with_free_func(g_free);
  for (gsize i = 0; old_recent && i < old_length; i++) {
    if (old_recent[i] && *old_recent[i] && g_strcmp0(old_recent[i], uri) != 0)
      g_ptr_array_add(recent, g_strdup(old_recent[i]));
  }
  workspace_history_set_recent(state, recent);
  workspace_history_save(state, filename);

  g_ptr_array_unref(recent);
  g_strfreev(old_recent);
  g_free(current);
  g_free(uri);
  g_key_file_unref(state);
  g_free(filename);
}

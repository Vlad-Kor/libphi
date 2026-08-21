/*
 * Phi Markdown editor - native vault filesystem boundary
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "markdown-vault-adapter.h"

#include <string.h>

struct _PdfvMarkdownVaultAdapter {
  GObject parent_instance;
  GFile *root;
};

G_DEFINE_FINAL_TYPE(PdfvMarkdownVaultAdapter, pdfv_markdown_vault_adapter,
                    G_TYPE_OBJECT)

static void pdfv_markdown_vault_adapter_finalize(GObject *object) {
  PdfvMarkdownVaultAdapter *self = PDFV_MARKDOWN_VAULT_ADAPTER(object);
  g_clear_object(&self->root);
  G_OBJECT_CLASS(pdfv_markdown_vault_adapter_parent_class)->finalize(object);
}

static void pdfv_markdown_vault_adapter_class_init(
    PdfvMarkdownVaultAdapterClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_markdown_vault_adapter_finalize;
}

static void pdfv_markdown_vault_adapter_init(
    PdfvMarkdownVaultAdapter *self) {
  (void)self;
}

PdfvMarkdownVaultAdapter *pdfv_markdown_vault_adapter_new(GFile *root) {
  g_return_val_if_fail(G_IS_FILE(root), NULL);
  PdfvMarkdownVaultAdapter *self =
      g_object_new(PDFV_TYPE_MARKDOWN_VAULT_ADAPTER, NULL);
  self->root = g_object_ref(root);
  return self;
}

GFile *pdfv_markdown_vault_adapter_get_root(
    PdfvMarkdownVaultAdapter *self) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  return self->root;
}

static gboolean path_is_safe(const gchar *path) {
  if (!path || !*path || g_path_is_absolute(path) || strchr(path, '\\'))
    return FALSE;
  gchar **parts = g_strsplit(path, "/", -1);
  gboolean safe = TRUE;
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_str_equal(parts[i], ".") ||
        g_str_equal(parts[i], "..")) {
      safe = FALSE;
      break;
    }
  }
  g_strfreev(parts);
  return safe;
}

static gboolean path_has_symlink(GFile *root, const gchar *path) {
  GFile *cursor = g_object_ref(root);
  gchar **parts = g_strsplit(path, "/", -1);
  gboolean symlink = FALSE;
  for (guint i = 0; parts[i]; i++) {
    GFile *child = g_file_get_child(cursor, parts[i]);
    GFileInfo *info = g_file_query_info(
        child, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    if (info && g_file_info_get_is_symlink(info))
      symlink = TRUE;
    g_clear_object(&info);
    g_object_unref(cursor);
    cursor = child;
    if (symlink)
      break;
  }
  g_strfreev(parts);
  g_object_unref(cursor);
  return symlink;
}

GFile *pdfv_markdown_vault_adapter_resolve(PdfvMarkdownVaultAdapter *self,
                                           const gchar *relative_path,
                                           GError **error) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  gchar *decoded = g_uri_unescape_string(relative_path, NULL);
  if (!path_is_safe(decoded)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Unsafe vault path");
    g_free(decoded);
    return NULL;
  }
  if (path_has_symlink(self->root, decoded)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Vault symlinks are not exposed to Markdown");
    g_free(decoded);
    return NULL;
  }
  GFile *file = g_file_resolve_relative_path(self->root, decoded);
  gchar *back = g_file_get_relative_path(self->root, file);
  if (!back || !path_is_safe(back)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Path escapes the vault");
    g_clear_object(&file);
  }
  g_free(back);
  g_free(decoded);
  return file;
}

gchar *pdfv_markdown_vault_adapter_relative_path(
    PdfvMarkdownVaultAdapter *self, GFile *file) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  g_return_val_if_fail(G_IS_FILE(file), NULL);
  gchar *relative = g_file_get_relative_path(self->root, file);
  if (!relative || !path_is_safe(relative))
    g_clear_pointer(&relative, g_free);
  return relative;
}

static gboolean filename_is_markdown(const gchar *name) {
  const gchar *dot = strrchr(name, '.');
  return dot && g_ascii_strcasecmp(dot, ".md") == 0;
}

static gboolean fuzzy_match(const gchar *value, const gchar *query) {
  if (!query || !*query)
    return TRUE;
  gchar *folded = g_utf8_casefold(value, -1);
  gchar *needle = g_utf8_casefold(query, -1);
  const gchar *cursor = folded;
  for (const gchar *at = needle; *at;) {
    gunichar wanted = g_utf8_get_char(at);
    gboolean found = FALSE;
    while (*cursor) {
      gunichar current = g_utf8_get_char(cursor);
      cursor = g_utf8_next_char(cursor);
      if (current == wanted) {
        found = TRUE;
        break;
      }
    }
    if (!found) {
      g_free(folded);
      g_free(needle);
      return FALSE;
    }
    at = g_utf8_next_char(at);
  }
  g_free(folded);
  g_free(needle);
  return TRUE;
}

static gint string_ptr_compare(gconstpointer left, gconstpointer right) {
  return g_utf8_collate(*(const gchar *const *)left,
                        *(const gchar *const *)right);
}

static gboolean list_notes_recursive(PdfvMarkdownVaultAdapter *self,
                                     GFile *folder, const gchar *prefix,
                                     const gchar *query, GPtrArray *result,
                                     GError **error) {
  GFileEnumerator *enumerator = g_file_enumerate_children(
      folder, G_FILE_ATTRIBUTE_STANDARD_NAME ","
              G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!enumerator)
    return FALSE;
  gboolean success = TRUE;
  for (;;) {
    GFileInfo *info = g_file_enumerator_next_file(enumerator, NULL, error);
    if (!info)
      break;
    const gchar *name = g_file_info_get_name(info);
    GFileType type = g_file_info_get_file_type(info);
    GFile *child = g_file_get_child(folder, name);
    gchar *relative = prefix && *prefix
                          ? g_build_filename(prefix, name, NULL)
                          : g_strdup(name);
    if (type == G_FILE_TYPE_DIRECTORY &&
        !g_str_equal(name, ".obsidian") && !g_str_has_prefix(name, ".git")) {
      success = list_notes_recursive(self, child, relative, query, result,
                                     error);
    } else if (type == G_FILE_TYPE_REGULAR && filename_is_markdown(name) &&
               fuzzy_match(relative, query)) {
      g_ptr_array_add(result, g_strdup(relative));
    }
    g_free(relative);
    g_object_unref(child);
    g_object_unref(info);
    if (!success || (error && *error))
      break;
  }
  g_object_unref(enumerator);
  return success && (!error || !*error);
}

GPtrArray *pdfv_markdown_vault_adapter_list_notes(
    PdfvMarkdownVaultAdapter *self, const gchar *query, GError **error) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  if (!list_notes_recursive(self, self->root, "", query, result, error)) {
    g_ptr_array_unref(result);
    return NULL;
  }
  g_ptr_array_sort(result, string_ptr_compare);
  return result;
}

static gchar *note_target_path(const gchar *target) {
  gchar *decoded = g_uri_unescape_string(target ? target : "", NULL);
  if (!decoded)
    return NULL;
  gchar *section = strpbrk(decoded, "#|");
  if (section)
    *section = '\0';
  g_strstrip(decoded);
  if (!*decoded) {
    g_free(decoded);
    return NULL;
  }
  if (!filename_is_markdown(decoded)) {
    gchar *with_extension = g_strconcat(decoded, ".md", NULL);
    g_free(decoded);
    decoded = with_extension;
  }
  return decoded;
}

GFile *pdfv_markdown_vault_adapter_resolve_new_note(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, GError **error) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  gchar *path = note_target_path(target);
  if (!path) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Empty note target");
    return NULL;
  }
  gchar *candidate = NULL;
  if (source_path && *source_path && !strchr(path, '/')) {
    gchar *directory = g_path_get_dirname(source_path);
    candidate = g_str_equal(directory, ".")
                    ? g_strdup(path)
                    : g_build_filename(directory, path, NULL);
    g_free(directory);
  } else {
    candidate = g_strdup(path);
  }
  GFile *file = pdfv_markdown_vault_adapter_resolve(self, candidate, error);
  g_free(candidate);
  g_free(path);
  return file;
}

GFile *pdfv_markdown_vault_adapter_resolve_note(
    PdfvMarkdownVaultAdapter *self, const gchar *source_path,
    const gchar *target, GError **error) {
  gchar *path = note_target_path(target);
  if (!path) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "Empty note target");
    return NULL;
  }
  GFile *result = NULL;
  if (source_path && *source_path && !strchr(path, '/')) {
    gchar *directory = g_path_get_dirname(source_path);
    gchar *near_path = g_str_equal(directory, ".")
                           ? g_strdup(path)
                           : g_build_filename(directory, path, NULL);
    result = pdfv_markdown_vault_adapter_resolve(self, near_path, NULL);
    if (result && !g_file_query_exists(result, NULL))
      g_clear_object(&result);
    g_free(near_path);
    g_free(directory);
  }
  if (!result) {
    result = pdfv_markdown_vault_adapter_resolve(self, path, NULL);
    if (result && !g_file_query_exists(result, NULL))
      g_clear_object(&result);
  }
  if (!result && !strchr(path, '/')) {
    GPtrArray *notes = pdfv_markdown_vault_adapter_list_notes(self, path, NULL);
    for (guint i = 0; notes && i < notes->len; i++) {
      gchar *candidate = g_ptr_array_index(notes, i);
      gchar *basename = g_path_get_basename(candidate);
      if (g_ascii_strcasecmp(basename, path) == 0) {
        result = pdfv_markdown_vault_adapter_resolve(self, candidate, NULL);
        g_free(basename);
        break;
      }
      g_free(basename);
    }
    g_clear_pointer(&notes, g_ptr_array_unref);
  }
  if (!result)
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Note '%s' was not found", target ? target : "");
  g_free(path);
  return result;
}

gchar *pdfv_markdown_vault_adapter_read_text(
    PdfvMarkdownVaultAdapter *self, GFile *file, gchar **etag,
    GError **error) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(self), NULL);
  g_return_val_if_fail(G_IS_FILE(file), NULL);
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_load_contents(file, NULL, &contents, &length, etag, error))
    return NULL;
  if (!g_utf8_validate(contents, length, NULL) ||
      memchr(contents, '\0', length) != NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Markdown file is not valid UTF-8 text");
    g_free(contents);
    return NULL;
  }
  return contents;
}

GBytes *pdfv_markdown_vault_adapter_read_bytes(
    PdfvMarkdownVaultAdapter *self, const gchar *relative_path,
    gchar **content_type, GError **error) {
  GFile *file = pdfv_markdown_vault_adapter_resolve(self, relative_path, error);
  if (!file)
    return NULL;
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_load_contents(file, NULL, &contents, &length, NULL, error)) {
    g_object_unref(file);
    return NULL;
  }
  if (content_type) {
    gboolean uncertain = FALSE;
    gchar *guessed = g_content_type_guess(relative_path,
                                          (const guchar *)contents,
                                          MIN(length, 512), &uncertain);
    *content_type = g_content_type_get_mime_type(guessed);
    g_free(guessed);
  }
  g_object_unref(file);
  return g_bytes_new_take(contents, length);
}

static GPtrArray *extract_matches(PdfvMarkdownVaultAdapter *self, GFile *file,
                                  GRegex *regex, guint capture,
                                  GError **error) {
  gchar *text = pdfv_markdown_vault_adapter_read_text(self, file, NULL, error);
  if (!text)
    return NULL;
  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  GMatchInfo *match = NULL;
  g_regex_match(regex, text, 0, &match);
  while (g_match_info_matches(match)) {
    gchar *value = g_match_info_fetch(match, capture);
    if (value && *value)
      g_ptr_array_add(result, value);
    else
      g_free(value);
    g_match_info_next(match, NULL);
  }
  g_match_info_free(match);
  g_free(text);
  return result;
}

GPtrArray *pdfv_markdown_vault_adapter_get_headings(
    PdfvMarkdownVaultAdapter *self, GFile *file, GError **error) {
  GRegex *regex = g_regex_new("^#{1,6}[ \\t]+(.+?)[ \\t]*#*[ \\t]*$",
                              G_REGEX_MULTILINE, 0, NULL);
  GPtrArray *result = extract_matches(self, file, regex, 1, error);
  g_regex_unref(regex);
  return result;
}

GPtrArray *pdfv_markdown_vault_adapter_get_blocks(
    PdfvMarkdownVaultAdapter *self, GFile *file, GError **error) {
  GRegex *regex = g_regex_new("(?:^|\\s)\\^([A-Za-z0-9-]+)[ \\t]*$",
                              G_REGEX_MULTILINE, 0, NULL);
  GPtrArray *result = extract_matches(self, file, regex, 1, error);
  g_regex_unref(regex);
  return result;
}

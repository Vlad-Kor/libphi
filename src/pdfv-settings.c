/* Global application preferences for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-settings.h"

#include <glib/gstdio.h>
#include <errno.h>

#define SETTINGS_GROUP "Markdown"
#define GENERAL_GROUP "General"

struct _PdfvSettings {
  gdouble markdown_font_scale;
  gboolean readable_line_width;
  gboolean allow_remote_images;
  gboolean latex_conceal;
  gboolean pdf_inverted;
  gboolean remember_document_positions;
  gchar *latex_snippets;
  gchar *latex_snippet_variables;
  GKeyFile *file;
};

static gchar *workspace_group(GFile *workspace) {
  if (!workspace)
    return NULL;
  gchar *uri = g_file_get_uri(workspace);
  gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
  gchar *group = g_strdup_printf("Workspace %s", digest);
  g_free(digest);
  g_free(uri);
  return group;
}

static gchar *settings_filename(void) {
  return g_build_filename(g_get_user_config_dir(), "phi-pdf-viewer",
                          "settings.ini", NULL);
}

PdfvSettings *pdfv_settings_new(void) {
  PdfvSettings *self = g_new0(PdfvSettings, 1);
  self->markdown_font_scale = 1.0;
  self->readable_line_width = TRUE;
  self->latex_snippets = g_strdup("");
  self->latex_snippet_variables = g_strdup("");
  self->file = g_key_file_new();

  gchar *filename = settings_filename();
  if (g_key_file_load_from_file(self->file, filename,
                                G_KEY_FILE_KEEP_COMMENTS, NULL)) {
    GError *error = NULL;
    gdouble scale = g_key_file_get_double(self->file, SETTINGS_GROUP,
                                          "font-scale", &error);
    if (!error)
      self->markdown_font_scale = CLAMP(scale, 0.6875, 2.0);
    g_clear_error(&error);
    gboolean readable = g_key_file_get_boolean(
        self->file, SETTINGS_GROUP, "readable-line-width", &error);
    if (!error)
      self->readable_line_width = readable;
    g_clear_error(&error);
    self->allow_remote_images = g_key_file_get_boolean(
        self->file, SETTINGS_GROUP, "allow-remote-images", &error);
    g_clear_error(&error);
    gboolean conceal = g_key_file_get_boolean(
        self->file, SETTINGS_GROUP, "latex-conceal", &error);
    if (!error)
      self->latex_conceal = conceal;
    g_clear_error(&error);
    gboolean inverted = g_key_file_get_boolean(
        self->file, SETTINGS_GROUP, "pdf-inverted", &error);
    if (!error)
      self->pdf_inverted = inverted;
    g_clear_error(&error);
    gboolean remember = g_key_file_get_boolean(
        self->file, GENERAL_GROUP, "remember-document-positions", &error);
    if (!error)
      self->remember_document_positions = remember;
    g_clear_error(&error);
    gchar *snippets = g_key_file_get_string(self->file, SETTINGS_GROUP,
                                             "latex-snippets", NULL);
    if (snippets) {
      g_free(self->latex_snippets);
      self->latex_snippets = snippets;
    }
    gchar *variables = g_key_file_get_string(
        self->file, SETTINGS_GROUP, "latex-snippet-variables", NULL);
    if (variables) {
      g_free(self->latex_snippet_variables);
      self->latex_snippet_variables = variables;
    }
  }
  g_free(filename);
  return self;
}

void pdfv_settings_free(PdfvSettings *self) {
  if (!self)
    return;
  g_clear_pointer(&self->file, g_key_file_unref);
  g_free(self->latex_snippets);
  g_free(self->latex_snippet_variables);
  g_free(self);
}

gboolean pdfv_settings_save(PdfvSettings *self, GError **error) {
  g_return_val_if_fail(self != NULL, FALSE);
  g_key_file_set_double(self->file, SETTINGS_GROUP, "font-scale",
                        self->markdown_font_scale);
  g_key_file_set_boolean(self->file, SETTINGS_GROUP, "readable-line-width",
                         self->readable_line_width);
  g_key_file_set_boolean(self->file, SETTINGS_GROUP, "allow-remote-images",
                         self->allow_remote_images);
  g_key_file_set_boolean(self->file, SETTINGS_GROUP, "latex-conceal",
                         self->latex_conceal);
  g_key_file_set_boolean(self->file, SETTINGS_GROUP, "pdf-inverted",
                         self->pdf_inverted);
  g_key_file_set_boolean(self->file, GENERAL_GROUP,
                         "remember-document-positions",
                         self->remember_document_positions);
  /* This used to control two incompatible fullscreen behaviors. Fullscreen
   * and presentation are explicit actions now, so discard the obsolete key
   * when rewriting an older settings file. */
  g_key_file_remove_key(self->file, SETTINGS_GROUP,
                        "fullscreen-single-page", NULL);
  g_key_file_set_string(self->file, SETTINGS_GROUP, "latex-snippets",
                        self->latex_snippets);
  g_key_file_set_string(self->file, SETTINGS_GROUP,
                        "latex-snippet-variables",
                        self->latex_snippet_variables);
  gsize length = 0;
  gchar *contents = g_key_file_to_data(self->file, &length, error);
  gchar *filename = settings_filename();
  gchar *directory = g_path_get_dirname(filename);
  gboolean saved = contents &&
      (g_mkdir_with_parents(directory, 0700) == 0) &&
      g_file_set_contents(filename, contents, length, error);
  if (!saved && contents && error && !*error)
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not create the settings directory");
  g_free(directory);
  g_free(filename);
  g_free(contents);
  return saved;
}

gdouble pdfv_settings_get_markdown_font_scale(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, 1.0);
  return self->markdown_font_scale;
}

void pdfv_settings_set_markdown_font_scale(PdfvSettings *self,
                                           gdouble scale) {
  g_return_if_fail(self != NULL);
  self->markdown_font_scale = CLAMP(scale, 0.6875, 2.0);
}

gboolean pdfv_settings_get_readable_line_width(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, TRUE);
  return self->readable_line_width;
}

void pdfv_settings_set_readable_line_width(PdfvSettings *self,
                                           gboolean enabled) {
  g_return_if_fail(self != NULL);
  self->readable_line_width = enabled;
}

gboolean pdfv_settings_get_allow_remote_images(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->allow_remote_images;
}

void pdfv_settings_set_allow_remote_images(PdfvSettings *self,
                                           gboolean allowed) {
  g_return_if_fail(self != NULL);
  self->allow_remote_images = allowed;
}

gboolean pdfv_settings_get_latex_conceal(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->latex_conceal;
}

void pdfv_settings_set_latex_conceal(PdfvSettings *self,
                                     gboolean enabled) {
  g_return_if_fail(self != NULL);
  self->latex_conceal = enabled;
}

gboolean pdfv_settings_get_pdf_inverted(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->pdf_inverted;
}

void pdfv_settings_set_pdf_inverted(PdfvSettings *self,
                                    gboolean inverted) {
  g_return_if_fail(self != NULL);
  self->pdf_inverted = inverted;
}

gboolean pdfv_settings_get_remember_document_positions(
    PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->remember_document_positions;
}

void pdfv_settings_set_remember_document_positions(
    PdfvSettings *self, gboolean enabled) {
  g_return_if_fail(self != NULL);
  self->remember_document_positions = enabled;
}

const gchar *pdfv_settings_get_latex_snippets(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, "");
  return self->latex_snippets;
}

void pdfv_settings_set_latex_snippets(PdfvSettings *self,
                                      const gchar *snippets) {
  g_return_if_fail(self != NULL);
  g_free(self->latex_snippets);
  self->latex_snippets = g_strdup(snippets ? snippets : "");
}

const gchar *pdfv_settings_get_latex_snippet_variables(
    PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, "");
  return self->latex_snippet_variables;
}

void pdfv_settings_set_latex_snippet_variables(
    PdfvSettings *self, const gchar *variables) {
  g_return_if_fail(self != NULL);
  g_free(self->latex_snippet_variables);
  self->latex_snippet_variables = g_strdup(variables ? variables : "");
}

gboolean pdfv_settings_get_workspace_attachment_fixed(
    PdfvSettings *self, GFile *workspace) {
  g_return_val_if_fail(self != NULL, FALSE);
  if (!workspace)
    return FALSE;
  gchar *group = workspace_group(workspace);
  gboolean fixed = g_key_file_get_boolean(
      self->file, group, "fixed-attachment-folder", NULL);
  g_free(group);
  return fixed;
}

gchar *pdfv_settings_dup_workspace_attachment_folder_uri(
    PdfvSettings *self, GFile *workspace) {
  g_return_val_if_fail(self != NULL, NULL);
  if (!workspace)
    return NULL;
  gchar *group = workspace_group(workspace);
  gchar *uri = g_key_file_get_string(
      self->file, group, "attachment-folder-uri", NULL);
  g_free(group);
  return uri;
}

void pdfv_settings_set_workspace_attachment_policy(
    PdfvSettings *self, GFile *workspace, gboolean fixed,
    const gchar *folder_uri) {
  g_return_if_fail(self != NULL);
  g_return_if_fail(G_IS_FILE(workspace));
  gchar *group = workspace_group(workspace);
  g_key_file_set_boolean(self->file, group, "fixed-attachment-folder",
                         fixed);
  if (folder_uri && *folder_uri)
    g_key_file_set_string(self->file, group, "attachment-folder-uri",
                          folder_uri);
  g_free(group);
}

gchar **pdfv_settings_dup_workspace_open_tabs(
    PdfvSettings *self, GFile *workspace, gsize *length) {
  g_return_val_if_fail(self != NULL, NULL);
  if (length)
    *length = 0;
  if (!workspace)
    return NULL;
  gchar *group = workspace_group(workspace);
  gchar **tabs = g_key_file_get_string_list(
      self->file, group, "open-tabs", length, NULL);
  g_free(group);
  return tabs;
}

gchar *pdfv_settings_dup_workspace_active_tab(
    PdfvSettings *self, GFile *workspace) {
  g_return_val_if_fail(self != NULL, NULL);
  if (!workspace)
    return NULL;
  gchar *group = workspace_group(workspace);
  gchar *active = g_key_file_get_string(
      self->file, group, "active-tab", NULL);
  g_free(group);
  return active;
}

void pdfv_settings_set_workspace_tabs(
    PdfvSettings *self, GFile *workspace,
    const gchar *const *relative_paths, gsize length,
    const gchar *active_relative_path) {
  g_return_if_fail(self != NULL);
  g_return_if_fail(G_IS_FILE(workspace));
  gchar *group = workspace_group(workspace);
  if (relative_paths && length > 0) {
    g_key_file_set_string_list(self->file, group, "open-tabs",
                               relative_paths, length);
  } else {
    g_key_file_remove_key(self->file, group, "open-tabs", NULL);
  }
  if (active_relative_path && *active_relative_path) {
    g_key_file_set_string(self->file, group, "active-tab",
                          active_relative_path);
  } else {
    g_key_file_remove_key(self->file, group, "active-tab", NULL);
  }
  g_free(group);
}

gchar **pdfv_settings_dup_workspace_expanded_folders(
    PdfvSettings *self, GFile *workspace, gsize *length) {
  g_return_val_if_fail(self != NULL, NULL);
  if (length)
    *length = 0;
  if (!workspace)
    return NULL;
  gchar *group = workspace_group(workspace);
  gchar **folders = g_key_file_get_string_list(
      self->file, group, "expanded-folders", length, NULL);
  g_free(group);
  return folders;
}

void pdfv_settings_set_workspace_expanded_folders(
    PdfvSettings *self, GFile *workspace,
    const gchar *const *relative_paths, gsize length) {
  g_return_if_fail(self != NULL);
  g_return_if_fail(G_IS_FILE(workspace));
  gchar *group = workspace_group(workspace);
  if (relative_paths && length > 0) {
    g_key_file_set_string_list(self->file, group, "expanded-folders",
                               relative_paths, length);
  } else {
    g_key_file_remove_key(self->file, group, "expanded-folders", NULL);
  }
  g_free(group);
}

void pdfv_settings_copy(PdfvSettings *destination,
                        PdfvSettings *source) {
  g_return_if_fail(destination != NULL);
  g_return_if_fail(source != NULL);
  destination->markdown_font_scale = source->markdown_font_scale;
  destination->readable_line_width = source->readable_line_width;
  destination->allow_remote_images = source->allow_remote_images;
  destination->latex_conceal = source->latex_conceal;
  destination->pdf_inverted = source->pdf_inverted;
  destination->remember_document_positions =
      source->remember_document_positions;
  pdfv_settings_set_latex_snippets(destination, source->latex_snippets);
  pdfv_settings_set_latex_snippet_variables(
      destination, source->latex_snippet_variables);
  gsize length = 0;
  gchar *data = g_key_file_to_data(source->file, &length, NULL);
  GKeyFile *copy = g_key_file_new();
  if (data && g_key_file_load_from_data(copy, data, length,
                                        G_KEY_FILE_KEEP_COMMENTS, NULL)) {
    g_key_file_unref(destination->file);
    destination->file = copy;
  } else {
    g_key_file_unref(copy);
  }
  g_free(data);
}

/* Global application preferences for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-settings.h"

#include <glib/gstdio.h>
#include <errno.h>

#define SETTINGS_GROUP "Markdown"

struct _PdfvSettings {
  gdouble markdown_font_scale;
  gboolean allow_remote_images;
  gchar *latex_snippets;
};

static gchar *settings_filename(void) {
  return g_build_filename(g_get_user_config_dir(), "phi-pdf-viewer",
                          "settings.ini", NULL);
}

PdfvSettings *pdfv_settings_new(void) {
  PdfvSettings *self = g_new0(PdfvSettings, 1);
  self->markdown_font_scale = 1.0;
  self->latex_snippets = g_strdup("");

  gchar *filename = settings_filename();
  GKeyFile *file = g_key_file_new();
  if (g_key_file_load_from_file(file, filename, G_KEY_FILE_NONE, NULL)) {
    GError *error = NULL;
    gdouble scale = g_key_file_get_double(file, SETTINGS_GROUP,
                                          "font-scale", &error);
    if (!error)
      self->markdown_font_scale = CLAMP(scale, 0.6875, 2.0);
    g_clear_error(&error);
    self->allow_remote_images = g_key_file_get_boolean(
        file, SETTINGS_GROUP, "allow-remote-images", &error);
    g_clear_error(&error);
    gchar *snippets = g_key_file_get_string(file, SETTINGS_GROUP,
                                             "latex-snippets", NULL);
    if (snippets) {
      g_free(self->latex_snippets);
      self->latex_snippets = snippets;
    }
  }
  g_key_file_unref(file);
  g_free(filename);
  return self;
}

void pdfv_settings_free(PdfvSettings *self) {
  if (!self)
    return;
  g_free(self->latex_snippets);
  g_free(self);
}

gboolean pdfv_settings_save(PdfvSettings *self, GError **error) {
  g_return_val_if_fail(self != NULL, FALSE);
  GKeyFile *file = g_key_file_new();
  g_key_file_set_double(file, SETTINGS_GROUP, "font-scale",
                        self->markdown_font_scale);
  g_key_file_set_boolean(file, SETTINGS_GROUP, "allow-remote-images",
                         self->allow_remote_images);
  g_key_file_set_string(file, SETTINGS_GROUP, "latex-snippets",
                        self->latex_snippets);
  gsize length = 0;
  gchar *contents = g_key_file_to_data(file, &length, error);
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
  g_key_file_unref(file);
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

gboolean pdfv_settings_get_allow_remote_images(PdfvSettings *self) {
  g_return_val_if_fail(self != NULL, FALSE);
  return self->allow_remote_images;
}

void pdfv_settings_set_allow_remote_images(PdfvSettings *self,
                                           gboolean allowed) {
  g_return_if_fail(self != NULL);
  self->allow_remote_images = allowed;
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

void pdfv_settings_copy(PdfvSettings *destination,
                        PdfvSettings *source) {
  g_return_if_fail(destination != NULL);
  g_return_if_fail(source != NULL);
  destination->markdown_font_scale = source->markdown_font_scale;
  destination->allow_remote_images = source->allow_remote_images;
  pdfv_settings_set_latex_snippets(destination, source->latex_snippets);
}

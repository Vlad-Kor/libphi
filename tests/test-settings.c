/* Phi settings persistence tests
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-settings.h"

#include <glib/gstdio.h>

static gchar *config_root;

static void test_workspace_attachment_policy(void) {
  GFile *workspace_a = g_file_new_for_path("/tmp/phi-workspace-a");
  GFile *workspace_b = g_file_new_for_path("/tmp/phi-workspace-b");
  GFile *folder = g_file_new_for_path("/tmp/phi-workspace-a/Images");
  gchar *folder_uri = g_file_get_uri(folder);

  PdfvSettings *settings = pdfv_settings_new();
  g_assert_true(pdfv_settings_get_readable_line_width(settings));
  g_assert_false(pdfv_settings_get_latex_conceal(settings));
  g_assert_false(pdfv_settings_get_pdf_inverted(settings));
  g_assert_false(
      pdfv_settings_get_remember_document_positions(settings));
  gint window_width = 0;
  gint window_height = 0;
  pdfv_settings_get_window_size(settings, &window_width, &window_height);
  g_assert_cmpint(window_width, ==, 900);
  g_assert_cmpint(window_height, ==, 700);
  g_assert_cmpint(pdfv_settings_get_image_paste_style(settings), ==,
                  PDFV_IMAGE_PASTE_STYLE_WIKI_EMBED);
  pdfv_settings_set_workspace_attachment_policy(
      settings, workspace_a, TRUE, folder_uri);
  pdfv_settings_set_workspace_attachment_policy(
      settings, workspace_b, FALSE, NULL);
  const gchar *tabs[] = {"AI/Introduction.md", "papers/vision.pdf"};
  const gchar *expanded[] = {"AI", "AI/Zusammenfassung"};
  pdfv_settings_set_workspace_tabs(settings, workspace_a, tabs,
                                   G_N_ELEMENTS(tabs), tabs[0]);
  pdfv_settings_set_workspace_expanded_folders(
      settings, workspace_a, expanded, G_N_ELEMENTS(expanded));
  pdfv_settings_set_markdown_font_scale(settings, 1.25);
  pdfv_settings_set_readable_line_width(settings, FALSE);
  pdfv_settings_set_latex_conceal(settings, TRUE);
  pdfv_settings_set_pdf_inverted(settings, TRUE);
  pdfv_settings_set_remember_document_positions(settings, TRUE);
  pdfv_settings_set_window_size(settings, 1280, 840);
  pdfv_settings_set_image_paste_style(
      settings, PDFV_IMAGE_PASTE_STYLE_MARKDOWN_LINK);
  pdfv_settings_set_latex_snippets(settings, "[{trigger:'x'}]");
  pdfv_settings_set_latex_snippet_variables(settings,
                                             "{GREEK:'alpha'}");
  GError *error = NULL;
  g_assert_true(pdfv_settings_save(settings, &error));
  g_assert_no_error(error);
  pdfv_settings_free(settings);

  settings = pdfv_settings_new();
  g_assert_true(pdfv_settings_get_workspace_attachment_fixed(
      settings, workspace_a));
  g_assert_false(pdfv_settings_get_workspace_attachment_fixed(
      settings, workspace_b));
  gchar *restored = pdfv_settings_dup_workspace_attachment_folder_uri(
      settings, workspace_a);
  g_assert_cmpstr(restored, ==, folder_uri);
  g_assert_cmpfloat(pdfv_settings_get_markdown_font_scale(settings), ==,
                    1.25);
  g_assert_false(pdfv_settings_get_readable_line_width(settings));
  g_assert_true(pdfv_settings_get_latex_conceal(settings));
  g_assert_true(pdfv_settings_get_pdf_inverted(settings));
  g_assert_true(
      pdfv_settings_get_remember_document_positions(settings));
  pdfv_settings_get_window_size(settings, &window_width, &window_height);
  g_assert_cmpint(window_width, ==, 1280);
  g_assert_cmpint(window_height, ==, 840);
  g_assert_cmpint(pdfv_settings_get_image_paste_style(settings), ==,
                  PDFV_IMAGE_PASTE_STYLE_MARKDOWN_LINK);
  g_assert_cmpstr(pdfv_settings_get_latex_snippets(settings), ==,
                  "[{trigger:'x'}]");
  g_assert_cmpstr(pdfv_settings_get_latex_snippet_variables(settings), ==,
                  "{GREEK:'alpha'}");
  gsize tab_count = 0;
  gchar **restored_tabs = pdfv_settings_dup_workspace_open_tabs(
      settings, workspace_a, &tab_count);
  gchar *active_tab = pdfv_settings_dup_workspace_active_tab(
      settings, workspace_a);
  gsize expanded_count = 0;
  gchar **restored_expanded =
      pdfv_settings_dup_workspace_expanded_folders(
          settings, workspace_a, &expanded_count);
  g_assert_cmpuint(tab_count, ==, G_N_ELEMENTS(tabs));
  g_assert_cmpstr(restored_tabs[0], ==, tabs[0]);
  g_assert_cmpstr(restored_tabs[1], ==, tabs[1]);
  g_assert_cmpstr(active_tab, ==, tabs[0]);
  g_assert_cmpuint(expanded_count, ==, G_N_ELEMENTS(expanded));
  g_assert_cmpstr(restored_expanded[0], ==, expanded[0]);
  g_assert_cmpstr(restored_expanded[1], ==, expanded[1]);
  g_assert_null(pdfv_settings_dup_workspace_open_tabs(
      settings, workspace_b, NULL));
  g_assert_null(pdfv_settings_dup_workspace_expanded_folders(
      settings, workspace_b, NULL));
  g_strfreev(restored_expanded);
  g_strfreev(restored_tabs);
  g_free(active_tab);
  g_free(restored);
  pdfv_settings_free(settings);

  g_free(folder_uri);
  g_object_unref(folder);
  g_object_unref(workspace_b);
  g_object_unref(workspace_a);
}

int main(int argc, char **argv) {
  GError *error = NULL;
  config_root = g_dir_make_tmp("phi-settings-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(config_root);
  g_assert_true(g_setenv("XDG_CONFIG_HOME", config_root, TRUE));
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/settings/workspace-attachment-policy",
                  test_workspace_attachment_policy);
  int result = g_test_run();

  gchar *settings_file = g_build_filename(
      config_root, "phi-pdf-viewer", "settings.ini", NULL);
  gchar *settings_directory = g_path_get_dirname(settings_file);
  g_remove(settings_file);
  g_rmdir(settings_directory);
  g_rmdir(config_root);
  g_free(settings_directory);
  g_free(settings_file);
  g_free(config_root);
  return result;
}

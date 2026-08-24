/* Phi window component lifecycle tests.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-document-view.h"
#include "pdfv-document-properties.h"
#include "pdfv-page-selector.h"
#include "pdfv-thumbnail-list.h"

static PhiDocument *open_fixture(void) {
  GFile *file =
      g_file_new_for_path(TEST_DATA_DIR "/separate-diacritic.pdf");
  GError *error = NULL;
  PhiDocument *document = phi_document_new_from_file(file, &error);
  g_assert_no_error(error);
  g_assert_nonnull(document);
  g_object_unref(file);
  return document;
}

static void test_page_selector_tracks_view(void) {
  PhiDocument *document = open_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  pdfv_document_view_set_document(view, document);

  PdfvPageSelector *selector = pdfv_page_selector_new();
  g_object_ref_sink(selector);
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(selector)));

  pdfv_page_selector_set_view(selector, view);
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(selector)));
  GtkWidget *entry = gtk_widget_get_first_child(GTK_WIDGET(selector));
  GtkWidget *count = gtk_widget_get_next_sibling(entry);
  g_assert_true(GTK_IS_ENTRY(entry));
  g_assert_true(GTK_IS_LABEL(count));
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "1");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(count)), ==, "of 1");

  gtk_editable_set_text(GTK_EDITABLE(entry), "999");
  g_signal_emit_by_name(entry, "activate");
  g_assert_cmpint(pdfv_document_view_get_current_page(view), ==, 0);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "1");

  pdfv_page_selector_set_view(selector, NULL);
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(selector)));
  g_object_unref(selector);
  /* The selector must have disconnected before the surviving view emits. */
  pdfv_document_view_set_document(view, NULL);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_thumbnail_list_accepts_document_changes(void) {
  PhiDocument *document = open_fixture();
  PdfvThumbnailList *list = pdfv_thumbnail_list_new();
  g_object_ref_sink(list);
  g_assert_cmpuint(g_signal_lookup("page-activated",
                                   PDFV_TYPE_THUMBNAIL_LIST), !=, 0);
  pdfv_thumbnail_list_set_document(list, document);
  pdfv_thumbnail_list_set_document(list, document);
  /* Disposing with a live model exercises the component-owned model release. */
  g_object_unref(list);
  g_object_unref(document);
}

static void test_document_property_formatting(void) {
  gchar *a4 = pdfv_document_properties_format_page_size(595.28f, 841.89f);
  g_assert_nonnull(g_strstr_len(a4, -1, "A4"));
  g_assert_nonnull(g_strstr_len(a4, -1, "portrait"));
  g_free(a4);

  gchar *letter_landscape =
      pdfv_document_properties_format_page_size(792.0f, 612.0f);
  g_assert_nonnull(g_strstr_len(letter_landscape, -1, "Letter"));
  g_assert_nonnull(g_strstr_len(letter_landscape, -1, "landscape"));
  g_free(letter_landscape);

  gchar *date = pdfv_document_properties_format_pdf_date(
      "D:20240506070809+02'00'");
  g_assert_nonnull(date);
  g_assert_cmpstr(date, !=, "D:20240506070809+02'00'");
  g_free(date);
}

static void test_document_properties_dialog_lifecycle(void) {
  GFile *file =
      g_file_new_for_path(TEST_DATA_DIR "/separate-diacritic.pdf");
  PhiDocument *document = open_fixture();
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  g_object_ref_sink(window);
  gtk_window_present(window);

  pdfv_document_properties_present(GTK_WIDGET(window), file, document, 0);
  while (g_main_context_iteration(NULL, FALSE))
    ;

  gtk_window_destroy(window);
  g_object_unref(window);
  g_object_unref(document);
  g_object_unref(file);
}

int main(int argc, char **argv) {
  gtk_test_init(&argc, &argv, NULL);
  g_test_add_func("/window-components/page-selector",
                  test_page_selector_tracks_view);
  g_test_add_func("/window-components/thumbnail-list",
                  test_thumbnail_list_accepts_document_changes);
  g_test_add_func("/window-components/document-property-formatting",
                  test_document_property_formatting);
  g_test_add_func("/window-components/document-properties-dialog",
                  test_document_properties_dialog_lifecycle);
  return g_test_run();
}

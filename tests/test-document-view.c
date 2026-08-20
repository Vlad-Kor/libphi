/*
 * Phi PDF Viewer - document view interaction tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 */

#include "pdfv-document-view.h"

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

static void test_internal_link_history(void) {
  PhiDocument *document = open_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  pdfv_document_view_set_document(view, document);

  PhiLinkDest destination;
  g_assert_true(phi_document_resolve_link(document, "#page=1",
                                          &destination));
  gtk_adjustment_set_value(vertical, 250);
  pdfv_document_view_activate_link(view, "#page=1");

  g_assert_true(pdfv_document_view_can_go_back(view));
  g_assert_false(pdfv_document_view_can_go_forward(view));
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0, 0.01);

  /* History uses a location within the page, so it remains accurate if the
   * zoom changes after following the link. */
  pdfv_document_view_set_zoom(view, 2.0);
  pdfv_document_view_go_back(view);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 500,
                                 0.01);
  g_assert_false(pdfv_document_view_can_go_back(view));
  g_assert_true(pdfv_document_view_can_go_forward(view));

  pdfv_document_view_go_forward(view);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0, 0.01);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

int main(int argc, char **argv) {
  gtk_test_init(&argc, &argv, NULL);
  g_test_add_func("/document-view/internal-link-history",
                  test_internal_link_history);
  return g_test_run();
}

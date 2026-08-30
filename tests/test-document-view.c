/*
 * Phi PDF Viewer - document view interaction tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 */

#include "pdfv-document-view.h"
#include <cairo-pdf.h>
#include <glib/gstdio.h>
#include <unistd.h>

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

static PhiDocument *open_two_page_fixture(void) {
  GError *error = NULL;
  gchar *path = NULL;
  gint fd = g_file_open_tmp("phi-document-view-XXXXXX.pdf", &path, &error);
  g_assert_no_error(error);
  g_assert_cmpint(fd, >=, 0);
  close(fd);

  cairo_surface_t *surface = cairo_pdf_surface_create(path, 300, 800);
  cairo_t *cr = cairo_create(surface);
  cairo_show_page(cr);
  cairo_pdf_surface_set_size(surface, 600, 800);
  cairo_show_page(cr);
  cairo_destroy(cr);
  cairo_surface_finish(surface);
  g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(surface);

  GFile *file = g_file_new_for_path(path);
  PhiDocument *document = phi_document_new_from_file(file, &error);
  g_assert_no_error(error);
  g_assert_nonnull(document);
  g_assert_cmpint(phi_document_get_n_pages(document), ==, 2);
  g_object_unref(file);
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_free(path);
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

static void test_presentation_zoom_floor(void) {
  PhiDocument *document = open_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  pdfv_document_view_set_document(view, document);

  g_assert_false(pdfv_document_view_get_presentation_mode(view));
  pdfv_document_view_set_presentation_mode(view, TRUE);
  g_assert_true(pdfv_document_view_get_presentation_mode(view));

  g_assert_cmpfloat_with_epsilon(
      pdfv_document_view_get_minimum_zoom(view), 0.1, 0.001);
  pdfv_document_view_set_minimum_zoom(view, 1.5);
  g_assert_cmpfloat_with_epsilon(pdfv_document_view_get_zoom(view), 1.5,
                                 0.001);
  pdfv_document_view_zoom_out(view);
  g_assert_cmpfloat_with_epsilon(pdfv_document_view_get_zoom(view), 1.5,
                                 0.001);
  gtk_adjustment_set_value(horizontal, 100);
  gtk_adjustment_set_value(vertical, 100);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(horizontal), 0,
                                 0.001);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0,
                                 0.001);

  pdfv_document_view_set_minimum_zoom(view, 0.1);
  pdfv_document_view_zoom_out(view);
  g_assert_cmpfloat(pdfv_document_view_get_zoom(view), <, 1.5);

  pdfv_document_view_set_presentation_mode(view, FALSE);
  g_assert_false(pdfv_document_view_get_presentation_mode(view));

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_scroll_state_survives_zoom(void) {
  PhiDocument *document = open_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  pdfv_document_view_set_document(view, document);

  gtk_adjustment_set_value(vertical, 250);
  gint page = -1;
  gdouble fraction = -1;
  gdouble center = -1;
  pdfv_document_view_get_scroll_state(view, &page, &fraction, &center);
  g_assert_cmpint(page, ==, 0);
  g_assert_cmpfloat(fraction, >, 0);

  pdfv_document_view_set_zoom(view, 2.0);
  pdfv_document_view_go_to_page(view, 0);
  pdfv_document_view_restore_scroll_state(view, page, fraction, center);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 500,
                                 0.01);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_fit_width_after_restoring_later_page(void) {
  PhiDocument *document = open_two_page_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  pdfv_document_view_set_document(view, document);
  gtk_widget_allocate(GTK_WIDGET(view), 1000, 700, -1, NULL);

  /* Only page zero enters the view's page cache during initial layout. A
   * restored position moves to page two before the initial fit-width pass. */
  pdfv_document_view_go_to_page(view, 1);
  g_assert_cmpfloat_with_epsilon(pdfv_document_view_get_zoom(view), 1.0,
                                 0.001);
  pdfv_document_view_zoom_fit_width(view);
  g_assert_cmpfloat_with_epsilon(pdfv_document_view_get_zoom(view), 1.6,
                                 0.001);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_page_shadow_margins(void) {
  PhiDocument *document = open_two_page_fixture();
  PdfvDocumentView *view = pdfv_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  pdfv_document_view_set_document(view, document);

  gtk_widget_allocate(GTK_WIDGET(view), 1000, 700, -1, NULL);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_upper(vertical), 1622,
                                 0.01);
  gtk_adjustment_set_value(vertical, G_MAXDOUBLE);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 922,
                                 0.01);

  /* A fitting document must not gain a tiny, otherwise useless scrollbar. */
  gtk_widget_allocate(GTK_WIDGET(view), 1000, 2000, -1, NULL);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_upper(vertical), 2000,
                                 0.01);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0,
                                 0.01);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

int main(int argc, char **argv) {
  gtk_test_init(&argc, &argv, NULL);
  g_test_add_func("/document-view/internal-link-history",
                  test_internal_link_history);
  g_test_add_func("/document-view/presentation-zoom-floor",
                  test_presentation_zoom_floor);
  g_test_add_func("/document-view/scroll-state-survives-zoom",
                  test_scroll_state_survives_zoom);
  g_test_add_func("/document-view/fit-width-after-restoring-later-page",
                  test_fit_width_after_restoring_later_page);
  g_test_add_func("/document-view/page-shadow-margins",
                  test_page_shadow_margins);
  return g_test_run();
}

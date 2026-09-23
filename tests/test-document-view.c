/*
 * Phi PDF Viewer - document view interaction tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 */

#include <phi/phidocumentview.h>
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
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);

  PhiLinkDest destination;
  g_assert_true(phi_document_resolve_link(document, "#page=1",
                                          &destination));
  gtk_adjustment_set_value(vertical, 250);
  phi_document_view_activate_link(view, "#page=1");

  g_assert_true(phi_document_view_can_go_back(view));
  g_assert_false(phi_document_view_can_go_forward(view));
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0, 0.01);

  /* History uses a location within the page, so it remains accurate if the
   * zoom changes after following the link. */
  phi_document_view_set_zoom(view, 2.0);
  phi_document_view_go_back(view);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 500,
                                 0.01);
  g_assert_false(phi_document_view_can_go_back(view));
  g_assert_true(phi_document_view_can_go_forward(view));

  phi_document_view_go_forward(view);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0, 0.01);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_presentation_zoom_floor(void) {
  PhiDocument *document = open_fixture();
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);

  g_assert_false(phi_document_view_get_presentation_mode(view));
  phi_document_view_set_presentation_mode(view, TRUE);
  g_assert_true(phi_document_view_get_presentation_mode(view));

  g_assert_cmpfloat_with_epsilon(
      phi_document_view_get_minimum_zoom(view), 0.1, 0.001);
  phi_document_view_set_minimum_zoom(view, 1.5);
  g_assert_cmpfloat_with_epsilon(phi_document_view_get_zoom(view), 1.5,
                                 0.001);
  phi_document_view_zoom_out(view);
  g_assert_cmpfloat_with_epsilon(phi_document_view_get_zoom(view), 1.5,
                                 0.001);
  gtk_adjustment_set_value(horizontal, 100);
  gtk_adjustment_set_value(vertical, 100);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(horizontal), 0,
                                 0.001);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 0,
                                 0.001);

  phi_document_view_set_minimum_zoom(view, 0.1);
  phi_document_view_zoom_out(view);
  g_assert_cmpfloat(phi_document_view_get_zoom(view), <, 1.5);

  phi_document_view_set_presentation_mode(view, FALSE);
  g_assert_false(phi_document_view_get_presentation_mode(view));

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_scroll_state_survives_zoom(void) {
  PhiDocument *document = open_fixture();
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);

  gtk_adjustment_set_value(vertical, 250);
  gint page = -1;
  gdouble fraction = -1;
  gdouble center = -1;
  phi_document_view_get_scroll_state(view, &page, &fraction, &center);
  g_assert_cmpint(page, ==, 0);
  g_assert_cmpfloat(fraction, >, 0);

  phi_document_view_set_zoom(view, 2.0);
  phi_document_view_go_to_page(view, 0);
  phi_document_view_restore_scroll_state(view, page, fraction, center);
  g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(vertical), 500,
                                 0.01);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_fit_width_after_restoring_later_page(void) {
  PhiDocument *document = open_two_page_fixture();
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);
  gtk_widget_allocate(GTK_WIDGET(view), 1000, 700, -1, NULL);

  /* Only page zero enters the view's page cache during initial layout. A
   * restored position moves to page two before the initial fit-width pass. */
  phi_document_view_go_to_page(view, 1);
  g_assert_cmpfloat_with_epsilon(phi_document_view_get_zoom(view), 1.0,
                                 0.001);
  phi_document_view_zoom_fit_width(view);
  g_assert_cmpfloat_with_epsilon(phi_document_view_get_zoom(view), 1.6,
                                 0.001);

  g_object_unref(horizontal);
  g_object_unref(vertical);
  g_object_unref(view);
  g_object_unref(document);
}

static void test_page_shadow_margins(void) {
  PhiDocument *document = open_two_page_fixture();
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);

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

static cairo_status_t append_to_byte_array(void *closure,
                                           const unsigned char *data,
                                           unsigned int length) {
  g_byte_array_append(closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

/* A two-page PDF generated in memory, like a Typst export. It contains only
 * vector content, so any texture in the view's output is a worker raster. */
static GBytes *generate_pdf_bytes(void) {
  GByteArray *array = g_byte_array_new();
  cairo_surface_t *surface =
      cairo_pdf_surface_create_for_stream(append_to_byte_array, array, 300,
                                          400);
  cairo_t *cr = cairo_create(surface);
  for (gint page = 0; page < 2; page++) {
    cairo_rectangle(cr, 50, 50, 100, 100);
    cairo_fill(cr);
    cairo_show_page(cr);
  }
  cairo_destroy(cr);
  cairo_surface_finish(surface);
  g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(surface);
  return g_byte_array_free_to_bytes(array);
}

static guint count_texture_nodes(GskRenderNode *node) {
  if (!node)
    return 0;
  switch (gsk_render_node_get_node_type(node)) {
  case GSK_TEXTURE_NODE:
  case GSK_TEXTURE_SCALE_NODE:
    return 1;
  case GSK_CONTAINER_NODE: {
    guint count = 0;
    for (guint i = 0; i < gsk_container_node_get_n_children(node); i++)
      count += count_texture_nodes(gsk_container_node_get_child(node, i));
    return count;
  }
  case GSK_TRANSFORM_NODE:
    return count_texture_nodes(gsk_transform_node_get_child(node));
  case GSK_CLIP_NODE:
    return count_texture_nodes(gsk_clip_node_get_child(node));
  case GSK_ROUNDED_CLIP_NODE:
    return count_texture_nodes(gsk_rounded_clip_node_get_child(node));
  case GSK_OPACITY_NODE:
    return count_texture_nodes(gsk_opacity_node_get_child(node));
  case GSK_COLOR_MATRIX_NODE:
    return count_texture_nodes(gsk_color_matrix_node_get_child(node));
  case GSK_DEBUG_NODE:
    return count_texture_nodes(gsk_debug_node_get_child(node));
  default:
    return 0;
  }
}

static guint snapshot_texture_count(PhiDocumentView *view) {
  GtkSnapshot *snapshot = gtk_snapshot_new();
  GTK_WIDGET_GET_CLASS(view)->snapshot(GTK_WIDGET(view), snapshot);
  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
  guint count = count_texture_nodes(node);
  g_clear_pointer(&node, gsk_render_node_unref);
  return count;
}

/* Draw the view repeatedly, letting worker renders complete in between,
 * until it shows raster textures or the timeout expires. */
static guint wait_for_texture_nodes(PhiDocumentView *view,
                                    gint64 timeout_us) {
  gint64 deadline = g_get_monotonic_time() + timeout_us;
  guint count = snapshot_texture_count(view);
  while (count == 0 && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
    count = snapshot_texture_count(view);
  }
  return count;
}

static PhiDocumentView *new_allocated_view(PhiDocument *document) {
  PhiDocumentView *view = phi_document_view_new();
  g_object_ref_sink(view);
  GtkAdjustment *horizontal = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  GtkAdjustment *vertical = gtk_adjustment_new(0, 0, 0, 1, 10, 0);
  g_object_set(view, "hadjustment", horizontal, "vadjustment", vertical,
               NULL);
  phi_document_view_set_document(view, document);
  gtk_widget_allocate(GTK_WIDGET(view), 800, 600, -1, NULL);
  return view;
}

static void test_bytes_document_uses_raster_renderer(void) {
  GBytes *bytes = generate_pdf_bytes();
  GError *error = NULL;
  PhiDocument *document = phi_document_new_from_bytes(bytes, NULL, &error);
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  PhiDocumentView *view = new_allocated_view(document);
  g_assert_true(phi_document_view_get_document(view) == document);
  g_assert_cmpuint(wait_for_texture_nodes(view, 5 * G_USEC_PER_SEC), >, 0);

  /* Zooming re-rasterizes at the new scale through the same path. */
  phi_document_view_set_zoom(view, 3.0);
  g_assert_cmpuint(wait_for_texture_nodes(view, 5 * G_USEC_PER_SEC), >, 0);

  phi_document_view_set_document(view, NULL);
  g_object_unref(view);
  g_object_unref(document);
}

/* Control for the test above: a stream-only document cannot be reopened by
 * the worker and is drawn with vector fallback nodes instead of textures. */
static void test_stream_document_uses_fallback(void) {
  GBytes *bytes = generate_pdf_bytes();
  GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
  GError *error = NULL;
  PhiDocument *document =
      phi_document_new_from_stream(stream, "application/pdf", &error);
  g_assert_no_error(error);

  PhiDocumentView *view = new_allocated_view(document);
  g_assert_cmpuint(wait_for_texture_nodes(view, G_USEC_PER_SEC / 2), ==, 0);

  phi_document_view_set_document(view, NULL);
  g_object_unref(view);
  g_object_unref(document);
  g_object_unref(stream);
  g_bytes_unref(bytes);
}

static PhiDocument *open_text_fixture(gint n_pages) {
  GError *error = NULL;
  gchar *path = NULL;
  gint fd = g_file_open_tmp("phi-document-view-XXXXXX.pdf", &path, &error);
  g_assert_no_error(error);
  close(fd);

  cairo_surface_t *surface = cairo_pdf_surface_create(path, 300, 400);
  cairo_t *cr = cairo_create(surface);
  cairo_set_font_size(cr, 12);
  for (gint i = 0; i < n_pages; i++) {
    for (gint line = 0; line < 20; line++) {
      cairo_move_to(cr, 20, 20 + line * 16);
      cairo_show_text(cr, "needle in a haystack");
    }
    cairo_show_page(cr);
  }
  cairo_destroy(cr);
  cairo_surface_finish(surface);
  cairo_surface_destroy(surface);

  GFile *file = g_file_new_for_path(path);
  PhiDocument *document = phi_document_new_from_file(file, &error);
  g_assert_no_error(error);
  g_object_unref(file);
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_free(path);
  return document;
}

static void on_search_completed(PhiDocumentView *view, gint n_matches,
                                gint *completed) {
  (void)view;
  (void)n_matches;
  (*completed)++;
}

static void spin_main_context(gint64 duration_us) {
  gint64 deadline = g_get_monotonic_time() + duration_us;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
}

static void test_search_completes(void) {
  PhiDocument *document = open_text_fixture(3);
  PhiDocumentView *view = new_allocated_view(document);
  gint completed = 0;
  g_signal_connect(view, "search-completed",
                   G_CALLBACK(on_search_completed), &completed);

  phi_document_view_search(view, "needle");
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (completed == 0 && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpint(completed, ==, 1);
  g_assert_cmpint(phi_document_view_get_search_match_count(view), ==, 60);

  phi_document_view_set_document(view, NULL);
  g_object_unref(view);
  g_object_unref(document);
}

/* A running page scan referenced the old page array after the document was
 * replaced, and the view itself after it was destroyed. */
static void test_search_cancelled_with_document(void) {
  PhiDocument *document = open_text_fixture(300);
  PhiDocument *replacement = open_text_fixture(1);
  PhiDocumentView *view = new_allocated_view(document);
  gint completed = 0;
  g_signal_connect(view, "search-completed",
                   G_CALLBACK(on_search_completed), &completed);

  /* Past the 250 ms debounce, into the incremental scan. */
  phi_document_view_search(view, "needle");
  spin_main_context(300 * 1000);
  phi_document_view_set_document(view, replacement);
  spin_main_context(300 * 1000);
  g_assert_cmpint(completed, ==, 0);

  phi_document_view_search(view, "needle");
  spin_main_context(260 * 1000);
  g_object_unref(view);
  spin_main_context(100 * 1000);

  g_object_unref(replacement);
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
  g_test_add_func("/document-view/bytes-document-uses-raster-renderer",
                  test_bytes_document_uses_raster_renderer);
  g_test_add_func("/document-view/stream-document-uses-fallback",
                  test_stream_document_uses_fallback);
  g_test_add_func("/document-view/search-completes", test_search_completes);
  g_test_add_func("/document-view/search-cancelled-with-document",
                  test_search_cancelled_with_document);
  return g_test_run();
}

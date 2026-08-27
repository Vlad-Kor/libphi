/* Phi bounded document-position history tests.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "document-history.h"

#include <glib/gstdio.h>

static void test_disabled_and_persistent_history(void) {
  GError *error = NULL;
  gchar *directory = g_dir_make_tmp("phi-document-history-XXXXXX", &error);
  g_assert_no_error(error);
  gchar *path = g_build_filename(directory, "positions", NULL);
  GFile *storage = g_file_new_for_path(path);
  GFile *pdf = g_file_new_for_path("/tmp/phi-history.pdf");
  GFile *markdown = g_file_new_for_path("/tmp/phi-history.md");
  PdfvDocumentPosition pdf_position = {
      .kind = PDFV_DOCUMENT_POSITION_PDF,
      .anchor = 17,
      .offset = 0.375,
      .horizontal = 22.5,
  };

  PdfvDocumentHistory *history = pdfv_document_history_new(storage);
  pdfv_document_history_remember(history, pdf, &pdf_position);
  g_assert_true(pdfv_document_history_flush(history, &error));
  g_assert_no_error(error);
  g_assert_false(g_file_query_exists(storage, NULL));

  pdfv_document_history_set_enabled(history, TRUE);
  pdfv_document_history_remember(history, pdf, &pdf_position);
  PdfvDocumentPosition markdown_position = {
      .kind = PDFV_DOCUMENT_POSITION_MARKDOWN,
      .anchor = 812,
      .offset = 6.5,
      .horizontal = 1400,
  };
  pdfv_document_history_remember(history, markdown, &markdown_position);
  g_assert_true(pdfv_document_history_flush(history, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_query_exists(storage, NULL));
  g_object_unref(history);

  history = pdfv_document_history_new(storage);
  pdfv_document_history_set_enabled(history, TRUE);
  PdfvDocumentPosition restored = {0};
  g_assert_true(pdfv_document_history_lookup(
      history, pdf, PDFV_DOCUMENT_POSITION_PDF, &restored));
  g_assert_cmpint(restored.anchor, ==, 17);
  g_assert_cmpfloat(restored.offset, ==, 0.375);
  g_assert_cmpfloat(restored.horizontal, ==, 22.5);
  g_assert_false(pdfv_document_history_lookup(
      history, pdf, PDFV_DOCUMENT_POSITION_MARKDOWN, NULL));
  g_assert_true(pdfv_document_history_lookup(
      history, markdown, PDFV_DOCUMENT_POSITION_MARKDOWN, &restored));
  g_assert_cmpint(restored.anchor, ==, 812);

  /* Async Markdown closes may complete out of order. Request order, rather
   * than completion timing, decides which duplicate tab wins. */
  guint64 older = pdfv_document_history_reserve_update(history);
  guint64 newer = pdfv_document_history_reserve_update(history);
  PdfvDocumentPosition older_position = pdf_position;
  PdfvDocumentPosition newer_position = pdf_position;
  older_position.anchor = 3;
  newer_position.anchor = 29;
  pdfv_document_history_remember_ordered(
      history, pdf, &newer_position, newer);
  pdfv_document_history_remember_ordered(
      history, pdf, &older_position, older);
  g_assert_true(pdfv_document_history_lookup(
      history, pdf, PDFV_DOCUMENT_POSITION_PDF, &restored));
  g_assert_cmpint(restored.anchor, ==, 29);

  /* The global history remains bounded even for very large libraries. */
  for (guint i = 0; i < 4100; i++) {
    gchar *uri = g_strdup_printf("file:///tmp/phi-history-%u.pdf", i);
    GFile *many = g_file_new_for_uri(uri);
    pdfv_document_history_remember(history, many, &pdf_position);
    g_object_unref(many);
    g_free(uri);
  }
  GFile *oldest = g_file_new_for_uri("file:///tmp/phi-history-0.pdf");
  GFile *newest = g_file_new_for_uri("file:///tmp/phi-history-4099.pdf");
  g_assert_false(pdfv_document_history_lookup(
      history, oldest, PDFV_DOCUMENT_POSITION_PDF, NULL));
  g_assert_true(pdfv_document_history_lookup(
      history, newest, PDFV_DOCUMENT_POSITION_PDF, NULL));
  g_object_unref(newest);
  g_object_unref(oldest);

  /* Turning the opt-in feature off removes both memory and disk history. */
  pdfv_document_history_set_enabled(history, FALSE);
  g_assert_false(g_file_query_exists(storage, NULL));
  pdfv_document_history_set_enabled(history, TRUE);
  g_assert_false(pdfv_document_history_lookup(
      history, pdf, PDFV_DOCUMENT_POSITION_PDF, NULL));

  g_object_unref(history);
  const gchar *malformed =
      "# Phi document positions v1\n"
      "1\tm\t999999\t-4\tinf\tfile:///tmp/phi-history.md\n"
      "2\tp\t9223372036854775807\t0.5\t0\t"
      "file:///tmp/phi-history.pdf\n";
  g_assert_true(g_file_set_contents(path, malformed, -1, &error));
  g_assert_no_error(error);
  history = pdfv_document_history_new(storage);
  pdfv_document_history_set_enabled(history, TRUE);
  g_assert_false(pdfv_document_history_lookup(
      history, markdown, PDFV_DOCUMENT_POSITION_MARKDOWN, NULL));
  g_assert_false(pdfv_document_history_lookup(
      history, pdf, PDFV_DOCUMENT_POSITION_PDF, NULL));
  g_object_unref(history);

  g_object_unref(markdown);
  g_object_unref(pdf);
  g_object_unref(storage);
  g_remove(path);
  g_rmdir(directory);
  g_free(path);
  g_free(directory);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/document-history/disabled-and-persistent",
                  test_disabled_and_persistent_history);
  return g_test_run();
}

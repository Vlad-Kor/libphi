/*
 * Phi Markdown vault boundary tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "markdown-vault-adapter.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  gchar *path;
  GFile *root;
  GFile *overview;
  GFile *nested;
  GFile *relative_assets;
  GFile *assets;
  PdfvMarkdownVaultAdapter *vault;
} VaultFixture;

static void vault_fixture_setup(VaultFixture *fixture, gconstpointer data) {
  (void)data;
  GError *error = NULL;
  fixture->path = g_dir_make_tmp("phi-markdown-vault-XXXXXX", &error);
  g_assert_no_error(error);
  fixture->root = g_file_new_for_path(fixture->path);

  gchar *source = NULL;
  gsize source_length = 0;
  g_assert_true(g_file_get_contents(
      TEST_MARKDOWN_FIXTURE_DIR "/obsidian-complete.md", &source,
      &source_length, &error));
  g_assert_no_error(error);
  fixture->overview = g_file_get_child(fixture->root, "Overview.md");
  gchar *overview_path = g_file_get_path(fixture->overview);
  g_assert_true(g_file_set_contents(overview_path, source, source_length,
                                    &error));
  g_assert_no_error(error);
  g_free(overview_path);
  g_free(source);

  fixture->nested = g_file_get_child(fixture->root, "Nested");
  g_assert_true(g_file_make_directory(fixture->nested, NULL, &error));
  g_assert_no_error(error);
  GFile *other = g_file_get_child(fixture->nested, "Other.md");
  gchar *other_path = g_file_get_path(other);
  g_assert_true(g_file_set_contents(other_path,
                                    "# Other heading\n\nA block. ^other-block\n",
                                    -1, &error));
  g_assert_no_error(error);
  g_free(other_path);
  g_object_unref(other);

  fixture->relative_assets = g_file_get_child(fixture->nested, "Images");
  g_assert_true(g_file_make_directory(fixture->relative_assets, NULL,
                                      &error));
  g_assert_no_error(error);
  GFile *relative_image = g_file_get_child(
      fixture->relative_assets, "Relative image.png");
  gchar *relative_image_path = g_file_get_path(relative_image);
  g_assert_true(g_file_set_contents(relative_image_path, "relative-image",
                                    -1, &error));
  g_assert_no_error(error);
  g_free(relative_image_path);
  g_object_unref(relative_image);

  fixture->assets = g_file_get_child(fixture->root, "~Images");
  g_assert_true(g_file_make_directory(fixture->assets, NULL, &error));
  g_assert_no_error(error);
  GFile *image = g_file_get_child(fixture->assets, "Diagram.png");
  gchar *image_path = g_file_get_path(image);
  g_assert_true(g_file_set_contents(image_path, "not-a-real-png", -1,
                                    &error));
  g_assert_no_error(error);
  g_free(image_path);
  g_object_unref(image);

  fixture->vault = pdfv_markdown_vault_adapter_new(fixture->root);
}

static void vault_fixture_teardown(VaultFixture *fixture,
                                   gconstpointer data) {
  (void)data;
  GError *error = NULL;
  GFile *other = g_file_get_child(fixture->nested, "Other.md");
  GFile *image = g_file_get_child(fixture->assets, "Diagram.png");
  GFile *relative_image = g_file_get_child(
      fixture->relative_assets, "Relative image.png");
  g_assert_true(g_file_delete(image, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(fixture->assets, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(other, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(relative_image, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(fixture->relative_assets, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(fixture->overview, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(fixture->nested, NULL, &error));
  g_assert_no_error(error);
  g_assert_true(g_file_delete(fixture->root, NULL, &error));
  g_assert_no_error(error);
  g_object_unref(other);
  g_object_unref(image);
  g_object_unref(relative_image);
  g_object_unref(fixture->vault);
  g_object_unref(fixture->nested);
  g_object_unref(fixture->relative_assets);
  g_object_unref(fixture->assets);
  g_object_unref(fixture->overview);
  g_object_unref(fixture->root);
  g_free(fixture->path);
}

static void test_safe_resolution(VaultFixture *fixture, gconstpointer data) {
  (void)data;
  const gchar *unsafe[] = {"../secret.md", "%2e%2e/secret.md",
                           "/etc/passwd", "Nested\\Other.md", "./note.md"};
  for (guint i = 0; i < G_N_ELEMENTS(unsafe); i++) {
    GError *error = NULL;
    GFile *file = pdfv_markdown_vault_adapter_resolve(
        fixture->vault, unsafe[i], &error);
    g_assert_null(file);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
  }

  GError *error = NULL;
  GFile *other = pdfv_markdown_vault_adapter_resolve(
      fixture->vault, "Nested/Other.md", &error);
  g_assert_no_error(error);
  g_assert_nonnull(other);
  gchar *relative = pdfv_markdown_vault_adapter_relative_path(
      fixture->vault, other);
  g_assert_cmpstr(relative, ==, "Nested/Other.md");
  g_free(relative);
  g_object_unref(other);

  gchar *link_path = g_build_filename(fixture->path, "Linked.md", NULL);
  gchar *overview_path = g_file_get_path(fixture->overview);
  g_assert_cmpint(symlink(overview_path, link_path), ==, 0);
  GFile *linked = pdfv_markdown_vault_adapter_resolve(
      fixture->vault, "Linked.md", &error);
  g_assert_null(linked);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  g_assert_cmpint(g_unlink(link_path), ==, 0);
  g_free(overview_path);
  g_free(link_path);

  gchar *linked_dir = g_build_filename(fixture->path, "Linked", NULL);
  gchar *nested_path = g_file_get_path(fixture->nested);
  g_assert_cmpint(symlink(nested_path, linked_dir), ==, 0);
  GFile *new_note = pdfv_markdown_vault_adapter_resolve_new_note(
      fixture->vault, "Overview.md", "Linked/New note", &error);
  g_assert_null(new_note);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error(&error);
  g_assert_cmpint(g_unlink(linked_dir), ==, 0);
  g_free(nested_path);
  g_free(linked_dir);
}

static void test_note_metadata(VaultFixture *fixture, gconstpointer data) {
  (void)data;
  GError *error = NULL;
  GPtrArray *notes = pdfv_markdown_vault_adapter_list_notes(
      fixture->vault, "ovw", &error);
  g_assert_no_error(error);
  g_assert_cmpuint(notes->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(notes, 0), ==, "Overview.md");
  g_ptr_array_unref(notes);

  GPtrArray *headings = pdfv_markdown_vault_adapter_get_headings(
      fixture->vault, fixture->overview, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(headings->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(headings, 0), ==, "Heading one");
  g_ptr_array_unref(headings);

  GPtrArray *blocks = pdfv_markdown_vault_adapter_get_blocks(
      fixture->vault, fixture->overview, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(blocks->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(blocks, 0), ==, "fixture-block");
  g_ptr_array_unref(blocks);

  GFile *resolved = pdfv_markdown_vault_adapter_resolve_note(
      fixture->vault, "Overview.md", "Nested/Other#Other heading", &error);
  g_assert_no_error(error);
  g_assert_nonnull(resolved);
  gchar *relative = pdfv_markdown_vault_adapter_relative_path(
      fixture->vault, resolved);
  g_assert_cmpstr(relative, ==, "Nested/Other.md");
  g_free(relative);
  g_object_unref(resolved);

  GFile *new_note = pdfv_markdown_vault_adapter_resolve_new_note(
      fixture->vault, "Nested/Other.md", "New lecture#Section", &error);
  g_assert_no_error(error);
  g_assert_nonnull(new_note);
  relative = pdfv_markdown_vault_adapter_relative_path(fixture->vault,
                                                        new_note);
  g_assert_cmpstr(relative, ==, "Nested/New lecture.md");
  g_free(relative);
  g_object_unref(new_note);

  GFile *image = pdfv_markdown_vault_adapter_resolve_attachment(
      fixture->vault, "Nested/Other.md", "Diagram.png", FALSE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(image);
  relative = pdfv_markdown_vault_adapter_relative_path(fixture->vault, image);
  g_assert_cmpstr(relative, ==, "~Images/Diagram.png");
  g_free(relative);
  g_object_unref(image);

  image = pdfv_markdown_vault_adapter_resolve_attachment(
      fixture->vault, "Nested/Other.md", "../~Images/Diagram.png", TRUE,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(image);
  g_object_unref(image);

  image = pdfv_markdown_vault_adapter_resolve_attachment(
      fixture->vault, "Nested/Other.md", "Images/Relative image.png", TRUE,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(image);
  relative = pdfv_markdown_vault_adapter_relative_path(fixture->vault, image);
  g_assert_cmpstr(relative, ==, "Nested/Images/Relative image.png");
  g_free(relative);
  g_object_unref(image);

  /* Opening a note without a workspace roots the adapter beside that note.
   * The same relative request must still resolve its sibling image folder. */
  PdfvMarkdownVaultAdapter *standalone =
      pdfv_markdown_vault_adapter_new(fixture->nested);
  image = pdfv_markdown_vault_adapter_resolve_attachment(
      standalone, "Other.md", "Images/Relative image.png", TRUE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(image);
  g_object_unref(image);
  g_object_unref(standalone);
}

static void test_byte_preserving_read(VaultFixture *fixture,
                                      gconstpointer data) {
  (void)data;
  GError *error = NULL;
  gchar *expected = NULL;
  gsize expected_length = 0;
  g_assert_true(g_file_get_contents(
      TEST_MARKDOWN_FIXTURE_DIR "/obsidian-complete.md", &expected,
      &expected_length, &error));
  g_assert_no_error(error);
  gchar *actual = pdfv_markdown_vault_adapter_read_text(
      fixture->vault, fixture->overview, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpmem(actual, strlen(actual), expected, expected_length);
  g_free(actual);
  g_free(expected);

  GFile *binary = g_file_get_child(fixture->root, "Binary.md");
  gchar *binary_path = g_file_get_path(binary);
  const gchar bytes[] = {'a', '\0', 'b'};
  g_assert_true(g_file_set_contents(binary_path, bytes, sizeof(bytes),
                                    &error));
  g_assert_no_error(error);
  actual = pdfv_markdown_vault_adapter_read_text(fixture->vault, binary,
                                                  NULL, &error);
  g_assert_null(actual);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_assert_true(g_file_delete(binary, NULL, &error));
  g_assert_no_error(error);
  g_free(binary_path);
  g_object_unref(binary);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add("/markdown-vault/safe-resolution", VaultFixture, NULL,
             vault_fixture_setup, test_safe_resolution,
             vault_fixture_teardown);
  g_test_add("/markdown-vault/note-metadata", VaultFixture, NULL,
             vault_fixture_setup, test_note_metadata,
             vault_fixture_teardown);
  g_test_add("/markdown-vault/byte-preserving-read", VaultFixture, NULL,
             vault_fixture_setup, test_byte_preserving_read,
             vault_fixture_teardown);
  return g_test_run();
}

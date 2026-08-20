/*
 * libphi - High performance document renderer for GTK
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <phi/phidocument.h>
#include <phi/phipage.h>

static PhiDocument* open_fixture(void) {
	g_autoptr(GFile) file = g_file_new_for_path(TEST_DATA_DIR "/separate-diacritic.pdf");
	g_autoptr(GError) error = NULL;
	PhiDocument* document = phi_document_new_from_file(file, &error);
	g_assert_no_error(error);
	g_assert_nonnull(document);
	return document;
}

static void test_separate_diacritic_text(void) {
	g_autoptr(PhiDocument) document = open_fixture();
	g_assert_cmpuint(g_list_model_get_item_type(G_LIST_MODEL(document)), ==,
		PHI_TYPE_PAGE);
	g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(document)), ==, 1);
	PhiPage* page = phi_document_get_page(document, 0, NULL);
	g_assert_nonnull(page);

	g_autofree gchar* text = phi_page_get_text(page);
	g_assert_nonnull(text);
	g_assert_nonnull(g_strstr_len(text, -1, "präsentiert"));
	g_assert_nonnull(g_strstr_len(text, -1, "literal ¨a"));

	graphene_point_t start = GRAPHENE_POINT_INIT(70, 100);
	graphene_point_t end = GRAPHENE_POINT_INIT(220, 100);
	g_autofree gchar* selection = phi_page_copy_selection(page, &start, &end);
	g_assert_nonnull(selection);
	g_assert_nonnull(g_strstr_len(selection, -1, "präsentiert"));

	PhiTextQuad quads[8];
	g_assert_cmpint(phi_page_search_text(page, "präsentiert", quads,
		G_N_ELEMENTS(quads)), >, 0);
}

typedef struct {
	PhiDocument* document;
	cairo_surface_t* surface;
	GError* error;
} ThumbnailResult;

static gpointer render_thumbnail_thread(gpointer user_data) {
	ThumbnailResult* result = user_data;
	result->surface = phi_document_render_thumbnail(result->document, 0,
		120, 160, &result->error);
	return NULL;
}

static void test_thumbnail_render(void) {
	g_autoptr(PhiDocument) document = open_fixture();
	ThumbnailResult result = { .document = document };
	GThread* worker = g_thread_new("thumbnail-test",
		render_thumbnail_thread, &result);
	g_thread_join(worker);

	g_assert_no_error(result.error);
	g_assert_nonnull(result.surface);
	g_assert_cmpint(cairo_image_surface_get_width(result.surface), >, 0);
	g_assert_cmpint(cairo_image_surface_get_width(result.surface), <=, 120);
	g_assert_cmpint(cairo_image_surface_get_height(result.surface), >, 0);
	g_assert_cmpint(cairo_image_surface_get_height(result.surface), <=, 160);
	g_assert_cmpint(cairo_surface_status(result.surface), ==,
		CAIRO_STATUS_SUCCESS);

	cairo_surface_flush(result.surface);
	gboolean has_content = FALSE;
	gint width = cairo_image_surface_get_width(result.surface);
	gint height = cairo_image_surface_get_height(result.surface);
	gint stride = cairo_image_surface_get_stride(result.surface);
	const guchar* pixels = cairo_image_surface_get_data(result.surface);
	for (gint y = 0; y < height && !has_content; y++) {
		const guint32* row = (const guint32*)(pixels + y * stride);
		for (gint x = 0; x < width; x++) {
			if ((row[x] & 0x00ffffff) != 0x00ffffff) {
				has_content = TRUE;
				break;
			}
		}
	}
	g_assert_true(has_content);
	cairo_surface_destroy(result.surface);
}

int main(int argc, char** argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/text/separate-diacritic", test_separate_diacritic_text);
	g_test_add_func("/thumbnail/render", test_thumbnail_render);
	return g_test_run();
}

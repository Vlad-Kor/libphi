/*
 * libphi - memory-backed document tests
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <phi/phi.h>

#include <cairo-pdf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static cairo_status_t append_to_byte_array(void* closure,
		const unsigned char* data, unsigned int length) {
	g_byte_array_append(closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}

/* A two-page 300x400 pt PDF generated entirely in memory, like a Typst
 * export. Each page has a black square at (50, 50)-(150, 150) and a label. */
static GBytes* generate_pdf_bytes(void) {
	GByteArray* array = g_byte_array_new();
	cairo_surface_t* surface = cairo_pdf_surface_create_for_stream(
		append_to_byte_array, array, 300, 400);
	cairo_t* cr = cairo_create(surface);
	for (gint page = 1; page <= 2; page++) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_rectangle(cr, 50, 50, 100, 100);
		cairo_fill(cr);
		cairo_move_to(cr, 50, 250);
		cairo_set_font_size(cr, 24);
		gchar* label = g_strdup_printf("Generated page %d", page);
		cairo_show_text(cr, label);
		g_free(label);
		cairo_show_page(cr);
	}
	cairo_destroy(cr);
	cairo_surface_finish(surface);
	g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);
	return g_byte_array_free_to_bytes(array);
}

static PhiDocument* open_generated_bytes(void) {
	GBytes* bytes = generate_pdf_bytes();
	GError* error = NULL;
	PhiDocument* document = phi_document_new_from_bytes(bytes, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(document);
	g_bytes_unref(bytes);
	return document;
}

static void test_bytes_open(void) {
	GBytes* bytes = generate_pdf_bytes();
	GError* error = NULL;
	PhiDocument* detected = phi_document_new_from_bytes(bytes, NULL, &error);
	g_assert_no_error(error);
	PhiDocument* typed = phi_document_new_from_bytes(bytes, "application/pdf",
		&error);
	g_assert_no_error(error);

	/* The documents must keep the data alive on their own. */
	g_bytes_unref(bytes);

	for (gint i = 0; i < 2; i++) {
		PhiDocument* document = i == 0 ? detected : typed;
		g_assert_cmpint(phi_document_get_n_pages(document), ==, 2);
		g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(document)),
			==, 2);

		PhiPage* page = phi_document_get_page(document, 1, &error);
		g_assert_no_error(error);
		gfloat width = 0, height = 0;
		phi_page_get_size(page, &width, &height);
		g_assert_cmpfloat_with_epsilon(width, 300, 0.01);
		g_assert_cmpfloat_with_epsilon(height, 400, 0.01);

		gchar* text = phi_page_get_text(page);
		g_assert_nonnull(strstr(text, "Generated page 2"));
		g_free(text);

		gchar* format = phi_document_dup_metadata(document,
			PHI_DOCUMENT_METADATA_FORMAT);
		g_assert_true(g_str_has_prefix(format, "PDF"));
		g_free(format);
	}

	g_object_unref(detected);
	g_object_unref(typed);
}

static void test_bytes_invalid(void) {
	static const gchar garbage[] = "this is not a document";
	GBytes* bytes = g_bytes_new_static(garbage, sizeof garbage - 1);
	GError* error = NULL;
	PhiDocument* document = phi_document_new_from_bytes(bytes, NULL, &error);
	g_assert_null(document);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_bytes_unref(bytes);

	bytes = g_bytes_new(NULL, 0);
	document = phi_document_new_from_bytes(bytes, "application/pdf", &error);
	g_assert_null(document);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_bytes_unref(bytes);
}

typedef struct {
	PhiDocument* document;
	gdouble scale;
	gint tile_x, tile_y, tile_width, tile_height;
	GdkTexture* texture;
	cairo_surface_t* thumbnail;
	GError* error;
} RenderJob;

static void render_in_thread(GTask* task, gpointer source, gpointer data,
		GCancellable* cancellable) {
	(void)task;
	(void)source;
	RenderJob* job = data;
	job->texture = phi_document_render_page_texture(job->document, 0,
		job->scale, job->tile_x, job->tile_y, job->tile_width,
		job->tile_height, cancellable, &job->error);
	if (!job->error)
		job->thumbnail = phi_document_render_thumbnail(job->document, 1,
			60, 80, &job->error);
}

/* Render on a worker thread, as PhiDocumentView does. */
static void render_on_worker(RenderJob* job) {
	GTask* task = g_task_new(NULL, NULL, NULL, NULL);
	g_task_set_task_data(task, job, NULL);
	g_task_run_in_thread_sync(task, render_in_thread);
	g_object_unref(task);
}

static guchar* download(GdkTexture* texture, gsize* stride) {
	*stride = gdk_texture_get_width(texture) * 4;
	guchar* pixels = g_malloc(*stride * gdk_texture_get_height(texture));
	gdk_texture_download(texture, pixels, *stride);
	return pixels;
}

static void assert_black(const guchar* pixels, gsize stride, gint x, gint y) {
	const guchar* pixel = pixels + y * stride + x * 4;
	g_assert_cmpuint(pixel[0], <, 16);
	g_assert_cmpuint(pixel[1], <, 16);
	g_assert_cmpuint(pixel[2], <, 16);
}

static void assert_white(const guchar* pixels, gsize stride, gint x, gint y) {
	const guchar* pixel = pixels + y * stride + x * 4;
	g_assert_cmpuint(pixel[0], >, 240);
	g_assert_cmpuint(pixel[1], >, 240);
	g_assert_cmpuint(pixel[2], >, 240);
}

static void test_bytes_background_render(void) {
	PhiDocument* document = open_generated_bytes();

	RenderJob page = { .document = document, .scale = 2.0 };
	render_on_worker(&page);
	g_assert_no_error(page.error);
	g_assert_nonnull(page.texture);
	g_assert_cmpint(gdk_texture_get_width(page.texture), ==, 600);
	g_assert_cmpint(gdk_texture_get_height(page.texture), ==, 800);

	gsize stride;
	guchar* pixels = download(page.texture, &stride);
	assert_black(pixels, stride, 200, 200); /* inside the square */
	assert_white(pixels, stride, 400, 200); /* beside it */
	g_free(pixels);

	g_assert_nonnull(page.thumbnail);
	g_assert_cmpint(cairo_image_surface_get_width(page.thumbnail), <=, 60);
	g_assert_cmpint(cairo_image_surface_get_height(page.thumbnail), <=, 80);

	/* A tile straddling the square's bottom-right corner (300, 300). */
	RenderJob tile = { .document = document, .scale = 2.0,
		.tile_x = 268, .tile_y = 268, .tile_width = 64, .tile_height = 64 };
	render_on_worker(&tile);
	g_assert_no_error(tile.error);
	g_assert_cmpint(gdk_texture_get_width(tile.texture), ==, 64);
	g_assert_cmpint(gdk_texture_get_height(tile.texture), ==, 64);
	pixels = download(tile.texture, &stride);
	assert_black(pixels, stride, 8, 8);
	assert_white(pixels, stride, 56, 56);
	g_free(pixels);

	g_object_unref(page.texture);
	cairo_surface_destroy(page.thumbnail);
	g_object_unref(tile.texture);
	cairo_surface_destroy(tile.thumbnail);
	g_object_unref(document);
}

/* The byte-backed renderer must produce exactly what the file-backed one
 * produces for the same PDF. */
static void test_bytes_render_matches_file(void) {
	GBytes* bytes = generate_pdf_bytes();
	GError* error = NULL;
	gchar* path = NULL;
	gint fd = g_file_open_tmp("phi-document-XXXXXX.pdf", &path, &error);
	g_assert_no_error(error);
	close(fd);
	gsize size;
	const gchar* data = g_bytes_get_data(bytes, &size);
	g_assert_true(g_file_set_contents(path, data, size, &error));
	g_assert_no_error(error);

	GFile* file = g_file_new_for_path(path);
	PhiDocument* from_file = phi_document_new_from_file(file, &error);
	g_assert_no_error(error);
	PhiDocument* from_bytes = phi_document_new_from_bytes(bytes, NULL, &error);
	g_assert_no_error(error);

	GdkTexture* file_texture = phi_document_render_page_texture(from_file, 0,
		1.5, 0, 0, 0, 0, NULL, &error);
	g_assert_no_error(error);
	GdkTexture* bytes_texture = phi_document_render_page_texture(from_bytes,
		0, 1.5, 0, 0, 0, 0, NULL, &error);
	g_assert_no_error(error);
	/* The file-backed renderer reopens the path lazily, so only remove it
	 * after rendering. */
	g_assert_cmpint(g_unlink(path), ==, 0);

	gsize file_stride, bytes_stride;
	guchar* file_pixels = download(file_texture, &file_stride);
	guchar* bytes_pixels = download(bytes_texture, &bytes_stride);
	g_assert_cmpint(gdk_texture_get_width(file_texture), ==,
		gdk_texture_get_width(bytes_texture));
	g_assert_cmpint(gdk_texture_get_height(file_texture), ==,
		gdk_texture_get_height(bytes_texture));
	g_assert_cmpmem(file_pixels,
		file_stride * gdk_texture_get_height(file_texture),
		bytes_pixels, bytes_stride * gdk_texture_get_height(bytes_texture));

	g_free(file_pixels);
	g_free(bytes_pixels);
	g_object_unref(file_texture);
	g_object_unref(bytes_texture);
	g_object_unref(from_file);
	g_object_unref(from_bytes);
	g_object_unref(file);
	g_bytes_unref(bytes);
	g_free(path);
}

/* Documents opened from a bare stream have nothing to reopen; the renderer
 * reports that so callers can fall back to vector rendering. */
static void test_stream_has_no_background_render(void) {
	GBytes* bytes = generate_pdf_bytes();
	GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
	GError* error = NULL;
	PhiDocument* document = phi_document_new_from_stream(stream,
		"application/pdf", &error);
	g_assert_no_error(error);

	GdkTexture* texture = phi_document_render_page_texture(document, 0, 1.0,
		0, 0, 0, 0, NULL, &error);
	g_assert_null(texture);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);

	g_object_unref(document);
	g_object_unref(stream);
	g_bytes_unref(bytes);
}

int main(int argc, char** argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/document/bytes/open", test_bytes_open);
	g_test_add_func("/document/bytes/invalid", test_bytes_invalid);
	g_test_add_func("/document/bytes/background-render",
		test_bytes_background_render);
	g_test_add_func("/document/bytes/render-matches-file",
		test_bytes_render_matches_file);
	g_test_add_func("/document/stream/no-background-render",
		test_stream_has_no_background_render);
	return g_test_run();
}

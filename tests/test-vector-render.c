/*
 * libphi - vector fallback rendering compared with MuPDF's rasterizer
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <phi/phi.h>

#include <string.h>

#define PAGE_WIDTH 300
#define PAGE_HEIGHT 400

/* A black and a red square, so that shifted, mirrored or rotated copies
 * are all visible as differences. The red square overhangs the 20x20 cell,
 * which each copy must clip away. */
#define OVERHANGING_CELL "0 0 0 rg 2 2 10 10 re f 1 0 0 rg 14 14 10 10 re f"
/* The same squares entirely inside the cell. */
#define CONTAINED_CELL "0 0 0 rg 2 2 10 10 re f 1 0 0 rg 12 12 6 6 re f"

/* One page filling a rectangle with a colored tiling pattern. */
static GBytes* pattern_pdf(const gchar* cell, const gchar* matrix,
		gdouble xstep, gdouble ystep) {
	const gchar* page =
		"/Pattern cs /P1 scn 40 40 220 320 re f";
	gchar* objects[5] = {
		g_strdup("<< /Type /Catalog /Pages 2 0 R >>"),
		g_strdup("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
		g_strdup_printf("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
			"/Resources << /Pattern << /P1 5 0 R >> >> /Contents 4 0 R >>",
			PAGE_WIDTH, PAGE_HEIGHT),
		g_strdup_printf("<< /Length %zu >>\nstream\n%s\nendstream",
			strlen(page), page),
		g_strdup_printf("<< /PatternType 1 /PaintType 1 /TilingType 1 "
			"/BBox [0 0 20 20] /XStep %g /YStep %g /Matrix [%s] "
			"/Resources << >> /Length %zu >>\nstream\n%s\nendstream",
			xstep, ystep, matrix, strlen(cell), cell),
	};

	GString* pdf = g_string_new("%PDF-1.4\n");
	gsize offsets[G_N_ELEMENTS(objects)];
	for (guint i = 0; i < G_N_ELEMENTS(objects); i++) {
		offsets[i] = pdf->len;
		g_string_append_printf(pdf, "%u 0 obj\n%s\nendobj\n", i + 1, objects[i]);
		g_free(objects[i]);
	}
	gsize xref = pdf->len;
	g_string_append_printf(pdf, "xref\n0 %u\n0000000000 65535 f \n",
		(guint)G_N_ELEMENTS(objects) + 1);
	for (guint i = 0; i < G_N_ELEMENTS(objects); i++)
		g_string_append_printf(pdf, "%010zu 00000 n \n", offsets[i]);
	g_string_append_printf(pdf, "trailer\n<< /Size %u /Root 1 0 R >>\n"
		"startxref\n%zu\n%%%%EOF\n", (guint)G_N_ELEMENTS(objects) + 1, xref);
	return g_string_free_to_bytes(pdf);
}

/* Pixels of the page drawn by the vector device, on white paper. */
static cairo_surface_t* vector_render(PhiDocument* document) {
	GError* error = NULL;
	PhiPage* page = phi_document_get_page(document, 0, &error);
	g_assert_no_error(error);
	GskRenderNode* node = phi_page_render_to_node(page, &error);
	g_assert_no_error(error);

	cairo_surface_t* surface = cairo_image_surface_create(
		CAIRO_FORMAT_RGB24, PAGE_WIDTH, PAGE_HEIGHT);
	cairo_t* cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	gsk_render_node_draw(node, cr);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	gsk_render_node_unref(node);
	return surface;
}

/* Fraction of pixels where the vector rendering visibly differs from
 * MuPDF's own rasterization of the same page. */
static gdouble mismatch_ratio(GBytes* bytes) {
	GError* error = NULL;
	PhiDocument* document = phi_document_new_from_bytes(bytes, NULL, &error);
	g_assert_no_error(error);

	GdkTexture* raster = phi_document_render_page_texture(
		document, 0, 1.0, 0, 0, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(gdk_texture_get_width(raster), ==, PAGE_WIDTH);
	g_assert_cmpint(gdk_texture_get_height(raster), ==, PAGE_HEIGHT);
	gsize raster_stride = PAGE_WIDTH * 4;
	guchar* expected = g_malloc(raster_stride * PAGE_HEIGHT);
	gdk_texture_download(raster, expected, raster_stride);

	cairo_surface_t* surface = vector_render(document);
	/* PHI_TEST_DUMP_DIR=dir keeps both renderings for inspection. */
	const gchar* dump = g_getenv("PHI_TEST_DUMP_DIR");
	if (dump) {
		static guint dumped;
		gchar* name = g_strdup_printf("%s/pattern-%u-mupdf.png", dump, dumped);
		gdk_texture_save_to_png(raster, name);
		g_free(name);
		name = g_strdup_printf("%s/pattern-%u-vector.png", dump, dumped++);
		cairo_surface_write_to_png(surface, name);
		g_free(name);
	}
	const guchar* actual = cairo_image_surface_get_data(surface);
	gint actual_stride = cairo_image_surface_get_stride(surface);

	/* Both buffers are native-endian 0xAARRGGBB words. The renderers smooth
	 * edges differently, particularly on rotated shapes, so compare 5x5
	 * averages: that hides subpixel differences but not misplaced tiles. */
	guint different = 0;
	gboolean painted = FALSE;
	for (gint y = 2; y < PAGE_HEIGHT - 2; y++) {
		for (gint x = 2; x < PAGE_WIDTH - 2; x++) {
			gint delta = 0;
			for (gint shift = 0; shift < 24; shift += 8) {
				gint want = 0;
				gint got = 0;
				for (gint dy = -2; dy <= 2; dy++) {
					const guint32* want_row = (const guint32*)(
						expected + (y + dy) * raster_stride);
					const guint32* got_row = (const guint32*)(
						actual + (y + dy) * actual_stride);
					for (gint dx = -2; dx <= 2; dx++) {
						want += (want_row[x + dx] >> shift) & 0xff;
						got += (got_row[x + dx] >> shift) & 0xff;
					}
				}
				delta = MAX(delta, ABS(want - got) / 25);
			}
			if (delta > 48)
				different++;
			const guint32* got_row = (const guint32*)(actual + y * actual_stride);
			if ((got_row[x] & 0xffffff) != 0xffffff)
				painted = TRUE;
		}
	}
	g_assert_true(painted);

	cairo_surface_destroy(surface);
	g_free(expected);
	g_object_unref(raster);
	g_object_unref(document);
	return (gdouble)different / (PAGE_WIDTH * PAGE_HEIGHT);
}

static void check_pattern(const gchar* cell, const gchar* matrix,
		gdouble xstep, gdouble ystep) {
	GBytes* bytes = pattern_pdf(cell, matrix, xstep, ystep);
	gdouble ratio = mismatch_ratio(bytes);
	g_test_message("pattern [%s] step %g %g: %.2f%% of pixels differ",
		matrix, xstep, ystep, ratio * 100);
	g_assert_cmpfloat(ratio, <, 0.03);
	g_bytes_unref(bytes);
}

/* Copies must sit at multiples of the step from the pattern origin, not
 * from the corner of the filled area. */
static void test_scaled_offset_pattern(void) {
	check_pattern(OVERHANGING_CELL, "1.5 0 0 1.5 7 11", 30, 30);
}

/* Copies follow a rotated pattern matrix. The content stays inside the
 * cell: the spec clips a cell to its BBox in pattern space, a rotated square
 * here, and so does the vector device, but MuPDF's rasterizer caches the
 * tile in an axis-aligned device rectangle and clips to that instead. */
static void test_rotated_pattern(void) {
	check_pattern(CONTAINED_CELL, "1.03923 0.6 -0.6 1.03923 100 60", 30, 30);
}

/* A step smaller than the cell makes neighbouring copies overlap; each copy
 * must still be clipped to its own cell. */
static void test_overlapping_pattern(void) {
	check_pattern(OVERHANGING_CELL, "1 0 0 1 3 5", 15, 25);
}

int main(int argc, char** argv) {
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/vector-render/pattern/scaled-offset",
		test_scaled_offset_pattern);
	g_test_add_func("/vector-render/pattern/rotated", test_rotated_pattern);
	g_test_add_func("/vector-render/pattern/overlapping",
		test_overlapping_pattern);
	return g_test_run();
}

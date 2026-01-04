/*
 * libphi - High performance document renderer for GTK
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Test for phi_node_device - the MuPDF to GSK render node converter
 * 
 * This test verifies basic MuPDF functionality that libphi depends on.
 */

#include <gtk/gtk.h>
#include <mupdf/fitz.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name, condition) do { \
	tests_run++; \
	if (condition) { \
		tests_passed++; \
		g_print("✓ %s\n", name); \
	} else { \
		g_printerr("✗ %s\n", name); \
	} \
} while(0)

static void test_context_creation(void) {
	g_print("\n=== Context Creation Tests ===\n");
	
	fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	TEST("Create MuPDF context", ctx != NULL);
	
	if (ctx) {
		fz_register_document_handlers(ctx);
		TEST("Register document handlers", TRUE);
		fz_drop_context(ctx);
	}
}

static void test_colorspace_types(void) {
	g_print("\n=== Colorspace Type Tests ===\n");
	
	fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx) {
		TEST("Create context for colorspace tests", FALSE);
		return;
	}
	
	/* Test RGB colorspace */
	fz_colorspace* rgb = fz_device_rgb(ctx);
	TEST("Get RGB colorspace", rgb != NULL);
	if (rgb) {
		TEST("RGB type is FZ_COLORSPACE_RGB", fz_colorspace_type(ctx, rgb) == FZ_COLORSPACE_RGB);
		TEST("RGB has 3 components", fz_colorspace_n(ctx, rgb) == 3);
	}
	
	/* Test Gray colorspace */
	fz_colorspace* gray = fz_device_gray(ctx);
	TEST("Get Gray colorspace", gray != NULL);
	if (gray) {
		TEST("Gray type is FZ_COLORSPACE_GRAY", fz_colorspace_type(ctx, gray) == FZ_COLORSPACE_GRAY);
		TEST("Gray has 1 component", fz_colorspace_n(ctx, gray) == 1);
	}
	
	/* Test CMYK colorspace */
	fz_colorspace* cmyk = fz_device_cmyk(ctx);
	TEST("Get CMYK colorspace", cmyk != NULL);
	if (cmyk) {
		TEST("CMYK type is FZ_COLORSPACE_CMYK", fz_colorspace_type(ctx, cmyk) == FZ_COLORSPACE_CMYK);
		TEST("CMYK has 4 components", fz_colorspace_n(ctx, cmyk) == 4);
	}
	
	fz_drop_context(ctx);
}

static void test_path_creation(void) {
	g_print("\n=== Path Creation Tests ===\n");
	
	fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx) {
		TEST("Create context for path tests", FALSE);
		return;
	}
	
	/* Create and manipulate paths */
	fz_path* path = fz_new_path(ctx);
	TEST("Create path", path != NULL);
	
	if (path) {
		fz_moveto(ctx, path, 0, 0);
		fz_lineto(ctx, path, 100, 0);
		fz_lineto(ctx, path, 100, 100);
		fz_lineto(ctx, path, 0, 100);
		fz_closepath(ctx, path);
		
		fz_rect bounds = fz_bound_path(ctx, path, NULL, fz_identity);
		TEST("Path has correct bounds", 
			bounds.x0 == 0 && bounds.y0 == 0 && 
			bounds.x1 == 100 && bounds.y1 == 100);
		
		fz_drop_path(ctx, path);
	}
	
	fz_drop_context(ctx);
}

static void test_pixmap_creation(void) {
	g_print("\n=== Pixmap Creation Tests ===\n");
	
	fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx) {
		TEST("Create context for pixmap tests", FALSE);
		return;
	}
	
	/* Test pixmap creation */
	fz_colorspace* rgb = fz_device_rgb(ctx);
	
	fz_pixmap* pixmap = fz_new_pixmap(ctx, rgb, 100, 100, NULL, 0);
	TEST("Create RGB pixmap without alpha", pixmap != NULL);
	if (pixmap) {
		TEST("Pixmap has 3 components", fz_pixmap_components(ctx, pixmap) == 3);
		TEST("Pixmap width is 100", fz_pixmap_width(ctx, pixmap) == 100);
		TEST("Pixmap height is 100", fz_pixmap_height(ctx, pixmap) == 100);
		fz_drop_pixmap(ctx, pixmap);
	}
	
	pixmap = fz_new_pixmap(ctx, rgb, 100, 100, NULL, 1);
	TEST("Create RGB pixmap with alpha", pixmap != NULL);
	if (pixmap) {
		TEST("RGBA pixmap has 4 components", fz_pixmap_components(ctx, pixmap) == 4);
		fz_drop_pixmap(ctx, pixmap);
	}
	
	fz_drop_context(ctx);
}

static void test_matrix_operations(void) {
	g_print("\n=== Matrix Operations Tests ===\n");
	
	/* Test identity matrix */
	fz_matrix m = fz_identity;
	TEST("Identity matrix a=1", m.a == 1.0f);
	TEST("Identity matrix d=1", m.d == 1.0f);
	TEST("Identity matrix e=0", m.e == 0.0f);
	
	/* Test translation */
	m = fz_translate(100, 200);
	TEST("Translation e=100", m.e == 100.0f);
	TEST("Translation f=200", m.f == 200.0f);
	
	/* Test scale */
	m = fz_scale(2.0f, 3.0f);
	TEST("Scale a=2", m.a == 2.0f);
	TEST("Scale d=3", m.d == 3.0f);
	
	/* Test matrix inversion */
	m = fz_scale(2.0f, 4.0f);
	fz_matrix inv = fz_invert_matrix(m);
	TEST("Inverted scale a=0.5", inv.a == 0.5f);
	TEST("Inverted scale d=0.25", inv.d == 0.25f);
	
	/* Test matrix concatenation */
	fz_matrix t = fz_translate(10, 20);
	fz_matrix s = fz_scale(2, 2);
	fz_matrix ts = fz_concat(t, s);
	TEST("Concat translation+scale", ts.e == 20.0f && ts.f == 40.0f);
}

static void test_gsk_path_builder(void) {
	g_print("\n=== GSK Path Builder Tests ===\n");
	
	GskPathBuilder* builder = gsk_path_builder_new();
	TEST("Create GSK path builder", builder != NULL);
	
	if (builder) {
		gsk_path_builder_move_to(builder, 0, 0);
		gsk_path_builder_line_to(builder, 100, 0);
		gsk_path_builder_line_to(builder, 100, 100);
		gsk_path_builder_line_to(builder, 0, 100);
		gsk_path_builder_close(builder);
		
		GskPath* path = gsk_path_builder_free_to_path(builder);
		TEST("Build GSK path", path != NULL);
		
		if (path) {
			graphene_rect_t bounds;
			gboolean has_bounds = gsk_path_get_bounds(path, &bounds);
			TEST("GSK path has bounds", has_bounds);
			if (has_bounds) {
				TEST("GSK path bounds correct", 
					bounds.origin.x == 0 && bounds.origin.y == 0 &&
					bounds.size.width == 100 && bounds.size.height == 100);
			}
			gsk_path_unref(path);
		}
	}
}

static void test_gsk_render_nodes(void) {
	g_print("\n=== GSK Render Node Tests ===\n");
	
	/* Test color node */
	graphene_rect_t bounds;
	graphene_rect_init(&bounds, 0, 0, 100, 100);
	GdkRGBA red = { 1.0, 0.0, 0.0, 1.0 };
	
	GskRenderNode* color_node = gsk_color_node_new(&red, &bounds);
	TEST("Create color node", color_node != NULL);
	
	if (color_node) {
		graphene_rect_t node_bounds;
		gsk_render_node_get_bounds(color_node, &node_bounds);
		TEST("Color node has correct bounds",
			node_bounds.origin.x == 0 && node_bounds.origin.y == 0 &&
			node_bounds.size.width == 100 && node_bounds.size.height == 100);
		gsk_render_node_unref(color_node);
	}
	
	/* Test container node */
	GskRenderNode* nodes[2];
	nodes[0] = gsk_color_node_new(&red, &bounds);
	graphene_rect_init(&bounds, 50, 50, 100, 100);
	GdkRGBA blue = { 0.0, 0.0, 1.0, 1.0 };
	nodes[1] = gsk_color_node_new(&blue, &bounds);
	
	GskRenderNode* container = gsk_container_node_new(nodes, 2);
	TEST("Create container node", container != NULL);
	
	if (container) {
		guint n_children = gsk_container_node_get_n_children(container);
		TEST("Container has 2 children", n_children == 2);
		gsk_render_node_unref(container);
	}
	
	gsk_render_node_unref(nodes[0]);
	gsk_render_node_unref(nodes[1]);
}

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;
	
	gtk_init();
	
	g_print("libphi Device Tests\n");
	g_print("==================\n");
	
	test_context_creation();
	test_colorspace_types();
	test_path_creation();
	test_pixmap_creation();
	test_matrix_operations();
	test_gsk_path_builder();
	test_gsk_render_nodes();
	
	g_print("\n==================\n");
	g_print("Results: %d/%d tests passed\n", tests_passed, tests_run);
	
	return tests_passed == tests_run ? 0 : 1;
}

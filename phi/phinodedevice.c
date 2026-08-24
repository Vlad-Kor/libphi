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

#include "phi/phinodedeviceprivate.h"

#include <gtk/gtk.h>
#include <math.h>

typedef enum {
	PHI_RENDER_STATE_NONE,
	PHI_RENDER_STATE_CLIP_PATH_FILL,
	PHI_RENDER_STATE_MASK,
	PHI_RENDER_STATE_IN_MASK,
	PHI_RENDER_STATE_GROUP,
	PHI_RENDER_STATE_TILE,
} PhiRenderContextState;

typedef struct {
	// GPtrArray<GskRenderNode>
	GPtrArray *children;

	PhiRenderContextState state;
	union {
		struct {
			GskPath* path;
			int even_odd;
			fz_matrix ctm;
			fz_rect scissor;
		} clip_path_fill;
		struct {
			GskRenderNode* mask;
			GskMaskMode mode;
			fz_rect scissor;
		} mask;
		struct {
			GskMaskMode mask_mode;
			fz_rect area;
		} in_mask;
		struct {
			fz_rect area;
			int isolated;
			int knockout;
			int blendmode;
			float alpha;
		} group;
		struct {
			fz_rect area;
			fz_rect view;
			float xstep;
			float ystep;
			fz_matrix ctm;
			int id;
		} tile;
	};
} PhiRenderContext;

static void phi_render_context_init(PhiRenderContext* self) {
	self->children = g_ptr_array_new_with_free_func((GDestroyNotify)gsk_render_node_unref);
	self->state = PHI_RENDER_STATE_NONE;
}

static void phi_render_context_clear(PhiRenderContext* self) {
	g_ptr_array_unref(self->children);
	switch (self->state) {
		case PHI_RENDER_STATE_NONE:
			break;
		case PHI_RENDER_STATE_CLIP_PATH_FILL:
			gsk_path_unref (self->clip_path_fill.path);
			break;
		case PHI_RENDER_STATE_MASK:
			gsk_render_node_unref(self->mask.mask);
			break;
		case PHI_RENDER_STATE_IN_MASK:
			break;
		case PHI_RENDER_STATE_GROUP:
			/* No resources to free */
			break;
		case PHI_RENDER_STATE_TILE:
			/* No resources to free */
			break;
	}
}


typedef struct {
	fz_device super;
	// GArray<PhiRenderContext>
	GArray *stack;
	/* Cloned MuPDF contexts keep using the originating context's lock
	 * callbacks. Keep their callback owner alive for as long as any render
	 * node backed by this device can survive. */
	GObject *context_owner;
} PhiNodeDevice;

static void phi_node_device_drop(fz_context*, fz_device* dev) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	g_array_unref(self->stack);
	g_clear_object(&self->context_owner);
}

static GskTransform* phi_node_device_transform_from_matrix(const fz_matrix* ctm) {
#if GTK_CHECK_VERSION(4, 20, 0)
	return gsk_transform_matrix_2d(NULL,
	                               ctm->a, ctm->b,
	                               ctm->c, ctm->d,
	                               ctm->e, ctm->f);
#else
	// fast-path
	if (ctm->b == 0. && ctm->c == 0.) {
		graphene_point_t offset;
		graphene_point_init(&offset, ctm->e, ctm->f);
		return gsk_transform_scale(gsk_transform_translate(NULL, &offset), ctm->a, ctm->d);
	}

	graphene_matrix_t mat;
	graphene_matrix_init_from_2d(&mat,
		ctm->a, ctm->b,
		ctm->c, ctm->d,
		ctm->e, ctm->f);
	return gsk_transform_matrix(NULL, &mat);
#endif
}

static GskRenderNode* phi_node_device_transform_child(GskRenderNode* child, const fz_matrix* ctm) {
	GskTransform* transform = phi_node_device_transform_from_matrix(ctm);
	if (!transform)
		return child;

	GskRenderNode* transformed = gsk_transform_node_new(child, transform);

	gsk_transform_unref(transform);
	gsk_render_node_unref(child);
	return transformed;
}

static void phi_node_device_path_walker_moveto(fz_context*, void* arg, float x, float y) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	gsk_path_builder_move_to(builder, x, y);
}
static void phi_node_device_path_walker_lineto(fz_context*, void* arg, float x, float y) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	gsk_path_builder_line_to(builder, x, y);
}
static void phi_node_device_path_walker_curveto(fz_context*, void* arg, float x1, float y1, float x2, float y2, float x3, float y3) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	gsk_path_builder_cubic_to(builder, x1, y1, x2, y2, x3, y3);
}
static void phi_node_device_path_walker_closepath(fz_context*, void* arg) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	gsk_path_builder_close(builder);
}
static void phi_node_device_path_walker_quadto(fz_context*, void* arg, float x1, float y1, float x2, float y2) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	gsk_path_builder_quad_to(builder, x1, y1, x2, y2);
}
static void phi_node_device_path_walker_rectto(fz_context*, void* arg,  float x1, float y1, float x2, float y2) {
	GskPathBuilder* builder = (GskPathBuilder*)arg;
	graphene_rect_t rect;
	graphene_rect_init(&rect, x1, y1, x2 - x1, y2 - y1);
	gsk_path_builder_add_rect(builder, &rect);
}
const fz_path_walker phi_node_device_path_walker = {
	.moveto = phi_node_device_path_walker_moveto,
	.lineto = phi_node_device_path_walker_lineto,
	.curveto = phi_node_device_path_walker_curveto,
	.closepath = phi_node_device_path_walker_closepath,
	.quadto = phi_node_device_path_walker_quadto,
	.rectto = phi_node_device_path_walker_rectto
};
static GskPath* phi_node_device_convert_path(fz_context* ctx, const fz_path* path) {
	GskPathBuilder* builder = gsk_path_builder_new ();
	fz_walk_path(ctx, path, &phi_node_device_path_walker, builder);
	return gsk_path_builder_free_to_path(builder);
}

static GskRenderNode* phi_node_device_make_color(fz_context* ctx, fz_colorspace* cs, const float* color, float alpha, const graphene_rect_t *bounds) {
	float rgb[3];
	
	/* Fast path for common colorspaces */
	switch (fz_colorspace_type(ctx, cs)) {
		case FZ_COLORSPACE_RGB:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[0], .green = color[1], .blue = color[2], .alpha = alpha }, bounds);
		case FZ_COLORSPACE_BGR:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[2], .green = color[1], .blue = color[0], .alpha = alpha }, bounds);
		case FZ_COLORSPACE_GRAY:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[0], .green = color[0], .blue = color[0], .alpha = alpha }, bounds);
		default:
			break;
	}
	
	/* Use MuPDF's color conversion for accurate results (CMYK, Lab, ICC profiles, etc.) */
	fz_try(ctx) {
		fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, NULL, fz_default_color_params);
	} fz_catch(ctx) {
		fz_warn(ctx, "Failed to convert colorspace type %d", fz_colorspace_type(ctx, cs));
		return NULL;
	}
	return gsk_color_node_new(&(GdkRGBA){ .red = rgb[0], .green = rgb[1], .blue = rgb[2], .alpha = alpha }, bounds);
}

static GskRenderNode* phi_node_device_node_from_fillpath(GskRenderNode* child, GskPath* path, int even_odd, const fz_matrix* child_ctm, const fz_matrix* ctm) {
	if (!fz_is_identity(*child_ctm))
		child = phi_node_device_transform_child(child, child_ctm);

	GskRenderNode* node = gsk_fill_node_new(child, path, even_odd ? GSK_FILL_RULE_EVEN_ODD : GSK_FILL_RULE_WINDING);
	gsk_render_node_unref(child);
	
	return phi_node_device_transform_child(node, ctm);
}

static void phi_node_device_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;

	GskPath* cpath = phi_node_device_convert_path(ctx, path);
	
	graphene_rect_t bounds;
	if (!gsk_path_get_bounds(cpath, &bounds))
		graphene_rect_init(&bounds, 0.f, 0.f, 0.f, 0.f);
	GskRenderNode* fill = phi_node_device_make_color(ctx, cs, color, alpha, &bounds);
	
	GskRenderNode* node = phi_node_device_node_from_fillpath(fill, cpath, even_odd, &fz_identity, &ctm);
	gsk_path_unref(cpath);

	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* ss, fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;

	/* If ss->linewith is 0, its supposed to be a hairline - Gsk.Stroke doesn't have that
	 * for now, we'll just hardcode .25 as size, but we might want to switch to a cairo node,
	 * which has cairo_set_hairline.
	 */
	GskStroke* stroke = gsk_stroke_new(ss->linewidth > 0 ? ss->linewidth : 1.);
	gsk_stroke_set_miter_limit(stroke, ss->miterlimit);
	switch (ss->start_cap) {
		case FZ_LINECAP_BUTT:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_BUTT);
			break;
		case FZ_LINECAP_ROUND:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
			break;
		case FZ_LINECAP_SQUARE:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_SQUARE);
			break;
		default:
			fz_warn(ctx, "Unsupported linecap %d", ss->start_cap);
	}
	switch (ss->linejoin) {
		case FZ_LINEJOIN_MITER:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_MITER);
			break;
		case FZ_LINEJOIN_ROUND:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
			break;
		case FZ_LINEJOIN_BEVEL:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_BEVEL);
			break;
		default:
			fz_warn(ctx, "Unsupported linejoin %d", ss->linejoin);
	}
	gsk_stroke_set_dash(stroke, ss->dash_list, ss->dash_len);
	gsk_stroke_set_dash_offset(stroke, ss->dash_phase);
	
	GskPath* cpath = phi_node_device_convert_path(ctx, path);
	graphene_rect_t bounds;
	if (!gsk_path_get_stroke_bounds(cpath, stroke, &bounds))
		graphene_rect_init(&bounds, 0.f, 0.f, 0.f, 0.f);
	GskRenderNode* fill = phi_node_device_make_color(ctx, cs, color, alpha, &bounds);
	
	GskRenderNode* node = gsk_stroke_node_new(fill, cpath, stroke);
	gsk_stroke_free(stroke);
	gsk_render_node_unref(fill);
	gsk_path_unref(cpath);

	node = phi_node_device_transform_child(node, &ctm);

	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_CLIP_PATH_FILL;
	new.clip_path_fill.path = phi_node_device_convert_path(ctx, path);
	new.clip_path_fill.even_odd = even_odd;
	new.clip_path_fill.ctm = ctm;
	new.clip_path_fill.scissor = scissor;

	g_array_append_val(self->stack, new);
}

static void phi_node_device_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* ss, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)ss; /* We can't easily clip to stroked paths in GSK without rasterizing */
	
	/* For stroke clipping, we use the scissor rect as the clip region.
	 * This is an approximation - proper stroke clipping would require
	 * rasterizing the stroke, but that would hurt performance.
	 * Most PDFs don't use stroke clipping extensively.
	 */
	GskPath* cpath = phi_node_device_convert_path(ctx, path);
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_CLIP_PATH_FILL;
	new.clip_path_fill.path = cpath;
	new.clip_path_fill.even_odd = 0;
	new.clip_path_fill.ctm = ctm;
	new.clip_path_fill.scissor = scissor;

	g_array_append_val(self->stack, new);
}

static GskRenderNode* phi_node_device_alpha(GskRenderNode* child, float alpha) {
	if (alpha == 1.)
		return child;
	GskRenderNode* node = gsk_opacity_node_new(child, alpha);
	gsk_render_node_unref(child);
	return node;
}

typedef struct {
	fz_context* ctx;
	fz_pixmap* pixmap;
	GObject* context_owner;
} PhiPixmapStorage;
static void phi_pixmap_storage_free(PhiPixmapStorage* self) {
	fz_drop_pixmap(self->ctx, self->pixmap);
	fz_drop_context(self->ctx);
	g_object_unref(self->context_owner);
	g_free(self);
}
static GskRenderNode* phi_node_device_node_from_image(fz_context* ctx,
	fz_image* img, fz_matrix ctm, GObject* context_owner) {
	fz_pixmap* pixmap = NULL;
	fz_pixmap* converted = NULL;
	
	fz_try(ctx) {
		pixmap = fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
	} fz_catch(ctx) {
		fz_rethrow(ctx);
	}
	
	gint components = fz_pixmap_components(ctx, pixmap);
	gint colorants = fz_pixmap_colorants(ctx, pixmap);
	gint spots = fz_pixmap_spots(ctx, pixmap);
	gint alphas = fz_pixmap_alpha(ctx, pixmap);
	
	if (components > 256) {
		fz_drop_pixmap(ctx, pixmap);
		fz_throw(ctx, FZ_ERROR_LIMIT, "Pixmap has too many components (%d)", components);
	}
	
	guint32 fingerprint = (((guint8)components) << 24) | (((guint8)colorants) << 16) | (((guint8)spots) << 8) | ((guint8)alphas);
	GdkMemoryFormat format;
	gboolean needs_conversion = FALSE;
	
	switch (fingerprint) {
		case 0x03030000: /* RGB without alpha */
			format = GDK_MEMORY_R8G8B8;
			break;
		case 0x04030001: /* RGB with alpha */
			format = GDK_MEMORY_R8G8B8A8;
			break;
		case 0x01010000: /* Gray without alpha */
			format = GDK_MEMORY_G8;
			break;
		case 0x02010001: /* Gray with alpha */
			format = GDK_MEMORY_G8A8;
			break;
		case 0x01000001: /* Alpha only */
			format = GDK_MEMORY_A8;
			break;
		case 0x04040000: /* CMYK without alpha */
		case 0x05040001: /* CMYK with alpha */
			/* Convert CMYK to RGB */
			needs_conversion = TRUE;
			break;
		default:
			/* Try to convert unknown formats to RGB */
			needs_conversion = TRUE;
			break;
	}
	
	if (needs_conversion) {
		/* Convert to RGB(A) */
		fz_colorspace* rgb = fz_device_rgb(ctx);
		gint has_alpha = alphas > 0;
		
		fz_try(ctx) {
			converted = fz_convert_pixmap(ctx, pixmap, rgb, NULL, NULL, fz_default_color_params, has_alpha);
			fz_drop_pixmap(ctx, pixmap);
			pixmap = converted;
			converted = NULL;
			format = has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
		} fz_catch(ctx) {
			fz_drop_pixmap(ctx, pixmap);
			if (converted)
				fz_drop_pixmap(ctx, converted);
			fz_rethrow(ctx);
		}
	}

	gint width = fz_pixmap_width(ctx, pixmap);
	gint height = fz_pixmap_height(ctx, pixmap);

	PhiPixmapStorage *pixmap_store = g_new(PhiPixmapStorage, 1);
	pixmap_store->ctx = fz_clone_context(ctx);
	pixmap_store->pixmap = pixmap; // takes ownership
	pixmap_store->context_owner = g_object_ref(context_owner);

	GBytes* bytes = g_bytes_new_with_free_func(fz_pixmap_samples(ctx, pixmap), fz_pixmap_size(ctx, pixmap), (GDestroyNotify)phi_pixmap_storage_free, pixmap_store);
	GdkTexture* texture = gdk_memory_texture_new(width, height, format, bytes, fz_pixmap_stride(ctx, pixmap));
	g_bytes_unref(bytes);
	GskRenderNode *texture_node = gsk_texture_node_new(texture, &GRAPHENE_RECT_INIT(0, 0, width, height));
	g_object_unref(texture);

	// mat = inv([width 0 0; 0 height 0; 0 0 1])*ctm
	fz_matrix mat = fz_make_matrix(
		ctm.a / width, ctm.b / width,
		ctm.c / height, ctm.d / height,
		ctm.e, ctm.f);
	return phi_node_device_transform_child(texture_node, &mat);
}

static void phi_node_device_fill_image(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, float alpha, fz_color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	GskRenderNode *node = phi_node_device_node_from_image(ctx, img, ctm,
		self->context_owner);
	node = phi_node_device_alpha(node, alpha);
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	GskRenderNode *node = phi_node_device_node_from_image(ctx, img, ctm,
		self->context_owner);

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_MASK;
	new.mask.mask = node;
	new.mask.mode = GSK_MASK_MODE_ALPHA;
	new.mask.scissor = scissor;

	g_array_append_val(self->stack, new);
}

static void phi_node_device_fill_image_mask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)color_params;
	
	/* Get image as alpha mask */
	GskRenderNode* mask = phi_node_device_node_from_image(ctx, img, ctm,
		self->context_owner);
	
	/* Create the color fill */
	graphene_rect_t bounds;
	gsk_render_node_get_bounds(mask, &bounds);
	GskRenderNode* fill = phi_node_device_make_color(ctx, cs, color, alpha, &bounds);
	if (!fill) {
		gsk_render_node_unref(mask);
		fz_warn(ctx, "Unsupported colorspace in fill_image_mask");
		return;
	}
	
	/* Apply mask to color */
	GskRenderNode* node = gsk_mask_node_new(fill, mask, GSK_MASK_MODE_ALPHA);
	gsk_render_node_unref(fill);
	gsk_render_node_unref(mask);
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

/* Text rendering: convert glyphs to paths for vector rendering */
static GskPath* phi_node_device_text_to_path(fz_context* ctx, const fz_text* text, fz_matrix ctm) {
	GskPathBuilder* builder = gsk_path_builder_new();
	
	for (fz_text_span* span = text->head; span; span = span->next) {
		fz_font* font = span->font;
		fz_matrix trm = span->trm;
		
		for (int i = 0; i < span->len; i++) {
			fz_text_item* item = &span->items[i];
			if (item->gid < 0)
				continue;
			
			/* Get glyph outline */
			fz_matrix glyph_trm = fz_make_matrix(trm.a, trm.b, trm.c, trm.d, item->x, item->y);
			glyph_trm = fz_concat(glyph_trm, ctm);
			
			fz_path* glyph_path = fz_outline_glyph(ctx, font, item->gid, glyph_trm);
			if (glyph_path) {
				fz_walk_path(ctx, glyph_path, &phi_node_device_path_walker, builder);
				fz_drop_path(ctx, glyph_path);
			}
		}
	}
	
	return gsk_path_builder_free_to_path(builder);
}

static void phi_node_device_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)color_params;
	
	GskPath* path = phi_node_device_text_to_path(ctx, text, ctm);
	
	graphene_rect_t bounds;
	if (!gsk_path_get_bounds(path, &bounds)) {
		gsk_path_unref(path);
		return; /* Empty path */
	}
	
	GskRenderNode* fill = phi_node_device_make_color(ctx, cs, color, alpha, &bounds);
	if (!fill) {
		gsk_path_unref(path);
		fz_warn(ctx, "Unsupported colorspace in fill_text");
		return;
	}
	
	GskRenderNode* node = gsk_fill_node_new(fill, path, GSK_FILL_RULE_WINDING);
	gsk_render_node_unref(fill);
	gsk_path_unref(path);
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* ss, fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)color_params;
	
	GskPath* path = phi_node_device_text_to_path(ctx, text, ctm);
	
	GskStroke* stroke = gsk_stroke_new(ss->linewidth > 0 ? ss->linewidth : 1.);
	gsk_stroke_set_miter_limit(stroke, ss->miterlimit);
	switch (ss->start_cap) {
		case FZ_LINECAP_BUTT:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_BUTT);
			break;
		case FZ_LINECAP_ROUND:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
			break;
		case FZ_LINECAP_SQUARE:
			gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_SQUARE);
			break;
		default:
			break;
	}
	switch (ss->linejoin) {
		case FZ_LINEJOIN_MITER:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_MITER);
			break;
		case FZ_LINEJOIN_ROUND:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
			break;
		case FZ_LINEJOIN_BEVEL:
			gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_BEVEL);
			break;
		default:
			break;
	}
	gsk_stroke_set_dash(stroke, ss->dash_list, ss->dash_len);
	gsk_stroke_set_dash_offset(stroke, ss->dash_phase);
	
	graphene_rect_t bounds;
	if (!gsk_path_get_stroke_bounds(path, stroke, &bounds)) {
		gsk_stroke_free(stroke);
		gsk_path_unref(path);
		return;
	}
	
	GskRenderNode* fill = phi_node_device_make_color(ctx, cs, color, alpha, &bounds);
	if (!fill) {
		gsk_stroke_free(stroke);
		gsk_path_unref(path);
		fz_warn(ctx, "Unsupported colorspace in stroke_text");
		return;
	}
	
	GskRenderNode* node = gsk_stroke_node_new(fill, path, stroke);
	gsk_stroke_free(stroke);
	gsk_render_node_unref(fill);
	gsk_path_unref(path);
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	GskPath* path = phi_node_device_text_to_path(ctx, text, ctm);
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_CLIP_PATH_FILL;
	new.clip_path_fill.path = path;
	new.clip_path_fill.even_odd = 0;
	new.clip_path_fill.ctm = fz_identity;
	new.clip_path_fill.scissor = scissor;
	
	g_array_append_val(self->stack, new);
}

static void phi_node_device_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* ss, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)ss; /* Similar to clip_stroke_path, we use the text path directly */
	
	/* For stroke text clipping, use the text outline path.
	 * Proper stroke clipping would require complex path operations
	 * that GSK doesn't support directly.
	 */
	GskPath* path = phi_node_device_text_to_path(ctx, text, ctm);
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_CLIP_PATH_FILL;
	new.clip_path_fill.path = path;
	new.clip_path_fill.even_odd = 0;
	new.clip_path_fill.ctm = fz_identity;
	new.clip_path_fill.scissor = scissor;
	
	g_array_append_val(self->stack, new);
}

static void phi_node_device_ignore_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm) {
	/* Ignore text is used for invisible text (e.g., for searchable PDFs) */
	(void)ctx;
	(void)dev;
	(void)text;
	(void)ctm;
}

static GskRenderNode* phi_node_device_scissor_clip(GskRenderNode* child, const fz_rect* clip) {
	graphene_rect_t rect;
	graphene_rect_init(&rect, clip->x0, clip->y0, clip->x1 - clip->x0, clip->y1 - clip->y0);
	
	GskRenderNode* node = gsk_clip_node_new(child, &rect);
	gsk_render_node_unref(child);
	
	return node;
}

static void phi_node_device_pop_clip(fz_context* ctx, fz_device* dev) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	if (self->stack->len < 2)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "fz_pop_clip called on root");

	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);

	GskRenderNode* node;
	if (current->children->len == 0) {
		/* Empty clip - just pop and return */
		g_array_remove_index(self->stack, self->stack->len - 1);
		return;
	} else if (current->children->len == 1) {
		node = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	} else {
		node = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);
	}
	
	switch (current->state) {
		case PHI_RENDER_STATE_NONE:
			break;
		case PHI_RENDER_STATE_CLIP_PATH_FILL: {
			fz_matrix inv;
			if (fz_try_invert_matrix(&inv, current->clip_path_fill.ctm) != 0) {
				fz_warn(ctx, "Failed to invert matrix, using identity");
				inv = fz_identity;
			}
			node = phi_node_device_node_from_fillpath(node, current->clip_path_fill.path, current->clip_path_fill.even_odd, &inv, &current->clip_path_fill.ctm);
			node = phi_node_device_scissor_clip(node, &current->clip_path_fill.scissor);
		} break;
		case PHI_RENDER_STATE_MASK: {
			GskRenderNode *source = node;
			node = gsk_mask_node_new(source, current->mask.mask, current->mask.mode);
			gsk_render_node_unref(source);
			node = phi_node_device_scissor_clip(node, &current->mask.scissor);
		} break;
		case PHI_RENDER_STATE_IN_MASK:
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "pop_clip called in mask context");
			break;
		case PHI_RENDER_STATE_GROUP:
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "pop_clip called in group context (use end_group)");
			break;
		case PHI_RENDER_STATE_TILE:
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "pop_clip called in tile context (use end_tile)");
			break;
	}
	g_array_remove_index(self->stack, self->stack->len - 1);
	current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_begin_mask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity, fz_colorspace* cs, const float* bc, fz_color_params color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)color_params;

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_IN_MASK;
	new.in_mask.mask_mode = luminosity ? GSK_MASK_MODE_LUMINANCE : GSK_MASK_MODE_ALPHA;
	new.in_mask.area = area;
	
	/* If background color is specified for the soft mask, add it as the first child */
	if (bc && cs && !fz_is_empty_rect(area)) {
		graphene_rect_t bounds;
		graphene_rect_init(&bounds, area.x0, area.y0, 
			area.x1 - area.x0, area.y1 - area.y0);
		GskRenderNode* bg = phi_node_device_make_color(ctx, cs, bc, 1.0f, &bounds);
		if (bg)
			g_ptr_array_add(new.children, bg);
	}

	g_array_append_val(self->stack, new);
}

static void phi_node_device_end_mask(fz_context* ctx, fz_device* dev, fz_function*) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	if (current->state != PHI_RENDER_STATE_IN_MASK)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_mask called in invalid state");
	
	GskRenderNode* node;
	if (current->children->len == 0) {
		/* Empty mask - create transparent rect */
		graphene_rect_t bounds;
		graphene_rect_init(&bounds, 
			current->in_mask.area.x0, current->in_mask.area.y0,
			current->in_mask.area.x1 - current->in_mask.area.x0,
			current->in_mask.area.y1 - current->in_mask.area.y0);
		node = gsk_color_node_new(&(GdkRGBA){0, 0, 0, 0}, &bounds);
	} else if (current->children->len == 1) {
		node = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	} else {
		node = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);
	}

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_MASK;
	new.mask.mask = node;
	new.mask.mode = current->in_mask.mask_mode;
	new.mask.scissor = current->in_mask.area;

	g_array_remove_index(self->stack, self->stack->len - 1);
	g_array_append_val(self->stack, new);
}

/* Shading (gradients) rendering - rasterize to pixmap for complex shades */
static void phi_node_device_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shade, fz_matrix ctm, float alpha, fz_color_params color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	/* Get shade bounds */
	fz_rect bounds = fz_bound_shade(ctx, shade, ctm);
	
	/* Get current scissor from device */
	fz_rect scissor = fz_device_current_scissor(ctx, dev);
	
	/* For infinite bounds, use the device scissor */
	if (fz_is_infinite_rect(bounds)) {
		bounds = scissor;
	}
	
	/* Intersect with device scissor to ensure we don't render outside clip */
	bounds = fz_intersect_rect(bounds, scissor);
	
	/* Skip if still invalid */
	if (fz_is_infinite_rect(bounds) || fz_is_empty_rect(bounds)) {
		return;
	}
	
	/* Round to integers for pixmap */
	fz_irect ibounds = fz_round_rect(bounds);
	int width = ibounds.x1 - ibounds.x0;
	int height = ibounds.y1 - ibounds.y0;
	
	if (width <= 0 || height <= 0)
		return;
	
	/* Limit size to prevent huge allocations */
	if (width > 4096) width = 4096;
	if (height > 4096) height = 4096;
	
	/* Create RGB pixmap with alpha to render shade into */
	fz_pixmap* pixmap = fz_new_pixmap(ctx, fz_device_rgb(ctx), width, height, NULL, 1);
	/* Clear to transparent - the shade will draw on top */
	fz_clear_pixmap(ctx, pixmap);
	fz_set_pixmap_resolution(ctx, pixmap, 72, 72);
	pixmap->x = ibounds.x0;
	pixmap->y = ibounds.y0;
	
	fz_try(ctx) {
		/* Paint shade into pixmap */
		fz_paint_shade(ctx, shade, NULL, ctm, pixmap, color_params, ibounds, NULL, NULL);
	} fz_catch(ctx) {
		fz_drop_pixmap(ctx, pixmap);
		fz_rethrow(ctx);
	}
	
	/* Convert pixmap to GskRenderNode */
	PhiPixmapStorage* pixmap_store = g_new(PhiPixmapStorage, 1);
	pixmap_store->ctx = fz_clone_context(ctx);
	pixmap_store->pixmap = pixmap;
	pixmap_store->context_owner = g_object_ref(self->context_owner);
	
	GBytes* bytes = g_bytes_new_with_free_func(
		fz_pixmap_samples(ctx, pixmap),
		fz_pixmap_size(ctx, pixmap),
		(GDestroyNotify)phi_pixmap_storage_free,
		pixmap_store
	);
	
	GdkTexture* texture = gdk_memory_texture_new(
		width, height,
		GDK_MEMORY_R8G8B8A8,
		bytes,
		fz_pixmap_stride(ctx, pixmap)
	);
	g_bytes_unref(bytes);
	
	graphene_rect_t grect;
	graphene_rect_init(&grect, ibounds.x0, ibounds.y0, width, height);
	GskRenderNode* node = gsk_texture_node_new(texture, &grect);
	g_object_unref(texture);
	
	node = phi_node_device_alpha(node, alpha);
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

/* Transparency groups */
static void phi_node_device_begin_group(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs, int isolated, int knockout, int blendmode, float alpha) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)ctx;
	(void)cs; /* We ignore colorspace for now, render in device RGB */
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_GROUP;
	new.group.area = area;
	new.group.isolated = isolated;
	new.group.knockout = knockout;
	new.group.blendmode = blendmode;
	new.group.alpha = alpha;
	
	g_array_append_val(self->stack, new);
}

static GskBlendMode phi_node_device_convert_blendmode(int blendmode) {
	/* MuPDF blend modes from fz_blend_mode enum */
	switch (blendmode) {
		case 0: return GSK_BLEND_MODE_DEFAULT;    /* Normal */
		case 1: return GSK_BLEND_MODE_MULTIPLY;
		case 2: return GSK_BLEND_MODE_SCREEN;
		case 3: return GSK_BLEND_MODE_OVERLAY;
		case 4: return GSK_BLEND_MODE_DARKEN;
		case 5: return GSK_BLEND_MODE_LIGHTEN;
		case 6: return GSK_BLEND_MODE_COLOR_DODGE;
		case 7: return GSK_BLEND_MODE_COLOR_BURN;
		case 8: return GSK_BLEND_MODE_HARD_LIGHT;
		case 9: return GSK_BLEND_MODE_SOFT_LIGHT;
		case 10: return GSK_BLEND_MODE_DIFFERENCE;
		case 11: return GSK_BLEND_MODE_EXCLUSION;
		case 12: return GSK_BLEND_MODE_HUE;
		case 13: return GSK_BLEND_MODE_SATURATION;
		case 14: return GSK_BLEND_MODE_COLOR;
		case 15: return GSK_BLEND_MODE_LUMINOSITY;
		default: return GSK_BLEND_MODE_DEFAULT;
	}
}

static void phi_node_device_end_group(fz_context* ctx, fz_device* dev) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	if (self->stack->len < 2)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_group called on root");
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	if (current->state != PHI_RENDER_STATE_GROUP)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_group called in invalid state");
	
	GskRenderNode* node;
	if (current->children->len == 0) {
		g_array_remove_index(self->stack, self->stack->len - 1);
		return;
	} else if (current->children->len == 1) {
		node = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	} else {
		node = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);
	}
	
	/* Apply clip to group area */
	graphene_rect_t clip_rect;
	graphene_rect_init(&clip_rect,
		current->group.area.x0,
		current->group.area.y0,
		current->group.area.x1 - current->group.area.x0,
		current->group.area.y1 - current->group.area.y0
	);
	if (!fz_is_infinite_rect(current->group.area)) {
		GskRenderNode* clipped = gsk_clip_node_new(node, &clip_rect);
		gsk_render_node_unref(node);
		node = clipped;
	}
	
	/* Apply alpha */
	if (current->group.alpha < 1.0f) {
		node = phi_node_device_alpha(node, current->group.alpha);
	}
	
	/* Apply blend mode if not normal */
	GskBlendMode blend = phi_node_device_convert_blendmode(current->group.blendmode);
	
	g_array_remove_index(self->stack, self->stack->len - 1);
	PhiRenderContext* parent = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	
	if (blend != GSK_BLEND_MODE_DEFAULT && parent->children->len > 0) {
		/* Create blend node with previous content as bottom */
		GskRenderNode* bottom;
		if (parent->children->len == 1) {
			bottom = gsk_render_node_ref(g_ptr_array_index(parent->children, 0));
		} else {
			bottom = gsk_container_node_new((GskRenderNode**)parent->children->pdata, parent->children->len);
		}
		
		GskRenderNode* blended = gsk_blend_node_new(bottom, node, blend);
		gsk_render_node_unref(bottom);
		gsk_render_node_unref(node);
		
		/* Clear parent children and add blended result */
		g_ptr_array_set_size(parent->children, 0);
		g_ptr_array_add(parent->children, blended);
	} else {
		g_ptr_array_add(parent->children, node);
	}
}

/* Tiling patterns */
static int phi_node_device_begin_tile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep, float ystep, fz_matrix ctm, int id, int doc_id) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	(void)ctx;
	(void)doc_id;
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_TILE;
	new.tile.area = area;
	new.tile.view = view;
	new.tile.xstep = xstep;
	new.tile.ystep = ystep;
	new.tile.ctm = ctm;
	new.tile.id = id;
	
	g_array_append_val(self->stack, new);
	
	/* Return 0 to indicate we need the tile content rendered */
	return 0;
}

static void phi_node_device_end_tile(fz_context* ctx, fz_device* dev) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	if (self->stack->len < 2)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_tile called on root");
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	if (current->state != PHI_RENDER_STATE_TILE)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_tile called in invalid state");
	
	if (current->children->len == 0) {
		g_array_remove_index(self->stack, self->stack->len - 1);
		return;
	}
	
	/* Build tile pattern node */
	GskRenderNode* tile_content;
	if (current->children->len == 1) {
		tile_content = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	} else {
		tile_content = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);
	}
	
	/* Clip to view bounds */
	graphene_rect_t view_rect;
	graphene_rect_init(&view_rect,
		current->tile.view.x0,
		current->tile.view.y0,
		current->tile.view.x1 - current->tile.view.x0,
		current->tile.view.y1 - current->tile.view.y0
	);
	GskRenderNode* clipped = gsk_clip_node_new(tile_content, &view_rect);
	gsk_render_node_unref(tile_content);
	tile_content = clipped;
	
	/* Calculate tile area in device coordinates */
	fz_rect area = current->tile.area;
	float xstep = current->tile.xstep;
	float ystep = current->tile.ystep;
	fz_matrix ctm = current->tile.ctm;
	
	/* Build container with repeated tiles */
	GPtrArray* tiles = g_ptr_array_new_with_free_func((GDestroyNotify)gsk_render_node_unref);
	
	/* Calculate iteration range */
	float x0 = area.x0;
	float y0 = area.y0;
	float x1 = area.x1;
	float y1 = area.y1;
	
	/* Limit iterations for safety (very large patterns can cause issues) */
	int max_tiles_x = (int)ceilf((x1 - x0) / fabsf(xstep)) + 1;
	int max_tiles_y = (int)ceilf((y1 - y0) / fabsf(ystep)) + 1;
	int max_tiles = 10000; /* Safety limit */
	
	if (max_tiles_x * max_tiles_y > max_tiles) {
		fz_warn(ctx, "Tile pattern too large (%d x %d), limiting", max_tiles_x, max_tiles_y);
		max_tiles_x = (int)sqrtf(max_tiles);
		max_tiles_y = max_tiles_x;
	}
	
	for (int iy = 0; iy < max_tiles_y; iy++) {
		for (int ix = 0; ix < max_tiles_x; ix++) {
			float tx = x0 + ix * xstep;
			float ty = y0 + iy * ystep;
			
			/* Transform tile position */
			fz_matrix tile_ctm = fz_concat(fz_translate(tx, ty), ctm);
			GskTransform* transform = phi_node_device_transform_from_matrix(&tile_ctm);
			
			GskRenderNode* tile_instance = gsk_transform_node_new(tile_content, transform);
			gsk_transform_unref(transform);
			
			g_ptr_array_add(tiles, tile_instance);
		}
	}
	
	gsk_render_node_unref(tile_content);
	
	GskRenderNode* result = gsk_container_node_new((GskRenderNode**)tiles->pdata, tiles->len);
	g_ptr_array_unref(tiles);
	
	/* Clip to area */
	graphene_rect_t area_rect;
	fz_rect transformed_area = fz_transform_rect(area, ctm);
	graphene_rect_init(&area_rect,
		transformed_area.x0,
		transformed_area.y0,
		transformed_area.x1 - transformed_area.x0,
		transformed_area.y1 - transformed_area.y0
	);
	GskRenderNode* final = gsk_clip_node_new(result, &area_rect);
	gsk_render_node_unref(result);
	
	g_array_remove_index(self->stack, self->stack->len - 1);
	PhiRenderContext* parent = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(parent->children, final);
}

fz_device* phi_node_device_new(fz_context* ctx, GObject* context_owner) {
	g_return_val_if_fail(G_IS_OBJECT(context_owner), NULL);
	PhiNodeDevice* self = fz_new_derived_device(ctx, PhiNodeDevice);
	self->stack = g_array_new(FALSE, FALSE, sizeof(PhiRenderContext));
	g_array_set_clear_func(self->stack, (GDestroyNotify)phi_render_context_clear);
	self->context_owner = g_object_ref(context_owner);

	self->super.drop_device = phi_node_device_drop;
	
	/* Path operations */
	self->super.fill_path = phi_node_device_fill_path;
	self->super.stroke_path = phi_node_device_stroke_path;
	self->super.clip_path = phi_node_device_clip_path;
	self->super.clip_stroke_path = phi_node_device_clip_stroke_path;
	
	/* Text operations */
	self->super.fill_text = phi_node_device_fill_text;
	self->super.stroke_text = phi_node_device_stroke_text;
	self->super.clip_text = phi_node_device_clip_text;
	self->super.clip_stroke_text = phi_node_device_clip_stroke_text;
	self->super.ignore_text = phi_node_device_ignore_text;
	
	/* Image operations */
	self->super.fill_image = phi_node_device_fill_image;
	self->super.fill_image_mask = phi_node_device_fill_image_mask;
	self->super.clip_image_mask = phi_node_device_clip_image_mask;
	
	/* Shading */
	self->super.fill_shade = phi_node_device_fill_shade;
	
	/* Clipping and masking */
	self->super.pop_clip = phi_node_device_pop_clip;
	self->super.begin_mask = phi_node_device_begin_mask;
	self->super.end_mask = phi_node_device_end_mask;
	
	/* Transparency groups */
	self->super.begin_group = phi_node_device_begin_group;
	self->super.end_group = phi_node_device_end_group;
	
	/* Tiling patterns */
	self->super.begin_tile = phi_node_device_begin_tile;
	self->super.end_tile = phi_node_device_end_tile;

	PhiRenderContext root;
	phi_render_context_init(&root);
	g_array_append_val(self->stack, root);

	return (fz_device*)self;
}

GskRenderNode* phi_node_device_pop_root(fz_device* dev) {
	g_return_val_if_fail(dev->drop_device == phi_node_device_drop, NULL);
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	g_return_val_if_fail(self->stack->len == 1, NULL);

	PhiRenderContext* root = &g_array_index(self->stack, PhiRenderContext, 0);
	return gsk_container_node_new ((GskRenderNode**)root->children->pdata, root->children->len);
}

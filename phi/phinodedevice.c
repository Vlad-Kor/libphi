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

typedef enum {
	PHI_RENDER_STATE_NONE,
	PHI_RENDER_STATE_CLIP_PATH_FILL,
	PHI_RENDER_STATE_MASK,
	PHI_RENDER_STATE_IN_MASK,
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
	}
}


typedef struct {
	fz_device super;
	// GArray<PhiRenderContext>
	GArray *stack;
} PhiNodeDevice;

static void phi_node_device_drop(fz_context*, fz_device* dev) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	g_array_unref(self->stack);
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
	switch (fz_colorspace_type(ctx, cs)) {
		case FZ_COLORSPACE_RGB:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[0], .green = color[1], .blue = color[2], .alpha = alpha }, bounds);
		case FZ_COLORSPACE_BGR:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[2], .green = color[1], .blue = color[0], .alpha = alpha }, bounds);
		case FZ_COLORSPACE_GRAY:
			return gsk_color_node_new(&(GdkRGBA){ .red = color[0], .green = color[0], .blue = color[0], .alpha = alpha }, bounds);
	default:
		return NULL;
	}	
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

static void phi_node_device_clip_stroke_path(fz_context*, fz_device* dev, const fz_path*, const fz_stroke_state*, fz_matrix, fz_rect) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;

	g_critical("unimplemented: clip_stroke_path");
	
	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_NONE;

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
} PhiPixmapStorage;
static void phi_pixmap_storage_free(PhiPixmapStorage* self) {
	fz_drop_pixmap(self->ctx, self->pixmap);
	fz_drop_context(self->ctx);
	g_free(self);
}
static GskRenderNode* phi_node_device_node_from_image(fz_context* ctx, fz_image* img, fz_matrix ctm) {
	fz_pixmap* pixmap = fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
	gint components = fz_pixmap_components(ctx, pixmap);
	gint colorants = fz_pixmap_colorants(ctx, pixmap);
	gint spots = fz_pixmap_spots(ctx, pixmap);
	gint alphas = fz_pixmap_alpha(ctx, pixmap);
	if (components > 256)
		fz_throw(ctx, FZ_ERROR_LIMIT, "Pixmap has too many components (%d)", components);
	
	guint32 fingerprint = (((guint8)components) << 24) | (((guint8)colorants) << 16) | (((guint8)spots) << 8) | ((guint8)alphas);
	GdkMemoryFormat format;
	switch (fingerprint) {
		case 0x03030000:
			format = GDK_MEMORY_R8G8B8;
			break;
		case 0x01000001:
			format = GDK_MEMORY_A8;
			break;
		default:
			fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "Format of pixmap %p is unsupported (%x)", pixmap, fingerprint);
	}

	gint width = fz_pixmap_width(ctx, pixmap);
	gint height = fz_pixmap_height(ctx, pixmap);

	PhiPixmapStorage *pixmap_store = g_new(PhiPixmapStorage, 1);
	pixmap_store->ctx = fz_clone_context(ctx);
	pixmap_store->pixmap = pixmap; // takes ownership

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
	GskRenderNode *node = phi_node_device_node_from_image(ctx, img, ctm);
	node = phi_node_device_alpha(node, alpha);
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, fz_rect scissor) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	GskRenderNode *node = phi_node_device_node_from_image(ctx, img, ctm);

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_MASK;
	new.mask.mask = node;
	new.mask.mode = GSK_MASK_MODE_ALPHA;
	new.mask.scissor = scissor;

	g_array_append_val(self->stack, new);
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
	if (current->children->len == 1)
		node = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	else
		node = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);
	
	switch (current->state) {
		case PHI_RENDER_STATE_NONE:
			break;
		case PHI_RENDER_STATE_CLIP_PATH_FILL:
			/* This produces transform(fill(transform(node, ctm), fill_path), ctm).
			 * Transforming it twice (with the same matrix!) seems to make it work, but it is a total
			 * mystery to me, why that might be the case...
			 */
			node = phi_node_device_node_from_fillpath(node, current->clip_path_fill.path, current->clip_path_fill.even_odd, &current->clip_path_fill.ctm, &current->clip_path_fill.ctm);
			node = phi_node_device_scissor_clip(node, &current->clip_path_fill.scissor);
		break;
		case PHI_RENDER_STATE_MASK: {
			GskRenderNode *source = node;
			node = gsk_mask_node_new(source, current->mask.mask, current->mask.mode);
			gsk_render_node_unref(source);
			node = phi_node_device_scissor_clip(node, &current->mask.scissor);
		} break;
		case PHI_RENDER_STATE_IN_MASK:
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "pop_clip called in mask context");
			break;
	}
	g_array_remove_index(self->stack, self->stack->len - 1);
	current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	g_ptr_array_add(current->children, node);
}

static void phi_node_device_begin_mask(fz_context*, fz_device* dev, fz_rect area, int luminosity, fz_colorspace*, G_GNUC_UNUSED const float* bc, fz_color_params) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	// TODO: background color

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_IN_MASK;
	new.in_mask.mask_mode = luminosity ? GSK_MASK_MODE_LUMINANCE : GSK_MASK_MODE_ALPHA;
	new.in_mask.area = area;

	g_array_append_val(self->stack, new);
}

static void phi_node_device_end_mask(fz_context* ctx, fz_device* dev, fz_function*) {
	PhiNodeDevice* self = (PhiNodeDevice*)dev;
	
	PhiRenderContext* current = &g_array_index(self->stack, PhiRenderContext, self->stack->len - 1);
	if (current->state != PHI_RENDER_STATE_IN_MASK)
		fz_throw(ctx, FZ_ERROR_ARGUMENT, "end_mask called in invalid state");
	
	GskRenderNode* node;
	if (current->children->len == 1)
		node = gsk_render_node_ref(g_ptr_array_index(current->children, 0));
	else
		node = gsk_container_node_new((GskRenderNode**)current->children->pdata, current->children->len);

	PhiRenderContext new;
	phi_render_context_init(&new);
	new.state = PHI_RENDER_STATE_MASK;
	new.mask.mask = node;
	new.mask.mode = current->in_mask.mask_mode;
	new.mask.scissor = current->in_mask.area;

	g_array_remove_index(self->stack, self->stack->len - 1);
	g_array_append_val(self->stack, new);
}

fz_device* phi_node_device_new(fz_context* ctx) {
	PhiNodeDevice* self = fz_new_derived_device(ctx, PhiNodeDevice);
	self->stack = g_array_new(FALSE, FALSE, sizeof(PhiRenderContext));
	g_array_set_clear_func(self->stack, (GDestroyNotify)phi_render_context_clear);

	self->super.drop_device = phi_node_device_drop;
	self->super.fill_path = phi_node_device_fill_path;
	self->super.stroke_path = phi_node_device_stroke_path;
	self->super.clip_path = phi_node_device_clip_path;
	self->super.clip_stroke_path = phi_node_device_clip_stroke_path;
	self->super.fill_image = phi_node_device_fill_image;
	self->super.clip_image_mask = phi_node_device_clip_image_mask;
	self->super.pop_clip = phi_node_device_pop_clip;
	self->super.begin_mask = phi_node_device_begin_mask;
	self->super.end_mask = phi_node_device_end_mask;

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

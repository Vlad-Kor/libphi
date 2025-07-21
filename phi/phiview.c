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

#include "phi/phiview.h"

#include <math.h>

struct _PhiView {
	GtkWidget parent_instance;

	GskRenderer* renderer;

	GskRenderNode* node;
	GskRenderNode* cached_low_res;
	GskRenderNode* cached_high_res;
	guint generate_cache_source;

	guint high_res_timeout;

	gdouble x,y;
	gdouble scale;
	gdouble inverted;

	gdouble pointer_x,pointer_y;
	gdouble drag_start_x, drag_start_y;
	gdouble scale_zoom_start;
};

G_DEFINE_FINAL_TYPE (PhiView, phi_view, GTK_TYPE_WIDGET)

enum {
	PROP_NODE = 1,
	PROP_HIGH_RES_TIMEOUT,
	PROP_INVERTED,
	N_PROPERTIES
};
static GParamSpec* obj_properties[N_PROPERTIES] = { 0, };

static void phi_view_object_dispose(GObject* object) {
	PhiView* self = PHI_VIEW(object);
	g_clear_pointer(&self->node, gsk_render_node_unref);
	g_clear_pointer(&self->cached_low_res, gsk_render_node_unref);
	g_clear_pointer(&self->cached_high_res, gsk_render_node_unref);
	G_OBJECT_CLASS(phi_view_parent_class)->dispose(object);
}

static void phi_view_object_get_property(GObject* object, guint prop_id, GValue* val, GParamSpec* pspec) {
	PhiView* self = PHI_VIEW(object);
	switch (prop_id) {
		case PROP_NODE:
			g_value_set_pointer(val, phi_view_get_node(self));
			break;
		case PROP_HIGH_RES_TIMEOUT:
			g_value_set_uint(val, phi_view_get_high_res_timeout(self));
			break;
		case PROP_INVERTED:
			g_value_set_boolean(val, phi_view_is_inverted(self));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}
static void phi_view_object_set_property(GObject* object, guint prop_id, const GValue* val, GParamSpec* pspec) {
	PhiView* self = PHI_VIEW(object);
	switch (prop_id) {
		case PROP_NODE:
			phi_view_set_node(self, g_value_get_pointer(val));
			break;
		case PROP_HIGH_RES_TIMEOUT:
			phi_view_set_high_res_timeout(self, g_value_get_uint(val));
			break;
		case PROP_INVERTED:
			phi_view_set_inverted(self, g_value_get_boolean(val));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void phi_view_regenerate_high_res_cache_cb(PhiView* self) {
	self->generate_cache_source = 0;
	if (!self->renderer)
		return;

	GskTransform *transform = gsk_transform_scale(
		gsk_transform_translate(NULL, &GRAPHENE_POINT_INIT(self->x, self->y)),
		self->scale, self->scale
	);

	GskRenderNode* node;
	if (transform) {
		node = gsk_transform_node_new(self->node, transform);
		gsk_transform_unref(transform);
	} else {
		node = gsk_render_node_ref(self->node);
	}

	graphene_rect_t view;
	graphene_rect_init(&view,
		0., 0.,
		gtk_widget_get_width(GTK_WIDGET(self)),
		gtk_widget_get_height(GTK_WIDGET(self))
	);

	GskRenderNode* clipped = gsk_clip_node_new(node, &view);
	gsk_render_node_unref(node);

	GdkTexture* texture = gsk_renderer_render_texture(self->renderer, clipped, &view);
	self->cached_high_res = gsk_texture_node_new(texture, &view);
	gsk_render_node_unref(clipped);
	g_object_unref(texture);

	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void phi_view_queue_regenerate_high_res_cache(PhiView* self) {
	g_clear_pointer(&self->cached_high_res, gsk_render_node_unref);
	gtk_widget_queue_draw(GTK_WIDGET(self));

	if (self->generate_cache_source)
		g_source_remove(self->generate_cache_source);
	self->generate_cache_source = g_timeout_add_once(250, (GSourceOnceFunc)phi_view_regenerate_high_res_cache_cb, self);
}

static void phi_view_regenerate_full_cache(PhiView* self) {
	g_clear_pointer(&self->cached_low_res, gsk_render_node_unref);

	if (!self->renderer) {
		self->cached_low_res = gsk_render_node_ref(self->node);
		return;
	}

	graphene_rect_t view;
	gsk_render_node_get_bounds(self->node, &view);

	GdkTexture* texture = gsk_renderer_render_texture(self->renderer, self->node, &view);
	self->cached_low_res = gsk_texture_node_new(texture, &view);
	g_object_unref(texture);

	phi_view_queue_regenerate_high_res_cache(self);

	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void phi_view_widget_realize(GtkWidget* widget) {
	PhiView* self = PHI_VIEW(widget);
	g_assert(self->renderer == NULL);

	GTK_WIDGET_CLASS(phi_view_parent_class)->realize(widget);

	GError* err = NULL;
	if (g_strcmp0(g_getenv("GSK_RENDERER"), "cairo") != 0) {
		self->renderer = gsk_gl_renderer_new();
		if (!gsk_renderer_realize_for_display(self->renderer, gtk_widget_get_display(widget), &err)) {
			g_warning("Failed to realize GL renderer: %s", err->message);
			g_clear_error(&err);
			g_object_unref(self->renderer);
			goto realize_cairo;
		}
	} else {
realize_cairo:
		self->renderer = gsk_cairo_renderer_new();
		if (!gsk_renderer_realize_for_display(self->renderer, gtk_widget_get_display(widget), &err)) {
			g_critical("Failed to realize cairo renderer: %s", err->message);
			g_clear_error(&err);
			g_clear_object(&self->renderer);
		}
	}

	phi_view_regenerate_full_cache(self);
}
static void phi_view_widget_unrealize(GtkWidget* widget) {
	PhiView* self = PHI_VIEW(widget);
	if (self->renderer) {
		gsk_renderer_unrealize(self->renderer);
		g_object_unref(self->renderer);
		self->renderer = NULL;
	}

	GTK_WIDGET_CLASS(phi_view_parent_class)->unrealize(widget);
}

static void phi_view_widget_size_allocate(GtkWidget* widget, G_GNUC_UNUSED int width, G_GNUC_UNUSED int height, G_GNUC_UNUSED int baseline) {
	PhiView* self = PHI_VIEW(widget);
	phi_view_queue_regenerate_high_res_cache(self);
}

static void phi_view_widget_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
	PhiView* self = PHI_VIEW(widget);
	if (!self->node)
		return;

	if (self->inverted) {
		graphene_matrix_t mat;
		graphene_matrix_init_scale(&mat, -1, -1, -1);
		graphene_vec4_t off;
		graphene_vec4_init(&off, 1., 1., 1., 0.);
		gtk_snapshot_push_color_matrix(snapshot, &mat, &off);
	}

	GskRenderNode* active = self->cached_high_res != NULL ? self->cached_high_res : self->cached_low_res;

	GskRenderer* current = gtk_native_get_renderer(gtk_widget_get_native(widget));
	// The cairo renderer is generally a lot faster at drawing paths compared to sampling textures
	if (G_OBJECT_TYPE(current) == GSK_TYPE_CAIRO_RENDERER)
		active = self->node;

	if (active != self->cached_high_res) {
		gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(self->x, self->y));
		gtk_snapshot_scale(snapshot, self->scale, self->scale);
	}

	graphene_rect_t bounds;
	gsk_render_node_get_bounds(active, &bounds);
	gtk_snapshot_push_clip(snapshot, &bounds);
	gtk_snapshot_append_node(snapshot, active);
	gtk_snapshot_pop(snapshot);

	if (self->inverted)
		gtk_snapshot_pop(snapshot);
}

static void phi_view_class_init(PhiViewClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = phi_view_object_dispose;
	object_class->get_property = phi_view_object_get_property;
	object_class->set_property = phi_view_object_set_property;
	widget_class->realize = phi_view_widget_realize;
	widget_class->unrealize = phi_view_widget_unrealize;
	widget_class->size_allocate = phi_view_widget_size_allocate;
	widget_class->snapshot = phi_view_widget_snapshot;

	obj_properties[PROP_NODE] = g_param_spec_pointer("node", NULL, NULL, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
	obj_properties[PROP_HIGH_RES_TIMEOUT] = g_param_spec_uint("high-res-timeout", NULL, NULL, 10, 10000, 250, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
	obj_properties[PROP_INVERTED] = g_param_spec_boolean("inverted", NULL, NULL, FALSE, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
	g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);
}

static void phi_view_motion_move(GtkEventController*, gdouble x, gdouble y, PhiView* self) {
	self->pointer_x = x;
	self->pointer_y = y;
}

static void phi_view_motion_leave(GtkEventController*, PhiView* self) {
	self->pointer_x = NAN;
	self->pointer_y = NAN;
}

static void phi_view_zoom_begin(GtkGesture*, GdkEventSequence*, PhiView* self) {
	self->scale_zoom_start = self->scale;
}
static void phi_view_zoom_update(GtkGesture*, gdouble scale, PhiView* self) {
	gdouble old = self->scale;
	self->scale = scale * self->scale_zoom_start;
	
	if (!isnan(self->pointer_x) && !isnan(self->pointer_y)) {
		gdouble cx = self->pointer_x;
		gdouble cy = self->pointer_y;
		// $\frac{s'}{s}*(o-c)+c$, where $s'=\texttt{scale}, s=\texttt{old}, o=(x\ y)^T, c=(\texttt{cx}\ \texttt{cy})^T$
		self->x = self->scale*((self->x - cx)/old) + cx;
		self->y = self->scale*((self->y - cy)/old) + cy;
	}

	phi_view_queue_regenerate_high_res_cache(self);
}

static void phi_view_drag_begin(GtkGesture*, gdouble, gdouble, PhiView* self) {
	self->drag_start_x = self->x;
	self->drag_start_y = self->y;
}
static void phi_view_drag_update(GtkGesture*, gdouble off_x, gdouble off_y, PhiView* self) {
	self->x = self->drag_start_x + off_x;
	self->y = self->drag_start_y + off_y;
	phi_view_queue_regenerate_high_res_cache(self);
}

static void phi_view_init(PhiView* self) {
	self->x = 0.;
	self->y = 0.;
	self->scale = 1.;
	self->inverted = FALSE;
	
	self->pointer_x = NAN;
	self->pointer_y = NAN;
	
	GtkEventController* motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(phi_view_motion_move), self);
	g_signal_connect(motion, "leave", G_CALLBACK(phi_view_motion_leave), self);
	gtk_widget_add_controller(GTK_WIDGET(self), motion);

	GtkGesture* zoom = gtk_gesture_zoom_new();
	g_signal_connect(zoom, "begin", G_CALLBACK(phi_view_zoom_begin), self);
	g_signal_connect(zoom, "scale-changed", G_CALLBACK(phi_view_zoom_update), self);
	gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(zoom));

	GtkGesture* drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(phi_view_drag_begin), self);
	g_signal_connect(drag, "drag-update", G_CALLBACK(phi_view_drag_update), self);
	g_signal_connect(drag, "drag-end", G_CALLBACK(phi_view_drag_update), self);
	gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));
}

GtkWidget* phi_view_new(GskRenderNode* node) {
	return g_object_new(PHI_TYPE_VIEW, "node", node, NULL);
}

GskRenderNode* phi_view_get_node(PhiView* self) {
	g_return_val_if_fail(PHI_IS_VIEW(self), NULL);
	return self->node;
}

void phi_view_set_node(PhiView* self, GskRenderNode* node) {
	g_return_if_fail(PHI_IS_VIEW(self));
	g_clear_pointer(&self->node, gsk_render_node_unref);
	g_clear_pointer(&self->cached_low_res, gsk_render_node_unref);
	g_clear_pointer(&self->cached_high_res, gsk_render_node_unref);
	if (node) {
		self->node = gsk_render_node_ref(node);
		phi_view_regenerate_full_cache(self);
	}
	g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_NODE]);
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

guint phi_view_get_high_res_timeout(PhiView* self) {
	g_return_val_if_fail(PHI_IS_VIEW(self), 0);
	return self->high_res_timeout;
}

void phi_view_set_high_res_timeout(PhiView* self, guint timeout) {
	g_return_if_fail(PHI_IS_VIEW(self));
	self->high_res_timeout = timeout;
	g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_HIGH_RES_TIMEOUT]);
	if (self->generate_cache_source)
		phi_view_queue_regenerate_high_res_cache(self);
}

gboolean phi_view_is_inverted(PhiView* self) {
	g_return_val_if_fail(PHI_IS_VIEW(self), FALSE);
	return self->inverted;
}

void phi_view_set_inverted(PhiView* self, gboolean inverted) {
	g_return_if_fail(PHI_IS_VIEW(self));
	self->inverted = inverted;
	g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_INVERTED]);
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

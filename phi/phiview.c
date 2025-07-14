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

	GdkPaintable* paintable;
	gdouble x,y;
	gdouble scale;
	gdouble inverted;

	gdouble pointer_x,pointer_y;
	gdouble drag_start_x, drag_start_y;
	gdouble scale_zoom_start;
};

G_DEFINE_FINAL_TYPE (PhiView, phi_view, GTK_TYPE_WIDGET)

enum {
	PROP_PAINTABLE = 1,
	PROP_INVERTED,
	N_PROPERTIES
};
static GParamSpec* obj_properties[N_PROPERTIES] = { 0, };

static void phi_view_object_dispose(GObject* object) {
	PhiView* self = PHI_VIEW(object);
	g_clear_object(&self->paintable);
	G_OBJECT_CLASS(phi_view_parent_class)->dispose(object);
}

static void phi_view_object_get_property(GObject* object, guint prop_id, GValue* val, GParamSpec* pspec) {
	PhiView* self = PHI_VIEW(object);
	switch (prop_id) {
		case PROP_PAINTABLE:
			g_value_set_object(val, phi_view_get_paintable(self));
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
		case PROP_PAINTABLE:
			phi_view_set_paintable(self, g_value_get_object(val));
			break;
		case PROP_INVERTED:
			phi_view_set_inverted(self, g_value_get_boolean(val));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void phi_view_widget_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
	PhiView* self = PHI_VIEW(widget);
	if (!self->paintable)
		return;

	if (self->inverted) {
		graphene_matrix_t mat;
		graphene_matrix_init_scale(&mat, -1, -1, -1);
		graphene_vec4_t off;
		graphene_vec4_init(&off, 1., 1., 1., 0.);
		gtk_snapshot_push_color_matrix(snapshot, &mat, &off);
	}

	double w = gdk_paintable_get_intrinsic_width(self->paintable) * self->scale;
	double h = gdk_paintable_get_intrinsic_height(self->paintable) * self->scale;

	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(self->x, self->y));
	gtk_snapshot_scale(snapshot, self->scale, self->scale);
	gdk_paintable_snapshot(self->paintable, snapshot, w / self->scale, h / self->scale);

	if (self->inverted)
		gtk_snapshot_pop(snapshot);
}

static void phi_view_class_init(PhiViewClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = phi_view_object_dispose;
	object_class->get_property = phi_view_object_get_property;
	object_class->set_property = phi_view_object_set_property;
	widget_class->snapshot = phi_view_widget_snapshot;

	obj_properties[PROP_PAINTABLE] = g_param_spec_object("paintable", NULL, NULL, GDK_TYPE_PAINTABLE, G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
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

	gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void phi_view_drag_begin(GtkGesture*, gdouble, gdouble, PhiView* self) {
	self->drag_start_x = self->x;
	self->drag_start_y = self->y;
}
static void phi_view_drag_update(GtkGesture*, gdouble off_x, gdouble off_y, PhiView* self) {
	self->x = self->drag_start_x + off_x;
	self->y = self->drag_start_y + off_y;
	gtk_widget_queue_draw(GTK_WIDGET(self));
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

GtkWidget* phi_view_new(GdkPaintable* paintable) {
	return g_object_new(PHI_TYPE_VIEW, "paintable", paintable, NULL);
}

GdkPaintable* phi_view_get_paintable(PhiView* self) {
	g_return_val_if_fail(PHI_IS_VIEW(self), NULL);
	return self->paintable;
}

void phi_view_set_paintable(PhiView* self, GdkPaintable* paintable) {
	g_return_if_fail(PHI_IS_VIEW(self));
	g_clear_object(&self->paintable);
	if (paintable)
		self->paintable = g_object_ref(paintable);
	g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_PAINTABLE]);
	gtk_widget_queue_draw(GTK_WIDGET(self));
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

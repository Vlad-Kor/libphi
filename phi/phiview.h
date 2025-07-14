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

#ifndef __PHIVIEW_H__
#define __PHIVIEW_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PHI_TYPE_VIEW (phi_view_get_type())
G_DECLARE_FINAL_TYPE (PhiView, phi_view, PHI, VIEW, GtkWidget)

GtkWidget* phi_view_new(GdkPaintable* paintable);

GdkPaintable* phi_view_get_paintable(PhiView* self);
void phi_view_set_paintable(PhiView* self, GdkPaintable* paintable);

gboolean phi_view_is_inverted(PhiView* self);
void phi_view_set_inverted(PhiView* self, gboolean inverted);

G_END_DECLS

#endif // __PHIVIEW_H__

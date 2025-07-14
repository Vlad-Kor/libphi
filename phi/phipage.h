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

#ifndef __PHIPAGE_H__
#define __PHIPAGE_H__

#include <gtk/gtk.h>

#include <phi/phierrors.h>

G_BEGIN_DECLS

#define PHI_TYPE_PAGE (phi_page_get_type())
G_DECLARE_FINAL_TYPE(PhiPage, phi_page, PHI, PAGE, GObject)

GskRenderNode* phi_page_render_to_node(PhiPage* self, GError** error);
GdkPaintable* phi_page_render_to_paintable(PhiPage* self, GError** error);

G_END_DECLS

#endif // __PHIPAGE_H__

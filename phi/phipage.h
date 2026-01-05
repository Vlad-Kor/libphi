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

void phi_page_get_size(PhiPage* self, gfloat* width, gfloat* height);
void phi_page_get_bounds(PhiPage* self, gfloat* x0, gfloat* y0, gfloat* x1, gfloat* y1);
GskRenderNode* phi_page_render_to_node(PhiPage* self, GError** error);
GdkPaintable* phi_page_render_to_paintable(PhiPage* self, GError** error);

/* Links on page */
typedef struct _PhiLink PhiLink;
struct _PhiLink {
	graphene_rect_t rect;
	gchar* uri;
	PhiLink* next;
};

PhiLink* phi_page_get_links(PhiPage* self);
void phi_link_free(PhiLink* link);

/* Text selection and search */
typedef struct _PhiTextQuad PhiTextQuad;
struct _PhiTextQuad {
	graphene_point_t ul;  /* upper left */
	graphene_point_t ur;  /* upper right */
	graphene_point_t ll;  /* lower left */
	graphene_point_t lr;  /* lower right */
};

/* Search for text on page, returns array of quads for highlighting matches */
gint phi_page_search_text(PhiPage* self, const gchar* needle, PhiTextQuad* quads, gint max_quads);

/* Get highlighted quads for text selection between two points */
gint phi_page_get_selection_quads(PhiPage* self, graphene_point_t* start, graphene_point_t* end, PhiTextQuad* quads, gint max_quads);

/* Copy selected text between two points */
gchar* phi_page_copy_selection(PhiPage* self, graphene_point_t* start, graphene_point_t* end);

/* Get all text on page */
gchar* phi_page_get_text(PhiPage* self);

G_END_DECLS

#endif // __PHIPAGE_H__

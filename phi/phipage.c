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

#include "phi/phipageprivate.h"

#include "phi/phidocumentprivate.h"
#include "phi/phinodedeviceprivate.h"

G_DEFINE_FINAL_TYPE(PhiPage, phi_page, G_TYPE_OBJECT)

static void phi_page_object_dispose(GObject* object) {
	PhiPage* self = PHI_PAGE(object);
	if (self->page) {
		fz_drop_page(self->document->ctx, self->page);
		self->page = NULL;
	}
	g_clear_weak_pointer(&self->document);
	G_OBJECT_CLASS(phi_page_parent_class)->dispose(object);
}

static void phi_page_class_init(PhiPageClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = phi_page_object_dispose;
}

static void phi_page_init(PhiPage* self) {
	self->document = NULL;
	self->page = NULL;
}

void phi_page_get_size(PhiPage* self, gfloat* width, gfloat* height) {
	g_return_if_fail(PHI_IS_PAGE(self));
	
	fz_rect bounds = fz_bound_page(self->document->ctx, self->page);
	if (width)
		*width = bounds.x1 - bounds.x0;
	if (height)
		*height = bounds.y1 - bounds.y0;
}

GskRenderNode* phi_page_render_to_node(PhiPage* self, GError** error) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	
	fz_device* device = NULL;
	GskRenderNode* ret = NULL;
	fz_try(self->document->ctx) {
		device = phi_node_device_new(self->document->ctx);
		fz_run_page(self->document->ctx, self->page, device, fz_identity, NULL);
		ret = phi_node_device_pop_root(device);
	} fz_always(self->document->ctx) {
		if (device)
			fz_drop_device(self->document->ctx, device);
	} fz_catch(self->document->ctx) {
		if (ret)
			gsk_render_node_unref(ret);
		g_set_error_literal(error, PHI_MU_ERROR, fz_caught(self->document->ctx), fz_caught_message(self->document->ctx));
		return NULL;
	}
	return ret;
}

GdkPaintable* phi_page_render_to_paintable(PhiPage* self, GError** error) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	
	GskRenderNode* node = phi_page_render_to_node(self, error);
	if (!node)
		return NULL;

	GtkSnapshot* snapshot = gtk_snapshot_new();
	gtk_snapshot_append_node(snapshot, node);
	
	gsk_render_node_unref(node);
	GdkPaintable* ret = gtk_snapshot_free_to_paintable(snapshot, NULL);
	return ret;
}

PhiLink* phi_page_get_links(PhiPage* self) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	
	fz_link* links = NULL;
	PhiLink* result = NULL;
	PhiLink** tail = &result;
	
	fz_try(self->document->ctx) {
		links = fz_load_links(self->document->ctx, self->page);
		
		for (fz_link* link = links; link; link = link->next) {
			PhiLink* phi_link = g_new0(PhiLink, 1);
			graphene_rect_init(&phi_link->rect, 
				link->rect.x0, link->rect.y0,
				link->rect.x1 - link->rect.x0,
				link->rect.y1 - link->rect.y0);
			phi_link->uri = g_strdup(link->uri);
			phi_link->next = NULL;
			
			*tail = phi_link;
			tail = &phi_link->next;
		}
	} fz_always(self->document->ctx) {
		if (links)
			fz_drop_link(self->document->ctx, links);
	} fz_catch(self->document->ctx) {
		phi_link_free(result);
		return NULL;
	}
	
	return result;
}

void phi_link_free(PhiLink* link) {
	while (link) {
		PhiLink* next = link->next;
		g_free(link->uri);
		g_free(link);
		link = next;
	}
}

/* Helper to convert fz_quad to PhiTextQuad */
static void fz_quad_to_phi_quad(const fz_quad* fq, PhiTextQuad* pq) {
	pq->ul.x = fq->ul.x;
	pq->ul.y = fq->ul.y;
	pq->ur.x = fq->ur.x;
	pq->ur.y = fq->ur.y;
	pq->ll.x = fq->ll.x;
	pq->ll.y = fq->ll.y;
	pq->lr.x = fq->lr.x;
	pq->lr.y = fq->lr.y;
}

gint phi_page_search_text(PhiPage* self, const gchar* needle, PhiTextQuad* quads, gint max_quads) {
	g_return_val_if_fail(PHI_IS_PAGE(self), 0);
	g_return_val_if_fail(needle != NULL, 0);
	
	if (max_quads <= 0 || !quads)
		return 0;
	
	fz_stext_page* stext = NULL;
	fz_quad* fz_quads = NULL;
	gint count = 0;
	
	fz_try(self->document->ctx) {
		stext = fz_new_stext_page_from_page(self->document->ctx, self->page, NULL);
		fz_quads = g_new(fz_quad, max_quads);
		
		count = fz_search_stext_page(self->document->ctx, stext, needle, NULL, fz_quads, max_quads);
		
		for (gint i = 0; i < count && i < max_quads; i++) {
			fz_quad_to_phi_quad(&fz_quads[i], &quads[i]);
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
		g_free(fz_quads);
	} fz_catch(self->document->ctx) {
		return 0;
	}
	
	return count;
}

gint phi_page_get_selection_quads(PhiPage* self, graphene_point_t* start, graphene_point_t* end, PhiTextQuad* quads, gint max_quads) {
	g_return_val_if_fail(PHI_IS_PAGE(self), 0);
	g_return_val_if_fail(start != NULL && end != NULL, 0);
	
	if (max_quads <= 0 || !quads)
		return 0;
	
	fz_stext_page* stext = NULL;
	fz_quad* fz_quads = NULL;
	gint count = 0;
	
	fz_try(self->document->ctx) {
		stext = fz_new_stext_page_from_page(self->document->ctx, self->page, NULL);
		fz_quads = g_new(fz_quad, max_quads);
		
		fz_point a = { start->x, start->y };
		fz_point b = { end->x, end->y };
		
		count = fz_highlight_selection(self->document->ctx, stext, a, b, fz_quads, max_quads);
		
		for (gint i = 0; i < count && i < max_quads; i++) {
			fz_quad_to_phi_quad(&fz_quads[i], &quads[i]);
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
		g_free(fz_quads);
	} fz_catch(self->document->ctx) {
		return 0;
	}
	
	return count;
}

gchar* phi_page_copy_selection(PhiPage* self, graphene_point_t* start, graphene_point_t* end) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	g_return_val_if_fail(start != NULL && end != NULL, NULL);
	
	fz_stext_page* stext = NULL;
	gchar* result = NULL;
	
	fz_try(self->document->ctx) {
		stext = fz_new_stext_page_from_page(self->document->ctx, self->page, NULL);
		
		fz_point a = { start->x, start->y };
		fz_point b = { end->x, end->y };
		
		char* text = fz_copy_selection(self->document->ctx, stext, a, b, 0);
		if (text) {
			result = g_strdup(text);
			fz_free(self->document->ctx, text);
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
	} fz_catch(self->document->ctx) {
		return NULL;
	}
	
	return result;
}

gchar* phi_page_get_text(PhiPage* self) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	
	fz_stext_page* stext = NULL;
	gchar* result = NULL;
	
	fz_try(self->document->ctx) {
		stext = fz_new_stext_page_from_page(self->document->ctx, self->page, NULL);
		
		/* Get page bounds and copy all text */
		fz_rect bounds = fz_bound_page(self->document->ctx, self->page);
		char* text = fz_copy_rectangle(self->document->ctx, stext, bounds, 0);
		if (text) {
			result = g_strdup(text);
			fz_free(self->document->ctx, text);
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
	} fz_catch(self->document->ctx) {
		return NULL;
	}
	
	return result;
}

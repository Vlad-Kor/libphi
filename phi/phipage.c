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

void phi_page_detach_document(PhiPage* self) {
	g_return_if_fail(PHI_IS_PAGE(self));

	/* A GListModel consumer can keep a page alive after PhiDocument releases
	 * its page cache. Drop the MuPDF page while the owning context is still
	 * valid, then leave the detached GObject safe to dispose later. */
	if (self->stext && self->document) {
		fz_drop_stext_page(self->document->ctx, self->stext);
	}
	self->stext = NULL;
	if (self->page && self->document) {
		fz_drop_page(self->document->ctx, self->page);
	}
	self->page = NULL;
	g_clear_weak_pointer(&self->document);
}

static void phi_page_object_dispose(GObject* object) {
	PhiPage* self = PHI_PAGE(object);
	phi_page_detach_document(self);
	G_OBJECT_CLASS(phi_page_parent_class)->dispose(object);
}

static void phi_page_class_init(PhiPageClass* klass) {
	GObjectClass* object_class = G_OBJECT_CLASS(klass);
	object_class->dispose = phi_page_object_dispose;
}

static void phi_page_init(PhiPage* self) {
	self->document = NULL;
	self->page = NULL;
	self->stext = NULL;
	self->bounds_valid = FALSE;
}

void phi_page_get_size(PhiPage* self, gfloat* width, gfloat* height) {
	g_return_if_fail(PHI_IS_PAGE(self));
	
	if (!self->bounds_valid) {
		self->bounds = fz_bound_page(self->document->ctx, self->page);
		self->bounds_valid = TRUE;
	}
	if (width)
		*width = self->bounds.x1 - self->bounds.x0;
	if (height)
		*height = self->bounds.y1 - self->bounds.y0;
}

void phi_page_get_bounds(PhiPage* self, gfloat* x0, gfloat* y0, gfloat* x1, gfloat* y1) {
	g_return_if_fail(PHI_IS_PAGE(self));
	
	if (!self->bounds_valid) {
		self->bounds = fz_bound_page(self->document->ctx, self->page);
		self->bounds_valid = TRUE;
	}
	if (x0) *x0 = self->bounds.x0;
	if (y0) *y0 = self->bounds.y0;
	if (x1) *x1 = self->bounds.x1;
	if (y1) *y1 = self->bounds.y1;
}

GskRenderNode* phi_page_render_to_node(PhiPage* self, GError** error) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	
	fz_device* device = NULL;
	GskRenderNode* ret = NULL;
	fz_try(self->document->ctx) {
		device = phi_node_device_new(self->document->ctx,
			G_OBJECT(self->document));
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

/*
 * Some PDF generators paint accents as separate spacing glyphs before the
 * character they decorate.  MuPDF quite reasonably preserves that content
 * stream order, which turns "präsentiert" into "pr¨asentiert"
 * when copied and also prevents a search for the correctly composed word.
 *
 * Only repair pairs whose glyph boxes overlap and whose base/combining form
 * has a canonical composition.  The geometry check is important: a literal
 * spacing accent followed by a letter must remain two separate characters.
 */
static gunichar phi_spacing_diacritic_to_combining(gunichar c) {
	switch (c) {
	case 0x005e: return 0x0302; /* CIRCUMFLEX ACCENT */
	case 0x0060: return 0x0300; /* GRAVE ACCENT */
	case 0x007e: return 0x0303; /* TILDE */
	case 0x00a8: return 0x0308; /* DIAERESIS */
	case 0x00af: return 0x0304; /* MACRON */
	case 0x00b4: return 0x0301; /* ACUTE ACCENT */
	case 0x00b8: return 0x0327; /* CEDILLA */
	case 0x02c6: return 0x0302; /* MODIFIER LETTER CIRCUMFLEX */
	case 0x02c7: return 0x030c; /* CARON */
	case 0x02d8: return 0x0306; /* BREVE */
	case 0x02d9: return 0x0307; /* DOT ABOVE */
	case 0x02da: return 0x030a; /* RING ABOVE */
	case 0x02db: return 0x0328; /* OGONEK */
	case 0x02dc: return 0x0303; /* SMALL TILDE */
	case 0x02dd: return 0x030b; /* DOUBLE ACUTE ACCENT */
	default: return 0;
	}
}

static gboolean phi_diacritic_overlaps_base(const fz_stext_char* mark,
		const fz_stext_char* base) {
	fz_rect mr = fz_rect_from_quad(mark->quad);
	fz_rect br = fz_rect_from_quad(base->quad);
	float mark_center_x = (mr.x0 + mr.x1) / 2.0f;

	return br.x1 > br.x0 && mark_center_x >= br.x0 && mark_center_x <= br.x1;
}

static gunichar phi_compose_diacritic(gunichar base, gunichar combining) {
	gchar source[12];
	gint length = g_unichar_to_utf8(base, source);
	length += g_unichar_to_utf8(combining, source + length);
	source[length] = '\0';

	gchar* normalized = g_utf8_normalize(source, length, G_NORMALIZE_NFC);
	gunichar composed = normalized && g_utf8_strlen(normalized, -1) == 1
		? g_utf8_get_char(normalized) : 0;
	g_free(normalized);
	return composed;
}

static void phi_stext_fix_separate_diacritics(fz_stext_page* page) {
	for (fz_stext_block* block = page->first_block; block; block = block->next) {
		if (block->type != FZ_STEXT_BLOCK_TEXT)
			continue;

		for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
			for (fz_stext_char* mark = line->first_char; mark && mark->next; mark = mark->next) {
				fz_stext_char* base = mark->next;
				gunichar combining = phi_spacing_diacritic_to_combining(mark->c);
				gunichar composed = combining
					? phi_compose_diacritic(base->c, combining) : 0;

				if (!combining || !g_unichar_isalpha(base->c) ||
					!phi_diacritic_overlaps_base(mark, base) ||
					!composed)
					continue;

				/* Collapse the overlaid pair so text and quad indexes remain 1:1. */
				mark->c = composed;
				mark->bidi = base->bidi;
				mark->flags = base->flags;
				mark->argb = base->argb;
				mark->origin = base->origin;
				mark->quad = base->quad;
				mark->size = base->size;
				mark->font = base->font;
				mark->next = base->next;
				if (line->last_char == base)
					line->last_char = mark;
			}
		}
	}
}

static fz_stext_page* phi_page_extract_stext(PhiPage* self) {
	if (!self->stext) {
		self->stext = fz_new_stext_page_from_page(
			self->document->ctx, self->page, NULL);
		phi_stext_fix_separate_diacritics(self->stext);
	}
	return fz_keep_stext_page(self->document->ctx, self->stext);
}

static gchar* phi_normalize_extracted_text(const char* text) {
	gchar* normalized = g_utf8_normalize(text, -1, G_NORMALIZE_NFC);
	return normalized ? normalized : g_strdup(text);
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
		stext = phi_page_extract_stext(self);
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
		stext = phi_page_extract_stext(self);
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

gboolean phi_page_select_word_at(PhiPage* self, graphene_point_t* point, graphene_point_t* word_start, graphene_point_t* word_end) {
	g_return_val_if_fail(PHI_IS_PAGE(self), FALSE);
	g_return_val_if_fail(point != NULL && word_start != NULL && word_end != NULL, FALSE);
	
	fz_stext_page* stext = NULL;
	gboolean found = FALSE;
	
	fz_try(self->document->ctx) {
		stext = phi_page_extract_stext(self);
		
		/* Iterate through text blocks, lines, and chars to find word at point */
		for (fz_stext_block* block = stext->first_block; block && !found; block = block->next) {
			if (block->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			
			for (fz_stext_line* line = block->u.t.first_line; line && !found; line = line->next) {
				/* Check if point is roughly on this line */
				fz_rect line_bbox = fz_empty_rect;
				for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
					fz_rect char_bbox = fz_rect_from_quad(ch->quad);
					line_bbox = fz_union_rect(line_bbox, char_bbox);
				}
				
				if (point->y < line_bbox.y0 || point->y > line_bbox.y1)
					continue;
				
				/* Find the character at point */
				fz_stext_char* target_char = NULL;
				for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
					fz_rect char_bbox = fz_rect_from_quad(ch->quad);
					if (point->x >= char_bbox.x0 && point->x <= char_bbox.x1) {
						target_char = ch;
						break;
					}
				}
				
				if (!target_char)
					continue;
				
				/* If clicked on whitespace, skip */
				if (g_unichar_isspace(target_char->c))
					continue;
				
				/* Find word boundaries */
				fz_stext_char* word_begin = target_char;
				fz_stext_char* word_last = target_char;
				
				/* Search backwards for word start */
				for (fz_stext_char* ch = line->first_char; ch && ch != target_char; ch = ch->next) {
					if (g_unichar_isspace(ch->c) || g_unichar_ispunct(ch->c)) {
						word_begin = ch->next;
					}
				}
				if (!word_begin)
					word_begin = line->first_char;
				
				/* Search forwards for word end */
				for (fz_stext_char* ch = target_char; ch; ch = ch->next) {
					if (g_unichar_isspace(ch->c) || g_unichar_ispunct(ch->c))
						break;
					word_last = ch;
				}
				
				/* Get bounding boxes */
				fz_rect start_bbox = fz_rect_from_quad(word_begin->quad);
				fz_rect end_bbox = fz_rect_from_quad(word_last->quad);
				
				word_start->x = start_bbox.x0;
				word_start->y = (start_bbox.y0 + start_bbox.y1) / 2;
				word_end->x = end_bbox.x1;
				word_end->y = (end_bbox.y0 + end_bbox.y1) / 2;
				
				found = TRUE;
			}
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
	} fz_catch(self->document->ctx) {
		return FALSE;
	}
	
	return found;
}

gboolean phi_page_has_text_at(PhiPage* self, graphene_point_t* point) {
	g_return_val_if_fail(PHI_IS_PAGE(self), FALSE);
	g_return_val_if_fail(point != NULL, FALSE);

	fz_stext_page* stext = NULL;
	gboolean found = FALSE;
	fz_try(self->document->ctx) {
		stext = phi_page_extract_stext(self);
		for (fz_stext_block* block = stext->first_block;
			block && !found; block = block->next) {
			if (block->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (fz_stext_line* line = block->u.t.first_line;
				line && !found; line = line->next) {
				fz_rect line_bbox = fz_empty_rect;
				for (fz_stext_char* ch = line->first_char;
					ch; ch = ch->next)
					line_bbox = fz_union_rect(
						line_bbox, fz_rect_from_quad(ch->quad));
				found = line->first_char &&
					point->x >= line_bbox.x0 && point->x <= line_bbox.x1 &&
					point->y >= line_bbox.y0 && point->y <= line_bbox.y1;
			}
		}
	} fz_always(self->document->ctx) {
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
	} fz_catch(self->document->ctx) {
		return FALSE;
	}
	return found;
}

static gboolean phi_sentence_terminator(int character) {
	return character == '.' || character == '?' || character == '!';
}

gboolean phi_page_select_sentence_at(PhiPage* self, graphene_point_t* point,
		graphene_point_t* sentence_start, graphene_point_t* sentence_end) {
	g_return_val_if_fail(PHI_IS_PAGE(self), FALSE);
	g_return_val_if_fail(point != NULL && sentence_start != NULL &&
		sentence_end != NULL, FALSE);

	fz_stext_page* stext = NULL;
	GPtrArray* characters = NULL;
	gboolean found = FALSE;
	fz_try(self->document->ctx) {
		stext = phi_page_extract_stext(self);
		for (fz_stext_block* block = stext->first_block;
			block && !found; block = block->next) {
			if (block->type != FZ_STEXT_BLOCK_TEXT)
				continue;

			fz_stext_char* target = NULL;
			for (fz_stext_line* line = block->u.t.first_line;
				line && !target; line = line->next) {
				for (fz_stext_char* ch = line->first_char;
					ch; ch = ch->next) {
					fz_rect bbox = fz_rect_from_quad(ch->quad);
					if (point->x >= bbox.x0 && point->x <= bbox.x1 &&
						point->y >= bbox.y0 && point->y <= bbox.y1) {
						target = ch;
						break;
					}
				}
			}
			if (!target || g_unichar_isspace(target->c))
				continue;

			characters = g_ptr_array_new();
			guint target_index = 0;
			for (fz_stext_line* line = block->u.t.first_line;
				line; line = line->next) {
				for (fz_stext_char* ch = line->first_char;
					ch; ch = ch->next) {
					if (ch == target)
						target_index = characters->len;
					g_ptr_array_add(characters, ch);
				}
			}

			guint begin_index = 0;
			for (guint i = 0; i < target_index; i++) {
				fz_stext_char* ch = g_ptr_array_index(characters, i);
				if (phi_sentence_terminator(ch->c))
					begin_index = i + 1;
			}
			while (begin_index < characters->len) {
				fz_stext_char* ch = g_ptr_array_index(
					characters, begin_index);
				if (!g_unichar_isspace(ch->c))
					break;
				begin_index++;
			}

			guint end_index = characters->len - 1;
			for (guint i = target_index; i < characters->len; i++) {
				fz_stext_char* ch = g_ptr_array_index(characters, i);
				if (phi_sentence_terminator(ch->c)) {
					end_index = i;
					break;
				}
			}

			fz_stext_char* begin = g_ptr_array_index(
				characters, MIN(begin_index, target_index));
			fz_stext_char* end = g_ptr_array_index(characters, end_index);
			fz_rect begin_bbox = fz_rect_from_quad(begin->quad);
			fz_rect end_bbox = fz_rect_from_quad(end->quad);
			sentence_start->x = begin_bbox.x0;
			sentence_start->y = (begin_bbox.y0 + begin_bbox.y1) / 2;
			sentence_end->x = end_bbox.x1;
			sentence_end->y = (end_bbox.y0 + end_bbox.y1) / 2;
			found = TRUE;
		}
	} fz_always(self->document->ctx) {
		g_clear_pointer(&characters, g_ptr_array_unref);
		if (stext)
			fz_drop_stext_page(self->document->ctx, stext);
	} fz_catch(self->document->ctx) {
		return FALSE;
	}
	return found;
}

gchar* phi_page_copy_selection(PhiPage* self, graphene_point_t* start, graphene_point_t* end) {
	g_return_val_if_fail(PHI_IS_PAGE(self), NULL);
	g_return_val_if_fail(start != NULL && end != NULL, NULL);
	
	fz_stext_page* stext = NULL;
	gchar* result = NULL;
	
	fz_try(self->document->ctx) {
		stext = phi_page_extract_stext(self);
		
		fz_point a = { start->x, start->y };
		fz_point b = { end->x, end->y };
		
		char* text = fz_copy_selection(self->document->ctx, stext, a, b, 0);
		if (text) {
			result = phi_normalize_extracted_text(text);
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
		stext = phi_page_extract_stext(self);
		
		/* Get page bounds and copy all text */
		fz_rect bounds = fz_bound_page(self->document->ctx, self->page);
		char* text = fz_copy_rectangle(self->document->ctx, stext, bounds, 0);
		if (text) {
			result = phi_normalize_extracted_text(text);
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

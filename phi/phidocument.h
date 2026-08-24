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

#ifndef __PHIDOCUMENT_H__
#define __PHIDOCUMENT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include <phi/phierrors.h>
#include <phi/phipage.h>

G_BEGIN_DECLS

#define PHI_TYPE_DOCUMENT (phi_document_get_type())
G_DECLARE_FINAL_TYPE(PhiDocument, phi_document, PHI, DOCUMENT, GObject)

PhiDocument* phi_document_new_from_stream(GInputStream* stream, const gchar* magic, GError** error);
PhiDocument* phi_document_new_from_file(GFile* file, GError** error);

gint phi_document_get_n_pages(PhiDocument* self);
PhiPage* phi_document_get_page(PhiDocument* self, gint pageno, GError** error);

/* Common document metadata exposed without leaking MuPDF's key strings into
 * applications. The returned string belongs to the caller; NULL means that
 * the document does not provide the requested field. */
typedef enum {
	PHI_DOCUMENT_METADATA_FORMAT,
	PHI_DOCUMENT_METADATA_ENCRYPTION,
	PHI_DOCUMENT_METADATA_TITLE,
	PHI_DOCUMENT_METADATA_AUTHOR,
	PHI_DOCUMENT_METADATA_SUBJECT,
	PHI_DOCUMENT_METADATA_KEYWORDS,
	PHI_DOCUMENT_METADATA_CREATOR,
	PHI_DOCUMENT_METADATA_PRODUCER,
	PHI_DOCUMENT_METADATA_CREATION_DATE,
	PHI_DOCUMENT_METADATA_MODIFICATION_DATE,
} PhiDocumentMetadata;

gchar* phi_document_dup_metadata(PhiDocument* self,
	PhiDocumentMetadata metadata);

/*
 * Render a page with an independent MuPDF context. This is intended for a
 * serialized background worker; the returned image surface may be handed to
 * the GTK thread after this function returns.
 */
cairo_surface_t* phi_document_render_thumbnail(PhiDocument* self, gint pageno,
	gint max_width, gint max_height, GError** error);

/* Build an immutable page scene graph using a file-backed MuPDF context that
 * is independent from the document's interactive context. This function is
 * safe to call from a worker thread; calls for one document are serialized. */
GskRenderNode* phi_document_render_page_node(PhiDocument* self, gint pageno,
	GCancellable* cancellable, GError** error);

/* Outline (Table of Contents) */
typedef struct _PhiOutlineItem PhiOutlineItem;
struct _PhiOutlineItem {
	gchar* title;
	gchar* uri;
	gint page;
	PhiOutlineItem* children;
	PhiOutlineItem* next;
};

PhiOutlineItem* phi_document_get_outline(PhiDocument* self);
void phi_outline_item_free(PhiOutlineItem* item);

/* Link destination resolution */
typedef struct {
	gint page;
	gfloat x, y;
	gfloat zoom;
} PhiLinkDest;

gboolean phi_document_resolve_link(PhiDocument* self, const gchar* uri, PhiLinkDest* dest);

G_END_DECLS

#endif // __PHIDOCUMENT_H__

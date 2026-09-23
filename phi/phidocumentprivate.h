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

#ifndef __PHIDOCUMENTPRIVATE_H__
#define __PHIDOCUMENTPRIVATE_H__

#include "phi/phidocument.h"

#include <mupdf/fitz.h>

G_BEGIN_DECLS

struct _PhiDocument {
	GObject parent_instance;
	
	GMutex ctx_locks[FZ_LOCK_MAX];

	fz_context* ctx;
	fz_document* document;

	/* Source that worker contexts reopen the document from: a file, or
	 * immutable bytes that MuPDF streams read in place. Stream-only
	 * documents have neither. */
	GFile* source_file;
	GBytes* source_bytes;
	gchar* source_magic;

	/* Independent, serialized renderer used only by the thumbnail worker. */
	GMutex thumbnail_lock;
	fz_context* thumbnail_ctx;
	fz_document* thumbnail_document;

	/* Independent renderer for asynchronous page rasters and tiles. */
	GMutex render_lock;
	fz_context* render_ctx;
	fz_document* render_document;
	
	gint n_pages;
	PhiPage** pages;
};

G_END_DECLS

#endif // __PHIDOCUMENTPRIVATE_H__

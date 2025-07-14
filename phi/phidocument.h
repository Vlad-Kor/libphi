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

PhiPage* phi_document_get_page(PhiDocument* self, gint pageno, GError** error);

G_END_DECLS

#endif // __PHIDOCUMENT_H__

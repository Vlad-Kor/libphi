/*
 * Phi PDF Viewer - Document properties dialog
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_DOCUMENT_PROPERTIES_H
#define PDFV_DOCUMENT_PROPERTIES_H

#include <adwaita.h>
#include <phi/phidocument.h>

G_BEGIN_DECLS

void pdfv_document_properties_present(GtkWidget *parent, GFile *file,
                                      PhiDocument *document,
                                      gint page_number);

/* Formatting helpers are kept here so the properties presentation and its
 * focused tests share the exact same rules. */
gchar *pdfv_document_properties_format_page_size(gfloat width_points,
                                                 gfloat height_points);
gchar *pdfv_document_properties_format_pdf_date(const gchar *value);

G_END_DECLS

#endif /* PDFV_DOCUMENT_PROPERTIES_H */

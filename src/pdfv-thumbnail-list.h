/* Asynchronous PDF thumbnail browser for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_THUMBNAIL_LIST_H
#define PDFV_THUMBNAIL_LIST_H

#include <gtk/gtk.h>
#include <phi/phidocument.h>

G_BEGIN_DECLS

#define PDFV_TYPE_THUMBNAIL_LIST (pdfv_thumbnail_list_get_type())
G_DECLARE_FINAL_TYPE(PdfvThumbnailList, pdfv_thumbnail_list, PDFV,
                     THUMBNAIL_LIST, GtkWidget)

PdfvThumbnailList *pdfv_thumbnail_list_new(void);
void pdfv_thumbnail_list_set_document(PdfvThumbnailList *self,
                                      PhiDocument *document);

G_END_DECLS

#endif

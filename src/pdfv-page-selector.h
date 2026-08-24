/* Compact PDF page selector for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_PAGE_SELECTOR_H
#define PDFV_PAGE_SELECTOR_H

#include "pdfv-document-view.h"

G_BEGIN_DECLS

#define PDFV_TYPE_PAGE_SELECTOR (pdfv_page_selector_get_type())
G_DECLARE_FINAL_TYPE(PdfvPageSelector, pdfv_page_selector, PDFV,
                     PAGE_SELECTOR, GtkBox)

PdfvPageSelector *pdfv_page_selector_new(void);
void pdfv_page_selector_set_view(PdfvPageSelector *self,
                                 PdfvDocumentView *view);

G_END_DECLS

#endif

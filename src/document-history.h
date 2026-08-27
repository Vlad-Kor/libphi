/* Bounded global document-position history for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_DOCUMENT_HISTORY_H
#define PDFV_DOCUMENT_HISTORY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  PDFV_DOCUMENT_POSITION_PDF,
  PDFV_DOCUMENT_POSITION_MARKDOWN,
} PdfvDocumentPositionKind;

/* The three values deliberately have format-neutral names so the on-disk
 * record stays compact. PDF uses page/fraction/horizontal center; Markdown
 * uses source anchor/pixel offset/fallback scroll position. */
typedef struct {
  PdfvDocumentPositionKind kind;
  gint64 anchor;
  gdouble offset;
  gdouble horizontal;
} PdfvDocumentPosition;

#define PDFV_TYPE_DOCUMENT_HISTORY (pdfv_document_history_get_type())
G_DECLARE_FINAL_TYPE(PdfvDocumentHistory, pdfv_document_history, PDFV,
                     DOCUMENT_HISTORY, GObject)

PdfvDocumentHistory *pdfv_document_history_get_default(void);
PdfvDocumentHistory *pdfv_document_history_new(GFile *storage_file);

void pdfv_document_history_set_enabled(PdfvDocumentHistory *self,
                                       gboolean enabled);
gboolean pdfv_document_history_get_enabled(PdfvDocumentHistory *self);
gboolean pdfv_document_history_lookup(PdfvDocumentHistory *self, GFile *file,
                                      PdfvDocumentPositionKind kind,
                                      PdfvDocumentPosition *position);
void pdfv_document_history_remember(PdfvDocumentHistory *self, GFile *file,
                                    const PdfvDocumentPosition *position);
guint64 pdfv_document_history_reserve_update(
    PdfvDocumentHistory *self);
void pdfv_document_history_remember_ordered(
    PdfvDocumentHistory *self, GFile *file,
    const PdfvDocumentPosition *position, guint64 update_order);
void pdfv_document_history_clear(PdfvDocumentHistory *self);
gboolean pdfv_document_history_flush(PdfvDocumentHistory *self,
                                     GError **error);

G_END_DECLS

#endif

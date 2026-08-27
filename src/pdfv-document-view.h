/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PDFV_DOCUMENT_VIEW_H__
#define __PDFV_DOCUMENT_VIEW_H__

#include <adwaita.h>
#include <phi/phidocument.h>

G_BEGIN_DECLS

#define PDFV_TYPE_DOCUMENT_VIEW (pdfv_document_view_get_type())
G_DECLARE_FINAL_TYPE(PdfvDocumentView, pdfv_document_view, PDFV, DOCUMENT_VIEW, GtkWidget)

PdfvDocumentView* pdfv_document_view_new(void);

void pdfv_document_view_set_document(PdfvDocumentView* self, PhiDocument* document);
PhiDocument* pdfv_document_view_get_document(PdfvDocumentView* self);

void pdfv_document_view_go_to_page(PdfvDocumentView* self, gint page);
gint pdfv_document_view_get_current_page(PdfvDocumentView* self);

/* A zoom-independent position suitable for persistent document history. */
void pdfv_document_view_get_scroll_state(PdfvDocumentView* self,
                                         gint* page,
                                         gdouble* page_fraction,
                                         gdouble* horizontal_center);
void pdfv_document_view_restore_scroll_state(
    PdfvDocumentView* self, gint page, gdouble page_fraction,
    gdouble horizontal_center);

void pdfv_document_view_set_zoom(PdfvDocumentView* self, gdouble zoom);
gdouble pdfv_document_view_get_zoom(PdfvDocumentView* self);
void pdfv_document_view_zoom_in(PdfvDocumentView* self);
void pdfv_document_view_zoom_out(PdfvDocumentView* self);
void pdfv_document_view_zoom_fit_width(PdfvDocumentView* self);
void pdfv_document_view_zoom_fit_page(PdfvDocumentView* self);
void pdfv_document_view_zoom_fit_page_full(PdfvDocumentView* self);
void pdfv_document_view_set_minimum_zoom(PdfvDocumentView* self,
                                         gdouble zoom);
gdouble pdfv_document_view_get_minimum_zoom(PdfvDocumentView* self);
void pdfv_document_view_set_presentation_mode(PdfvDocumentView* self,
                                              gboolean presentation);
gboolean pdfv_document_view_get_presentation_mode(PdfvDocumentView* self);

/* Capture Ctrl+scroll pinch emulation before an enclosing scrolled window. */
void pdfv_document_view_capture_zoom_scroll(PdfvDocumentView* self,
                                            GtkWidget* ancestor);

void pdfv_document_view_set_continuous(PdfvDocumentView* self, gboolean continuous);
gboolean pdfv_document_view_get_continuous(PdfvDocumentView* self);

void pdfv_document_view_set_dual_page(PdfvDocumentView* self, gboolean dual);
gboolean pdfv_document_view_get_dual_page(PdfvDocumentView* self);

void pdfv_document_view_set_inverted(PdfvDocumentView* self, gboolean inverted);
gboolean pdfv_document_view_get_inverted(PdfvDocumentView* self);

/* Navigation history for link jumps */
gboolean pdfv_document_view_can_go_back(PdfvDocumentView* self);
gboolean pdfv_document_view_can_go_forward(PdfvDocumentView* self);
void pdfv_document_view_go_back(PdfvDocumentView* self);
void pdfv_document_view_go_forward(PdfvDocumentView* self);

/* Link activation */
void pdfv_document_view_activate_link(PdfvDocumentView* self, const gchar* uri);

/* Text search */
void pdfv_document_view_search(PdfvDocumentView* self, const gchar* text);
void pdfv_document_view_search_next(PdfvDocumentView* self);
void pdfv_document_view_search_prev(PdfvDocumentView* self);
void pdfv_document_view_clear_search(PdfvDocumentView* self);
gint pdfv_document_view_get_search_match_count(PdfvDocumentView* self);
gint pdfv_document_view_get_search_current_match(PdfvDocumentView* self);

/* Text selection */
gchar* pdfv_document_view_get_selected_text(PdfvDocumentView* self);
void pdfv_document_view_clear_selection(PdfvDocumentView* self);

G_END_DECLS

#endif // __PDFV_DOCUMENT_VIEW_H__

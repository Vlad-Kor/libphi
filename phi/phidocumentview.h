/*
 * libphi - High performance document renderer for GTK
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHI_DOCUMENT_VIEW_H__
#define __PHI_DOCUMENT_VIEW_H__

#include <gtk/gtk.h>

#include <phi/phidocument.h>

G_BEGIN_DECLS

#define PHI_TYPE_DOCUMENT_VIEW (phi_document_view_get_type())
G_DECLARE_FINAL_TYPE(PhiDocumentView, phi_document_view, PHI, DOCUMENT_VIEW, GtkWidget)

PhiDocumentView* phi_document_view_new(void);

void phi_document_view_set_document(PhiDocumentView* self, PhiDocument* document);
PhiDocument* phi_document_view_get_document(PhiDocumentView* self);

void phi_document_view_go_to_page(PhiDocumentView* self, gint page);
gint phi_document_view_get_current_page(PhiDocumentView* self);

/* A zoom-independent position suitable for persistent document history. */
void phi_document_view_get_scroll_state(PhiDocumentView* self,
                                         gint* page,
                                         gdouble* page_fraction,
                                         gdouble* horizontal_center);
void phi_document_view_restore_scroll_state(
    PhiDocumentView* self, gint page, gdouble page_fraction,
    gdouble horizontal_center);

void phi_document_view_set_zoom(PhiDocumentView* self, gdouble zoom);
gdouble phi_document_view_get_zoom(PhiDocumentView* self);
void phi_document_view_zoom_in(PhiDocumentView* self);
void phi_document_view_zoom_out(PhiDocumentView* self);
void phi_document_view_zoom_fit_width(PhiDocumentView* self);
void phi_document_view_zoom_fit_page(PhiDocumentView* self);
void phi_document_view_zoom_fit_page_full(PhiDocumentView* self);
void phi_document_view_set_minimum_zoom(PhiDocumentView* self,
                                         gdouble zoom);
gdouble phi_document_view_get_minimum_zoom(PhiDocumentView* self);
void phi_document_view_set_presentation_mode(PhiDocumentView* self,
                                              gboolean presentation);
gboolean phi_document_view_get_presentation_mode(PhiDocumentView* self);

/* Capture Ctrl+scroll pinch emulation before an enclosing scrolled window. */
void phi_document_view_capture_zoom_scroll(PhiDocumentView* self,
                                            GtkWidget* ancestor);

void phi_document_view_set_continuous(PhiDocumentView* self, gboolean continuous);
gboolean phi_document_view_get_continuous(PhiDocumentView* self);

void phi_document_view_set_dual_page(PhiDocumentView* self, gboolean dual);
gboolean phi_document_view_get_dual_page(PhiDocumentView* self);

void phi_document_view_set_inverted(PhiDocumentView* self, gboolean inverted);
gboolean phi_document_view_get_inverted(PhiDocumentView* self);

/* Navigation history for link jumps */
gboolean phi_document_view_can_go_back(PhiDocumentView* self);
gboolean phi_document_view_can_go_forward(PhiDocumentView* self);
void phi_document_view_go_back(PhiDocumentView* self);
void phi_document_view_go_forward(PhiDocumentView* self);

/* Link activation */
void phi_document_view_activate_link(PhiDocumentView* self, const gchar* uri);

/* Text search */
void phi_document_view_search(PhiDocumentView* self, const gchar* text);
void phi_document_view_search_next(PhiDocumentView* self);
void phi_document_view_search_prev(PhiDocumentView* self);
void phi_document_view_clear_search(PhiDocumentView* self);
gint phi_document_view_get_search_match_count(PhiDocumentView* self);
gint phi_document_view_get_search_current_match(PhiDocumentView* self);

/* Text selection */
gchar* phi_document_view_get_selected_text(PhiDocumentView* self);
void phi_document_view_clear_selection(PhiDocumentView* self);

G_END_DECLS

#endif // __PHI_DOCUMENT_VIEW_H__

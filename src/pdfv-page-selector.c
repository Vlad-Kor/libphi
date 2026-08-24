/* Compact PDF page selector for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-page-selector.h"

#include <phi/phidocument.h>
#include <string.h>

struct _PdfvPageSelector {
  GtkBox parent_instance;

  GtkEntry *entry;
  GtkLabel *count_label;
  PdfvDocumentView *view;
};

G_DEFINE_TYPE(PdfvPageSelector, pdfv_page_selector, GTK_TYPE_BOX)

static void pdfv_page_selector_update(PdfvPageSelector *self) {
  PhiDocument *document = self->view
      ? pdfv_document_view_get_document(self->view) : NULL;
  gtk_widget_set_visible(GTK_WIDGET(self), document != NULL);
  if (!document)
    return;

  gint page = pdfv_document_view_get_current_page(self->view);
  gint pages = phi_document_get_n_pages(document);
  gchar *page_text = g_strdup_printf("%d", page + 1);
  gchar *count_text = g_strdup_printf("of %d", pages);
  gtk_editable_set_text(GTK_EDITABLE(self->entry), page_text);
  gtk_editable_set_position(GTK_EDITABLE(self->entry), -1);
  gtk_label_set_text(self->count_label, count_text);

  gchar total_text[32];
  g_snprintf(total_text, sizeof(total_text), "%d", pages);
  gint page_chars = MAX(1, (gint)strlen(total_text));
  gint count_chars = MAX(5, (gint)strlen(count_text));
  gtk_editable_set_width_chars(GTK_EDITABLE(self->entry), page_chars);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry), page_chars);

  /* width-chars uses an average glyph width. Reserve the measured width of
   * the widest numeric string for this document, plus the compact insets. */
  gchar *widest_page = g_strnfill(page_chars, '8');
  PangoLayout *layout = gtk_widget_create_pango_layout(
      GTK_WIDGET(self->entry), widest_page);
  gint page_width = 0;
  pango_layout_get_pixel_size(layout, &page_width, NULL);
  gtk_widget_set_size_request(GTK_WIDGET(self->entry), page_width + 18, -1);
  g_object_unref(layout);
  g_free(widest_page);

  gtk_label_set_width_chars(self->count_label, count_chars);
  g_free(count_text);
  g_free(page_text);
}

static void on_view_changed(PdfvDocumentView *view, GParamSpec *pspec,
                            PdfvPageSelector *self) {
  (void)view;
  (void)pspec;
  pdfv_page_selector_update(self);
}

static void on_entry_activated(GtkEntry *entry, PdfvPageSelector *self) {
  if (!self->view)
    return;
  PhiDocument *document = pdfv_document_view_get_document(self->view);
  if (!document)
    return;

  gchar *normalized = g_utf8_normalize(
      gtk_editable_get_text(GTK_EDITABLE(entry)), -1, G_NORMALIZE_ALL);
  gchar *end = NULL;
  gint64 requested = g_ascii_strtoll(normalized, &end, 10);
  while (end && g_ascii_isspace(*end))
    end++;
  gint pages = phi_document_get_n_pages(document);
  if (normalized && end && end != normalized && *end == '\0' &&
      requested >= 1 && requested <= pages)
    pdfv_document_view_go_to_page(self->view, (gint)requested - 1);
  pdfv_page_selector_update(self);
  g_free(normalized);
}

static void on_entry_focus_leave(GtkEventControllerFocus *controller,
                                 PdfvPageSelector *self) {
  (void)controller;
  pdfv_page_selector_update(self);
}

static gboolean on_entry_scroll(GtkEventControllerScroll *controller,
                                gdouble dx, gdouble dy,
                                PdfvPageSelector *self) {
  (void)controller;
  (void)dx;
  if (!self->view || dy == 0)
    return GDK_EVENT_PROPAGATE;
  gint page = pdfv_document_view_get_current_page(self->view);
  pdfv_document_view_go_to_page(self->view, page + (dy > 0 ? 1 : -1));
  return GDK_EVENT_STOP;
}

void pdfv_page_selector_set_view(PdfvPageSelector *self,
                                 PdfvDocumentView *view) {
  g_return_if_fail(PDFV_IS_PAGE_SELECTOR(self));
  g_return_if_fail(view == NULL || PDFV_IS_DOCUMENT_VIEW(view));

  if (self->view == view) {
    pdfv_page_selector_update(self);
    return;
  }
  if (self->view)
    g_signal_handlers_disconnect_by_data(self->view, self);
  g_set_object(&self->view, view);
  if (self->view) {
    g_signal_connect(self->view, "notify::current-page",
                     G_CALLBACK(on_view_changed), self);
    g_signal_connect(self->view, "notify::document",
                     G_CALLBACK(on_view_changed), self);
  }
  pdfv_page_selector_update(self);
}

static void pdfv_page_selector_dispose(GObject *object) {
  PdfvPageSelector *self = PDFV_PAGE_SELECTOR(object);
  if (self->view)
    g_signal_handlers_disconnect_by_data(self->view, self);
  g_clear_object(&self->view);
  G_OBJECT_CLASS(pdfv_page_selector_parent_class)->dispose(object);
}

static void pdfv_page_selector_class_init(PdfvPageSelectorClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pdfv_page_selector_dispose;
}

static void pdfv_page_selector_init(PdfvPageSelector *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 0);
  gtk_widget_add_css_class(GTK_WIDGET(self), "numeric");
  gtk_widget_add_css_class(GTK_WIDGET(self), "pdfv-page-selector");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self), "Page Count");
  gtk_widget_set_visible(GTK_WIDGET(self), FALSE);

  self->entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_direction(GTK_WIDGET(self->entry), GTK_TEXT_DIR_LTR);
  gtk_entry_set_alignment(self->entry, 0.9f);
  gtk_entry_set_max_length(self->entry, 12);
  gtk_editable_set_width_chars(GTK_EDITABLE(self->entry), 1);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry), 1);
  gtk_widget_set_hexpand(GTK_WIDGET(self->entry), FALSE);
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(self->entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
      "Select page", -1);
  g_signal_connect(self->entry, "activate",
                   G_CALLBACK(on_entry_activated), self);

  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "leave", G_CALLBACK(on_entry_focus_leave), self);
  gtk_widget_add_controller(GTK_WIDGET(self->entry), focus);

  GtkEventController *scroll = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
      GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect(scroll, "scroll", G_CALLBACK(on_entry_scroll), self);
  gtk_widget_add_controller(GTK_WIDGET(self->entry), scroll);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->entry));

  self->count_label = GTK_LABEL(gtk_label_new("of 0"));
  gtk_label_set_width_chars(self->count_label, 5);
  gtk_widget_set_sensitive(GTK_WIDGET(self->count_label), FALSE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->count_label));
}

PdfvPageSelector *pdfv_page_selector_new(void) {
  return g_object_new(PDFV_TYPE_PAGE_SELECTOR, NULL);
}

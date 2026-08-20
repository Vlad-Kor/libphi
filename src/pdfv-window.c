/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-window.h"
#include "pdfv-document-view.h"
#include <phi/phidocument.h>
#include <phi/phipage.h>

struct _PdfvWindow {
  AdwApplicationWindow parent_instance;

  /* Tab system */
  AdwTabView *tab_view;
  AdwTabBar *tab_bar;
  AdwTabOverview *tab_overview;

  /* Main layout */
  AdwOverlaySplitView *split_view;
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;

  /* Header bar buttons */
  GtkWidget *nav_box;
  GtkButton *back_button;
  GtkButton *forward_button;
  GtkToggleButton *sidebar_button;
  GtkMenuButton *menu_button;

  /* Sidebar */
  GtkListView *thumbnail_list;
  GtkSingleSelection *thumbnail_selection;

  /* Floating zoom controls */
  GtkWidget *zoom_box;
  GtkButton *zoom_in_btn;
  GtkButton *zoom_out_btn;
  GtkLabel *zoom_label;

  /* Search bar */
  GtkSearchBar *search_bar;
  GtkSearchEntry *search_entry;
  GtkLabel *search_status;

  /* Current view (active tab) */
  PdfvDocumentView *current_view;

  /* Outline data for current document */
  PhiOutlineItem *current_outline;
};

G_DEFINE_TYPE(PdfvWindow, pdfv_window, ADW_TYPE_APPLICATION_WINDOW)

static void update_navigation_buttons(PdfvWindow *self) {
  gboolean can_back = FALSE;
  gboolean can_forward = FALSE;

  if (self->current_view) {
    can_back = pdfv_document_view_can_go_back(self->current_view);
    can_forward = pdfv_document_view_can_go_forward(self->current_view);
  }

  /* Hide buttons when not usable */
  gtk_widget_set_visible(GTK_WIDGET(self->back_button), can_back);
  gtk_widget_set_visible(GTK_WIDGET(self->forward_button), can_forward);
  gtk_widget_set_visible(self->nav_box, can_back || can_forward);
}

static void update_zoom_info(PdfvWindow *self) {
  if (!self->current_view) {
    gtk_label_set_text(self->zoom_label, "100%");
    return;
  }

  gdouble zoom = pdfv_document_view_get_zoom(self->current_view);
  gchar *text = g_strdup_printf("%.0f%%", zoom * 100);
  gtk_label_set_text(self->zoom_label, text);
  g_free(text);
}

static void update_sidebar_button(PdfvWindow *self) {
  gboolean has_document = FALSE;

  if (self->current_view) {
    PhiDocument *doc = pdfv_document_view_get_document(self->current_view);
    has_document = (doc != NULL);
  }

  GAction *action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "toggle-sidebar");
  if (action) {
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), has_document);
  }

  /* Hide floating zoom controls when no document */
  gtk_widget_set_visible(self->zoom_box, has_document);
}

static void on_view_notify(PdfvDocumentView *view, GParamSpec *pspec,
                           PdfvWindow *self) {
  (void)view;
  const gchar *name = g_param_spec_get_name(pspec);

  if (g_strcmp0(name, "can-go-back") == 0 ||
      g_strcmp0(name, "can-go-forward") == 0) {
    update_navigation_buttons(self);
  } else if (g_strcmp0(name, "zoom") == 0) {
    update_zoom_info(self);
  }
}

static void on_link_activated(PdfvDocumentView *view, const gchar *uri,
                              PdfvWindow *self) {
  (void)view;
  GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
  gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
  g_object_unref(launcher);
}

static void on_search_completed(PdfvDocumentView *view, gint match_count,
                                PdfvWindow *self) {
  (void)view;
  gchar *status;
  if (match_count == 0)
    status = g_strdup("No matches");
  else
    status = g_strdup_printf("%d match%s", match_count,
                             match_count == 1 ? "" : "es");
  gtk_label_set_text(self->search_status, status);
  g_free(status);
}

static void setup_document_view_signals(PdfvWindow *self,
                                        PdfvDocumentView *view) {
  g_signal_connect(view, "notify", G_CALLBACK(on_view_notify), self);
  g_signal_connect(view, "link-activated", G_CALLBACK(on_link_activated), self);
  g_signal_connect(view, "search-completed", G_CALLBACK(on_search_completed),
                   self);
}

/* Thumbnail rendering is serialized on one worker. Each document uses a
 * background-only MuPDF context, separate from the one used by the view. */
typedef struct _ThumbnailJob ThumbnailJob;

typedef struct {
  gint ref_count;
  GtkWidget *drawing_area;
  GtkLabel *page_label;
  PhiDocument *document;
  gint page_num;
  guint generation;
  cairo_surface_t *cached_surface;
  ThumbnailJob *job;
} ThumbnailData;

struct _ThumbnailJob {
  ThumbnailData *data;
  PhiDocument *document;
  gint page_num;
  guint generation;
  gint cancelled;
  cairo_surface_t *surface;
  GError *error;
};

static void thumbnail_render_worker(gpointer user_data, gpointer pool_data);

static gpointer thumbnail_pool_init(gpointer user_data) {
  (void)user_data;
  GError *error = NULL;
  GThreadPool *pool =
      g_thread_pool_new(thumbnail_render_worker, NULL, 1, TRUE, &error);
  if (!pool) {
    g_warning("Could not create thumbnail worker: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }
  return pool;
}

static GThreadPool *thumbnail_get_pool(void) {
  static GOnce once = G_ONCE_INIT;
  return g_once(&once, thumbnail_pool_init, NULL);
}

static ThumbnailData *thumbnail_data_ref(ThumbnailData *data) {
  g_atomic_int_inc(&data->ref_count);
  return data;
}

static void thumbnail_data_unref(ThumbnailData *data) {
  if (!g_atomic_int_dec_and_test(&data->ref_count))
    return;

  if (data->cached_surface)
    cairo_surface_destroy(data->cached_surface);
  g_clear_object(&data->document);
  g_free(data);
}

static void thumbnail_data_reset(ThumbnailData *data) {
  data->generation++;

  if (data->job) {
    g_atomic_int_set(&data->job->cancelled, TRUE);
    data->job = NULL;
  }

  if (data->cached_surface) {
    cairo_surface_destroy(data->cached_surface);
    data->cached_surface = NULL;
  }

  g_clear_object(&data->document);
  data->page_num = -1;
}

static void thumbnail_data_free(ThumbnailData *data) {
  thumbnail_data_reset(data);
  data->drawing_area = NULL;
  data->page_label = NULL;
  thumbnail_data_unref(data);
}

static gboolean thumbnail_render_complete(gpointer user_data) {
  ThumbnailJob *job = user_data;
  ThumbnailData *data = job->data;

  if (!g_atomic_int_get(&job->cancelled) && data->job == job &&
      data->generation == job->generation &&
      data->document == job->document && job->surface) {
    data->cached_surface = job->surface;
    job->surface = NULL;
    if (data->drawing_area)
      gtk_widget_queue_draw(data->drawing_area);
  }

  if (data->job == job)
    data->job = NULL;

  if (job->error && !g_error_matches(job->error, G_IO_ERROR,
                                      G_IO_ERROR_CANCELLED))
    g_debug("Could not render page %d thumbnail: %s", job->page_num + 1,
            job->error->message);

  g_clear_error(&job->error);
  if (job->surface)
    cairo_surface_destroy(job->surface);
  g_object_unref(job->document);
  thumbnail_data_unref(data);
  g_free(job);
  return G_SOURCE_REMOVE;
}

static void thumbnail_render_worker(gpointer user_data, gpointer pool_data) {
  (void)pool_data;
  ThumbnailJob *job = user_data;

  if (!g_atomic_int_get(&job->cancelled))
    job->surface = phi_document_render_thumbnail(
        job->document, job->page_num, 120, 160, &job->error);

  g_main_context_invoke(NULL, thumbnail_render_complete, job);
}

static void thumbnail_queue_render(ThumbnailData *data) {
  if (!data->document || data->page_num < 0 || data->cached_surface ||
      data->job)
    return;

  ThumbnailJob *job = g_new0(ThumbnailJob, 1);
  job->data = thumbnail_data_ref(data);
  job->document = g_object_ref(data->document);
  job->page_num = data->page_num;
  job->generation = data->generation;
  data->job = job;

  GThreadPool *pool = thumbnail_get_pool();
  if (!pool) {
    job->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Thumbnail worker is unavailable");
    g_main_context_invoke(NULL, thumbnail_render_complete, job);
    return;
  }

  GError *error = NULL;
  if (!g_thread_pool_push(pool, job, &error)) {
    job->error = error;
    if (!job->error)
      job->error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "Could not queue thumbnail render");
    g_main_context_invoke(NULL, thumbnail_render_complete, job);
  }
}

static void on_thumbnail_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                              int height, ThumbnailData *data) {
  (void)area;

  if (!data->cached_surface)
    thumbnail_queue_render(data);

  gdouble padding = 8.0;
  gdouble page_width = data->cached_surface
                           ? cairo_image_surface_get_width(data->cached_surface)
                           : 120.0;
  gdouble page_height =
      data->cached_surface
          ? cairo_image_surface_get_height(data->cached_surface)
          : 160.0;
  gdouble scale =
      MIN((width - padding * 2) / page_width,
          (height - padding * 2) / page_height);
  gdouble scaled_width = page_width * scale;
  gdouble scaled_height = page_height * scale;
  gdouble offset_x = (width - scaled_width) / 2.0;
  gdouble offset_y = (height - scaled_height) / 2.0;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
  cairo_rectangle(cr, offset_x + 2, offset_y + 2, scaled_width, scaled_height);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr, offset_x, offset_y, scaled_width, scaled_height);
  cairo_fill(cr);

  if (data->cached_surface) {
    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, data->cached_surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  }

  cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, offset_x + 0.5, offset_y + 0.5, scaled_width - 1,
                  scaled_height - 1);
  cairo_stroke(cr);
}

static void thumbnail_factory_setup(GtkSignalListItemFactory *factory,
                                    GtkListItem *list_item, PdfvWindow *self) {
  (void)factory;
  (void)self;

  ThumbnailData *data = g_new0(ThumbnailData, 1);
  data->ref_count = 1;
  data->page_num = -1;
  g_object_set_data_full(G_OBJECT(list_item), "thumb-data", data,
                         (GDestroyNotify)thumbnail_data_free);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  data->drawing_area = gtk_drawing_area_new();
  gtk_widget_set_size_request(data->drawing_area, 100, 130);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(data->drawing_area),
                                 (GtkDrawingAreaDrawFunc)on_thumbnail_draw,
                                 data, NULL);
  gtk_box_append(GTK_BOX(box), data->drawing_area);

  data->page_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(data->page_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(data->page_label), "dim-label");
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(data->page_label));

  gtk_list_item_set_child(list_item, box);
}

static void thumbnail_factory_bind(GtkSignalListItemFactory *factory,
                                   GtkListItem *list_item, PdfvWindow *self) {
  (void)factory;
  ThumbnailData *data =
      g_object_get_data(G_OBJECT(list_item), "thumb-data");
  GListModel *model = gtk_single_selection_get_model(self->thumbnail_selection);
  guint position = gtk_list_item_get_position(list_item);

  thumbnail_data_reset(data);
  if (!PHI_IS_DOCUMENT(model) || position == GTK_INVALID_LIST_POSITION)
    return;

  data->document = g_object_ref(PHI_DOCUMENT(model));
  data->page_num = (gint)position;

  gchar *label = g_strdup_printf("%u", position + 1);
  gtk_label_set_text(data->page_label, label);
  g_free(label);
  gtk_widget_queue_draw(data->drawing_area);
}

static void thumbnail_factory_unbind(GtkSignalListItemFactory *factory,
                                     GtkListItem *list_item,
                                     PdfvWindow *self) {
  (void)factory;
  (void)self;
  ThumbnailData *data =
      g_object_get_data(G_OBJECT(list_item), "thumb-data");
  thumbnail_data_reset(data);
}

static void populate_thumbnails(PdfvWindow *self, PhiDocument *document) {
  GListModel *model = document ? G_LIST_MODEL(document) : NULL;
  if (gtk_single_selection_get_model(self->thumbnail_selection) == model)
    return;

  gtk_single_selection_set_model(self->thumbnail_selection, model);
}

static void on_thumbnail_activated(GtkListView *list, guint position,
                                   PdfvWindow *self) {
  (void)list;
  if (self->current_view && position != GTK_INVALID_LIST_POSITION)
    pdfv_document_view_go_to_page(self->current_view, (gint)position);
}

/* Create empty state widget (shown when no document is open) */
static GtkWidget *create_empty_state(PdfvWindow *self) {
  GtkWidget *status = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
                                "document-open-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Document Open");
  adw_status_page_set_description(ADW_STATUS_PAGE(status),
                                  "Open a PDF file to view it");

  GtkWidget *button = gtk_button_new_with_label("Open File…");
  gtk_widget_add_css_class(button, "pill");
  gtk_widget_add_css_class(button, "suggested-action");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.open");
  gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
  adw_status_page_set_child(ADW_STATUS_PAGE(status), button);

  (void)self;
  return status;
}

static GtkWidget *create_tab_content(PdfvWindow *self) {
  /* Create a stack to switch between empty state and document */
  GtkWidget *stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(stack),
                                GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  /* Empty state */
  GtkWidget *empty = create_empty_state(self);
  gtk_stack_add_named(GTK_STACK(stack), empty, "empty");

  /* Document view */
  PdfvDocumentView *view = pdfv_document_view_new();
  setup_document_view_signals(self, view);

  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                GTK_WIDGET(view));
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_stack_add_named(GTK_STACK(stack), scrolled, "document");

  /* Start with empty state */
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");

  g_object_set_data(G_OBJECT(stack), "document-view", view);
  g_object_set_data(G_OBJECT(stack), "scrolled-window", scrolled);

  return stack;
}

static void on_tab_selected(AdwTabView *tab_view, GParamSpec *pspec,
                            PdfvWindow *self) {
  (void)pspec;

  AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);

  if (!page) {
    self->current_view = NULL;
    update_navigation_buttons(self);
    update_zoom_info(self);
    update_sidebar_button(self);
    /* Hide sidebar when no tab */
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
    return;
  }

  GtkWidget *stack = adw_tab_page_get_child(page);
  self->current_view = g_object_get_data(G_OBJECT(stack), "document-view");

  update_navigation_buttons(self);
  update_zoom_info(self);
  update_sidebar_button(self);

  if (self->current_view) {
    PhiDocument *doc = pdfv_document_view_get_document(self->current_view);
    if (doc) {
      populate_thumbnails(self, doc);
    } else {
      /* No document - hide sidebar */
      adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
      populate_thumbnails(self, NULL);
    }
  }
}

static gboolean on_tab_close_page(AdwTabView *tab_view, AdwTabPage *page,
                                  PdfvWindow *self) {
  /* Clear current_view if we're closing its tab */
  GtkWidget *stack = adw_tab_page_get_child(page);
  PdfvDocumentView *view = g_object_get_data(G_OBJECT(stack), "document-view");
  if (view == self->current_view) {
    /* Disconnect signals before destruction */
    g_signal_handlers_disconnect_by_data(view, self);
    self->current_view = NULL;
  }

  adw_tab_view_close_page_finish(tab_view, page, TRUE);
  return GDK_EVENT_STOP;
}

static void on_file_dialog_opened(GObject *source, GAsyncResult *result,
                                  gpointer user_data) {
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
  PdfvWindow *self = PDFV_WINDOW(user_data);

  GFile *file = gtk_file_dialog_open_finish(dialog, result, NULL);
  if (file) {
    pdfv_window_open_file(self, file);
    g_object_unref(file);
  }
}

static void action_open(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);

  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Open PDF");

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "PDF Documents");
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_filter_add_pattern(filter, "*.pdf");

  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  g_object_unref(filter);

  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  g_object_unref(filters);

  gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_file_dialog_opened,
                       self);
}

static void action_new_tab(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
  (void)action;
  (void)parameter;
  pdfv_window_new_tab(PDFV_WINDOW(user_data));
}

static void action_close_tab(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);

  /* If this is the last tab, close the window instead */
  if (adw_tab_view_get_n_pages(self->tab_view) <= 1) {
    gtk_window_close(GTK_WINDOW(self));
    return;
  }

  AdwTabPage *page = adw_tab_view_get_selected_page(self->tab_view);
  if (page)
    adw_tab_view_close_page(self->tab_view, page);
}

static void action_go_back(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_go_back(self->current_view);
}

static void action_go_forward(GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_go_forward(self->current_view);
}

static void action_zoom_in(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_zoom_in(self->current_view);
}

static void action_zoom_out(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_zoom_out(self->current_view);
}

static void action_zoom_reset(GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_set_zoom(self->current_view, 1.0);
}

static void action_zoom_fit_width(GSimpleAction *action, GVariant *parameter,
                                  gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_zoom_fit_width(self->current_view);
}

static void action_zoom_fit_page(GSimpleAction *action, GVariant *parameter,
                                 gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_zoom_fit_page(self->current_view);
}

static void action_fullscreen(GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
    gtk_window_unfullscreen(GTK_WINDOW(self));
  else
    gtk_window_fullscreen(GTK_WINDOW(self));
}

static void action_toggle_sidebar(GSimpleAction *action, GVariant *parameter,
                                  gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);

  /* Only toggle if there's a document loaded */
  if (self->current_view &&
      pdfv_document_view_get_document(self->current_view)) {
    gboolean visible =
        adw_overlay_split_view_get_show_sidebar(self->split_view);
    adw_overlay_split_view_set_show_sidebar(self->split_view, !visible);
  }
}

static void action_invert_colors(GSimpleAction *action, GVariant *parameter,
                                 gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view) {
    gboolean inverted = pdfv_document_view_get_inverted(self->current_view);
    pdfv_document_view_set_inverted(self->current_view, !inverted);
  }
}

static void action_page_next(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view) {
    gint page = pdfv_document_view_get_current_page(self->current_view);
    pdfv_document_view_go_to_page(self->current_view, page + 1);
  }
}

static void action_page_prev(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view) {
    gint page = pdfv_document_view_get_current_page(self->current_view);
    pdfv_document_view_go_to_page(self->current_view, page - 1);
  }
}

static void action_find(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);

  gtk_search_bar_set_search_mode(self->search_bar, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
}

static void action_find_next(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_search_next(self->current_view);
}

static void action_find_prev(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_view)
    pdfv_document_view_search_prev(self->current_view);
}

static GActionEntry win_actions[] = {
    {"open", action_open, NULL, NULL, NULL},
    {"new-tab", action_new_tab, NULL, NULL, NULL},
    {"close-tab", action_close_tab, NULL, NULL, NULL},
    {"go-back", action_go_back, NULL, NULL, NULL},
    {"go-forward", action_go_forward, NULL, NULL, NULL},
    {"zoom-in", action_zoom_in, NULL, NULL, NULL},
    {"zoom-out", action_zoom_out, NULL, NULL, NULL},
    {"zoom-reset", action_zoom_reset, NULL, NULL, NULL},
    {"zoom-fit-width", action_zoom_fit_width, NULL, NULL, NULL},
    {"zoom-fit-page", action_zoom_fit_page, NULL, NULL, NULL},
    {"fullscreen", action_fullscreen, NULL, NULL, NULL},
    {"toggle-sidebar", action_toggle_sidebar, NULL, NULL, NULL},
    {"invert-colors", action_invert_colors, NULL, NULL, NULL},
    {"page-next", action_page_next, NULL, NULL, NULL},
    {"page-prev", action_page_prev, NULL, NULL, NULL},
    {"find", action_find, NULL, NULL, NULL},
    {"find-next", action_find_next, NULL, NULL, NULL},
    {"find-prev", action_find_prev, NULL, NULL, NULL},
};

static void on_search_changed(GtkSearchEntry *entry, PdfvWindow *self) {
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));

  if (self->current_view) {
    pdfv_document_view_search(self->current_view, text);

    /* Update status label - show immediate feedback */
    if (!text || !*text) {
      gtk_label_set_text(self->search_status, "");
    } else if (g_utf8_strlen(text, -1) < 2) {
      gtk_label_set_text(self->search_status, "Type more...");
    } else {
      gtk_label_set_text(self->search_status, "Searching...");
    }
  }
}

static void on_search_next_match(GtkSearchEntry *entry, PdfvWindow *self) {
  (void)entry;
  if (self->current_view)
    pdfv_document_view_search_next(self->current_view);
}

static void on_search_prev_match(GtkSearchEntry *entry, PdfvWindow *self) {
  (void)entry;
  if (self->current_view)
    pdfv_document_view_search_prev(self->current_view);
}

static void on_search_stop(GtkSearchEntry *entry, PdfvWindow *self) {
  (void)entry;
  gtk_search_bar_set_search_mode(self->search_bar, FALSE);
  if (self->current_view)
    pdfv_document_view_clear_search(self->current_view);
  gtk_label_set_text(self->search_status, "");
}

static void on_tab_overview_button_clicked(GtkButton *button,
                                           PdfvWindow *self) {
  (void)button;
  gboolean is_open = adw_tab_overview_get_open(self->tab_overview);
  adw_tab_overview_set_open(self->tab_overview, !is_open);
}

/* Hide sidebar when tab overview opens to avoid weird interactions */
static void on_tab_overview_open_changed(AdwTabOverview *overview,
                                         GParamSpec *pspec, PdfvWindow *self) {
  (void)pspec;
  if (adw_tab_overview_get_open(overview)) {
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
  }
}

static AdwTabPage *on_tab_overview_create_tab(AdwTabOverview *overview,
                                              PdfvWindow *self) {
  (void)overview;
  GtkWidget *content = create_tab_content(self);
  AdwTabPage *page = adw_tab_view_append(self->tab_view, content);
  adw_tab_page_set_title(page, "New Tab");
  return page;
}

static void on_zoom_in_clicked(GtkButton *button, PdfvWindow *self) {
  (void)button;
  if (self->current_view)
    pdfv_document_view_zoom_in(self->current_view);
}

static void on_zoom_out_clicked(GtkButton *button, PdfvWindow *self) {
  (void)button;
  if (self->current_view)
    pdfv_document_view_zoom_out(self->current_view);
}

static void on_zoom_label_clicked(GtkGestureClick *gesture, gint n_press,
                                  gdouble x, gdouble y, PdfvWindow *self) {
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;
  if (self->current_view)
    pdfv_document_view_zoom_fit_width(self->current_view);
}

static void on_sidebar_show_changed(AdwOverlaySplitView *split_view,
                                    GParamSpec *pspec, PdfvWindow *self) {
  (void)pspec;
  gboolean visible = adw_overlay_split_view_get_show_sidebar(split_view);
  gtk_toggle_button_set_active(self->sidebar_button, visible);
}

static void pdfv_window_dispose(GObject *object) {
  PdfvWindow *self = PDFV_WINDOW(object);

  if (self->current_outline) {
    phi_outline_item_free(self->current_outline);
    self->current_outline = NULL;
  }

  G_OBJECT_CLASS(pdfv_window_parent_class)->dispose(object);
}

static void pdfv_window_init(PdfvWindow *self) {
  self->current_view = NULL;
  self->current_outline = NULL;

  g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                  G_N_ELEMENTS(win_actions), self);

  /* ===== WIDGET HIERARCHY =====
   *
   * AdwApplicationWindow
   *   └─ AdwOverlaySplitView (content) - full height sidebar
   *        ├─ Sidebar (sidebar)
   *        └─ AdwTabOverview (content)
   *             └─ AdwToolbarView (child)
   *                  ├─ AdwHeaderBar (top-bar)
   *                  ├─ AdwTabBar (top-bar)
   *                  └─ GtkOverlay (content)
   *                       ├─ AdwTabView (child)
   *                       └─ ZoomControls (overlay)
   */

  /* Split view - outermost for full-height sidebar */
  self->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  adw_overlay_split_view_set_sidebar_width_fraction(self->split_view, 0.22);
  adw_overlay_split_view_set_min_sidebar_width(self->split_view, 200);
  adw_overlay_split_view_set_max_sidebar_width(self->split_view, 280);
  adw_overlay_split_view_set_enable_hide_gesture(self->split_view, TRUE);
  adw_overlay_split_view_set_enable_show_gesture(self->split_view, TRUE);
  /* Don't force collapsed - let it adapt based on window width */
  /* When collapsed=FALSE (wide window), sidebar is inline like Nautilus */
  /* When collapsed=TRUE (narrow window), sidebar overlays like a popup */
  adw_overlay_split_view_set_pin_sidebar(self->split_view, FALSE);
  adw_overlay_split_view_set_show_sidebar(self->split_view,
                                          FALSE); /* Start hidden */
  g_signal_connect(self->split_view, "notify::show-sidebar",
                   G_CALLBACK(on_sidebar_show_changed), self);

  /* Tab Overview */
  self->tab_overview = ADW_TAB_OVERVIEW(adw_tab_overview_new());
  adw_tab_overview_set_enable_new_tab(self->tab_overview, TRUE);
  g_signal_connect(self->tab_overview, "create-tab",
                   G_CALLBACK(on_tab_overview_create_tab), self);
  g_signal_connect(self->tab_overview, "notify::open",
                   G_CALLBACK(on_tab_overview_open_changed), self);

  /* Set up the hierarchy: window -> split_view -> tab_overview */
  adw_overlay_split_view_set_content(self->split_view,
                                     GTK_WIDGET(self->tab_overview));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                     GTK_WIDGET(self->split_view));

  /* Main toolbar view - must be set AFTER tab_overview has a parent */
  self->toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  /* Ensure content doesn't scroll under the header bar */
  adw_toolbar_view_set_top_bar_style(self->toolbar_view, ADW_TOOLBAR_RAISED);
  adw_tab_overview_set_child(self->tab_overview,
                             GTK_WIDGET(self->toolbar_view));

  /* Header bar */
  self->header_bar = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(self->toolbar_view,
                               GTK_WIDGET(self->header_bar));

  /* Navigation buttons */
  self->nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(self->nav_box, "linked");
  gtk_widget_set_visible(self->nav_box, FALSE);

  self->back_button =
      GTK_BUTTON(gtk_button_new_from_icon_name("go-previous-symbolic"));
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->back_button),
                                 "win.go-back");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->back_button),
                              "Go Back (Alt+Left)");
  gtk_widget_set_visible(GTK_WIDGET(self->back_button), FALSE);
  gtk_box_append(GTK_BOX(self->nav_box), GTK_WIDGET(self->back_button));

  self->forward_button =
      GTK_BUTTON(gtk_button_new_from_icon_name("go-next-symbolic"));
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->forward_button),
                                 "win.go-forward");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->forward_button),
                              "Go Forward (Alt+Right)");
  gtk_widget_set_visible(GTK_WIDGET(self->forward_button), FALSE);
  gtk_box_append(GTK_BOX(self->nav_box), GTK_WIDGET(self->forward_button));

  adw_header_bar_pack_start(self->header_bar, self->nav_box);

  /* Sidebar toggle */
  self->sidebar_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  gtk_button_set_icon_name(GTK_BUTTON(self->sidebar_button),
                           "sidebar-show-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->sidebar_button),
                              "Toggle Sidebar (F9)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(self->sidebar_button),
                                 "win.toggle-sidebar");
  adw_header_bar_pack_start(self->header_bar, GTK_WIDGET(self->sidebar_button));

  /* New tab button */
  GtkWidget *new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(new_tab_btn), "win.new-tab");
  gtk_widget_set_tooltip_text(new_tab_btn, "New Tab (Ctrl+T)");
  adw_header_bar_pack_start(self->header_bar, new_tab_btn);

  /* Menu button */
  self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->menu_button), "Main Menu");
  gtk_menu_button_set_primary(self->menu_button, TRUE);

  GMenu *menu = g_menu_new();
  GMenu *file_section = g_menu_new();
  g_menu_append(file_section, "Open…", "win.open");
  g_menu_append(file_section, "New Tab", "win.new-tab");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(file_section));

  GMenu *zoom_section = g_menu_new();
  g_menu_append(zoom_section, "Zoom In", "win.zoom-in");
  g_menu_append(zoom_section, "Zoom Out", "win.zoom-out");
  g_menu_append(zoom_section, "Reset Zoom", "win.zoom-reset");
  g_menu_append(zoom_section, "Fit Width", "win.zoom-fit-width");
  g_menu_append(zoom_section, "Fit Page", "win.zoom-fit-page");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(zoom_section));

  GMenu *view_section = g_menu_new();
  g_menu_append(view_section, "Invert Colors", "win.invert-colors");
  g_menu_append(view_section, "Fullscreen", "win.fullscreen");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(view_section));

  GMenu *about_section = g_menu_new();
  g_menu_append(about_section, "About Phi PDF Viewer", "app.about");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(about_section));

  gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
  adw_header_bar_pack_end(self->header_bar, GTK_WIDGET(self->menu_button));

  g_object_unref(file_section);
  g_object_unref(zoom_section);
  g_object_unref(view_section);
  g_object_unref(about_section);
  g_object_unref(menu);

  /* Tab bar - below header bar */
  self->tab_bar = ADW_TAB_BAR(adw_tab_bar_new());
  adw_tab_bar_set_autohide(self->tab_bar, TRUE);
  adw_toolbar_view_add_top_bar(self->toolbar_view, GTK_WIDGET(self->tab_bar));

  /* Search bar */
  self->search_bar = GTK_SEARCH_BAR(gtk_search_bar_new());
  gtk_search_bar_set_show_close_button(self->search_bar, TRUE);

  GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  self->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(self->search_entry), TRUE);
  gtk_box_append(GTK_BOX(search_box), GTK_WIDGET(self->search_entry));

  /* Navigation buttons */
  GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-up-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(prev_btn), "win.find-prev");
  gtk_widget_set_tooltip_text(prev_btn, "Previous Match (Shift+F3)");
  gtk_box_append(GTK_BOX(search_box), prev_btn);

  GtkWidget *next_btn = gtk_button_new_from_icon_name("go-down-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(next_btn), "win.find-next");
  gtk_widget_set_tooltip_text(next_btn, "Next Match (F3)");
  gtk_box_append(GTK_BOX(search_box), next_btn);

  /* Search status label */
  self->search_status = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->search_status), "dim-label");
  gtk_box_append(GTK_BOX(search_box), GTK_WIDGET(self->search_status));

  gtk_search_bar_set_child(self->search_bar, search_box);
  gtk_search_bar_connect_entry(self->search_bar,
                               GTK_EDITABLE(self->search_entry));

  /* Connect search signals */
  g_signal_connect(self->search_entry, "search-changed",
                   G_CALLBACK(on_search_changed), self);
  g_signal_connect(self->search_entry, "next-match",
                   G_CALLBACK(on_search_next_match), self);
  g_signal_connect(self->search_entry, "previous-match",
                   G_CALLBACK(on_search_prev_match), self);
  g_signal_connect(self->search_entry, "stop-search",
                   G_CALLBACK(on_search_stop), self);

  adw_toolbar_view_add_top_bar(self->toolbar_view,
                               GTK_WIDGET(self->search_bar));

  /* Sidebar content - full height with proper Adwaita styling */
  GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(sidebar_box, "sidebar-pane");

  /* Sidebar toolbar/header */
  AdwHeaderBar *sidebar_header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_show_title(sidebar_header, TRUE);
  adw_header_bar_set_title_widget(sidebar_header, gtk_label_new("Pages"));
  gtk_widget_add_css_class(GTK_WIDGET(sidebar_header), "flat");

  /* Search button in sidebar header */
  GtkWidget *sidebar_search_btn =
      gtk_button_new_from_icon_name("system-search-symbolic");
  gtk_widget_set_tooltip_text(sidebar_search_btn, "Find in document (Ctrl+F)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(sidebar_search_btn),
                                 "win.find");
  adw_header_bar_pack_start(sidebar_header, sidebar_search_btn);

  gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(sidebar_header));

  GtkWidget *thumb_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thumb_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(thumb_scroll, TRUE);

  GtkListItemFactory *thumbnail_factory =
      gtk_signal_list_item_factory_new();
  g_signal_connect(thumbnail_factory, "setup",
                   G_CALLBACK(thumbnail_factory_setup), self);
  g_signal_connect(thumbnail_factory, "bind", G_CALLBACK(thumbnail_factory_bind),
                   self);
  g_signal_connect(thumbnail_factory, "unbind",
                   G_CALLBACK(thumbnail_factory_unbind), self);

  self->thumbnail_selection = gtk_single_selection_new(NULL);
  gtk_single_selection_set_autoselect(self->thumbnail_selection, FALSE);
  gtk_single_selection_set_can_unselect(self->thumbnail_selection, TRUE);
  self->thumbnail_list = GTK_LIST_VIEW(gtk_list_view_new(
      GTK_SELECTION_MODEL(self->thumbnail_selection), thumbnail_factory));
  gtk_list_view_set_single_click_activate(self->thumbnail_list, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->thumbnail_list),
                           "navigation-sidebar");
  g_signal_connect(self->thumbnail_list, "activate",
                   G_CALLBACK(on_thumbnail_activated), self);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(thumb_scroll),
                                GTK_WIDGET(self->thumbnail_list));
  gtk_box_append(GTK_BOX(sidebar_box), thumb_scroll);

  adw_overlay_split_view_set_sidebar(self->split_view, sidebar_box);

  /* Tab view */
  self->tab_view = ADW_TAB_VIEW(adw_tab_view_new());
  adw_tab_bar_set_view(self->tab_bar, self->tab_view);
  adw_tab_overview_set_view(self->tab_overview, self->tab_view);

  /* Tab overview button - must be set after tab_view exists */
  GtkWidget *tab_overview_btn = adw_tab_button_new();
  adw_tab_button_set_view(ADW_TAB_BUTTON(tab_overview_btn), self->tab_view);
  gtk_widget_set_tooltip_text(tab_overview_btn, "View Open Tabs");
  g_signal_connect(tab_overview_btn, "clicked",
                   G_CALLBACK(on_tab_overview_button_clicked), self);
  adw_header_bar_pack_end(self->header_bar, tab_overview_btn);

  g_signal_connect(self->tab_view, "notify::selected-page",
                   G_CALLBACK(on_tab_selected), self);
  g_signal_connect(self->tab_view, "close-page", G_CALLBACK(on_tab_close_page),
                   self);

  /* Content overlay for floating controls */
  GtkWidget *content_overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(content_overlay),
                        GTK_WIDGET(self->tab_view));
  adw_toolbar_view_set_content(self->toolbar_view, content_overlay);

  /* Floating zoom controls */
  self->zoom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(self->zoom_box, "osd");
  gtk_widget_add_css_class(self->zoom_box, "toolbar");
  gtk_widget_set_halign(self->zoom_box, GTK_ALIGN_END);
  gtk_widget_set_valign(self->zoom_box, GTK_ALIGN_END);
  gtk_widget_set_margin_end(self->zoom_box, 12);
  gtk_widget_set_margin_bottom(self->zoom_box, 12);

  self->zoom_out_btn =
      GTK_BUTTON(gtk_button_new_from_icon_name("zoom-out-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->zoom_out_btn), "circular");
  g_signal_connect(self->zoom_out_btn, "clicked",
                   G_CALLBACK(on_zoom_out_clicked), self);
  gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_out_btn));

  /* Zoom label - clickable to fit width */
  self->zoom_label = GTK_LABEL(gtk_label_new("100%"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->zoom_label),
                              "Click to fit width");
  gtk_label_set_width_chars(self->zoom_label, 5);
  GtkGesture *zoom_click = gtk_gesture_click_new();
  g_signal_connect(zoom_click, "pressed", G_CALLBACK(on_zoom_label_clicked),
                   self);
  gtk_widget_add_controller(GTK_WIDGET(self->zoom_label),
                            GTK_EVENT_CONTROLLER(zoom_click));
  gtk_widget_set_cursor_from_name(GTK_WIDGET(self->zoom_label), "pointer");
  gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_label));

  self->zoom_in_btn =
      GTK_BUTTON(gtk_button_new_from_icon_name("zoom-in-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->zoom_in_btn), "circular");
  g_signal_connect(self->zoom_in_btn, "clicked", G_CALLBACK(on_zoom_in_clicked),
                   self);
  gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_in_btn));

  gtk_overlay_add_overlay(GTK_OVERLAY(content_overlay), self->zoom_box);

  /* Window setup */
  gtk_window_set_default_size(GTK_WINDOW(self), 900, 700);
  gtk_window_set_title(GTK_WINDOW(self), "PDF Viewer");

  /* Create initial tab */
  pdfv_window_new_tab(self);

  update_navigation_buttons(self);
}

static void pdfv_window_class_init(PdfvWindowClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = pdfv_window_dispose;
}

PdfvWindow *pdfv_window_new(AdwApplication *app) {
  return g_object_new(PDFV_TYPE_WINDOW, "application", app, NULL);
}

void pdfv_window_open_file(PdfvWindow *self, GFile *file) {
  g_return_if_fail(PDFV_IS_WINDOW(self));
  g_return_if_fail(G_IS_FILE(file));

  GError *error = NULL;
  PhiDocument *doc = phi_document_new_from_file(file, &error);

  if (error) {
    AdwDialog *dialog =
        adw_alert_dialog_new("Error Opening File", error->message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(self));
    g_error_free(error);
    return;
  }

  PdfvDocumentView *view = NULL;
  AdwTabPage *page = adw_tab_view_get_selected_page(self->tab_view);
  GtkWidget *stack = NULL;

  if (page && self->current_view &&
      !pdfv_document_view_get_document(self->current_view)) {
    view = self->current_view;
    stack = adw_tab_page_get_child(page);
  } else {
    stack = create_tab_content(self);
    view = g_object_get_data(G_OBJECT(stack), "document-view");
    page = adw_tab_view_append(self->tab_view, stack);
    adw_tab_view_set_selected_page(self->tab_view, page);
  }

  pdfv_document_view_set_document(view, doc);

  /* Switch from empty state to document view */
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "document");

  gchar *basename = g_file_get_basename(file);
  adw_tab_page_set_title(page, basename);
  g_free(basename);

  /* Update current view reference if we reused the current tab */
  self->current_view = view;

  populate_thumbnails(self, doc);
  update_sidebar_button(self);

  g_object_unref(doc);
}

void pdfv_window_new_tab(PdfvWindow *self) {
  g_return_if_fail(PDFV_IS_WINDOW(self));

  GtkWidget *content = create_tab_content(self);
  AdwTabPage *page = adw_tab_view_append(self->tab_view, content);
  adw_tab_page_set_title(page, "New Tab");
  adw_tab_view_set_selected_page(self->tab_view, page);
}

PdfvDocumentView *pdfv_window_get_current_view(PdfvWindow *self) {
  g_return_val_if_fail(PDFV_IS_WINDOW(self), NULL);
  return self->current_view;
}

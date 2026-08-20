/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-window.h"
#include "pdfv-document-view.h"
#include "pdfv-workspace.h"
#include <phi/phidocument.h>
#include <phi/phipage.h>

#define WORKSPACE_VISIBLE_MATCHES 4

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
  GMenu *workspace_menu_section;

  /* Sidebar */
  GtkListView *thumbnail_list;
  GtkSingleSelection *thumbnail_selection;
  AdwViewStack *sidebar_stack;
  AdwViewStackPage *workspace_sidebar_page;
  GtkStack *workspace_content_stack;
  GtkSpinner *workspace_loading_spinner;
  AdwStatusPage *workspace_loading_page;
  GtkListView *workspace_list;
  GtkSingleSelection *workspace_selection;
  GtkTreeListModel *workspace_tree;

  /* Workspace and its in-memory index */
  PdfvWorkspace *workspace;
  GCancellable *workspace_scan_cancellable;
  guint workspace_search_debounce_id;
  GCancellable *workspace_search_cancellable;
  gboolean workspace_search_running;
  gboolean workspace_index_dirty;
  gboolean workspace_suppress_preview;
  GHashTable *workspace_document_cache; /* URI -> PhiDocument */
  GQueue workspace_document_cache_order; /* URI strings, oldest first */

  /* Floating workspace search */
  GtkWidget *content_overlay;
  GtkWidget *workspace_search_overlay;
  GtkSearchEntry *workspace_search_entry;
  GtkRevealer *workspace_results_revealer;
  GtkListBox *workspace_results_list;
  AdwAnimation *workspace_results_animation;
  GtkLabel *workspace_search_status;
  GPtrArray *workspace_results;
  gint workspace_result_group;
  gint workspace_result_match;
  guint workspace_group_select_id;
  gint workspace_pending_group;

  /* A search preview always uses its own reusable tab. */
  AdwTabPage *workspace_return_tab;
  AdwTabPage *workspace_preview_tab;
  GFile *workspace_preview_file;
  GCancellable *workspace_preview_cancellable;
  gint workspace_preview_page;
  guint workspace_preview_generation;

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

static GtkWidget *create_tab_content(PdfvWindow *self);
static void open_file_in_tab_async(PdfvWindow *self, GFile *file,
                                   AdwTabPage *page, gint target_page,
                                   gboolean preview, gboolean fit_width,
                                   guint generation);
static void pdfv_window_open_file_internal(PdfvWindow *self, GFile *file,
                                           gboolean fit_width);
static void workspace_search_close(PdfvWindow *self, gboolean commit);
static void workspace_search_schedule(PdfvWindow *self, guint delay_ms);

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
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action),
                                has_document || self->workspace != NULL);
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

static GListModel *workspace_create_children(gpointer item,
                                             gpointer user_data) {
  (void)user_data;
  PdfvWorkspaceItem *workspace_item = PDFV_WORKSPACE_ITEM(item);
  if (!pdfv_workspace_item_is_folder(workspace_item))
    return NULL;
  return g_object_ref(pdfv_workspace_item_get_children(workspace_item));
}

static void workspace_set_pdf_icon(GtkImage *image) {
  GIcon *icon = g_content_type_get_symbolic_icon("application/pdf");
  gtk_image_set_from_gicon(image, icon);
  g_object_unref(icon);
}

static void workspace_factory_setup(GtkSignalListItemFactory *factory,
                                    GtkListItem *list_item,
                                    PdfvWindow *self) {
  (void)factory;
  (void)self;
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon = gtk_image_new();
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_margin_top(box, 5);
  gtk_widget_set_margin_bottom(box, 5);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  g_object_set_data(G_OBJECT(list_item), "workspace-icon", icon);
  g_object_set_data(G_OBJECT(list_item), "workspace-label", label);
  gtk_list_item_set_child(list_item, expander);
}

static void workspace_factory_bind(GtkSignalListItemFactory *factory,
                                   GtkListItem *list_item,
                                   PdfvWindow *self) {
  (void)factory;
  (void)self;
  GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(list_item));
  GtkTreeExpander *expander =
      GTK_TREE_EXPANDER(gtk_list_item_get_child(list_item));
  gtk_tree_expander_set_list_row(expander, row);
  PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
  GtkImage *icon = GTK_IMAGE(
      g_object_get_data(G_OBJECT(list_item), "workspace-icon"));
  GtkLabel *label = GTK_LABEL(
      g_object_get_data(G_OBJECT(list_item), "workspace-label"));
  if (pdfv_workspace_item_is_folder(item))
    gtk_image_set_from_icon_name(icon, "folder-symbolic");
  else
    workspace_set_pdf_icon(icon);
  gtk_label_set_text(label, pdfv_workspace_item_get_name(item));
  gtk_widget_set_tooltip_text(GTK_WIDGET(label),
                              pdfv_workspace_item_get_relative_path(item));
  g_object_unref(item);
}

static void workspace_factory_unbind(GtkSignalListItemFactory *factory,
                                     GtkListItem *list_item,
                                     PdfvWindow *self) {
  (void)factory;
  (void)self;
  GtkTreeExpander *expander =
      GTK_TREE_EXPANDER(gtk_list_item_get_child(list_item));
  gtk_tree_expander_set_list_row(expander, NULL);
}

static void on_workspace_item_activated(GtkListView *list, guint position,
                                        PdfvWindow *self) {
  (void)list;
  if (!self->workspace_tree || position == GTK_INVALID_LIST_POSITION)
    return;
  GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(self->workspace_tree),
                                               position);
  if (!row)
    return;
  PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
  if (pdfv_workspace_item_is_folder(item)) {
    gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
  } else {
    pdfv_window_open_file_internal(self, pdfv_workspace_item_get_file(item),
                                   TRUE);
  }
  g_object_unref(item);
  g_object_unref(row);
}

static PdfvWorkspaceResultGroup *workspace_selected_group(PdfvWindow *self) {
  if (!self->workspace_results || self->workspace_result_group < 0 ||
      self->workspace_result_group >= (gint)self->workspace_results->len)
    return NULL;
  return g_ptr_array_index(self->workspace_results,
                           self->workspace_result_group);
}

static PdfvWorkspaceMatch *workspace_selected_match(PdfvWindow *self) {
  PdfvWorkspaceResultGroup *group = workspace_selected_group(self);
  if (!group || self->workspace_result_match < 0 ||
      self->workspace_result_match >= (gint)group->matches->len)
    return NULL;
  return g_ptr_array_index(group->matches, self->workspace_result_match);
}

static GtkListBoxRow *workspace_find_result_row(PdfvWindow *self, gint group,
                                                gint match) {
  for (GtkWidget *child = gtk_widget_get_first_child(
           GTK_WIDGET(self->workspace_results_list));
       child; child = gtk_widget_get_next_sibling(child)) {
    if (!GTK_IS_LIST_BOX_ROW(child))
      continue;
    gint child_group = GPOINTER_TO_INT(
                           g_object_get_data(G_OBJECT(child), "result-group")) -
                       1;
    gint child_match = GPOINTER_TO_INT(
                           g_object_get_data(G_OBJECT(child), "result-match")) -
                       1;
    if (child_group == group && child_match == match)
      return GTK_LIST_BOX_ROW(child);
  }
  return NULL;
}

static void workspace_search_set_status(PdfvWindow *self,
                                        const gchar *status) {
  gboolean visible = status && *status;
  gtk_label_set_text(self->workspace_search_status, visible ? status : "");
  gtk_widget_set_visible(GTK_WIDGET(self->workspace_search_status), visible);
}

static void workspace_document_cache_clear(PdfvWindow *self) {
  if (self->workspace_document_cache)
    g_hash_table_remove_all(self->workspace_document_cache);
  g_queue_clear_full(&self->workspace_document_cache_order, g_free);
}

static void workspace_document_cache_touch(PdfvWindow *self,
                                           const gchar *uri) {
  GList *link = g_queue_find_custom(&self->workspace_document_cache_order, uri,
                                    (GCompareFunc)g_strcmp0);
  if (link) {
    g_free(link->data);
    g_queue_delete_link(&self->workspace_document_cache_order, link);
  }
  g_queue_push_tail(&self->workspace_document_cache_order, g_strdup(uri));
}

static PhiDocument *workspace_document_cache_lookup(PdfvWindow *self,
                                                    GFile *file) {
  gchar *uri = g_file_get_uri(file);
  PhiDocument *document = g_hash_table_lookup(self->workspace_document_cache,
                                              uri);
  if (document) {
    workspace_document_cache_touch(self, uri);
    g_object_ref(document);
  }
  g_free(uri);
  return document;
}

static void workspace_document_cache_store(PdfvWindow *self, GFile *file,
                                           PhiDocument *document) {
  gchar *uri = g_file_get_uri(file);
  g_hash_table_replace(self->workspace_document_cache, g_strdup(uri),
                       g_object_ref(document));
  workspace_document_cache_touch(self, uri);
  while (g_queue_get_length(&self->workspace_document_cache_order) > 4) {
    gchar *oldest = g_queue_pop_head(&self->workspace_document_cache_order);
    g_hash_table_remove(self->workspace_document_cache, oldest);
    g_free(oldest);
  }
  g_free(uri);
}

static void workspace_preview_selected(PdfvWindow *self) {
  PdfvWorkspaceResultGroup *group = workspace_selected_group(self);
  PdfvWorkspaceMatch *match = workspace_selected_match(self);
  if (!group || !match)
    return;

  if (!self->workspace_preview_tab) {
    GtkWidget *content = create_tab_content(self);
    self->workspace_preview_tab = adw_tab_view_append(self->tab_view, content);
    adw_tab_page_set_title(self->workspace_preview_tab, "Loading…");
  }
  self->workspace_preview_page = match->page;

  if (self->workspace_preview_file &&
      g_file_equal(self->workspace_preview_file, group->file)) {
    GtkWidget *stack = adw_tab_page_get_child(self->workspace_preview_tab);
    PdfvDocumentView *view =
        g_object_get_data(G_OBJECT(stack), "document-view");
    if (pdfv_document_view_get_document(view)) {
      adw_tab_view_set_selected_page(self->tab_view,
                                     self->workspace_preview_tab);
      pdfv_document_view_go_to_page(view, match->page);
    }
    return;
  }

  g_clear_object(&self->workspace_preview_file);
  self->workspace_preview_file = g_object_ref(group->file);
  if (self->workspace_preview_cancellable)
    g_cancellable_cancel(self->workspace_preview_cancellable);
  g_clear_object(&self->workspace_preview_cancellable);
  self->workspace_preview_cancellable = g_cancellable_new();
  self->workspace_preview_generation++;

  PhiDocument *cached =
      workspace_document_cache_lookup(self, group->file);
  if (cached) {
    GtkWidget *stack = adw_tab_page_get_child(self->workspace_preview_tab);
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    PdfvDocumentView *view =
        g_object_get_data(G_OBJECT(stack), "document-view");
    pdfv_document_view_set_document(view, cached);
    pdfv_document_view_go_to_page(view, match->page);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "document");
    pdfv_document_view_zoom_fit_width(view);
    g_object_set_data_full(G_OBJECT(stack), "document-file",
                           g_object_ref(group->file), g_object_unref);
    gchar *basename = g_file_get_basename(group->file);
    adw_tab_page_set_title(self->workspace_preview_tab, basename);
    g_free(basename);
    adw_tab_view_set_selected_page(self->tab_view,
                                   self->workspace_preview_tab);
    self->current_view = view;
    populate_thumbnails(self, cached);
    update_navigation_buttons(self);
    update_zoom_info(self);
    update_sidebar_button(self);
    g_object_unref(cached);
    return;
  }

  /* Keep the current document visible while an uncached PDF opens. The
   * search remains fully navigable and the preview appears only when ready. */
  if (adw_tab_view_get_selected_page(self->tab_view) ==
          self->workspace_preview_tab &&
      self->workspace_return_tab &&
      adw_tab_view_get_page_position(self->tab_view,
                                     self->workspace_return_tab) >= 0)
    adw_tab_view_set_selected_page(self->tab_view,
                                   self->workspace_return_tab);
  open_file_in_tab_async(self, group->file, self->workspace_preview_tab,
                         match->page, TRUE, TRUE,
                         self->workspace_preview_generation);
}

static void workspace_results_render(PdfvWindow *self);
static void workspace_select_result(PdfvWindow *self, gint group, gint match,
                                    gboolean preview);

static gboolean workspace_select_group_idle(gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  self->workspace_group_select_id = 0;
  if (self->workspace_results && self->workspace_pending_group >= 0 &&
      self->workspace_pending_group < (gint)self->workspace_results->len)
    workspace_select_result(self, self->workspace_pending_group, 0, TRUE);
  return G_SOURCE_REMOVE;
}

static void on_workspace_result_selected(GtkListBox *box, GtkListBoxRow *row,
                                         PdfvWindow *self) {
  (void)box;
  if (!row || self->workspace_suppress_preview)
    return;
  gint group =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "result-group")) - 1;
  gint match =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "result-match")) - 1;
  if (group < 0)
    return;
  if (match < 0) {
    self->workspace_pending_group = group;
    if (!self->workspace_group_select_id)
      self->workspace_group_select_id =
          g_idle_add(workspace_select_group_idle, self);
    return;
  }
  self->workspace_result_group = group;
  self->workspace_result_match = match;
  workspace_preview_selected(self);
}

static void workspace_results_animate(PdfvWindow *self) {
  g_clear_object(&self->workspace_results_animation);
  gtk_widget_set_opacity(GTK_WIDGET(self->workspace_results_list), 0.82);
  AdwAnimationTarget *target = adw_property_animation_target_new(
      G_OBJECT(self->workspace_results_list), "opacity");
  self->workspace_results_animation = adw_timed_animation_new(
      GTK_WIDGET(self->workspace_results_list), 0.82, 1.0, 130, target);
  adw_timed_animation_set_easing(
      ADW_TIMED_ANIMATION(self->workspace_results_animation),
      ADW_EASE_OUT_CUBIC);
  adw_animation_play(self->workspace_results_animation);
}

static void workspace_select_result(PdfvWindow *self, gint group, gint match,
                                    gboolean preview) {
  if (self->workspace_group_select_id) {
    g_source_remove(self->workspace_group_select_id);
    self->workspace_group_select_id = 0;
  }
  self->workspace_pending_group = -1;
  if (!self->workspace_results || self->workspace_results->len == 0)
    return;
  group = CLAMP(group, 0, (gint)self->workspace_results->len - 1);
  PdfvWorkspaceResultGroup *selected =
      g_ptr_array_index(self->workspace_results, group);
  if (selected->matches->len == 0)
    return;
  match = CLAMP(match, 0, (gint)selected->matches->len - 1);
  gboolean group_changed = group != self->workspace_result_group;
  self->workspace_result_group = group;
  self->workspace_result_match = match;
  workspace_results_render(self);
  if (group_changed)
    workspace_results_animate(self);
  gtk_widget_grab_focus(GTK_WIDGET(self->workspace_search_entry));
  if (preview)
    workspace_preview_selected(self);
}

static void workspace_result_row_activated(GtkListBox *box,
                                           GtkListBoxRow *row,
                                           PdfvWindow *self) {
  (void)box;
  if (!row)
    return;
  gint group =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "result-group")) - 1;
  gint match =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "result-match")) - 1;
  if (group < 0)
    return;
  if (match < 0) {
    workspace_select_result(self, group, 0, TRUE);
    return;
  }
  self->workspace_result_group = group;
  self->workspace_result_match = match;
  workspace_preview_selected(self);
  workspace_search_close(self, TRUE);
}

static void workspace_results_clear(PdfvWindow *self) {
  if (self->workspace_group_select_id) {
    g_source_remove(self->workspace_group_select_id);
    self->workspace_group_select_id = 0;
  }
  self->workspace_pending_group = -1;
  g_clear_object(&self->workspace_results_animation);
  gtk_widget_set_opacity(GTK_WIDGET(self->workspace_results_list), 1.0);
  gtk_list_box_remove_all(self->workspace_results_list);
  gtk_revealer_set_reveal_child(self->workspace_results_revealer, FALSE);
  g_clear_pointer(&self->workspace_results, g_ptr_array_unref);
  self->workspace_result_group = -1;
  self->workspace_result_match = -1;
}

static void workspace_move_result(PdfvWindow *self, gint direction) {
  PdfvWorkspaceResultGroup *group = workspace_selected_group(self);
  if (!group || group->matches->len == 0 || direction == 0)
    return;

  gint next_group = self->workspace_result_group;
  gint next_match = self->workspace_result_match + direction;
  if (next_match >= (gint)group->matches->len) {
    if (next_group + 1 >= (gint)self->workspace_results->len)
      return;
    next_group++;
    next_match = 0;
  } else if (next_match < 0) {
    if (next_group == 0)
      return;
    next_group--;
    group = g_ptr_array_index(self->workspace_results, next_group);
    next_match = (gint)group->matches->len - 1;
  }
  workspace_select_result(self, next_group, next_match, TRUE);
}

static void workspace_result_previous(GtkButton *button, PdfvWindow *self) {
  (void)button;
  workspace_move_result(self, -1);
}

static void workspace_result_next(GtkButton *button, PdfvWindow *self) {
  (void)button;
  workspace_move_result(self, 1);
}

static GtkWidget *workspace_group_header(PdfvWindow *self, gint group_index,
                                         guint visible_start,
                                         guint visible_end) {
  PdfvWorkspaceResultGroup *group =
      g_ptr_array_index(self->workspace_results, group_index);
  gboolean active = group_index == self->workspace_result_group;
  GtkWidget *row = gtk_list_box_row_new();
  g_object_set_data(G_OBJECT(row), "result-group",
                    GINT_TO_POINTER(group_index + 1));
  g_object_set_data(G_OBJECT(row), "result-match", GINT_TO_POINTER(0));
  gtk_widget_add_css_class(row, "workspace-result-header");
  gtk_widget_set_cursor_from_name(row, "pointer");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  GtkWidget *icon = gtk_image_new();
  workspace_set_pdf_icon(GTK_IMAGE(icon));
  GtkWidget *label = gtk_label_new(group->relative_path);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, "heading");
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);

  gchar *count_text = NULL;
  if (active && group->matches->len > WORKSPACE_VISIBLE_MATCHES)
    count_text = g_strdup_printf("%u–%u of %u", visible_start + 1,
                                 visible_end, group->matches->len);
  else
    count_text = g_strdup_printf("%u match%s", group->matches->len,
                                 group->matches->len == 1 ? "" : "es");
  GtkWidget *count = gtk_label_new(count_text);
  g_free(count_text);
  gtk_widget_add_css_class(count, "dim-label");
  gtk_box_append(GTK_BOX(box), count);

  if (active && (self->workspace_results->len > 1 ||
                 group->matches->len > 1)) {
    GtkWidget *previous =
        gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(previous, "flat");
    gtk_widget_add_css_class(previous, "circular");
    gtk_widget_set_tooltip_text(previous, "Previous result");
    gtk_widget_set_sensitive(
        previous,
        self->workspace_result_group > 0 || self->workspace_result_match > 0);
    g_signal_connect(previous, "clicked",
                     G_CALLBACK(workspace_result_previous), self);
    gtk_box_append(GTK_BOX(box), previous);
    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(next, "flat");
    gtk_widget_add_css_class(next, "circular");
    gtk_widget_set_tooltip_text(next, "Next result");
    gtk_widget_set_sensitive(
        next,
        self->workspace_result_group + 1 <
                (gint)self->workspace_results->len ||
            self->workspace_result_match + 1 < (gint)group->matches->len);
    g_signal_connect(next, "clicked", G_CALLBACK(workspace_result_next), self);
    gtk_box_append(GTK_BOX(box), next);
  }
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

static void workspace_results_render(PdfvWindow *self) {
  gtk_list_box_remove_all(self->workspace_results_list);
  if (!self->workspace_results || self->workspace_results->len == 0)
    return;

  self->workspace_result_group =
      CLAMP(self->workspace_result_group, 0,
            (gint)self->workspace_results->len - 1);
  PdfvWorkspaceResultGroup *selected = workspace_selected_group(self);
  self->workspace_result_match =
      CLAMP(self->workspace_result_match, 0, (gint)selected->matches->len - 1);

  for (guint g = 0; g < self->workspace_results->len; g++) {
    PdfvWorkspaceResultGroup *group =
        g_ptr_array_index(self->workspace_results, g);
    guint start = 0;
    guint end = 0;
    if ((gint)g == self->workspace_result_group) {
      start = (self->workspace_result_match / WORKSPACE_VISIBLE_MATCHES) *
              WORKSPACE_VISIBLE_MATCHES;
      end = MIN(start + WORKSPACE_VISIBLE_MATCHES, group->matches->len);
    }
    gtk_list_box_append(self->workspace_results_list,
                        workspace_group_header(self, g, start, end));
    if ((gint)g != self->workspace_result_group)
      continue;

    for (guint m = start; m < end; m++) {
      PdfvWorkspaceMatch *match = g_ptr_array_index(group->matches, m);
      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
      gchar *title = g_strdup_printf("Page %d", match->page + 1);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
      adw_action_row_set_subtitle(row, match->snippet);
      adw_action_row_set_subtitle_lines(row, 2);
      g_free(title);
      g_object_set_data(G_OBJECT(row), "result-group",
                        GINT_TO_POINTER((gint)g + 1));
      g_object_set_data(G_OBJECT(row), "result-match",
                        GINT_TO_POINTER((gint)m + 1));
      gtk_list_box_append(self->workspace_results_list, GTK_WIDGET(row));
    }
  }

  GtkListBoxRow *row = workspace_find_result_row(
      self, self->workspace_result_group, self->workspace_result_match);
  self->workspace_suppress_preview = TRUE;
  gtk_list_box_select_row(self->workspace_results_list, row);
  self->workspace_suppress_preview = FALSE;
}

static void workspace_results_show(PdfvWindow *self, GPtrArray *groups) {
  GFile *old_file = NULL;
  gint old_page = -1;
  PdfvWorkspaceResultGroup *old_group = workspace_selected_group(self);
  PdfvWorkspaceMatch *old_match = workspace_selected_match(self);
  if (old_group && old_match) {
    old_file = g_object_ref(old_group->file);
    old_page = old_match->page;
  }

  g_clear_pointer(&self->workspace_results, g_ptr_array_unref);
  self->workspace_results = groups;
  self->workspace_result_group = -1;
  self->workspace_result_match = -1;

  gint selected_group = 0;
  gint selected_match = 0;
  guint match_count = 0;
  for (guint g = 0; g < groups->len; g++) {
    PdfvWorkspaceResultGroup *group = g_ptr_array_index(groups, g);
    for (guint m = 0; m < group->matches->len; m++) {
      PdfvWorkspaceMatch *match = g_ptr_array_index(group->matches, m);
      match_count++;
      if (old_file && g_file_equal(old_file, group->file) &&
          old_page == match->page) {
        selected_group = g;
        selected_match = m;
      }
    }
  }
  g_clear_object(&old_file);

  guint indexed = pdfv_workspace_get_indexed_count(self->workspace);
  guint total = pdfv_workspace_get_pdf_count(self->workspace);
  gchar *status = total > indexed
                      ? g_strdup_printf("Indexing %u of %u PDFs…", indexed,
                                        total)
                  : match_count == 0 ? g_strdup("No results") : NULL;
  workspace_search_set_status(self, status);
  g_free(status);

  if (groups->len > 0) {
    self->workspace_result_group = selected_group;
    self->workspace_result_match = selected_match;
    workspace_results_render(self);
    gtk_revealer_set_reveal_child(self->workspace_results_revealer, TRUE);
  } else {
    g_clear_object(&self->workspace_results_animation);
    gtk_widget_set_opacity(GTK_WIDGET(self->workspace_results_list), 1.0);
    gtk_list_box_remove_all(self->workspace_results_list);
    gtk_revealer_set_reveal_child(self->workspace_results_revealer, FALSE);
  }
}

static void on_workspace_search_finished(GObject *source, GAsyncResult *result,
                                         gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GError *error = NULL;
  GPtrArray *groups = pdfv_workspace_search_finish(PDFV_WORKSPACE(source),
                                                   result, &error);
  gboolean current =
      G_IS_TASK(result) &&
      g_task_get_cancellable(G_TASK(result)) ==
          self->workspace_search_cancellable;
  if (current) {
    self->workspace_search_running = FALSE;
    gboolean refresh = self->workspace_index_dirty;
    self->workspace_index_dirty = FALSE;
    if (source == G_OBJECT(self->workspace) && groups &&
        gtk_widget_get_visible(self->workspace_search_overlay))
      workspace_results_show(self, groups);
    else if (groups)
      g_ptr_array_unref(groups);
    if (refresh && self->workspace &&
        gtk_widget_get_visible(self->workspace_search_overlay))
      workspace_search_schedule(self, 400);
  } else if (groups) {
    g_ptr_array_unref(groups);
  }
  if (current && error &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    workspace_search_set_status(self, error->message);
  g_clear_error(&error);
  g_object_unref(self);
}

static gboolean workspace_search_start(gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  self->workspace_search_debounce_id = 0;
  const gchar *query =
      gtk_editable_get_text(GTK_EDITABLE(self->workspace_search_entry));
  if (!self->workspace || g_utf8_strlen(query, -1) < 2)
    return G_SOURCE_REMOVE;

  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);
  g_clear_object(&self->workspace_search_cancellable);
  self->workspace_search_cancellable = g_cancellable_new();
  self->workspace_search_running = TRUE;
  self->workspace_index_dirty = FALSE;
  if (!self->workspace_results)
    workspace_search_set_status(self, "Searching…");
  AdwTabPage *origin_page = self->workspace_return_tab;
  GFile *origin_file = NULL;
  if (origin_page &&
      adw_tab_view_get_page_position(self->tab_view, origin_page) >= 0) {
    GtkWidget *stack = adw_tab_page_get_child(origin_page);
    origin_file = g_object_get_data(G_OBJECT(stack), "document-file");
  }
  pdfv_workspace_search_near_async(
      self->workspace, query, origin_file, self->workspace_search_cancellable,
      on_workspace_search_finished, g_object_ref(self));
  return G_SOURCE_REMOVE;
}

static void workspace_search_schedule(PdfvWindow *self, guint delay_ms) {
  if (self->workspace_search_debounce_id)
    g_source_remove(self->workspace_search_debounce_id);
  self->workspace_search_debounce_id =
      g_timeout_add(delay_ms, workspace_search_start, self);
}

static void on_workspace_search_changed(GtkSearchEntry *entry,
                                        PdfvWindow *self) {
  const gchar *query = gtk_editable_get_text(GTK_EDITABLE(entry));
  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);
  g_clear_object(&self->workspace_search_cancellable);
  self->workspace_search_running = FALSE;
  self->workspace_index_dirty = FALSE;
  if (g_utf8_strlen(query, -1) < 2) {
    if (self->workspace_search_debounce_id) {
      g_source_remove(self->workspace_search_debounce_id);
      self->workspace_search_debounce_id = 0;
    }
    workspace_results_clear(self);
    workspace_search_set_status(
        self, *query ? "Type at least 2 characters" : NULL);
    return;
  }
  workspace_results_clear(self);
  workspace_search_set_status(self, "Searching…");
  workspace_search_schedule(self, 180);
}

static void on_workspace_index_updated(PdfvWorkspace *workspace,
                                       PdfvWindow *self) {
  if (workspace != self->workspace ||
      !gtk_widget_get_visible(self->workspace_search_overlay))
    return;
  const gchar *query = gtk_editable_get_text(
      GTK_EDITABLE(self->workspace_search_entry));
  if (g_utf8_strlen(query, -1) < 2)
    return;
  if (self->workspace_search_running) {
    self->workspace_index_dirty = TRUE;
    return;
  }
  if (!self->workspace_search_debounce_id)
    self->workspace_search_debounce_id =
        g_timeout_add(400, workspace_search_start, self);
}

static gboolean on_workspace_search_key(GtkEventControllerKey *controller,
                                        guint keyval, guint keycode,
                                        GdkModifierType state,
                                        PdfvWindow *self) {
  (void)controller;
  (void)keycode;
  (void)state;
  switch (keyval) {
  case GDK_KEY_Down:
    workspace_move_result(self, 1);
    return GDK_EVENT_STOP;
  case GDK_KEY_Up:
    workspace_move_result(self, -1);
    return GDK_EVENT_STOP;
  case GDK_KEY_Right:
    if (self->workspace_results && self->workspace_results->len > 0)
      workspace_select_result(
          self, (self->workspace_result_group + 1) %
                    (gint)self->workspace_results->len,
          self->workspace_result_match, TRUE);
    return GDK_EVENT_STOP;
  case GDK_KEY_Left:
    if (self->workspace_results && self->workspace_results->len > 0)
      workspace_select_result(
          self,
          (self->workspace_result_group - 1 +
           (gint)self->workspace_results->len) %
              (gint)self->workspace_results->len,
          self->workspace_result_match, TRUE);
    return GDK_EVENT_STOP;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
    if (workspace_selected_match(self))
      workspace_search_close(self, TRUE);
    return GDK_EVENT_STOP;
  case GDK_KEY_Escape:
    workspace_search_close(self, FALSE);
    return GDK_EVENT_STOP;
  default:
    return GDK_EVENT_PROPAGATE;
  }
}

static void on_workspace_search_stop(GtkSearchEntry *entry,
                                     PdfvWindow *self) {
  (void)entry;
  workspace_search_close(self, FALSE);
}

static void workspace_search_open(PdfvWindow *self) {
  if (!self->workspace)
    return;
  if (gtk_widget_get_visible(self->workspace_search_overlay)) {
    gtk_widget_grab_focus(GTK_WIDGET(self->workspace_search_entry));
    return;
  }
  self->workspace_return_tab = adw_tab_view_get_selected_page(self->tab_view);
  if (self->workspace_return_tab)
    g_object_ref(self->workspace_return_tab);
  gtk_widget_set_visible(self->workspace_search_overlay, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(self->workspace_search_entry));
  const gchar *query = gtk_editable_get_text(
      GTK_EDITABLE(self->workspace_search_entry));
  if (g_utf8_strlen(query, -1) >= 2) {
    workspace_results_clear(self);
    workspace_search_set_status(self, "Searching…");
    workspace_search_schedule(self, 0);
  } else {
    workspace_search_set_status(
        self, *query ? "Type at least 2 characters" : NULL);
  }
}

static void workspace_search_close(PdfvWindow *self, gboolean commit) {
  if (!gtk_widget_get_visible(self->workspace_search_overlay))
    return;
  gtk_widget_set_visible(self->workspace_search_overlay, FALSE);
  if (self->workspace_search_debounce_id) {
    g_source_remove(self->workspace_search_debounce_id);
    self->workspace_search_debounce_id = 0;
  }
  if (self->workspace_group_select_id) {
    g_source_remove(self->workspace_group_select_id);
    self->workspace_group_select_id = 0;
  }
  self->workspace_pending_group = -1;
  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);

  if (commit && self->workspace_preview_tab) {
    adw_tab_view_set_selected_page(self->tab_view,
                                   self->workspace_preview_tab);
    self->workspace_preview_tab = NULL;
    g_clear_object(&self->workspace_preview_file);
    /* The committed request owns its own reference and may still finish.
     * Future previews must not cancel that now-independent tab. */
    g_clear_object(&self->workspace_preview_cancellable);
  } else if (!commit) {
    if (self->workspace_preview_cancellable)
      g_cancellable_cancel(self->workspace_preview_cancellable);
    self->workspace_preview_generation++;
    if (self->workspace_preview_tab) {
      AdwTabPage *preview = self->workspace_preview_tab;
      self->workspace_preview_tab = NULL;
      g_clear_object(&self->workspace_preview_file);
      adw_tab_view_close_page(self->tab_view, preview);
    }
    if (self->workspace_return_tab &&
        adw_tab_view_get_page_position(self->tab_view,
                                       self->workspace_return_tab) >= 0)
      adw_tab_view_set_selected_page(self->tab_view,
                                     self->workspace_return_tab);
  }
  g_clear_object(&self->workspace_return_tab);
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

  GtkWidget *loading = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(loading),
                                "document-open-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(loading), "Opening PDF…");
  adw_status_page_set_description(ADW_STATUS_PAGE(loading),
                                  "Preparing pages in the background");
  gtk_stack_add_named(GTK_STACK(stack), loading, "loading");

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

typedef struct {
  PdfvWindow *window;
  AdwTabPage *page;
  GFile *file;
  GCancellable *cancellable;
  gint target_page;
  gboolean preview;
  gboolean fit_width;
  guint generation;
} OpenRequest;

typedef struct {
  guint settled_frames;
} FitWidthRequest;

static gboolean fit_width_after_allocate(GtkWidget *widget,
                                         GdkFrameClock *frame_clock,
                                         gpointer user_data) {
  (void)frame_clock;
  FitWidthRequest *request = user_data;
  if (!gtk_widget_get_mapped(widget) || gtk_widget_get_width(widget) <= 1)
    return G_SOURCE_CONTINUE;

  /* Document loading and split-view changes each queue a layout. Waiting for
   * a second allocated frame prevents fitting against the pre-sidebar width. */
  if (request->settled_frames++ == 0)
    return G_SOURCE_CONTINUE;

  PdfvDocumentView *view = PDFV_DOCUMENT_VIEW(widget);
  if (pdfv_document_view_get_document(view))
    pdfv_document_view_zoom_fit_width(view);
  return G_SOURCE_REMOVE;
}

static void open_request_free(OpenRequest *request) {
  g_clear_object(&request->window);
  g_clear_object(&request->page);
  g_clear_object(&request->file);
  g_clear_object(&request->cancellable);
  g_free(request);
}

static void on_document_loaded(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  (void)source;
  OpenRequest *request = user_data;
  PdfvWindow *self = request->window;
  GError *error = NULL;
  PhiDocument *document =
      pdfv_workspace_load_document_finish(result, &error);
  gboolean page_is_open =
      adw_tab_view_get_page_position(self->tab_view, request->page) >= 0;
  gboolean stale_preview =
      request->preview && request->generation != self->workspace_preview_generation;
  if (document && request->preview && !stale_preview && self->workspace)
    workspace_document_cache_store(self, request->file, document);
  GtkWidget *target_stack =
      page_is_open ? adw_tab_page_get_child(request->page) : NULL;
  gboolean current_request =
      target_stack &&
      g_object_get_data(G_OBJECT(target_stack), "open-cancellable") ==
          request->cancellable;

  if (document && current_request && !stale_preview) {
    GtkWidget *stack = target_stack;
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    PdfvDocumentView *view =
        g_object_get_data(G_OBJECT(stack), "document-view");
    pdfv_document_view_set_document(view, document);
    gint target_page = request->preview ? self->workspace_preview_page
                                       : request->target_page;
    pdfv_document_view_go_to_page(view, target_page);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "document");
    if (request->fit_width) {
      FitWidthRequest *fit_request = g_new0(FitWidthRequest, 1);
      gtk_widget_add_tick_callback(GTK_WIDGET(view), fit_width_after_allocate,
                                   fit_request, g_free);
    }
    g_object_set_data_full(G_OBJECT(stack), "document-file",
                           g_object_ref(request->file), g_object_unref);
    gchar *basename = g_file_get_basename(request->file);
    adw_tab_page_set_title(request->page, basename);
    g_free(basename);

    if (request->preview)
      adw_tab_view_set_selected_page(self->tab_view, request->page);

    if (adw_tab_view_get_selected_page(self->tab_view) == request->page) {
      self->current_view = view;
      populate_thumbnails(self, document);
      update_navigation_buttons(self);
      update_zoom_info(self);
      update_sidebar_button(self);
    }
  } else if (error &&
             !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
             current_request && !stale_preview) {
    GtkWidget *stack = target_stack;
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    AdwDialog *dialog = adw_alert_dialog_new("Error Opening File",
                                             error->message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(self));
  }

  g_clear_error(&error);
  g_clear_object(&document);
  open_request_free(request);
}

static void open_file_in_tab_async(PdfvWindow *self, GFile *file,
                                   AdwTabPage *page, gint target_page,
                                   gboolean preview, gboolean fit_width,
                                   guint generation) {
  GtkWidget *stack = adw_tab_page_get_child(page);
  GCancellable *previous =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (previous)
    g_cancellable_cancel(previous);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
  gchar *basename = g_file_get_basename(file);
  adw_tab_page_set_title(page, basename);
  g_free(basename);

  OpenRequest *request = g_new0(OpenRequest, 1);
  request->window = g_object_ref(self);
  request->page = g_object_ref(page);
  request->file = g_object_ref(file);
  request->target_page = target_page;
  request->preview = preview;
  request->fit_width = fit_width;
  request->generation = generation;
  request->cancellable =
      preview ? g_object_ref(self->workspace_preview_cancellable)
              : g_cancellable_new();
  g_object_set_data_full(G_OBJECT(stack), "open-cancellable",
                         g_object_ref(request->cancellable), g_object_unref);
  pdfv_workspace_load_document_async(file, target_page, request->cancellable,
                                     on_document_loaded, request);
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
    if (!self->workspace)
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
      if (!self->workspace)
        adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
      populate_thumbnails(self, NULL);
    }
  }
}

static gboolean on_tab_close_page(AdwTabView *tab_view, AdwTabPage *page,
                                  PdfvWindow *self) {
  if (page == self->workspace_preview_tab) {
    self->workspace_preview_tab = NULL;
    self->workspace_preview_generation++;
    if (self->workspace_preview_cancellable)
      g_cancellable_cancel(self->workspace_preview_cancellable);
    g_clear_object(&self->workspace_preview_file);
  }
  if (page == self->workspace_return_tab)
    g_clear_object(&self->workspace_return_tab);

  /* Clear current_view if we're closing its tab */
  GtkWidget *stack = adw_tab_page_get_child(page);
  GCancellable *open_cancellable =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (open_cancellable)
    g_cancellable_cancel(open_cancellable);
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

static gchar *workspace_state_file(void) {
  return g_build_filename(g_get_user_config_dir(), "phi-pdf-viewer",
                          "state.ini", NULL);
}

static void remember_workspace(GFile *folder) {
  gchar *filename = workspace_state_file();
  gchar *directory = g_path_get_dirname(filename);
  GKeyFile *state = g_key_file_new();
  g_key_file_load_from_file(state, filename, G_KEY_FILE_KEEP_COMMENTS, NULL);
  if (folder) {
    gchar *uri = g_file_get_uri(folder);
    g_key_file_set_string(state, "workspace", "uri", uri);
    g_free(uri);
  } else {
    g_key_file_remove_key(state, "workspace", "uri", NULL);
  }

  gsize length = 0;
  gchar *contents = g_key_file_to_data(state, &length, NULL);
  if (g_mkdir_with_parents(directory, 0700) != 0 ||
      !g_file_set_contents(filename, contents, length, NULL))
    g_debug("Could not save workspace state at %s", filename);
  g_free(contents);
  g_key_file_unref(state);
  g_free(directory);
  g_free(filename);
}

static GFile *get_remembered_workspace(void) {
  gchar *filename = workspace_state_file();
  GKeyFile *state = g_key_file_new();
  GFile *folder = NULL;
  if (g_key_file_load_from_file(state, filename, G_KEY_FILE_NONE, NULL)) {
    gchar *uri = g_key_file_get_string(state, "workspace", "uri", NULL);
    if (uri && *uri)
      folder = g_file_new_for_uri(uri);
    g_free(uri);
  }
  g_key_file_unref(state);
  g_free(filename);
  return folder;
}

static void close_workspace(PdfvWindow *self, gboolean forget) {
  workspace_search_close(self, FALSE);
  if (self->workspace_scan_cancellable)
    g_cancellable_cancel(self->workspace_scan_cancellable);
  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);
  if (self->workspace_preview_cancellable)
    g_cancellable_cancel(self->workspace_preview_cancellable);
  if (self->workspace) {
    pdfv_workspace_cancel(self->workspace);
    g_signal_handlers_disconnect_by_data(self->workspace, self);
  }

  workspace_results_clear(self);
  gtk_editable_set_text(GTK_EDITABLE(self->workspace_search_entry), "");
  gtk_single_selection_set_model(self->workspace_selection, NULL);
  g_clear_object(&self->workspace_tree);
  g_clear_object(&self->workspace);
  g_clear_object(&self->workspace_scan_cancellable);
  g_clear_object(&self->workspace_search_cancellable);
  g_clear_object(&self->workspace_preview_cancellable);
  g_clear_object(&self->workspace_preview_file);
  workspace_document_cache_clear(self);
  self->workspace_search_running = FALSE;
  self->workspace_index_dirty = FALSE;
  self->workspace_suppress_preview = FALSE;
  gtk_spinner_stop(self->workspace_loading_spinner);

  adw_view_stack_page_set_visible(self->workspace_sidebar_page, FALSE);
  adw_view_stack_set_visible_child_name(self->sidebar_stack, "pages");
  g_menu_remove_all(self->workspace_menu_section);
  GAction *search_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "workspace-search");
  GAction *close_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "close-workspace");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(search_action), FALSE);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(close_action), FALSE);
  if (forget)
    remember_workspace(NULL);

  gboolean has_document =
      self->current_view &&
      pdfv_document_view_get_document(self->current_view) != NULL;
  if (!has_document)
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
  update_sidebar_button(self);
}

static void on_workspace_loaded(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  PdfvWorkspace *workspace = PDFV_WORKSPACE(source);
  GError *error = NULL;
  gboolean loaded = pdfv_workspace_load_finish(workspace, result, &error);
  if (workspace == self->workspace && loaded) {
    gtk_spinner_stop(self->workspace_loading_spinner);
    gtk_stack_set_visible_child_name(self->workspace_content_stack, "files");
    g_clear_object(&self->workspace_tree);
    self->workspace_tree = gtk_tree_list_model_new(
        g_object_ref(pdfv_workspace_get_items(workspace)), FALSE, FALSE,
        workspace_create_children, self, NULL);
    gtk_single_selection_set_model(
        self->workspace_selection, G_LIST_MODEL(self->workspace_tree));
    adw_view_stack_page_set_visible(self->workspace_sidebar_page, TRUE);
    adw_view_stack_set_visible_child_name(self->sidebar_stack, "workspace");
    adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
    remember_workspace(pdfv_workspace_get_folder(workspace));
    update_sidebar_button(self);
  } else if (workspace == self->workspace && error &&
             !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    gtk_spinner_stop(self->workspace_loading_spinner);
    gtk_widget_set_visible(GTK_WIDGET(self->workspace_loading_spinner), FALSE);
    adw_status_page_set_title(self->workspace_loading_page,
                              "Could Not Load Workspace");
    adw_status_page_set_description(self->workspace_loading_page,
                                    error->message);
    AdwDialog *dialog = adw_alert_dialog_new("Could Not Open Folder",
                                             error->message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(self));
  }
  g_clear_error(&error);
  g_object_unref(self);
}

static void open_workspace_folder(PdfvWindow *self, GFile *folder) {
  if (self->workspace)
    close_workspace(self, FALSE);
  self->workspace_scan_cancellable = g_cancellable_new();

  self->workspace = pdfv_workspace_new(folder);
  g_signal_connect(self->workspace, "index-updated",
                   G_CALLBACK(on_workspace_index_updated), self);
  GAction *search_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "workspace-search");
  GAction *close_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "close-workspace");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(search_action), TRUE);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(close_action), TRUE);
  if (g_menu_model_get_n_items(G_MENU_MODEL(self->workspace_menu_section)) ==
      0) {
    g_menu_append(self->workspace_menu_section, "Search Workspace…",
                  "win.workspace-search");
    g_menu_append(self->workspace_menu_section, "Close Workspace",
                  "win.close-workspace");
  }
  adw_status_page_set_title(self->workspace_loading_page,
                            "Loading Workspace…");
  adw_status_page_set_description(self->workspace_loading_page,
                                  "Scanning for PDF files");
  gtk_widget_set_visible(GTK_WIDGET(self->workspace_loading_spinner), TRUE);
  gtk_spinner_start(self->workspace_loading_spinner);
  gtk_stack_set_visible_child_name(self->workspace_content_stack, "loading");
  adw_view_stack_page_set_visible(self->workspace_sidebar_page, TRUE);
  adw_view_stack_set_visible_child_name(self->sidebar_stack, "workspace");
  adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
  pdfv_workspace_load_async(self->workspace, self->workspace_scan_cancellable,
                            on_workspace_loaded, g_object_ref(self));
}

static void on_folder_dialog_selected(GObject *source, GAsyncResult *result,
                                      gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GError *error = NULL;
  GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                       result, &error);
  if (folder) {
    open_workspace_folder(self, folder);
    g_object_unref(folder);
  } else if (error && !g_error_matches(error, GTK_DIALOG_ERROR,
                                        GTK_DIALOG_ERROR_DISMISSED)) {
    AdwDialog *dialog =
        adw_alert_dialog_new("Could Not Open Folder", error->message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(self));
  }
  g_clear_error(&error);
  g_object_unref(self);
}

static void action_open_folder(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Open Workspace Folder");
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL,
                                on_folder_dialog_selected, g_object_ref(self));
}

static void action_workspace_search(GSimpleAction *action, GVariant *parameter,
                                    gpointer user_data) {
  (void)action;
  (void)parameter;
  workspace_search_open(PDFV_WINDOW(user_data));
}

static void action_close_workspace(GSimpleAction *action, GVariant *parameter,
                                   gpointer user_data) {
  (void)action;
  (void)parameter;
  close_workspace(PDFV_WINDOW(user_data), TRUE);
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

  /* Workspaces also have useful sidebar content without an open document. */
  if (self->workspace ||
      (self->current_view &&
       pdfv_document_view_get_document(self->current_view))) {
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
    {.name = "open", .activate = action_open},
    {.name = "open-folder", .activate = action_open_folder},
    {.name = "workspace-search", .activate = action_workspace_search},
    {.name = "close-workspace", .activate = action_close_workspace},
    {.name = "new-tab", .activate = action_new_tab},
    {.name = "close-tab", .activate = action_close_tab},
    {.name = "go-back", .activate = action_go_back},
    {.name = "go-forward", .activate = action_go_forward},
    {.name = "zoom-in", .activate = action_zoom_in},
    {.name = "zoom-out", .activate = action_zoom_out},
    {.name = "zoom-reset", .activate = action_zoom_reset},
    {.name = "zoom-fit-width", .activate = action_zoom_fit_width},
    {.name = "zoom-fit-page", .activate = action_zoom_fit_page},
    {.name = "fullscreen", .activate = action_fullscreen},
    {.name = "toggle-sidebar", .activate = action_toggle_sidebar},
    {.name = "invert-colors", .activate = action_invert_colors},
    {.name = "page-next", .activate = action_page_next},
    {.name = "page-prev", .activate = action_page_prev},
    {.name = "find", .activate = action_find},
    {.name = "find-next", .activate = action_find_next},
    {.name = "find-prev", .activate = action_find_prev},
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

static gboolean on_window_close_request(GtkWindow *window, PdfvWindow *self) {
  (void)window;
  if (self->workspace_scan_cancellable)
    g_cancellable_cancel(self->workspace_scan_cancellable);
  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);
  if (self->workspace_preview_cancellable)
    g_cancellable_cancel(self->workspace_preview_cancellable);
  if (self->workspace)
    pdfv_workspace_cancel(self->workspace);
  self->workspace_preview_generation++;

  guint n_pages = adw_tab_view_get_n_pages(self->tab_view);
  for (guint i = 0; i < n_pages; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(self->tab_view, i);
    GtkWidget *stack = adw_tab_page_get_child(page);
    GCancellable *cancellable =
        g_object_get_data(G_OBJECT(stack), "open-cancellable");
    if (cancellable)
      g_cancellable_cancel(cancellable);
  }
  return GDK_EVENT_PROPAGATE;
}

static void pdfv_window_dispose(GObject *object) {
  PdfvWindow *self = PDFV_WINDOW(object);

  if (self->workspace_search_debounce_id) {
    g_source_remove(self->workspace_search_debounce_id);
    self->workspace_search_debounce_id = 0;
  }
  if (self->workspace_group_select_id) {
    g_source_remove(self->workspace_group_select_id);
    self->workspace_group_select_id = 0;
  }
  if (self->workspace_scan_cancellable)
    g_cancellable_cancel(self->workspace_scan_cancellable);
  if (self->workspace_search_cancellable)
    g_cancellable_cancel(self->workspace_search_cancellable);
  if (self->workspace_preview_cancellable)
    g_cancellable_cancel(self->workspace_preview_cancellable);
  if (self->workspace) {
    pdfv_workspace_cancel(self->workspace);
    g_signal_handlers_disconnect_by_data(self->workspace, self);
  }
  if (self->workspace_selection)
    gtk_single_selection_set_model(self->workspace_selection, NULL);
  self->workspace_selection = NULL;
  g_clear_object(&self->workspace_tree);
  g_clear_object(&self->workspace);
  g_clear_object(&self->workspace_scan_cancellable);
  g_clear_object(&self->workspace_search_cancellable);
  g_clear_object(&self->workspace_preview_cancellable);
  g_clear_object(&self->workspace_preview_file);
  g_clear_object(&self->workspace_return_tab);
  g_clear_object(&self->workspace_results_animation);
  g_clear_pointer(&self->workspace_results, g_ptr_array_unref);
  workspace_document_cache_clear(self);
  g_clear_pointer(&self->workspace_document_cache, g_hash_table_unref);
  g_clear_object(&self->workspace_menu_section);

  if (self->current_outline) {
    phi_outline_item_free(self->current_outline);
    self->current_outline = NULL;
  }

  G_OBJECT_CLASS(pdfv_window_parent_class)->dispose(object);
}

static void pdfv_window_init(PdfvWindow *self) {
  self->current_view = NULL;
  self->current_outline = NULL;
  self->workspace_pending_group = -1;
  self->workspace_document_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                  G_N_ELEMENTS(win_actions), self);
  GAction *workspace_search_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "workspace-search");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(workspace_search_action), FALSE);
  GAction *close_workspace_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "close-workspace");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(close_workspace_action), FALSE);
  g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request),
                   self);

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
  adw_overlay_split_view_set_sidebar_width_fraction(self->split_view, 0.28);
  adw_overlay_split_view_set_min_sidebar_width(self->split_view, 280);
  adw_overlay_split_view_set_max_sidebar_width(self->split_view, 400);
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
  g_menu_append(file_section, "Open Folder…", "win.open-folder");
  g_menu_append(file_section, "New Tab", "win.new-tab");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(file_section));

  self->workspace_menu_section = g_menu_new();
  g_menu_append_section(menu, NULL,
                        G_MENU_MODEL(self->workspace_menu_section));

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
  self->sidebar_stack = ADW_VIEW_STACK(adw_view_stack_new());
  AdwViewSwitcher *sidebar_switcher =
      ADW_VIEW_SWITCHER(adw_view_switcher_new());
  adw_view_switcher_set_policy(sidebar_switcher,
                               ADW_VIEW_SWITCHER_POLICY_NARROW);
  adw_view_switcher_set_stack(sidebar_switcher, self->sidebar_stack);
  adw_header_bar_set_title_widget(sidebar_header,
                                  GTK_WIDGET(sidebar_switcher));
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

  adw_view_stack_add_titled_with_icon(self->sidebar_stack, thumb_scroll,
                                      "pages", "Pages",
                                      "view-paged-symbolic");

  self->workspace_content_stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(self->workspace_content_stack,
                                GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(self->workspace_content_stack, 150);
  gtk_widget_set_vexpand(GTK_WIDGET(self->workspace_content_stack), TRUE);

  self->workspace_loading_page = ADW_STATUS_PAGE(adw_status_page_new());
  adw_status_page_set_title(self->workspace_loading_page,
                            "Loading Workspace…");
  adw_status_page_set_description(self->workspace_loading_page,
                                  "Scanning for PDF files");
  self->workspace_loading_spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_size_request(GTK_WIDGET(self->workspace_loading_spinner),
                              24, 24);
  adw_status_page_set_child(
      self->workspace_loading_page,
      GTK_WIDGET(self->workspace_loading_spinner));
  gtk_stack_add_named(self->workspace_content_stack,
                      GTK_WIDGET(self->workspace_loading_page), "loading");

  GtkWidget *workspace_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(workspace_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(workspace_scroll, TRUE);
  GtkListItemFactory *workspace_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(workspace_factory, "setup",
                   G_CALLBACK(workspace_factory_setup), self);
  g_signal_connect(workspace_factory, "bind",
                   G_CALLBACK(workspace_factory_bind), self);
  g_signal_connect(workspace_factory, "unbind",
                   G_CALLBACK(workspace_factory_unbind), self);
  self->workspace_selection = gtk_single_selection_new(NULL);
  gtk_single_selection_set_autoselect(self->workspace_selection, FALSE);
  gtk_single_selection_set_can_unselect(self->workspace_selection, TRUE);
  self->workspace_list = GTK_LIST_VIEW(gtk_list_view_new(
      GTK_SELECTION_MODEL(self->workspace_selection), workspace_factory));
  gtk_list_view_set_single_click_activate(self->workspace_list, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->workspace_list),
                           "navigation-sidebar");
  g_signal_connect(self->workspace_list, "activate",
                   G_CALLBACK(on_workspace_item_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(workspace_scroll),
                                GTK_WIDGET(self->workspace_list));
  gtk_stack_add_named(self->workspace_content_stack, workspace_scroll,
                      "files");
  gtk_stack_set_visible_child_name(self->workspace_content_stack, "loading");
  self->workspace_sidebar_page = adw_view_stack_add_titled_with_icon(
      self->sidebar_stack, GTK_WIDGET(self->workspace_content_stack),
      "workspace", "Workspace", "folder-symbolic");
  adw_view_stack_page_set_visible(self->workspace_sidebar_page, FALSE);
  gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(self->sidebar_stack));

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
  self->content_overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(self->content_overlay),
                        GTK_WIDGET(self->tab_view));
  adw_toolbar_view_set_content(self->toolbar_view, self->content_overlay);

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

  gtk_overlay_add_overlay(GTK_OVERLAY(self->content_overlay), self->zoom_box);

  /* Spotlight-style workspace search. Its selected result is previewed in a
   * dedicated tab behind this card, leaving the original tab untouched. */
  self->workspace_search_overlay = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(self->workspace_search_overlay), 760);
  adw_clamp_set_tightening_threshold(
      ADW_CLAMP(self->workspace_search_overlay), 620);
  gtk_widget_set_halign(self->workspace_search_overlay, GTK_ALIGN_FILL);
  gtk_widget_set_valign(self->workspace_search_overlay, GTK_ALIGN_START);
  gtk_widget_set_margin_top(self->workspace_search_overlay, 28);
  gtk_widget_set_margin_start(self->workspace_search_overlay, 16);
  gtk_widget_set_margin_end(self->workspace_search_overlay, 16);

  GtkWidget *workspace_search_card =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkCssProvider *workspace_search_css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      workspace_search_css,
      ".workspace-search-card {"
      "  background-color: @window_bg_color;"
      "  background-image: none;"
      "  color: @window_fg_color;"
      "  opacity: 1;"
      "  border: 1px solid alpha(@window_fg_color, 0.12);"
      "  border-radius: 18px;"
      "  box-shadow: 0 12px 32px alpha(black, 0.30);"
      "}"
      ".workspace-search-card list {"
      "  background-color: transparent;"
      "}"
      ".workspace-search-results {"
      "  background-color: alpha(@window_fg_color, 0.025);"
      "  border-radius: 12px;"
      "}"
      ".workspace-search-results row {"
      "  border-radius: 10px;"
      "}"
      ".workspace-result-header {"
      "  background-color: alpha(@window_fg_color, 0.035);"
      "}");
  gtk_style_context_add_provider_for_display(
      gtk_widget_get_display(GTK_WIDGET(self)),
      GTK_STYLE_PROVIDER(workspace_search_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(workspace_search_css);
  gtk_widget_add_css_class(workspace_search_card, "workspace-search-card");
  gtk_widget_set_overflow(workspace_search_card, GTK_OVERFLOW_HIDDEN);
  adw_clamp_set_child(ADW_CLAMP(self->workspace_search_overlay),
                      workspace_search_card);

  GtkWidget *workspace_search_header =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(workspace_search_header, 14);
  gtk_widget_set_margin_bottom(workspace_search_header, 14);
  gtk_widget_set_margin_start(workspace_search_header, 14);
  gtk_widget_set_margin_end(workspace_search_header, 14);

  self->workspace_search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(
      self->workspace_search_entry, "Search PDFs in this workspace");
  g_signal_connect(self->workspace_search_entry, "search-changed",
                   G_CALLBACK(on_workspace_search_changed), self);
  g_signal_connect(self->workspace_search_entry, "stop-search",
                   G_CALLBACK(on_workspace_search_stop), self);
  GtkEventController *workspace_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(workspace_keys,
                                             GTK_PHASE_CAPTURE);
  g_signal_connect(workspace_keys, "key-pressed",
                   G_CALLBACK(on_workspace_search_key), self);
  gtk_widget_add_controller(GTK_WIDGET(self->workspace_search_entry),
                            workspace_keys);
  gtk_box_append(GTK_BOX(workspace_search_header),
                 GTK_WIDGET(self->workspace_search_entry));

  self->workspace_search_status = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->workspace_search_status),
                           "dim-label");
  gtk_label_set_xalign(self->workspace_search_status, 0.0f);
  gtk_widget_set_margin_start(GTK_WIDGET(self->workspace_search_status), 4);
  gtk_widget_set_margin_end(GTK_WIDGET(self->workspace_search_status), 4);
  gtk_widget_set_visible(GTK_WIDGET(self->workspace_search_status), FALSE);
  gtk_box_append(GTK_BOX(workspace_search_header),
                 GTK_WIDGET(self->workspace_search_status));
  gtk_box_append(GTK_BOX(workspace_search_card), workspace_search_header);

  self->workspace_results_revealer = GTK_REVEALER(gtk_revealer_new());
  gtk_revealer_set_transition_type(self->workspace_results_revealer,
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration(self->workspace_results_revealer, 180);
  GtkWidget *result_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(result_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(result_scroll),
                                             430);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(result_scroll), TRUE);
  self->workspace_results_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->workspace_results_list,
                                  GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(self->workspace_results_list,
                                            FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(self->workspace_results_list),
                           "workspace-search-results");
  gtk_widget_set_overflow(GTK_WIDGET(self->workspace_results_list),
                          GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->workspace_results_list), 8);
  gtk_widget_set_margin_start(GTK_WIDGET(self->workspace_results_list), 8);
  gtk_widget_set_margin_end(GTK_WIDGET(self->workspace_results_list), 8);
  g_signal_connect(self->workspace_results_list, "row-selected",
                   G_CALLBACK(on_workspace_result_selected), self);
  g_signal_connect(self->workspace_results_list, "row-activated",
                   G_CALLBACK(workspace_result_row_activated), self);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(result_scroll),
                                GTK_WIDGET(self->workspace_results_list));
  gtk_revealer_set_child(self->workspace_results_revealer, result_scroll);
  gtk_box_append(GTK_BOX(workspace_search_card),
                 GTK_WIDGET(self->workspace_results_revealer));
  gtk_widget_set_visible(self->workspace_search_overlay, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->content_overlay),
                          self->workspace_search_overlay);

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

void pdfv_window_restore_last_workspace(PdfvWindow *self) {
  g_return_if_fail(PDFV_IS_WINDOW(self));
  GFile *folder = get_remembered_workspace();
  if (!folder)
    return;
  if (g_file_query_file_type(folder, G_FILE_QUERY_INFO_NONE, NULL) ==
      G_FILE_TYPE_DIRECTORY)
    open_workspace_folder(self, folder);
  g_object_unref(folder);
}

static void pdfv_window_open_file_internal(PdfvWindow *self, GFile *file,
                                           gboolean fit_width) {
  g_return_if_fail(PDFV_IS_WINDOW(self));
  g_return_if_fail(G_IS_FILE(file));

  AdwTabPage *page = adw_tab_view_get_selected_page(self->tab_view);
  GtkWidget *stack;
  if (page && self->current_view &&
      !pdfv_document_view_get_document(self->current_view)) {
    stack = adw_tab_page_get_child(page);
  } else {
    stack = create_tab_content(self);
    page = adw_tab_view_append(self->tab_view, stack);
    adw_tab_view_set_selected_page(self->tab_view, page);
  }
  open_file_in_tab_async(self, file, page, 0, FALSE, fit_width, 0);
}

void pdfv_window_open_file(PdfvWindow *self, GFile *file) {
  pdfv_window_open_file_internal(self, file, FALSE);
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

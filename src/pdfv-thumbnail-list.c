/* Asynchronous PDF thumbnail browser for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "pdfv-thumbnail-list.h"

#include <phi/phipage.h>

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

struct _PdfvThumbnailList {
  GtkWidget parent_instance;

  GtkListView *list;
  GtkSingleSelection *selection;
};

enum {
  SIGNAL_PAGE_ACTIVATED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(PdfvThumbnailList, pdfv_thumbnail_list, GTK_TYPE_WIDGET)

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
  if (job->error &&
      !g_error_matches(job->error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
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
      ? cairo_image_surface_get_width(data->cached_surface) : 120.0;
  gdouble page_height = data->cached_surface
      ? cairo_image_surface_get_height(data->cached_surface) : 160.0;
  gdouble scale = MIN((width - padding * 2) / page_width,
                      (height - padding * 2) / page_height);
  gdouble scaled_width = page_width * scale;
  gdouble scaled_height = page_height * scale;
  gdouble offset_x = (width - scaled_width) / 2.0;
  gdouble offset_y = (height - scaled_height) / 2.0;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
  cairo_rectangle(cr, offset_x + 2, offset_y + 2, scaled_width,
                  scaled_height);
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
                                    GtkListItem *list_item,
                                    PdfvThumbnailList *self) {
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
                                   GtkListItem *list_item,
                                   PdfvThumbnailList *self) {
  (void)factory;
  ThumbnailData *data =
      g_object_get_data(G_OBJECT(list_item), "thumb-data");
  GListModel *model = gtk_single_selection_get_model(self->selection);
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
                                     PdfvThumbnailList *self) {
  (void)factory;
  (void)self;
  ThumbnailData *data =
      g_object_get_data(G_OBJECT(list_item), "thumb-data");
  thumbnail_data_reset(data);
}

static void on_list_activated(GtkListView *list, guint position,
                              PdfvThumbnailList *self) {
  (void)list;
  if (position != GTK_INVALID_LIST_POSITION)
    g_signal_emit(self, signals[SIGNAL_PAGE_ACTIVATED], 0, position);
}

void pdfv_thumbnail_list_set_document(PdfvThumbnailList *self,
                                      PhiDocument *document) {
  g_return_if_fail(PDFV_IS_THUMBNAIL_LIST(self));
  g_return_if_fail(document == NULL || PHI_IS_DOCUMENT(document));
  GListModel *model = document ? G_LIST_MODEL(document) : NULL;
  if (gtk_single_selection_get_model(self->selection) != model)
    gtk_single_selection_set_model(self->selection, model);
}

static void pdfv_thumbnail_list_dispose(GObject *object) {
  PdfvThumbnailList *self = PDFV_THUMBNAIL_LIST(object);
  if (self->list) {
    gtk_list_view_set_model(self->list, NULL);
    self->selection = NULL;
    gtk_widget_unparent(GTK_WIDGET(self->list));
    self->list = NULL;
  }
  G_OBJECT_CLASS(pdfv_thumbnail_list_parent_class)->dispose(object);
}

static void pdfv_thumbnail_list_class_init(PdfvThumbnailListClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = pdfv_thumbnail_list_dispose;
  gtk_widget_class_set_layout_manager_type(widget_class,
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIGNAL_PAGE_ACTIVATED] = g_signal_new(
      "page-activated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
      NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void pdfv_thumbnail_list_init(PdfvThumbnailList *self) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(thumbnail_factory_setup),
                   self);
  g_signal_connect(factory, "bind", G_CALLBACK(thumbnail_factory_bind),
                   self);
  g_signal_connect(factory, "unbind", G_CALLBACK(thumbnail_factory_unbind),
                   self);

  self->selection = gtk_single_selection_new(NULL);
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);
  self->list = GTK_LIST_VIEW(gtk_list_view_new(
      GTK_SELECTION_MODEL(self->selection), factory));
  gtk_list_view_set_single_click_activate(self->list, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->list), "navigation-sidebar");
  g_signal_connect(self->list, "activate", G_CALLBACK(on_list_activated),
                   self);
  gtk_widget_set_parent(GTK_WIDGET(self->list), GTK_WIDGET(self));
}

PdfvThumbnailList *pdfv_thumbnail_list_new(void) {
  return g_object_new(PDFV_TYPE_THUMBNAIL_LIST, NULL);
}

/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-document-view.h"
#include <phi/phipage.h>
#include <phi/phidocument.h>
#include <math.h>

#define MAX_HISTORY 100
#define MIN_ZOOM 0.1
#define MAX_ZOOM 10.0
#define ZOOM_STEP 1.2
#define PAGE_GAP 10
#define RENDER_PREFETCH_DISTANCE 3
#define RASTER_SCALE_QUANTUM 64.0
#define RASTER_TILE_SIZE 1024
#define RASTER_TILE_GUTTER 2
#define RASTER_WHOLE_PAGE_MAX_PIXELS (8u * 1024u * 1024u)
#define RASTER_FALLBACK_MAX_PIXELS (2u * 1024u * 1024u)
#define RASTER_CACHE_BUDGET_BYTES (128u * 1024u * 1024u)

typedef struct {
    gint page;
    gdouble page_fraction;
    gdouble gap_offset;
    gboolean in_gap;
} VerticalAnchor;

typedef struct {
    VerticalAnchor top;
    gdouble center_x;
} HistoryEntry;

/* Search result for a single page */
typedef struct {
    gint page;
    gint quad_count;
    PhiTextQuad* quads;
} SearchPageResult;

typedef struct {
    gint page;
    guint scale_key;
    gint tile_x;
    gint tile_y;
    gboolean whole_page;
} RenderKey;

typedef enum {
    PAGE_RENDER_VISIBLE,
    PAGE_RENDER_FALLBACK,
    PAGE_RENDER_SURROUNDING,
    PAGE_RENDER_NEXT_PAGE,
} PageRenderScope;

typedef struct {
    RenderKey key;
    GdkTexture* texture;
    GskRenderNode* node;
    graphene_rect_t page_rect;
    gsize bytes;
    guint64 age;
} RenderCacheEntry;

struct _PdfvDocumentView {
    GtkWidget parent_instance;
    
    PhiDocument* document;
    GPtrArray* pages; /* PhiPage* array, lazily populated */
    
    gdouble zoom;
    gdouble minimum_zoom;
    gboolean continuous;
    gboolean dual_page;
    gboolean inverted;
    gboolean presentation_mode;
    
    gint current_page;
    gdouble scroll_x;
    gdouble scroll_y;
    
    /* Navigation history */
    GArray* history;
    gint history_pos;
    
    /* Cached page sizes for layout */
    GArray* page_widths;
    GArray* page_heights;
    GArray* page_offsets;  /* Cumulative Y offset for each page */
    gdouble total_height;
    gdouble max_width;
    
    /* Page links cache */
    GPtrArray* page_links; /* PhiLink* per page */
    
    /* Scrolling */
    GtkAdjustment* hadjustment;
    GtkAdjustment* vadjustment;
    GtkScrollablePolicy hscroll_policy;
    GtkScrollablePolicy vscroll_policy;
    
    /* Gestures */
    GtkGesture* click_gesture;
    GtkGesture* drag_gesture;
    GtkGesture* pan_gesture;
    GtkGesture* zoom_gesture;
    GtkEventController* scroll_controller;
    GtkEventController* motion_controller;
    
    /* Pinch zoom state */
    gdouble pinch_start_zoom;
    gdouble pinch_center_x;
    gdouble pinch_center_y;
    gdouble pinch_anchor_x;
    VerticalAnchor pinch_anchor_y;
    
    /* Middle-click pan state */
    gdouble pan_start_scroll_x;
    gdouble pan_start_scroll_y;
    
    /* Link under cursor */
    gchar* hover_link;
    gboolean has_pointer_position;
    gdouble pointer_x;
    gdouble pointer_y;
    
    /* Worker-rasterized page textures, keyed by page, zoom/device scale and
     * tile. The cache is bounded by bytes instead of an arbitrary page count. */
    GHashTable* render_cache;  /* RenderKey -> RenderCacheEntry */
    GHashTable* render_failed; /* RenderKey set */
    GskRenderNode** fallback_nodes; /* Stream-only documents */
    gint fallback_length;
    gsize render_cache_bytes;
    guint64 render_cache_clock;

    /* One render is active per view. A new visible range may cancel it so a
     * slow page that has already scrolled away never blocks the next page. */
    GCancellable* render_cancellable;
    gint render_job_page;
    guint render_job_serial;
    guint render_next_serial;
    guint render_generation;
    PageRenderScope render_job_scope;
    RenderKey render_job_key;
    gint render_visible_first;
    gint render_visible_last;
    gint render_observed_first;
    gint render_direction;
    
    /* Text selection */
    gboolean selecting;
    gboolean double_click_selected;  /* Flag to prevent drag clearing double-click selection */
    gint selection_start_page;
    graphene_point_t selection_start;
    gint selection_end_page;
    graphene_point_t selection_end;
    PhiTextQuad* selection_quads;
    gint selection_quad_count;
    
    /* Search */
    gchar* search_text;
    GArray* search_results;  /* Array of SearchPageResult */
    gint search_current_match;
    gint search_total_matches;
    guint search_debounce_id;  /* Debounce timeout source */
};

enum {
    PROP_0,
    PROP_DOCUMENT,
    PROP_ZOOM,
    PROP_CONTINUOUS,
    PROP_DUAL_PAGE,
    PROP_INVERTED,
    PROP_CURRENT_PAGE,
    PROP_CAN_GO_BACK,
    PROP_CAN_GO_FORWARD,
    /* GtkScrollable */
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
    N_PROPS
};

enum {
    SIGNAL_LINK_ACTIVATED,
    SIGNAL_SEARCH_COMPLETED,
    N_SIGNALS
};

static GParamSpec* props[N_PROPS];
static guint signals[N_SIGNALS];

static void pdfv_document_view_scrollable_init(GtkScrollableInterface* iface);
static void zoom_at_point(PdfvDocumentView* self, gdouble new_zoom, gdouble focus_x, gdouble focus_y);
static void zoom_from_anchor(PdfvDocumentView* self, gdouble new_zoom,
                             gdouble anchor_x,
                             const VerticalAnchor* anchor_y,
                             gdouble focus_x, gdouble focus_y);
static gboolean screen_to_page_coords(PdfvDocumentView* self, gdouble screen_x, gdouble screen_y, gint* page_num, graphene_point_t* page_point);
static void update_selection_quads(PdfvDocumentView* self);
static void start_next_page_render(PdfvDocumentView* self);

G_DEFINE_TYPE_WITH_CODE(PdfvDocumentView, pdfv_document_view, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, pdfv_document_view_scrollable_init))

static void
render_cache_entry_free(RenderCacheEntry* entry)
{
    if (!entry)
        return;
    g_clear_object(&entry->texture);
    g_clear_pointer(&entry->node, gsk_render_node_unref);
    g_free(entry);
}

static guint
render_key_hash(gconstpointer data)
{
    const RenderKey* key = data;
    guint hash = (guint)key->page * 2654435761u;
    hash = (hash ^ key->scale_key) * 16777619u;
    hash = (hash ^ (guint)key->tile_x) * 16777619u;
    hash = (hash ^ (guint)key->tile_y) * 16777619u;
    return hash ^ (key->whole_page ? 0x9e3779b9u : 0u);
}

static gboolean
render_key_equal(gconstpointer a, gconstpointer b)
{
    const RenderKey* left = a;
    const RenderKey* right = b;
    return left->page == right->page &&
        left->scale_key == right->scale_key &&
        left->tile_x == right->tile_x &&
        left->tile_y == right->tile_y &&
        left->whole_page == right->whole_page;
}

static void
clear_render_cache(PdfvDocumentView* self)
{
    if (self->render_cache)
        g_hash_table_remove_all(self->render_cache);
    if (self->render_failed)
        g_hash_table_remove_all(self->render_failed);
    if (self->fallback_nodes) {
        for (gint i = 0; i < self->fallback_length; i++)
            g_clear_pointer(&self->fallback_nodes[i],
                            gsk_render_node_unref);
    }
    g_clear_pointer(&self->fallback_nodes, g_free);
    self->fallback_length = 0;
    self->render_cache_bytes = 0;
    self->render_cache_clock = 0;
}

static void
allocate_render_cache(PdfvDocumentView* self, gint n_pages)
{
    clear_render_cache(self);
    if (n_pages <= 0)
        return;
    self->fallback_nodes = g_new0(GskRenderNode*, n_pages);
    self->fallback_length = n_pages;
}

static void
cancel_page_render(PdfvDocumentView* self, gboolean invalidate_generation)
{
    if (invalidate_generation)
        self->render_generation++;
    if (self->render_cancellable)
        g_cancellable_cancel(self->render_cancellable);
    g_clear_object(&self->render_cancellable);
    self->render_job_page = -1;
    self->render_job_serial = 0;
    self->render_job_scope = PAGE_RENDER_VISIBLE;
    self->render_job_key = (RenderKey){0};
}

static void
calculate_layout(PdfvDocumentView* self)
{
    if (!self->document)
        return;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (n_pages == 0)
        return;
    
    g_array_set_size(self->page_widths, n_pages);
    g_array_set_size(self->page_heights, n_pages);
    g_array_set_size(self->page_offsets, n_pages);
    self->total_height = 0;
    self->max_width = 0;

    /* Most PDFs use one page size throughout. Loading every page here made
     * opening a long document an O(page count) main-thread operation. Use the
     * first page as an estimate and replace individual values as pages become
     * visible. */
    gfloat reference_width = 612.0f;
    gfloat reference_height = 792.0f;
    PhiPage* reference_page = g_ptr_array_index(self->pages, 0);
    if (!reference_page) {
        reference_page = phi_document_get_page(self->document, 0, NULL);
        g_ptr_array_index(self->pages, 0) = reference_page;
    }
    if (reference_page)
        phi_page_get_size(reference_page, &reference_width, &reference_height);
    
    gdouble offset = 0;
    for (gint i = 0; i < n_pages; i++) {
        PhiPage* page = g_ptr_array_index(self->pages, i);
        g_array_index(self->page_offsets, gdouble, i) = offset;

        gfloat w = reference_width;
        gfloat h = reference_height;
        if (page) {
            phi_page_get_size(page, &w, &h);
        }
        gdouble scaled_w = w * self->zoom;
        gdouble scaled_h = h * self->zoom;
        g_array_index(self->page_widths, gdouble, i) = scaled_w;
        g_array_index(self->page_heights, gdouble, i) = scaled_h;
        offset += scaled_h + PAGE_GAP;
        if (scaled_w > self->max_width)
            self->max_width = scaled_w;
    }
    
    self->total_height = offset > 0 ? offset - PAGE_GAP : 0;
    if (!self->continuous) {
        gint page_num = CLAMP(self->current_page, 0, n_pages - 1);
        self->total_height =
            g_array_index(self->page_heights, gdouble, page_num);
        self->max_width =
            g_array_index(self->page_widths, gdouble, page_num);
    }
}

static gboolean
is_fitted_presentation(PdfvDocumentView* self)
{
    return self->presentation_mode &&
        self->zoom <= self->minimum_zoom * 1.0001 + 0.000001;
}

static void
update_adjustments(PdfvDocumentView* self)
{
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));
    gboolean fitted_presentation = is_fitted_presentation(self);

    /* A fitted slide is always centered. This also repairs the position if a
     * touchpad overshoot or a scrollbar drag managed to update an adjustment
     * while presentation mode was settling. */
    if (fitted_presentation) {
        self->scroll_x = 0;
        self->scroll_y = 0;
    }
    
    if (self->hadjustment) {
        /* When page is centered, scroll_x=0 means centered.
         * To see left edge: need scroll_x = -(max_width - width) / 2
         * To see right edge: need scroll_x = +(max_width - width) / 2
         * If page fits in viewport (max_width <= width), no scrolling needed.
         */
        gdouble scroll_range = MAX(0, self->max_width - width);
        gdouble lower = -scroll_range / 2.0;
        gdouble upper = scroll_range / 2.0 + width;
        self->scroll_x = CLAMP(self->scroll_x, lower, upper - width);
        
        gtk_adjustment_configure(self->hadjustment,
            self->scroll_x,
            lower, upper,
            width * 0.1,
            width * 0.9,
            width);
    }
    
    if (self->vadjustment) {
        /* GtkAdjustment requires its upper bound to include page_size. If a
         * non-continuous slide is shorter than the viewport, using the raw
         * document height produces a bogus scroll range and a visible,
         * draggable scrollbar. */
        gdouble upper = MAX(self->total_height, (gdouble)height);
        self->scroll_y = CLAMP(self->scroll_y, 0,
                               MAX(0, upper - height));
        gtk_adjustment_configure(self->vadjustment,
            self->scroll_y,
            0, upper,
            height * 0.1,
            height * 0.9,
            height);
    }
}

static gint
get_page_at_offset(PdfvDocumentView* self, gdouble y, gdouble* page_offset)
{
    if (page_offset)
        *page_offset = 0;

    if (!self->document)
        return 0;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (n_pages == 0)
        return 0;

    if (!self->continuous) {
        gint page = CLAMP(self->current_page, 0, n_pages - 1);
        if (page_offset) {
            gdouble page_height =
                g_array_index(self->page_heights, gdouble, page);
            gint view_height = gtk_widget_get_height(GTK_WIDGET(self));
            *page_offset = MAX(0, (view_height - page_height) / 2.0);
        }
        return page;
    }
    
    /* Binary search for the page containing y */
    gint low = 0, high = n_pages - 1;
    while (low < high) {
        gint mid = (low + high + 1) / 2;
        gdouble mid_offset = g_array_index(self->page_offsets, gdouble, mid);
        if (mid_offset <= y)
            low = mid;
        else
            high = mid - 1;
    }
    
    if (page_offset)
        *page_offset = g_array_index(self->page_offsets, gdouble, low);
    return low;
}

static gdouble
get_page_offset(PdfvDocumentView* self, gint page)
{
    if (!self->document || page < 0)
        return 0;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (page >= n_pages)
        page = n_pages - 1;
    
    if (self->page_offsets->len == 0)
        return 0;

    if (!self->continuous)
        return 0;
    
    return g_array_index(self->page_offsets, gdouble, page);
}

static VerticalAnchor
vertical_anchor_at(PdfvDocumentView* self, gdouble document_y)
{
    VerticalAnchor anchor = { 0 };
    if (!self->document || self->page_offsets->len == 0)
        return anchor;

    gdouble page_offset = 0;
    anchor.page = get_page_at_offset(self, document_y, &page_offset);
    gdouble page_height =
        g_array_index(self->page_heights, gdouble, anchor.page);
    gdouble within_page = MAX(0, document_y - page_offset);
    if (within_page > page_height &&
        anchor.page + 1 < (gint)self->page_offsets->len) {
        anchor.page_fraction = 1.0;
        anchor.gap_offset = within_page - page_height;
        anchor.in_gap = TRUE;
    } else if (page_height > 0) {
        anchor.page_fraction = within_page / page_height;
    }
    return anchor;
}

static gdouble
vertical_anchor_position(PdfvDocumentView* self,
                         const VerticalAnchor* anchor)
{
    if (!self->document || self->page_offsets->len == 0)
        return 0;

    gint page = CLAMP(anchor->page, 0, (gint)self->page_offsets->len - 1);
    gdouble page_height =
        g_array_index(self->page_heights, gdouble, page);
    gdouble page_offset = self->continuous
        ? g_array_index(self->page_offsets, gdouble, page)
        : MAX(0, (gtk_widget_get_height(GTK_WIDGET(self)) - page_height) /
                     2.0);
    return page_offset + page_height * anchor->page_fraction +
        (anchor->in_gap ? anchor->gap_offset : 0);
}

static HistoryEntry
current_history_entry(PdfvDocumentView* self)
{
    HistoryEntry entry = {
        .top = vertical_anchor_at(self, self->scroll_y),
        .center_x = self->zoom > 0 ? self->scroll_x / self->zoom : 0
    };
    return entry;
}

static void
notify_history_changed(PdfvDocumentView* self)
{
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_BACK]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_FORWARD]);
}

static void
save_current_history_entry(PdfvDocumentView* self)
{
    if (self->history_pos < 0 ||
        self->history_pos >= (gint)self->history->len)
        return;
    g_array_index(self->history, HistoryEntry, self->history_pos) =
        current_history_entry(self);
}

static void
append_current_history_entry(PdfvDocumentView* self)
{
    HistoryEntry entry = current_history_entry(self);
    g_array_append_val(self->history, entry);
    self->history_pos = self->history->len - 1;
}

static void
navigate_to_page_with_history(PdfvDocumentView* self, gint page)
{
    if (self->history_pos < 0)
        append_current_history_entry(self);
    else
        save_current_history_entry(self);

    if (self->history_pos < (gint)self->history->len - 1)
        g_array_set_size(self->history, self->history_pos + 1);

    pdfv_document_view_go_to_page(self, page);
    append_current_history_entry(self);

    while (self->history->len > MAX_HISTORY) {
        g_array_remove_index(self->history, 0);
        self->history_pos--;
    }
    notify_history_changed(self);
}

static void
restore_history_entry(PdfvDocumentView* self, const HistoryEntry* entry)
{
    self->scroll_y = vertical_anchor_position(self, &entry->top);
    self->scroll_x = entry->center_x * self->zoom;
    update_adjustments(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static PhiPage*
ensure_page_loaded_and_update_layout(PdfvDocumentView* self, gint page_num)
{
    if (!self->document || page_num < 0 ||
        page_num >= (gint)self->pages->len)
        return NULL;

    gint viewport_height = gtk_widget_get_height(GTK_WIDGET(self));
    gdouble layout_focus_y = gtk_gesture_is_active(self->zoom_gesture)
        ? self->pinch_center_y : viewport_height / 2.0;
    VerticalAnchor layout_anchor = vertical_anchor_at(
        self, self->scroll_y + layout_focus_y);

    PhiPage* page = g_ptr_array_index(self->pages, page_num);
    if (!page) {
        GError* error = NULL;
        page = phi_document_get_page(self->document, page_num, &error);
        if (!page) {
            g_warning("Failed to load page %d: %s", page_num + 1,
                      error ? error->message : "unknown error");
            g_clear_error(&error);
            return NULL;
        }
        g_ptr_array_index(self->pages, page_num) = page;
    }

    gfloat width, height;
    phi_page_get_size(page, &width, &height);
    gdouble old_width =
        g_array_index(self->page_widths, gdouble, page_num);
    gdouble old_height =
        g_array_index(self->page_heights, gdouble, page_num);
    gdouble new_width = width * self->zoom;
    gdouble new_height = height * self->zoom;
    gdouble height_delta = new_height - old_height;
    gboolean changed = fabs(new_width - old_width) > 0.01 ||
        fabs(height_delta) > 0.01;
    if (!changed)
        return page;

    g_array_index(self->page_widths, gdouble, page_num) = new_width;
    g_array_index(self->page_heights, gdouble, page_num) = new_height;

    if (self->continuous) {
        if (fabs(height_delta) > 0.01) {
            for (guint i = page_num + 1; i < self->page_offsets->len; i++)
                g_array_index(self->page_offsets, gdouble, i) += height_delta;
            self->total_height += height_delta;
            self->scroll_y = vertical_anchor_position(
                self, &layout_anchor) - layout_focus_y;
            self->scroll_y = CLAMP(
                self->scroll_y, 0,
                MAX(0, self->total_height - viewport_height));
        }
        self->max_width = 0;
        for (guint i = 0; i < self->page_widths->len; i++)
            self->max_width = MAX(
                self->max_width,
                g_array_index(self->page_widths, gdouble, i));
    } else if (page_num == self->current_page) {
        self->total_height = new_height;
        self->max_width = new_width;
    }

    update_adjustments(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    return page;
}

typedef struct {
    guint scale_key;
    gdouble scale;
    gint full_width;
    gint full_height;
    gboolean whole_page;
} RasterPlan;

typedef struct {
    gint first_x;
    gint last_x;
    gint first_y;
    gint last_y;
} TileRange;

typedef struct {
    RenderKey key;
    gdouble render_scale;
    gint raster_x;
    gint raster_y;
    gint tile_width;
    gint tile_height;
    guint generation;
    guint serial;
    PageRenderScope scope;
} PageRenderRequest;

static guint
current_render_scale_key(PdfvDocumentView* self)
{
    gint widget_scale = MAX(1,
        gtk_widget_get_scale_factor(GTK_WIDGET(self)));
    return MAX(1u, (guint)llround(
        self->zoom * widget_scale * RASTER_SCALE_QUANTUM));
}

static gdouble
render_scale_from_key(guint scale_key)
{
    return scale_key / RASTER_SCALE_QUANTUM;
}

static void
page_base_size(PdfvDocumentView* self, gint page, gdouble* width,
               gdouble* height)
{
    gdouble zoom = MAX(self->zoom, MIN_ZOOM);
    *width = g_array_index(self->page_widths, gdouble, page) / zoom;
    *height = g_array_index(self->page_heights, gdouble, page) / zoom;
}

static RasterPlan
raster_plan_for_page(PdfvDocumentView* self, gint page)
{
    RasterPlan plan = {0};
    gdouble page_width = 1;
    gdouble page_height = 1;
    page_base_size(self, page, &page_width, &page_height);
    plan.scale_key = current_render_scale_key(self);
    plan.scale = render_scale_from_key(plan.scale_key);
    plan.full_width = MAX(1, (gint)ceil(page_width * plan.scale));
    plan.full_height = MAX(1, (gint)ceil(page_height * plan.scale));
    plan.whole_page =
        (guint64)plan.full_width * plan.full_height <=
        RASTER_WHOLE_PAGE_MAX_PIXELS;
    return plan;
}

static gboolean
raster_fallback_for_page(PdfvDocumentView* self, gint page,
                         RenderKey* key, gdouble* scale)
{
    RasterPlan target = raster_plan_for_page(self, page);
    if (target.whole_page || target.scale_key <= 1)
        return FALSE;

    gdouble page_width = 1;
    gdouble page_height = 1;
    page_base_size(self, page, &page_width, &page_height);
    gdouble fallback_scale = sqrt(
        RASTER_FALLBACK_MAX_PIXELS / (page_width * page_height));
    guint fallback_key = MAX(1u, (guint)floor(
        fallback_scale * RASTER_SCALE_QUANTUM));
    fallback_key = MIN(fallback_key, target.scale_key - 1);

    *key = (RenderKey){
        .page = page,
        .scale_key = fallback_key,
        .whole_page = TRUE,
    };
    *scale = render_scale_from_key(fallback_key);
    return TRUE;
}

static void
page_display_geometry(PdfvDocumentView* self, gint page, gdouble* x,
                      gdouble* y, gdouble* width, gdouble* height)
{
    gint view_width = gtk_widget_get_width(GTK_WIDGET(self));
    gint view_height = gtk_widget_get_height(GTK_WIDGET(self));
    *width = g_array_index(self->page_widths, gdouble, page);
    *height = g_array_index(self->page_heights, gdouble, page);
    *x = (view_width - *width) / 2.0 - self->scroll_x;
    *y = self->continuous
        ? g_array_index(self->page_offsets, gdouble, page) - self->scroll_y
        : MAX(0, (view_height - *height) / 2.0) - self->scroll_y;
}

static TileRange
tile_range_for_page(PdfvDocumentView* self, gint page,
                    const RasterPlan* plan, PageRenderScope scope)
{
    TileRange range = {0};
    gdouble x, y, display_width, display_height;
    page_display_geometry(self, page, &x, &y, &display_width,
                          &display_height);
    gint view_width = gtk_widget_get_width(GTK_WIDGET(self));
    gint view_height = gtk_widget_get_height(GTK_WIDGET(self));
    gdouble base_width = display_width / MAX(self->zoom, MIN_ZOOM);
    gdouble base_height = display_height / MAX(self->zoom, MIN_ZOOM);
    gdouble base_x0 = CLAMP(-x / self->zoom, 0, base_width);
    gdouble base_x1 = CLAMP((view_width - x) / self->zoom,
                            base_x0, base_width);
    gint pixel_x0 = CLAMP((gint)floor(base_x0 * plan->scale),
                          0, plan->full_width - 1);
    gint pixel_x1 = CLAMP((gint)ceil(base_x1 * plan->scale),
                          pixel_x0 + 1, plan->full_width);

    range.first_x = (pixel_x0 / RASTER_TILE_SIZE) * RASTER_TILE_SIZE;
    range.last_x = ((pixel_x1 - 1) / RASTER_TILE_SIZE) * RASTER_TILE_SIZE;

    if (scope == PAGE_RENDER_NEXT_PAGE) {
        gint edge_y = self->render_direction < 0
            ? plan->full_height - 1 : 0;
        range.first_y = range.last_y =
            (edge_y / RASTER_TILE_SIZE) * RASTER_TILE_SIZE;
        return range;
    }

    gdouble base_y0 = CLAMP(-y / self->zoom, 0, base_height);
    gdouble base_y1 = CLAMP((view_height - y) / self->zoom,
                            base_y0, base_height);
    gint pixel_y0 = CLAMP((gint)floor(base_y0 * plan->scale),
                          0, plan->full_height - 1);
    gint pixel_y1 = CLAMP((gint)ceil(base_y1 * plan->scale),
                          pixel_y0 + 1, plan->full_height);
    range.first_y = (pixel_y0 / RASTER_TILE_SIZE) * RASTER_TILE_SIZE;
    range.last_y = ((pixel_y1 - 1) / RASTER_TILE_SIZE) * RASTER_TILE_SIZE;

    if (scope == PAGE_RENDER_SURROUNDING) {
        gint final_x = ((plan->full_width - 1) / RASTER_TILE_SIZE) *
            RASTER_TILE_SIZE;
        gint final_y = ((plan->full_height - 1) / RASTER_TILE_SIZE) *
            RASTER_TILE_SIZE;
        range.first_x = MAX(0, range.first_x - RASTER_TILE_SIZE);
        range.last_x = MIN(final_x, range.last_x + RASTER_TILE_SIZE);
        range.first_y = MAX(0, range.first_y - RASTER_TILE_SIZE);
        range.last_y = MIN(final_y, range.last_y + RASTER_TILE_SIZE);
    }
    return range;
}

static RenderCacheEntry*
render_cache_lookup(PdfvDocumentView* self, const RenderKey* key)
{
    return g_hash_table_lookup(self->render_cache, key);
}

static gboolean
render_key_failed(PdfvDocumentView* self, const RenderKey* key)
{
    return g_hash_table_contains(self->render_failed, key);
}

static void
render_key_mark_failed(PdfvDocumentView* self, const RenderKey* key)
{
    RenderKey* copy = g_memdup2(key, sizeof(*copy));
    g_hash_table_add(self->render_failed, copy);
}

static void
render_cache_touch(RenderCacheEntry* entry, PdfvDocumentView* self)
{
    entry->age = ++self->render_cache_clock;
}

static guint
render_cache_best_whole_scale(PdfvDocumentView* self, gint page,
                              guint target_scale)
{
    GHashTableIter iter;
    gpointer value = NULL;
    guint best_scale = 0;
    guint best_distance = G_MAXUINT;
    g_hash_table_iter_init(&iter, self->render_cache);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        RenderCacheEntry* entry = value;
        if (entry->key.page != page || !entry->key.whole_page ||
            entry->key.scale_key == target_scale)
            continue;
        guint distance = entry->key.scale_key > target_scale
            ? entry->key.scale_key - target_scale
            : target_scale - entry->key.scale_key;
        if (distance < best_distance) {
            best_distance = distance;
            best_scale = entry->key.scale_key;
        }
    }
    return best_scale;
}

static gboolean
render_key_in_tile_range(const RenderKey* key, const TileRange* range)
{
    return key->tile_x >= range->first_x &&
        key->tile_x <= range->last_x &&
        key->tile_y >= range->first_y &&
        key->tile_y <= range->last_y &&
        key->tile_x % RASTER_TILE_SIZE == 0 &&
        key->tile_y % RASTER_TILE_SIZE == 0;
}

static gboolean
render_cache_entry_is_visible_fallback(PdfvDocumentView* self,
                                       const RenderCacheEntry* entry)
{
    if (entry->key.page < self->render_visible_first ||
        entry->key.page > self->render_visible_last ||
        !entry->key.whole_page)
        return FALSE;

    RasterPlan plan = raster_plan_for_page(self, entry->key.page);
    return entry->key.scale_key == render_cache_best_whole_scale(
        self, entry->key.page, plan.scale_key);
}

static gboolean
render_cache_entry_protected(PdfvDocumentView* self,
                             const RenderCacheEntry* entry)
{
    if (entry->key.page < self->render_visible_first ||
        entry->key.page > self->render_visible_last)
        return FALSE;

    RasterPlan plan = raster_plan_for_page(self, entry->key.page);
    if (entry->key.scale_key == plan.scale_key &&
        entry->key.whole_page == plan.whole_page) {
        if (plan.whole_page)
            return TRUE;
        TileRange visible = tile_range_for_page(
            self, entry->key.page, &plan, PAGE_RENDER_VISIBLE);
        return render_key_in_tile_range(&entry->key, &visible);
    }

    return render_cache_entry_is_visible_fallback(self, entry);
}

static gboolean
render_cache_evict_oldest(PdfvDocumentView* self, gboolean allow_protected)
{
    GHashTableIter iter;
    gpointer value = NULL;
    RenderCacheEntry* oldest = NULL;
    g_hash_table_iter_init(&iter, self->render_cache);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        RenderCacheEntry* entry = value;
        /* The coarse whole-page image is the guarantee that a fast pan never
         * exposes the page background. Even the emergency eviction pass may
         * discard sharp visible tiles before discarding this fallback. */
        if (allow_protected &&
            render_cache_entry_is_visible_fallback(self, entry))
            continue;
        if (!allow_protected && render_cache_entry_protected(self, entry))
            continue;
        if (!oldest || entry->age < oldest->age)
            oldest = entry;
    }
    if (!oldest)
        return FALSE;
    RenderKey key = oldest->key;
    self->render_cache_bytes -= oldest->bytes;
    return g_hash_table_remove(self->render_cache, &key);
}

static void
render_cache_store(PdfvDocumentView* self,
                   const PageRenderRequest* request, GdkTexture* texture)
{
    if (!texture)
        return;

    RenderCacheEntry* previous = render_cache_lookup(
        self, &request->key);
    if (previous) {
        self->render_cache_bytes -= previous->bytes;
        g_hash_table_remove(self->render_cache, &request->key);
    }

    gdouble page_width = 1;
    gdouble page_height = 1;
    page_base_size(self, request->key.page, &page_width, &page_height);
    RenderCacheEntry* entry = g_new0(RenderCacheEntry, 1);
    entry->key = request->key;
    entry->texture = g_object_ref(texture);
    if (request->key.whole_page) {
        entry->page_rect = GRAPHENE_RECT_INIT(
            0, 0, page_width, page_height);
    } else {
        gdouble x = request->raster_x / request->render_scale;
        gdouble y = request->raster_y / request->render_scale;
        gdouble width = MIN(
            gdk_texture_get_width(texture) / request->render_scale,
            MAX(0, page_width - x));
        gdouble height = MIN(
            gdk_texture_get_height(texture) / request->render_scale,
            MAX(0, page_height - y));
        entry->page_rect = GRAPHENE_RECT_INIT(x, y, width, height);
    }
    entry->node = gsk_texture_node_new(texture, &entry->page_rect);
    entry->bytes = (gsize)gdk_texture_get_width(texture) *
        gdk_texture_get_height(texture) * 4;
    render_cache_touch(entry, self);
    self->render_cache_bytes += entry->bytes;
    g_hash_table_insert(self->render_cache, &entry->key, entry);

    while (self->render_cache_bytes > RASTER_CACHE_BUDGET_BYTES &&
           g_hash_table_size(self->render_cache) > 1) {
        if (!render_cache_evict_oldest(self, FALSE) &&
            !render_cache_evict_oldest(self, TRUE))
            break;
    }
}

typedef struct {
    GWeakRef view;
} PageRenderCallback;

static PageRenderCallback*
page_render_callback_new(PdfvDocumentView* self)
{
    PageRenderCallback* callback = g_new0(PageRenderCallback, 1);
    g_weak_ref_init(&callback->view, self);
    return callback;
}

static PdfvDocumentView*
page_render_callback_take_view(PageRenderCallback* callback)
{
    PdfvDocumentView* self = g_weak_ref_get(&callback->view);
    g_weak_ref_clear(&callback->view);
    g_free(callback);
    return self;
}

static void
render_page_worker(GTask* task, gpointer source_object, gpointer task_data,
                   GCancellable* cancellable)
{
    PhiDocument* document = PHI_DOCUMENT(source_object);
    PageRenderRequest* request = task_data;
    GError* error = NULL;
    GdkTexture* texture = phi_document_render_page_texture(
        document, request->key.page, request->render_scale,
        request->raster_x, request->raster_y,
        request->key.whole_page ? 0 : request->tile_width,
        request->key.whole_page ? 0 : request->tile_height,
        cancellable, &error);
    if (texture) {
        g_task_return_pointer(task, texture, g_object_unref);
    } else if (error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Page rendering returned no texture");
    }
}

static gboolean
render_key_matches_page(PdfvDocumentView* self, const RenderKey* key,
                        PageRenderScope scope)
{
    if (scope == PAGE_RENDER_FALLBACK) {
        RenderKey fallback_key;
        gdouble fallback_scale;
        return raster_fallback_for_page(
            self, key->page, &fallback_key, &fallback_scale) &&
            render_key_equal(key, &fallback_key);
    }

    RasterPlan plan = raster_plan_for_page(self, key->page);
    if (key->scale_key != plan.scale_key ||
        key->whole_page != plan.whole_page)
        return FALSE;
    if (plan.whole_page)
        return key->tile_x == 0 && key->tile_y == 0;
    TileRange range = tile_range_for_page(self, key->page, &plan,
                                          scope);
    return render_key_in_tile_range(key, &range);
}

static gboolean
find_missing_for_page(PdfvDocumentView* self, gint page,
                      PageRenderScope scope, PageRenderRequest* request)
{
    if (page < 0 || page >= self->fallback_length ||
        self->fallback_nodes[page])
        return FALSE;

    if (scope == PAGE_RENDER_FALLBACK) {
        RenderKey key;
        gdouble scale;
        if (!raster_fallback_for_page(self, page, &key, &scale) ||
            render_cache_lookup(self, &key) || render_key_failed(self, &key))
            return FALSE;
        *request = (PageRenderRequest){
            .key = key,
            .render_scale = scale,
            .scope = scope,
        };
        return TRUE;
    }

    RasterPlan plan = raster_plan_for_page(self, page);
    if (plan.whole_page) {
        RenderKey key = {
            .page = page,
            .scale_key = plan.scale_key,
            .whole_page = TRUE
        };
        if (render_cache_lookup(self, &key) || render_key_failed(self, &key))
            return FALSE;
        *request = (PageRenderRequest){
            .key = key,
            .render_scale = plan.scale,
            .scope = scope,
        };
        return TRUE;
    }

    TileRange range = tile_range_for_page(self, page, &plan, scope);
    gint y = self->render_direction < 0 ? range.last_y : range.first_y;
    gint y_end = self->render_direction < 0 ? range.first_y : range.last_y;
    gint y_step = self->render_direction < 0
        ? -RASTER_TILE_SIZE : RASTER_TILE_SIZE;
    for (;; y += y_step) {
        for (gint x = range.first_x; x <= range.last_x;
             x += RASTER_TILE_SIZE) {
            RenderKey key = {
                .page = page,
                .scale_key = plan.scale_key,
                .tile_x = x,
                .tile_y = y,
                .whole_page = FALSE
            };
            if (render_cache_lookup(self, &key) ||
                render_key_failed(self, &key))
                continue;
            gint raster_x = MAX(0, x - RASTER_TILE_GUTTER);
            gint raster_y = MAX(0, y - RASTER_TILE_GUTTER);
            gint raster_right = MIN(
                plan.full_width,
                x + RASTER_TILE_SIZE + RASTER_TILE_GUTTER);
            gint raster_bottom = MIN(
                plan.full_height,
                y + RASTER_TILE_SIZE + RASTER_TILE_GUTTER);
            *request = (PageRenderRequest){
                .key = key,
                .render_scale = plan.scale,
                .raster_x = raster_x,
                .raster_y = raster_y,
                .tile_width = raster_right - raster_x,
                .tile_height = raster_bottom - raster_y,
                .scope = scope,
            };
            return TRUE;
        }
        if (y == y_end)
            break;
    }
    return FALSE;
}

static gboolean
visible_page_is_missing(PdfvDocumentView* self)
{
    PageRenderRequest request;
    for (gint page = self->render_visible_first;
         page <= self->render_visible_last; page++) {
        if (find_missing_for_page(
                self, page, PAGE_RENDER_VISIBLE, &request))
            return TRUE;
    }
    return FALSE;
}

static gboolean
page_is_wanted(PdfvDocumentView* self, gint page)
{
    if (page >= self->render_visible_first &&
        page <= self->render_visible_last)
        return TRUE;
    if (self->render_direction < 0)
        return page < self->render_visible_first &&
            page >= self->render_visible_first -
                RENDER_PREFETCH_DISTANCE;
    return page > self->render_visible_last &&
        page <= self->render_visible_last + RENDER_PREFETCH_DISTANCE;
}

static void
on_page_rendered(GObject* source, GAsyncResult* result, gpointer user_data)
{
    PdfvDocumentView* self = page_render_callback_take_view(user_data);
    if (!self)
        return;
    GTask* task = G_TASK(result);
    PageRenderRequest* request = g_task_get_task_data(task);
    GError* error = NULL;
    GdkTexture* texture = g_task_propagate_pointer(task, &error);
    gboolean fallback_created = FALSE;
    gboolean current_document = request->generation ==
        self->render_generation && self->document == PHI_DOCUMENT(source);

    /* Stream-only API users have no independent source to reopen. Preserve
     * their existing rendering support outside snapshot(); application PDFs
     * are file-backed and always take the worker path above. */
    if (current_document && !texture && error &&
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
        g_clear_error(&error);
        PhiPage* page = ensure_page_loaded_and_update_layout(
            self, request->key.page);
        if (page && request->key.page < self->fallback_length &&
            !self->fallback_nodes[request->key.page]) {
            self->fallback_nodes[request->key.page] =
                phi_page_render_to_node(page, &error);
            fallback_created = self->fallback_nodes[request->key.page] != NULL;
        }
    }

    if (current_document && texture) {
        ensure_page_loaded_and_update_layout(self, request->key.page);
        render_cache_store(self, request, texture);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    } else if (current_document && fallback_created) {
        gtk_widget_queue_draw(GTK_WIDGET(self));
    } else if (current_document && error &&
               !g_error_matches(error, G_IO_ERROR,
                                G_IO_ERROR_CANCELLED)) {
        render_key_mark_failed(self, &request->key);
        g_warning("Failed to rasterize page %d: %s",
                  request->key.page + 1,
                  error->message);
    }

    if (request->serial == self->render_job_serial) {
        g_clear_object(&self->render_cancellable);
        self->render_job_page = -1;
        self->render_job_serial = 0;
        self->render_job_scope = PAGE_RENDER_VISIBLE;
    }
    g_clear_object(&texture);
    g_clear_error(&error);
    if (current_document)
        start_next_page_render(self);
    g_object_unref(self);
}

static gboolean
find_next_page_to_render(PdfvDocumentView* self,
                         PageRenderRequest* request)
{
    /* A tiled page needs one complete coarse texture beneath its tiles.
     * Otherwise a large jump can expose the paper background for several
     * seconds while a complex page rasterizes its newly visible tiles. */
    for (gint page = self->render_visible_first;
         page <= self->render_visible_last; page++) {
        RasterPlan plan = raster_plan_for_page(self, page);
        if (!plan.whole_page &&
            render_cache_best_whole_scale(
                self, page, plan.scale_key) == 0 &&
            find_missing_for_page(
                self, page, PAGE_RENDER_FALLBACK, request))
            return TRUE;
    }

    if (self->render_direction < 0) {
        for (gint page = self->render_visible_last;
             page >= self->render_visible_first; page--) {
            if (find_missing_for_page(
                    self, page, PAGE_RENDER_VISIBLE, request))
                return TRUE;
        }
    } else {
        for (gint page = self->render_visible_first;
             page <= self->render_visible_last; page++) {
            if (find_missing_for_page(
                    self, page, PAGE_RENDER_VISIBLE, request))
                return TRUE;
        }
    }

    for (gint page = self->render_visible_first;
         page <= self->render_visible_last; page++) {
        if (find_missing_for_page(
                self, page, PAGE_RENDER_FALLBACK, request))
            return TRUE;
    }

    for (gint page = self->render_visible_first;
         page <= self->render_visible_last; page++) {
        if (find_missing_for_page(
                self, page, PAGE_RENDER_SURROUNDING, request))
            return TRUE;
    }

    gint page_count = self->document
        ? phi_document_get_n_pages(self->document) : 0;
    for (gint distance = 1; distance <= RENDER_PREFETCH_DISTANCE;
         distance++) {
        gint page = self->render_direction < 0
            ? self->render_visible_first - distance
            : self->render_visible_last + distance;
        if (page < 0 || page >= page_count)
            break;
        if (find_missing_for_page(
                self, page, PAGE_RENDER_NEXT_PAGE, request))
            return TRUE;
    }
    return FALSE;
}

static void
start_next_page_render(PdfvDocumentView* self)
{
    if (!self->document || self->render_job_page >= 0 ||
        !self->render_cache || self->render_visible_first < 0)
        return;

    PageRenderRequest* request = g_new0(PageRenderRequest, 1);
    if (!find_next_page_to_render(self, request)) {
        g_free(request);
        return;
    }

    self->render_cancellable = g_cancellable_new();
    self->render_job_page = request->key.page;
    self->render_job_scope = request->scope;
    self->render_job_key = request->key;
    self->render_job_serial = ++self->render_next_serial;

    request->generation = self->render_generation;
    request->serial = self->render_job_serial;
    GTask* task = g_task_new(self->document, self->render_cancellable,
                            on_page_rendered,
                            page_render_callback_new(self));
    g_task_set_task_data(task, request, g_free);
    g_task_set_priority(task, request->scope != PAGE_RENDER_VISIBLE
        ? G_PRIORITY_LOW : G_PRIORITY_DEFAULT);
    g_task_run_in_thread(task, render_page_worker);
    g_object_unref(task);
}

static void
update_render_range(PdfvDocumentView* self, gint first_page, gint last_page)
{
    gint n_pages = self->document
        ? phi_document_get_n_pages(self->document) : 0;
    if (n_pages <= 0)
        return;
    first_page = CLAMP(first_page, 0, n_pages - 1);
    last_page = CLAMP(last_page, first_page, n_pages - 1);

    gboolean had_observation = self->render_observed_first >= 0;
    if (had_observation && first_page != self->render_observed_first)
        self->render_direction = first_page > self->render_observed_first
            ? 1 : -1;
    self->render_observed_first = first_page;

    self->render_visible_first = first_page;
    self->render_visible_last = last_page;

    if (self->render_job_page >= 0 &&
        (!page_is_wanted(self, self->render_job_page) ||
         !render_key_matches_page(self, &self->render_job_key,
                                  self->render_job_scope) ||
         (self->render_job_scope != PAGE_RENDER_VISIBLE &&
          self->render_job_scope != PAGE_RENDER_FALLBACK &&
          visible_page_is_missing(self))))
        cancel_page_render(self, FALSE);
    start_next_page_render(self);
}

static guint
render_cache_best_stale_scale(PdfvDocumentView* self, gint page,
                              guint target_scale)
{
    GHashTableIter iter;
    gpointer value = NULL;
    guint best_scale = 0;
    guint best_distance = G_MAXUINT;
    g_hash_table_iter_init(&iter, self->render_cache);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        RenderCacheEntry* entry = value;
        if (entry->key.page != page ||
            entry->key.scale_key == target_scale)
            continue;
        guint distance = entry->key.scale_key > target_scale
            ? entry->key.scale_key - target_scale
            : target_scale - entry->key.scale_key;
        if (distance < best_distance) {
            best_distance = distance;
            best_scale = entry->key.scale_key;
        }
    }
    return best_scale;
}

static void
snapshot_cached_scale(PdfvDocumentView* self, GtkSnapshot* snapshot,
                      gint page, guint scale_key,
                      const graphene_rect_t* visible_rect)
{
    if (!scale_key)
        return;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, self->render_cache);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        RenderCacheEntry* entry = value;
        if (entry->key.page != page ||
            entry->key.scale_key != scale_key)
            continue;
        graphene_rect_t intersection;
        if (!graphene_rect_intersection(
                &entry->page_rect, visible_rect, &intersection))
            continue;
        gtk_snapshot_append_node(snapshot, entry->node);
        render_cache_touch(entry, self);
    }
}

static void
pdfv_document_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot)
{
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(widget);
    
    if (!self->document)
        return;
    
    gint width = gtk_widget_get_width(widget);
    gint height = gtk_widget_get_height(widget);
    gint n_pages = phi_document_get_n_pages(self->document);
    
    if (n_pages == 0)
        return;
    
    /* Find visible page range */
    gdouble view_top = self->scroll_y;
    gdouble view_bottom = self->scroll_y + height;
    gint first_visible = self->continuous
        ? get_page_at_offset(self, view_top, NULL) : self->current_page;
    gint last_visible = self->continuous
        ? get_page_at_offset(self, view_bottom, NULL) : self->current_page;
    
    /* Update current page */
    gint center_page = self->continuous
        ? get_page_at_offset(self, view_top + height / 2, NULL)
        : self->current_page;
    if (self->continuous && center_page != self->current_page) {
        self->current_page = center_page;
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_PAGE]);
    }
    
    /* Snapshot only consumes completed immutable textures. Missing pages are
     * queued for the worker and keep their paper background in the meantime. */
    update_render_range(self, first_visible, last_visible);
    
    /* Match the surrounding window, empty tabs, and Markdown editor. A
     * presentation deliberately uses a pitch-black screen surround. */
    GdkRGBA background = {0, 0, 0, 1};
    if (!self->presentation_mode) {
        AdwStyleManager* style_manager = adw_style_manager_get_default();
        gboolean is_dark = adw_style_manager_get_dark(style_manager);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gboolean found = gtk_style_context_lookup_color(
            gtk_widget_get_style_context(widget), "window_bg_color",
            &background);
        G_GNUC_END_IGNORE_DEPRECATIONS
        gdouble luminance = found
            ? 0.2126 * background.red + 0.7152 * background.green +
                  0.0722 * background.blue
            : (is_dark ? 0.0 : 1.0);
        if (!found || (is_dark && luminance > 0.5) ||
            (!is_dark && luminance < 0.5))
            background = is_dark
                ? (GdkRGBA){.red = 0.141, .green = 0.141, .blue = 0.141,
                            .alpha = 1.0}
                : (GdkRGBA){.red = 0.980, .green = 0.980, .blue = 0.980,
                            .alpha = 1.0};
    }
    gtk_snapshot_append_color(snapshot, &background,
        &GRAPHENE_RECT_INIT(0, 0, width, height));
    
    /* Render visible pages - start from first visible page */
    static const GdkRGBA shadow_color = {0, 0, 0, 0.2};
    static const GdkRGBA page_bg_light = {1.0, 1.0, 1.0, 1.0};   /* #ffffff */
    static const GdkRGBA page_bg_dark = {0.102, 0.102, 0.102, 1.0}; /* #1a1a1a */
    const GdkRGBA* page_bg = self->inverted ? &page_bg_dark : &page_bg_light;
    
    for (gint i = first_visible; i < n_pages; i++) {
        gdouble y_offset = self->continuous
            ? g_array_index(self->page_offsets, gdouble, i) : 0;
        
        /* Stop if past visible area */
        if (self->continuous && y_offset > view_bottom)
            break;
        
        /* Estimated sizes are available before a page scene finishes. */
        gdouble pw = g_array_index(self->page_widths, gdouble, i);
        gdouble ph = g_array_index(self->page_heights, gdouble, i);
        
        /* Center page horizontally */
        gdouble x = (width - pw) / 2.0 - self->scroll_x;
        gdouble y = self->continuous
            ? y_offset - self->scroll_y
            : MAX(0, (height - ph) / 2.0) - self->scroll_y;
        
        /* Page shadow */
        gtk_snapshot_append_color(snapshot, &shadow_color,
            &GRAPHENE_RECT_INIT(x + 3, y + 3, pw, ph));
        
        /* Page background */
        gtk_snapshot_append_color(snapshot, page_bg,
            &GRAPHENE_RECT_INIT(x, y, pw, ph));
        
        PhiPage* page = g_ptr_array_index(self->pages, i);
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));

        /* Cache nodes use unscaled page coordinates. A texture rendered for
         * an older zoom therefore remains a valid, temporarily softer
         * fallback while its replacement is being rasterized. */
        if (self->zoom != 1.0)
            gtk_snapshot_scale(snapshot, self->zoom, self->zoom);
        gdouble base_width = pw / MAX(self->zoom, MIN_ZOOM);
        gdouble base_height = ph / MAX(self->zoom, MIN_ZOOM);
        gdouble visible_x0 = CLAMP(-x / self->zoom, 0, base_width);
        gdouble visible_y0 = CLAMP(-y / self->zoom, 0, base_height);
        gdouble visible_x1 = CLAMP(
            (width - x) / self->zoom, visible_x0, base_width);
        gdouble visible_y1 = CLAMP(
            (height - y) / self->zoom, visible_y0, base_height);
        graphene_rect_t visible_page_rect = GRAPHENE_RECT_INIT(
            visible_x0, visible_y0,
            visible_x1 - visible_x0, visible_y1 - visible_y0);
        gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(
            0, 0, base_width, base_height));

        if (self->inverted) {
            graphene_matrix_t hue_matrix;
            graphene_vec4_t hue_offset;
            graphene_matrix_init_from_float(&hue_matrix, (float[16]){
                -0.574f,  0.426f,  0.426f, 0,
                 0.285f, -0.715f,  0.285f, 0,
                 0.928f,  0.928f, -0.072f, 0,
                 0,       0,       0,      1
            });
            graphene_vec4_init(&hue_offset, 0, 0, 0, 0);
            gtk_snapshot_push_color_matrix(snapshot, &hue_matrix,
                                            &hue_offset);

            graphene_matrix_t invert_matrix;
            graphene_vec4_t invert_offset;
            graphene_matrix_init_from_float(&invert_matrix, (float[16]){
                -1, 0, 0, 0,
                0, -1, 0, 0,
                0, 0, -1, 0,
                0, 0, 0, 1
            });
            graphene_vec4_init(&invert_offset, 1, 1, 1, 0);
            gtk_snapshot_push_color_matrix(snapshot, &invert_matrix,
                                            &invert_offset);
        }

        if (i < self->fallback_length && self->fallback_nodes[i]) {
            gtk_snapshot_append_node(snapshot, self->fallback_nodes[i]);
        } else {
            guint target_scale = current_render_scale_key(self);
            guint whole_scale = render_cache_best_whole_scale(
                self, i, target_scale);
            guint stale_scale = render_cache_best_stale_scale(
                self, i, target_scale);
            snapshot_cached_scale(
                self, snapshot, i, whole_scale, &visible_page_rect);
            if (stale_scale != whole_scale)
                snapshot_cached_scale(
                    self, snapshot, i, stale_scale, &visible_page_rect);
            snapshot_cached_scale(
                self, snapshot, i, target_scale, &visible_page_rect);
        }

        if (self->inverted) {
            gtk_snapshot_pop(snapshot);  /* pop invert */
            gtk_snapshot_pop(snapshot);  /* pop hue rotate */
        }

        if (page) {
                /* Get page bounds for coordinate offset */
                gfloat bounds_x0, bounds_y0;
                phi_page_get_bounds(page, &bounds_x0, &bounds_y0, NULL, NULL);

                /* Render search highlights for this page. We are inside the
                 * scale context, so coordinates remain in page units. */
                if (self->search_results && self->search_results->len > 0) {
                for (guint sr = 0; sr < self->search_results->len; sr++) {
                    SearchPageResult* result = &g_array_index(self->search_results, SearchPageResult, sr);
                    if (result->page == i) {
                        GdkRGBA highlight_color = {1.0, 1.0, 0.0, 0.4}; /* Yellow */
                        for (gint q = 0; q < result->quad_count; q++) {
                            PhiTextQuad* quad = &result->quads[q];
                            float min_x = MIN(MIN(quad->ul.x, quad->ur.x), MIN(quad->ll.x, quad->lr.x)) - bounds_x0;
                            float max_x = MAX(MAX(quad->ul.x, quad->ur.x), MAX(quad->ll.x, quad->lr.x)) - bounds_x0;
                            float min_y = MIN(MIN(quad->ul.y, quad->ur.y), MIN(quad->ll.y, quad->lr.y)) - bounds_y0;
                            float max_y = MAX(MAX(quad->ul.y, quad->ur.y), MAX(quad->ll.y, quad->lr.y)) - bounds_y0;
                            gtk_snapshot_append_color(snapshot, &highlight_color,
                                &GRAPHENE_RECT_INIT(min_x, min_y, max_x - min_x, max_y - min_y));
                        }
                        break;
                    }
                }
                }

                /* Render text selection for this page */
                if (self->selection_quad_count > 0 &&
                    (i == self->selection_start_page ||
                     i == self->selection_end_page ||
                     (i > self->selection_start_page &&
                      i < self->selection_end_page))) {
                    GdkRGBA select_color = {0.2, 0.5, 1.0, 0.4};
                    for (gint q = 0; q < self->selection_quad_count; q++) {
                        PhiTextQuad* quad = &self->selection_quads[q];
                        float min_x = MIN(MIN(quad->ul.x, quad->ur.x), MIN(quad->ll.x, quad->lr.x)) - bounds_x0;
                        float max_x = MAX(MAX(quad->ul.x, quad->ur.x), MAX(quad->ll.x, quad->lr.x)) - bounds_x0;
                        float min_y = MIN(MIN(quad->ul.y, quad->ur.y), MIN(quad->ll.y, quad->lr.y)) - bounds_y0;
                        float max_y = MAX(MAX(quad->ul.y, quad->ur.y), MAX(quad->ll.y, quad->lr.y)) - bounds_y0;
                        gtk_snapshot_append_color(snapshot, &select_color,
                            &GRAPHENE_RECT_INIT(min_x, min_y,
                                                max_x - min_x,
                                                max_y - min_y));
                    }
                }
        }

        gtk_snapshot_pop(snapshot); /* page clip */
        gtk_snapshot_restore(snapshot);

        if (!self->continuous)
            break;
    }
}

static void
pdfv_document_view_measure(GtkWidget* widget, GtkOrientation orientation,
                           int for_size, int* minimum, int* natural,
                           int* minimum_baseline, int* natural_baseline)
{
    (void)for_size;
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(widget);
    
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = 200;
        *natural = MAX(200, (gint)self->max_width);
    } else {
        *minimum = 200;
        *natural = MAX(200, (gint)self->total_height);
    }
    
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

static void
pdfv_document_view_size_allocate(GtkWidget* widget, int width, int height, int baseline)
{
    (void)width;
    (void)height;
    (void)baseline;
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(widget);
    update_adjustments(self);
}

static void
on_hadjustment_changed(GtkAdjustment* adj, PdfvDocumentView* self)
{
    gdouble value = gtk_adjustment_get_value(adj);
    if (is_fitted_presentation(self)) {
        self->scroll_x = 0;
        if (fabs(value) > 0.000001)
            gtk_adjustment_set_value(adj, 0);
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    if (value != self->scroll_x) {
        self->scroll_x = value;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static void
on_vadjustment_changed(GtkAdjustment* adj, PdfvDocumentView* self)
{
    gdouble value = gtk_adjustment_get_value(adj);
    if (is_fitted_presentation(self)) {
        self->scroll_y = 0;
        if (fabs(value) > 0.000001)
            gtk_adjustment_set_value(adj, 0);
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    if (value != self->scroll_y) {
        self->render_direction = value > self->scroll_y ? 1 : -1;
        self->scroll_y = value;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static PhiLink*
find_link_at(PdfvDocumentView* self, gdouble x, gdouble y)
{
    if (!self->document)
        return NULL;
    
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    
    /* Find which page we're on */
    gdouble page_offset;
    gint page_num = get_page_at_offset(self, self->scroll_y + y, &page_offset);
    
    PhiPage* page = g_ptr_array_index(self->pages, page_num);
    if (!page)
        return NULL;
    
    gfloat pw, ph;
    phi_page_get_size(page, &pw, &ph);
    gdouble scaled_pw = pw * self->zoom;
    
    /* Convert to page coordinates */
    gdouble page_x = (x + self->scroll_x - (width - scaled_pw) / 2.0) / self->zoom;
    gdouble page_y = (self->scroll_y + y - page_offset) / self->zoom;
    
    /* Get or cache links */
    PhiLink* links = NULL;
    if (page_num < (gint)self->page_links->len) {
        links = g_ptr_array_index(self->page_links, page_num);
    }
    if (!links) {
        links = phi_page_get_links(page);
        if (page_num < (gint)self->page_links->len)
            g_ptr_array_index(self->page_links, page_num) = links;
    }
    
    /* Check links (linked list) */
    for (PhiLink* link = links; link; link = link->next) {
        if (page_x >= link->rect.origin.x &&
            page_x <= link->rect.origin.x + link->rect.size.width &&
            page_y >= link->rect.origin.y &&
            page_y <= link->rect.origin.y + link->rect.size.height) {
            return link;
        }
    }
    
    return NULL;
}

static void
on_click_pressed(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, 
                 PdfvDocumentView* self)
{
    (void)gesture;
    gtk_widget_grab_focus(GTK_WIDGET(self));
    
    /* Double-click selects a word; triple-click selects its sentence. */
    if (n_press == 2 || n_press == 3) {
        gint page_num;
        graphene_point_t page_point;
        if (screen_to_page_coords(self, x, y, &page_num, &page_point)) {
            PhiPage* page = g_ptr_array_index(self->pages, page_num);
            if (page) {
                graphene_point_t selection_start, selection_end;
                gboolean selected = n_press == 3
                    ? phi_page_select_sentence_at(
                        page, &page_point, &selection_start, &selection_end)
                    : phi_page_select_word_at(
                        page, &page_point, &selection_start, &selection_end);
                if (selected) {
                    self->selection_start_page = page_num;
                    self->selection_start = selection_start;
                    self->selection_end_page = page_num;
                    self->selection_end = selection_end;
                    self->double_click_selected = TRUE;
                    
                    update_selection_quads(self);
                    gtk_widget_queue_draw(GTK_WIDGET(self));
                    
                    /* Copy to clipboard */
                    gchar* text = pdfv_document_view_get_selected_text(self);
                    if (text && *text) {
                        GdkClipboard* clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
                        gdk_clipboard_set_text(clipboard, text);
                    }
                    g_free(text);
                }
            }
        }
        return;
    }
    
    if (n_press != 1)
        return;
    
    PhiLink* link = find_link_at(self, x, y);
    if (link && link->uri) {
        pdfv_document_view_activate_link(self, link->uri);
    } else {
        /* Single click not on link - clear selection */
        if (self->selection_quad_count > 0) {
            g_free(self->selection_quads);
            self->selection_quads = NULL;
            self->selection_quad_count = 0;
            self->selection_start_page = -1;
            self->selection_end_page = -1;
            gtk_widget_queue_draw(GTK_WIDGET(self));
        }
    }
}

static void
scroll_adjustment(GtkAdjustment* adjustment, gdouble delta)
{
    if (!adjustment)
        return;
    gdouble lower = gtk_adjustment_get_lower(adjustment);
    gdouble upper = gtk_adjustment_get_upper(adjustment) -
        gtk_adjustment_get_page_size(adjustment);
    gtk_adjustment_set_value(adjustment, CLAMP(
        gtk_adjustment_get_value(adjustment) + delta, lower,
        MAX(lower, upper)));
}

static gboolean
on_key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode,
               GdkModifierType state, PdfvDocumentView* self)
{
    (void)controller;
    (void)keycode;
    if (!self->document ||
        (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)))
        return GDK_EVENT_PROPAGATE;

    switch (keyval) {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        scroll_adjustment(self->vadjustment,
                          -gtk_adjustment_get_step_increment(
                              self->vadjustment));
        return GDK_EVENT_STOP;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        scroll_adjustment(self->vadjustment,
                          gtk_adjustment_get_step_increment(
                              self->vadjustment));
        return GDK_EVENT_STOP;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        pdfv_document_view_go_to_page(self, self->current_page - 1);
        return GDK_EVENT_STOP;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        pdfv_document_view_go_to_page(self, self->current_page + 1);
        return GDK_EVENT_STOP;
    case GDK_KEY_space:
        pdfv_document_view_go_to_page(self, self->current_page + 1);
        return GDK_EVENT_STOP;
    default:
        return GDK_EVENT_PROPAGATE;
    }
}

static void
on_motion(GtkEventControllerMotion* controller, gdouble x, gdouble y,
          PdfvDocumentView* self)
{
    (void)controller;

    self->has_pointer_position = TRUE;
    self->pointer_x = x;
    self->pointer_y = y;
    
    PhiLink* link = find_link_at(self, x, y);
    const gchar* new_hover = link ? link->uri : NULL;
    if (g_strcmp0(new_hover, self->hover_link) != 0) {
        g_free(self->hover_link);
        self->hover_link = g_strdup(new_hover);
    }

    gboolean over_text = FALSE;
    if (!link) {
        gint page_num;
        graphene_point_t page_point;
        if (screen_to_page_coords(self, x, y, &page_num, &page_point)) {
            PhiPage* page = g_ptr_array_index(self->pages, page_num);
            over_text = page && phi_page_has_text_at(page, &page_point);
        }
    }
    if (link)
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "pointer");
    else if (over_text)
        gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");
    else
        gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
}

static void
on_motion_enter(GtkEventControllerMotion* controller, gdouble x, gdouble y,
                PdfvDocumentView* self)
{
    on_motion(controller, x, y, self);
}

static void
on_motion_leave(GtkEventControllerMotion* controller, PdfvDocumentView* self)
{
    (void)controller;

    self->has_pointer_position = FALSE;
    g_clear_pointer(&self->hover_link, g_free);
    gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
}

static void
cancel_scroll_momentum(PdfvDocumentView* self)
{
    GtkWidget* parent = gtk_widget_get_parent(GTK_WIDGET(self));
    while (parent && !GTK_IS_SCROLLED_WINDOW(parent))
        parent = gtk_widget_get_parent(parent);
    if (!parent)
        return;

    GtkScrolledWindow* scrolled = GTK_SCROLLED_WINDOW(parent);
    gboolean kinetic = gtk_scrolled_window_get_kinetic_scrolling(scrolled);
    if (kinetic) {
        /* Changing this property cancels GtkScrolledWindow's private kinetic
         * tick.  Without doing so, that tick can overwrite the adjustment we
         * set for the pinch anchor and then spring back on a later update. */
        gtk_scrolled_window_set_kinetic_scrolling(scrolled, FALSE);
        gtk_scrolled_window_set_kinetic_scrolling(scrolled, TRUE);
    }
}

static void
on_zoom_begin(GtkGestureZoom* gesture, GdkEventSequence* sequence,
              PdfvDocumentView* self)
{
    (void)sequence;
    cancel_scroll_momentum(self);
    self->pinch_start_zoom = self->zoom;
    
    /* Store the center point of the pinch gesture */
    gdouble x, y;
    if (gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &x, &y)) {
        self->pinch_center_x = x;
        self->pinch_center_y = y;
    } else {
        gint width = gtk_widget_get_width(GTK_WIDGET(self));
        gint height = gtk_widget_get_height(GTK_WIDGET(self));
        self->pinch_center_x = width / 2.0;
        self->pinch_center_y = height / 2.0;
    }

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    self->pinch_anchor_x = self->zoom > 0
        ? (self->scroll_x + self->pinch_center_x - width / 2.0) / self->zoom
        : 0;
    self->pinch_anchor_y = vertical_anchor_at(
        self, self->scroll_y + self->pinch_center_y);

    /* Prevent the surrounding GtkScrolledWindow from interpreting the same
     * native pinch as a scroll sequence. */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_zoom_scale_changed(GtkGestureZoom* gesture, gdouble scale,
                      PdfvDocumentView* self)
{
    GdkEvent* event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    gboolean touchpad = event &&
        gdk_event_get_event_type(event) == GDK_TOUCHPAD_PINCH;

    /* For native touchpad gestures GTK's reported point includes the
     * accumulated dx/dy of the fingers.  Following that moving point turns a
     * slightly diagonal pinch into a vertical pan and may snap the page back
     * as the delta settles.  Keep its focal point fixed for the whole pinch.
     * Real touchscreen points have absolute positions, so retain combined
     * pinch-and-pan there. */
    if (!touchpad) {
        gdouble x, y;
        if (gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &x, &y)) {
            self->pinch_center_x = x;
            self->pinch_center_y = y;
        }
    }

    gdouble new_zoom = self->pinch_start_zoom * scale;
    zoom_from_anchor(self, new_zoom, self->pinch_anchor_x,
                     &self->pinch_anchor_y, self->pinch_center_x,
                     self->pinch_center_y);
}

static gboolean
on_scroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy,
          PdfvDocumentView* self)
{
    (void)dx;
    if (!self->document)
        return FALSE;

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    
    if (state & GDK_CONTROL_MASK) {
        if (dy == 0.0 || !isfinite(dy))
            return TRUE;

        /* Zoom with Ctrl+Scroll towards cursor position */
        gdouble x = self->has_pointer_position
            ? self->pointer_x
            : gtk_widget_get_width(GTK_WIDGET(self)) / 2.0;
        gdouble y = self->has_pointer_position
            ? self->pointer_y
            : gtk_widget_get_height(GTK_WIDGET(self)) / 2.0;

        x = CLAMP(x, 0, gtk_widget_get_width(GTK_WIDGET(self)));
        y = CLAMP(y, 0, gtk_widget_get_height(GTK_WIDGET(self)));
        
        GdkScrollUnit unit = gtk_event_controller_scroll_get_unit(
            GTK_EVENT_CONTROLLER_SCROLL(controller));
        gdouble factor;
        if (unit == GDK_SCROLL_UNIT_SURFACE) {
            /* Smooth touchpad deltas are surface pixels, not wheel notches.
             * About 300 pixels doubles the zoom and each individual event is
             * capped so a malformed delta cannot fling the document. */
            factor = pow(2.0, -CLAMP(dy, -300.0, 300.0) / 300.0);
        } else {
            factor = pow(ZOOM_STEP, -CLAMP(dy, -4.0, 4.0));
        }
        
        zoom_at_point(self, self->zoom * factor, x, y);
        return TRUE;
    }

    /* At the fitted presentation floor there is intentionally nowhere to
     * pan. Consume smooth scrolling before GtkScrolledWindow can apply its
     * elastic overshoot and expose empty space around the slide. Zoomed-in
     * slides remain scrollable normally. */
    if (is_fitted_presentation(self))
        return TRUE;
    
    return FALSE; /* Let default scrolling happen */
}

static void
on_scroll_begin(GtkEventControllerScroll* controller, PdfvDocumentView* self)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    if (state & GDK_CONTROL_MASK)
        cancel_scroll_momentum(self);
}

void
pdfv_document_view_capture_zoom_scroll(PdfvDocumentView* self,
                                       GtkWidget* ancestor)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    g_return_if_fail(GTK_IS_WIDGET(ancestor));

    /* GtkScrolledWindow consumes smooth scroll in its capture phase.  Some
     * touchpad backends expose pinch zoom as Ctrl+scroll, so a controller on
     * the document itself runs too late: the same delta has already changed
     * the vertical adjustment.  Install this on an ancestor outside the
     * scrolled window to intercept zoom before scrolling can occur. */
    GtkEventController* controller = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
    g_signal_connect_object(controller, "scroll-begin",
                            G_CALLBACK(on_scroll_begin), self, 0);
    g_signal_connect_object(controller, "scroll", G_CALLBACK(on_scroll), self,
                            0);
    gtk_widget_add_controller(ancestor, controller);
}

/* Helper to convert screen coordinates to page coordinates */
static gboolean
screen_to_page_coords(PdfvDocumentView* self, gdouble screen_x, gdouble screen_y,
                      gint* page_num, graphene_point_t* page_point)
{
    if (!self->document)
        return FALSE;
    
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    
    /* Find which page we're on */
    gdouble page_offset;
    gint pn = get_page_at_offset(self, self->scroll_y + screen_y, &page_offset);
    
    PhiPage* page = g_ptr_array_index(self->pages, pn);
    if (!page)
        return FALSE;
    
    gfloat pw, ph;
    phi_page_get_size(page, &pw, &ph);
    gdouble scaled_pw = pw * self->zoom;
    
    /* Get page bounds - some PDFs have non-zero origin */
    gfloat bounds_x0, bounds_y0;
    phi_page_get_bounds(page, &bounds_x0, &bounds_y0, NULL, NULL);
    
    /* Convert to page coordinates, accounting for page origin */
    gdouble page_x = (screen_x + self->scroll_x - (width - scaled_pw) / 2.0) / self->zoom + bounds_x0;
    gdouble page_y = (self->scroll_y + screen_y - page_offset) / self->zoom + bounds_y0;
    
    if (page_num)
        *page_num = pn;
    if (page_point) {
        page_point->x = page_x;
        page_point->y = page_y;
    }
    
    return TRUE;
}

static void
update_selection_quads(PdfvDocumentView* self)
{
    g_free(self->selection_quads);
    self->selection_quads = NULL;
    self->selection_quad_count = 0;
    
    if (self->selection_start_page < 0 || self->selection_end_page < 0)
        return;
    
    /* For now, only support selection within single page */
    if (self->selection_start_page != self->selection_end_page)
        return;
    
    PhiPage* page = g_ptr_array_index(self->pages, self->selection_start_page);
    if (!page)
        return;
    
    /* Get selection quads */
    PhiTextQuad quads[256];
    gint count = phi_page_get_selection_quads(page, &self->selection_start, 
                                               &self->selection_end, quads, 256);
    
    if (count > 0) {
        self->selection_quads = g_memdup2(quads, count * sizeof(PhiTextQuad));
        self->selection_quad_count = count;
    }
}

/* Middle-click pan handlers */
static void
on_pan_begin(GtkGestureDrag* gesture, gdouble x, gdouble y, PdfvDocumentView* self)
{
    (void)gesture;
    (void)x;
    (void)y;
    
    self->pan_start_scroll_x = self->scroll_x;
    self->pan_start_scroll_y = self->scroll_y;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "grabbing");
}

static void
on_pan_update(GtkGestureDrag* gesture, gdouble offset_x, gdouble offset_y, PdfvDocumentView* self)
{
    (void)gesture;
    
    /* Pan in opposite direction of drag */
    gdouble new_scroll_x = self->pan_start_scroll_x - offset_x;
    gdouble new_scroll_y = self->pan_start_scroll_y - offset_y;
    
    if (self->hadjustment)
        gtk_adjustment_set_value(self->hadjustment, new_scroll_x);
    if (self->vadjustment)
        gtk_adjustment_set_value(self->vadjustment, new_scroll_y);
}

static void
on_pan_end(GtkGestureDrag* gesture, gdouble offset_x, gdouble offset_y, PdfvDocumentView* self)
{
    (void)gesture;
    (void)offset_x;
    (void)offset_y;
    
    if (self->has_pointer_position)
        on_motion(NULL, self->pointer_x, self->pointer_y, self);
    else
        gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
}

static void
on_drag_begin(GtkGestureDrag* gesture, gdouble x, gdouble y, PdfvDocumentView* self)
{
    (void)gesture;
    
    /* Check if we're over a link - don't start selection */
    PhiLink* link = find_link_at(self, x, y);
    if (link)
        return;
    
    self->selecting = TRUE;
    
    gint page_num;
    graphene_point_t page_point;
    if (screen_to_page_coords(self, x, y, &page_num, &page_point)) {
        self->selection_start_page = page_num;
        self->selection_start = page_point;
        self->selection_end_page = page_num;
        self->selection_end = page_point;
    }
    
    /* Don't clear selection here - wait for drag_end to see if it's a click or drag */
    
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");
}

static void
on_drag_update(GtkGestureDrag* gesture, gdouble offset_x, gdouble offset_y, PdfvDocumentView* self)
{
    if (!self->selecting)
        return;
    
    gdouble start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    
    gdouble x = start_x + offset_x;
    gdouble y = start_y + offset_y;
    
    gint page_num;
    graphene_point_t page_point;
    if (screen_to_page_coords(self, x, y, &page_num, &page_point)) {
        self->selection_end_page = page_num;
        self->selection_end = page_point;
        
        update_selection_quads(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static void
on_drag_end(GtkGestureDrag* gesture, gdouble offset_x, gdouble offset_y, PdfvDocumentView* self)
{
    (void)gesture;
    
    if (!self->selecting) {
        return;
    }
    
    self->selecting = FALSE;
    if (self->has_pointer_position)
        on_motion(NULL, self->pointer_x, self->pointer_y, self);
    else
        gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
    
    /* If we just did a double-click selection, don't process as click */
    if (self->double_click_selected) {
        self->double_click_selected = FALSE;
        return;
    }
    
    /* Check if this was just a click (minimal movement) */
    gdouble distance = sqrt(offset_x * offset_x + offset_y * offset_y);
    if (distance < 5.0) {
        /* This was a click, not a drag - clear selection */
        if (self->selection_quad_count > 0) {
            g_free(self->selection_quads);
            self->selection_quads = NULL;
            self->selection_quad_count = 0;
            self->selection_start_page = -1;
            self->selection_end_page = -1;
            gtk_widget_queue_draw(GTK_WIDGET(self));
        }
        return;
    }
    
    /* This was a drag - update selection and copy to clipboard */
    update_selection_quads(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    gchar* text = pdfv_document_view_get_selected_text(self);
    if (text && *text) {
        GdkClipboard* clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
        gdk_clipboard_set_text(clipboard, text);
    }
    g_free(text);
}

static void
pdfv_document_view_get_property(GObject* object, guint prop_id,
                                GValue* value, GParamSpec* pspec)
{
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(object);
    
    switch (prop_id) {
    case PROP_DOCUMENT:
        g_value_set_object(value, self->document);
        break;
    case PROP_ZOOM:
        g_value_set_double(value, self->zoom);
        break;
    case PROP_CONTINUOUS:
        g_value_set_boolean(value, self->continuous);
        break;
    case PROP_DUAL_PAGE:
        g_value_set_boolean(value, self->dual_page);
        break;
    case PROP_INVERTED:
        g_value_set_boolean(value, self->inverted);
        break;
    case PROP_CURRENT_PAGE:
        g_value_set_int(value, self->current_page);
        break;
    case PROP_CAN_GO_BACK:
        g_value_set_boolean(value, pdfv_document_view_can_go_back(self));
        break;
    case PROP_CAN_GO_FORWARD:
        g_value_set_boolean(value, pdfv_document_view_can_go_forward(self));
        break;
    case PROP_HADJUSTMENT:
        g_value_set_object(value, self->hadjustment);
        break;
    case PROP_VADJUSTMENT:
        g_value_set_object(value, self->vadjustment);
        break;
    case PROP_HSCROLL_POLICY:
        g_value_set_enum(value, self->hscroll_policy);
        break;
    case PROP_VSCROLL_POLICY:
        g_value_set_enum(value, self->vscroll_policy);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
pdfv_document_view_set_property(GObject* object, guint prop_id,
                                const GValue* value, GParamSpec* pspec)
{
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(object);
    
    switch (prop_id) {
    case PROP_DOCUMENT:
        pdfv_document_view_set_document(self, g_value_get_object(value));
        break;
    case PROP_ZOOM:
        pdfv_document_view_set_zoom(self, g_value_get_double(value));
        break;
    case PROP_CONTINUOUS:
        pdfv_document_view_set_continuous(self, g_value_get_boolean(value));
        break;
    case PROP_DUAL_PAGE:
        pdfv_document_view_set_dual_page(self, g_value_get_boolean(value));
        break;
    case PROP_INVERTED:
        pdfv_document_view_set_inverted(self, g_value_get_boolean(value));
        break;
    case PROP_HADJUSTMENT:
        if (self->hadjustment)
            g_signal_handlers_disconnect_by_func(self->hadjustment, 
                on_hadjustment_changed, self);
        g_set_object(&self->hadjustment, g_value_get_object(value));
        if (self->hadjustment) {
            g_signal_connect(self->hadjustment, "value-changed",
                G_CALLBACK(on_hadjustment_changed), self);
            update_adjustments(self);
        }
        break;
    case PROP_VADJUSTMENT:
        if (self->vadjustment)
            g_signal_handlers_disconnect_by_func(self->vadjustment,
                on_vadjustment_changed, self);
        g_set_object(&self->vadjustment, g_value_get_object(value));
        if (self->vadjustment) {
            g_signal_connect(self->vadjustment, "value-changed",
                G_CALLBACK(on_vadjustment_changed), self);
            update_adjustments(self);
        }
        break;
    case PROP_HSCROLL_POLICY:
        self->hscroll_policy = g_value_get_enum(value);
        break;
    case PROP_VSCROLL_POLICY:
        self->vscroll_policy = g_value_get_enum(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
pdfv_document_view_dispose(GObject* object)
{
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(object);
    
    /* Cancel any pending search */
    if (self->search_debounce_id) {
        g_source_remove(self->search_debounce_id);
        self->search_debounce_id = 0;
    }
    
    cancel_page_render(self, TRUE);
    clear_render_cache(self);
    g_clear_pointer(&self->render_cache, g_hash_table_unref);
    g_clear_pointer(&self->render_failed, g_hash_table_unref);
    g_clear_object(&self->document);
    g_clear_pointer(&self->pages, g_ptr_array_unref);
    g_clear_pointer(&self->page_widths, g_array_unref);
    g_clear_pointer(&self->page_heights, g_array_unref);
    g_clear_pointer(&self->page_offsets, g_array_unref);
    g_clear_pointer(&self->history, g_array_unref);
    g_clear_object(&self->hadjustment);
    g_clear_object(&self->vadjustment);
    g_clear_pointer(&self->hover_link, g_free);
    
    /* Free page links */
    if (self->page_links) {
        for (guint i = 0; i < self->page_links->len; i++) {
            PhiLink* links = g_ptr_array_index(self->page_links, i);
            phi_link_free(links);
        }
        g_ptr_array_unref(self->page_links);
        self->page_links = NULL;
    }
    
    G_OBJECT_CLASS(pdfv_document_view_parent_class)->dispose(object);
}

static void
pdfv_document_view_class_init(PdfvDocumentViewClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    
    object_class->get_property = pdfv_document_view_get_property;
    object_class->set_property = pdfv_document_view_set_property;
    object_class->dispose = pdfv_document_view_dispose;
    
    widget_class->snapshot = pdfv_document_view_snapshot;
    widget_class->measure = pdfv_document_view_measure;
    widget_class->size_allocate = pdfv_document_view_size_allocate;
    
    props[PROP_DOCUMENT] = g_param_spec_object("document", NULL, NULL,
        PHI_TYPE_DOCUMENT,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_ZOOM] = g_param_spec_double("zoom", NULL, NULL,
        MIN_ZOOM, MAX_ZOOM, 1.0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_CONTINUOUS] = g_param_spec_boolean("continuous", NULL, NULL,
        TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_DUAL_PAGE] = g_param_spec_boolean("dual-page", NULL, NULL,
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_INVERTED] = g_param_spec_boolean("inverted", NULL, NULL,
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_CURRENT_PAGE] = g_param_spec_int("current-page", NULL, NULL,
        0, G_MAXINT, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_CAN_GO_BACK] = g_param_spec_boolean("can-go-back", NULL, NULL,
        FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    
    props[PROP_CAN_GO_FORWARD] = g_param_spec_boolean("can-go-forward", NULL, NULL,
        FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    
    /* GtkScrollable properties */
    g_object_class_override_property(object_class, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(object_class, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
    
    g_object_class_install_properties(object_class, PROP_HADJUSTMENT, props);
    
    signals[SIGNAL_LINK_ACTIVATED] = g_signal_new("link-activated",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    
    signals[SIGNAL_SEARCH_COMPLETED] = g_signal_new("search-completed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_INT);  /* match count */
}

static void
pdfv_document_view_scrollable_init(GtkScrollableInterface* iface)
{
    (void)iface;
    /* Use default implementation */
}

static void
pdfv_document_view_init(PdfvDocumentView* self)
{
    self->zoom = 1.0;
    self->minimum_zoom = MIN_ZOOM;
    self->continuous = TRUE;
    self->dual_page = FALSE;
    self->inverted = FALSE;
    self->presentation_mode = FALSE;
    self->current_page = 0;
    self->render_job_page = -1;
    self->render_visible_first = -1;
    self->render_visible_last = -1;
    self->render_observed_first = -1;
    self->render_direction = 1;

    self->render_cache = g_hash_table_new_full(
        render_key_hash, render_key_equal, NULL,
        (GDestroyNotify)render_cache_entry_free);
    self->render_failed = g_hash_table_new_full(
        render_key_hash, render_key_equal, g_free, NULL);
    
    self->pages = g_ptr_array_new();  /* We don't own the pages, document does */
    self->page_widths = g_array_new(FALSE, TRUE, sizeof(gdouble));
    self->page_heights = g_array_new(FALSE, TRUE, sizeof(gdouble));
    self->page_offsets = g_array_new(FALSE, TRUE, sizeof(gdouble));
    self->history = g_array_new(FALSE, FALSE, sizeof(HistoryEntry));
    self->history_pos = -1;
    self->page_links = g_ptr_array_new();
    
    /* Click gesture for links */
    self->click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(self->click_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(self->click_gesture, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->click_gesture));
    
    /* Drag gesture for text selection */
    self->drag_gesture = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(self->drag_gesture), GDK_BUTTON_PRIMARY);
    g_signal_connect(self->drag_gesture, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(self->drag_gesture, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(self->drag_gesture, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->drag_gesture));
    
    /* Middle-click pan gesture */
    self->pan_gesture = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(self->pan_gesture), GDK_BUTTON_MIDDLE);
    g_signal_connect(self->pan_gesture, "drag-begin", G_CALLBACK(on_pan_begin), self);
    g_signal_connect(self->pan_gesture, "drag-update", G_CALLBACK(on_pan_update), self);
    g_signal_connect(self->pan_gesture, "drag-end", G_CALLBACK(on_pan_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->pan_gesture));
    
    /* Initialize selection state */
    self->selection_start_page = -1;
    self->selection_end_page = -1;
    
    /* Pinch-to-zoom gesture for touchpad/touchscreen */
    self->zoom_gesture = gtk_gesture_zoom_new();
    g_signal_connect(self->zoom_gesture, "begin", G_CALLBACK(on_zoom_begin), self);
    g_signal_connect(self->zoom_gesture, "scale-changed", G_CALLBACK(on_zoom_scale_changed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->zoom_gesture));
    
    /* Scroll controller for zoom */
    self->scroll_controller = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(self->scroll_controller, "scroll-begin",
        G_CALLBACK(on_scroll_begin), self);
    g_signal_connect(self->scroll_controller, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), self->scroll_controller);
    
    /* Motion controller for link hover */
    self->motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(self->motion_controller, "enter", G_CALLBACK(on_motion_enter), self);
    g_signal_connect(self->motion_controller, "motion", G_CALLBACK(on_motion), self);
    g_signal_connect(self->motion_controller, "leave", G_CALLBACK(on_motion_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), self->motion_controller);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed",
        G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);
    
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

PdfvDocumentView*
pdfv_document_view_new(void)
{
    return g_object_new(PDFV_TYPE_DOCUMENT_VIEW, NULL);
}

void
pdfv_document_view_set_document(PdfvDocumentView* self, PhiDocument* document)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (self->document == document)
        return;
    
    cancel_page_render(self, TRUE);
    clear_render_cache(self);
    g_clear_object(&self->document);
    g_ptr_array_set_size(self->pages, 0);
    g_array_set_size(self->history, 0);
    self->history_pos = -1;
    self->scroll_x = 0;
    self->scroll_y = 0;
    self->current_page = 0;
    self->render_visible_first = -1;
    self->render_visible_last = -1;
    self->render_observed_first = -1;
    self->render_direction = 1;
    
    /* Free old page links */
    for (guint i = 0; i < self->page_links->len; i++) {
        phi_link_free(g_ptr_array_index(self->page_links, i));
    }
    g_ptr_array_set_size(self->page_links, 0);
    
    if (document) {
        self->document = g_object_ref(document);
        gint n_pages = phi_document_get_n_pages(document);
        g_ptr_array_set_size(self->pages, n_pages);
        g_ptr_array_set_size(self->page_links, n_pages);
        allocate_render_cache(self, n_pages);
        calculate_layout(self);
    }
    
    update_adjustments(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DOCUMENT]);
    notify_history_changed(self);
}

PhiDocument*
pdfv_document_view_get_document(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), NULL);
    return self->document;
}

void
pdfv_document_view_go_to_page(PdfvDocumentView* self, gint page)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!self->document)
        return;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    page = CLAMP(page, 0, n_pages - 1);
    gboolean changed = self->current_page != page;
    if (changed)
        self->render_direction = page > self->current_page ? 1 : -1;

    if (!self->continuous) {
        self->current_page = page;
        self->scroll_x = 0;
        self->scroll_y = 0;
        calculate_layout(self);
        update_adjustments(self);
        gtk_widget_queue_resize(GTK_WIDGET(self));
        if (changed)
            g_object_notify_by_pspec(G_OBJECT(self),
                                     props[PROP_CURRENT_PAGE]);
        return;
    }

    self->current_page = page;
    self->scroll_y = get_page_offset(self, page);
    
    if (self->vadjustment)
        gtk_adjustment_set_value(self->vadjustment, self->scroll_y);

    gtk_widget_queue_draw(GTK_WIDGET(self));
    if (changed)
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_PAGE]);
}

gint
pdfv_document_view_get_current_page(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), 0);
    return self->current_page;
}

void
pdfv_document_view_get_scroll_state(PdfvDocumentView* self,
                                    gint* page,
                                    gdouble* page_fraction,
                                    gdouble* horizontal_center)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    VerticalAnchor anchor = vertical_anchor_at(self, self->scroll_y);
    if (page)
        *page = anchor.page;
    if (page_fraction)
        *page_fraction = CLAMP(anchor.page_fraction, 0.0, 1.0);
    if (horizontal_center)
        *horizontal_center =
            self->zoom > 0 ? self->scroll_x / self->zoom : 0;
}

void
pdfv_document_view_restore_scroll_state(PdfvDocumentView* self,
                                        gint page,
                                        gdouble page_fraction,
                                        gdouble horizontal_center)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    if (!self->document || self->page_offsets->len == 0)
        return;

    gint n_pages = phi_document_get_n_pages(self->document);
    page = CLAMP(page, 0, n_pages - 1);
    pdfv_document_view_go_to_page(self, page);
    VerticalAnchor anchor = {
        .page = page,
        .page_fraction = CLAMP(page_fraction, 0.0, 1.0),
    };
    self->scroll_y = vertical_anchor_position(self, &anchor);
    self->scroll_x = horizontal_center * self->zoom;
    update_adjustments(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
pdfv_document_view_set_zoom(PdfvDocumentView* self, gdouble zoom)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    zoom_at_point(self, zoom,
                  gtk_widget_get_width(GTK_WIDGET(self)) / 2.0,
                  gtk_widget_get_height(GTK_WIDGET(self)) / 2.0);
}

static void
zoom_from_anchor(PdfvDocumentView* self, gdouble new_zoom, gdouble anchor_x,
                 const VerticalAnchor* anchor_y, gdouble focus_x,
                 gdouble focus_y)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    new_zoom = CLAMP(new_zoom, self->minimum_zoom, MAX_ZOOM);
    if (!isfinite(new_zoom) || new_zoom == self->zoom)
        return;

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));

    self->zoom = new_zoom;
    calculate_layout(self);

    /* The vertical anchor is relative to a page. PAGE_GAP is deliberately
     * fixed-size, so scaling the entire document offset would increasingly
     * drift upward on later pages. */
    self->scroll_x = anchor_x * new_zoom - focus_x + width / 2.0;
    self->scroll_y = vertical_anchor_position(self, anchor_y) - focus_y;

    /* Clamp vertical scroll */
    gdouble max_scroll_y = MAX(0, self->total_height - height);
    self->scroll_y = CLAMP(self->scroll_y, 0, max_scroll_y);

    /* Clamp horizontal scroll - same logic as update_adjustments */
    gdouble scroll_range = MAX(0, self->max_width - width);
    gdouble min_scroll_x = -scroll_range / 2.0;
    gdouble max_scroll_x = scroll_range / 2.0;
    self->scroll_x = CLAMP(self->scroll_x, min_scroll_x, max_scroll_x);

    update_adjustments(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ZOOM]);
}

/* Zoom towards a specific point in widget coordinates. */
static void
zoom_at_point(PdfvDocumentView* self, gdouble new_zoom, gdouble focus_x,
              gdouble focus_y)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gdouble anchor_x = self->zoom > 0
        ? (self->scroll_x + focus_x - width / 2.0) / self->zoom
        : 0;
    VerticalAnchor anchor_y = vertical_anchor_at(
        self, self->scroll_y + focus_y);
    zoom_from_anchor(self, new_zoom, anchor_x, &anchor_y, focus_x, focus_y);
}

gdouble
pdfv_document_view_get_zoom(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), 1.0);
    return self->zoom;
}

void
pdfv_document_view_zoom_in(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    pdfv_document_view_set_zoom(self, self->zoom * ZOOM_STEP);
}

void
pdfv_document_view_zoom_out(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    pdfv_document_view_set_zoom(self, self->zoom / ZOOM_STEP);
}

static gboolean
get_current_page_size_for_fit(PdfvDocumentView* self, gdouble* width,
                              gdouble* height)
{
    if (!self->document || self->current_page < 0 ||
        self->current_page >= (gint)self->pages->len)
        return FALSE;

    PhiPage* page = g_ptr_array_index(self->pages, self->current_page);
    if (!page) {
        /* The asynchronous document opener prepares both page zero and the
         * requested page. Adopt that prepared page here so restoring a saved
         * position on a later page cannot make the initial fit silently keep
         * the constructor's 100% zoom. An explicit fit action may load an
         * otherwise uncached page, which is preferable to doing nothing. */
        page = phi_document_get_page(
            self->document, self->current_page, NULL);
        if (page)
            g_ptr_array_index(self->pages, self->current_page) = page;
    }

    if (page) {
        gfloat page_width = 0;
        gfloat page_height = 0;
        phi_page_get_size(page, &page_width, &page_height);
        if (page_width > 0 && page_height > 0) {
            if (width)
                *width = page_width;
            if (height)
                *height = page_height;
            return TRUE;
        }
    }

    /* Retain a non-blocking fallback for an unusual page-load failure. The
     * layout cache contains zoomed dimensions, initially estimated from page
     * zero and refined as pages become visible. */
    if (self->zoom <= 0 ||
        self->current_page >= (gint)self->page_widths->len ||
        self->current_page >= (gint)self->page_heights->len)
        return FALSE;
    gdouble page_width = g_array_index(
        self->page_widths, gdouble, self->current_page) / self->zoom;
    gdouble page_height = g_array_index(
        self->page_heights, gdouble, self->current_page) / self->zoom;
    if (page_width <= 0 || page_height <= 0)
        return FALSE;
    if (width)
        *width = page_width;
    if (height)
        *height = page_height;
    return TRUE;
}

void
pdfv_document_view_zoom_fit_width(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    gdouble page_width = 0;
    if (!get_current_page_size_for_fit(self, &page_width, NULL))
        return;

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gdouble new_zoom =
        (width - 40) / page_width; /* 20px padding on each side */

    pdfv_document_view_set_zoom(self, new_zoom);
}

void
pdfv_document_view_zoom_fit_page(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    gdouble page_width = 0;
    gdouble page_height = 0;
    if (!get_current_page_size_for_fit(
            self, &page_width, &page_height))
        return;

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));

    gdouble zoom_w = (width - 40) / page_width;
    gdouble zoom_h = (height - 40) / page_height;

    pdfv_document_view_set_zoom(self, MIN(zoom_w, zoom_h));
}

void
pdfv_document_view_zoom_fit_page_full(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    gdouble page_width = 0;
    gdouble page_height = 0;
    if (!get_current_page_size_for_fit(
            self, &page_width, &page_height))
        return;

    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));
    pdfv_document_view_set_zoom(
        self, MIN(width / page_width, height / page_height));
}

void
pdfv_document_view_set_minimum_zoom(PdfvDocumentView* self, gdouble zoom)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    self->minimum_zoom = CLAMP(zoom, MIN_ZOOM, MAX_ZOOM);
    if (self->zoom < self->minimum_zoom)
        pdfv_document_view_set_zoom(self, self->minimum_zoom);
    else if (self->presentation_mode)
        update_adjustments(self);
}

gdouble
pdfv_document_view_get_minimum_zoom(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), MIN_ZOOM);
    return self->minimum_zoom;
}

void
pdfv_document_view_set_presentation_mode(PdfvDocumentView* self,
                                         gboolean presentation)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));

    presentation = !!presentation;
    if (self->presentation_mode == presentation)
        return;

    self->presentation_mode = presentation;
    if (presentation)
        cancel_scroll_momentum(self);
    update_adjustments(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gboolean
pdfv_document_view_get_presentation_mode(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), FALSE);
    return self->presentation_mode;
}

void
pdfv_document_view_set_continuous(PdfvDocumentView* self, gboolean continuous)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (self->continuous == continuous)
        return;
    
    self->continuous = continuous;
    calculate_layout(self);
    self->scroll_y = continuous ? get_page_offset(self, self->current_page) : 0;
    update_adjustments(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CONTINUOUS]);
}

gboolean
pdfv_document_view_get_continuous(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), TRUE);
    return self->continuous;
}

void
pdfv_document_view_set_dual_page(PdfvDocumentView* self, gboolean dual)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (self->dual_page == dual)
        return;
    
    self->dual_page = dual;
    calculate_layout(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_DUAL_PAGE]);
}

gboolean
pdfv_document_view_get_dual_page(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), FALSE);
    return self->dual_page;
}

void
pdfv_document_view_set_inverted(PdfvDocumentView* self, gboolean inverted)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (self->inverted == inverted)
        return;
    
    self->inverted = inverted;
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_INVERTED]);
}

gboolean
pdfv_document_view_get_inverted(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), FALSE);
    return self->inverted;
}

gboolean
pdfv_document_view_can_go_back(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), FALSE);
    return self->history_pos > 0;
}

gboolean
pdfv_document_view_can_go_forward(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), FALSE);
    return self->history_pos < (gint)self->history->len - 1;
}

void
pdfv_document_view_go_back(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!pdfv_document_view_can_go_back(self))
        return;

    save_current_history_entry(self);
    self->history_pos--;

    HistoryEntry* entry = &g_array_index(self->history, HistoryEntry, self->history_pos);
    restore_history_entry(self, entry);
    notify_history_changed(self);
}

void
pdfv_document_view_go_forward(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!pdfv_document_view_can_go_forward(self))
        return;

    save_current_history_entry(self);
    self->history_pos++;

    HistoryEntry* entry = &g_array_index(self->history, HistoryEntry, self->history_pos);
    restore_history_entry(self, entry);
    notify_history_changed(self);
}

void
pdfv_document_view_activate_link(PdfvDocumentView* self, const gchar* uri)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    g_return_if_fail(uri != NULL);
    g_return_if_fail(self->document != NULL);
    
    /* Try to resolve as internal link first */
    PhiLinkDest dest;
    if (phi_document_resolve_link(self->document, uri, &dest)) {
        if (dest.page >= 0)
            navigate_to_page_with_history(self, dest.page);
    } else {
        /* External URI */
        g_signal_emit(self, signals[SIGNAL_LINK_ACTIVATED], 0, uri);
    }
}

/* Clear search results */
static void
clear_search_results(PdfvDocumentView* self)
{
    if (self->search_results) {
        for (guint i = 0; i < self->search_results->len; i++) {
            SearchPageResult* result = &g_array_index(self->search_results, SearchPageResult, i);
            g_free(result->quads);
        }
        g_array_free(self->search_results, TRUE);
        self->search_results = NULL;
    }
    self->search_current_match = -1;
    self->search_total_matches = 0;
}

/* Incremental search state */
typedef struct {
    PdfvDocumentView* view;
    gchar* search_text;
    gint current_page;
    gint n_pages;
    GArray* results;
    gint total_matches;
} IncrementalSearchData;

static void
incremental_search_data_free(IncrementalSearchData* data)
{
    g_free(data->search_text);
    if (data->results) {
        for (guint i = 0; i < data->results->len; i++) {
            SearchPageResult* r = &g_array_index(data->results, SearchPageResult, i);
            g_free(r->quads);
        }
        g_array_free(data->results, TRUE);
    }
    g_slice_free(IncrementalSearchData, data);
}

/* Idle callback for incremental search - processes a few pages at a time */
static gboolean
search_idle_callback(gpointer user_data)
{
    IncrementalSearchData* data = user_data;
    PdfvDocumentView* self = data->view;
    
    /* Check if search was cancelled (text changed) */
    if (!self->search_text || g_strcmp0(self->search_text, data->search_text) != 0) {
        incremental_search_data_free(data);
        return G_SOURCE_REMOVE;
    }
    
    /* Process a batch of pages (5 at a time to keep UI responsive) */
    gint pages_to_process = MIN(5, data->n_pages - data->current_page);
    
    for (gint i = 0; i < pages_to_process; i++) {
        gint page_idx = data->current_page + i;
        
        PhiPage* page = g_ptr_array_index(self->pages, page_idx);
        if (!page) {
            page = phi_document_get_page(self->document, page_idx, NULL);
            g_ptr_array_index(self->pages, page_idx) = page;
        }
        
        if (!page)
            continue;
        
        PhiTextQuad quads[100];  /* Max 100 matches per page */
        gint count = phi_page_search_text(page, data->search_text, quads, 100);
        
        if (count > 0) {
            SearchPageResult pr = {
                .page = page_idx,
                .quad_count = count,
                .quads = g_memdup2(quads, count * sizeof(PhiTextQuad))
            };
            g_array_append_val(data->results, pr);
            data->total_matches += count;
        }
    }
    
    data->current_page += pages_to_process;
    
    /* Check if we're done */
    if (data->current_page >= data->n_pages) {
        /* Search complete - transfer results */
        clear_search_results(self);
        self->search_results = data->results;
        self->search_total_matches = data->total_matches;
        data->results = NULL;  /* Ownership transferred */
        
        /* Jump to first match */
        if (self->search_results && self->search_results->len > 0) {
            self->search_current_match = 0;
            SearchPageResult* first = &g_array_index(self->search_results, SearchPageResult, 0);
            pdfv_document_view_go_to_page(self, first->page);
        }
        
        /* Emit signal for UI to update status */
        g_signal_emit(self, signals[SIGNAL_SEARCH_COMPLETED], 0, self->search_total_matches);
        
        gtk_widget_queue_draw(GTK_WIDGET(self));
        g_free(data->search_text);
        g_slice_free(IncrementalSearchData, data);
        return G_SOURCE_REMOVE;
    }
    
    /* Continue searching */
    return G_SOURCE_CONTINUE;
}

/* Debounce callback - starts incremental search */
static gboolean
search_debounce_callback(gpointer user_data)
{
    PdfvDocumentView* self = PDFV_DOCUMENT_VIEW(user_data);
    self->search_debounce_id = 0;
    
    if (!self->document || !self->search_text || !*self->search_text)
        return G_SOURCE_REMOVE;
    
    /* Set up incremental search */
    IncrementalSearchData* data = g_slice_new0(IncrementalSearchData);
    data->view = self;
    data->search_text = g_strdup(self->search_text);
    data->current_page = 0;
    data->n_pages = phi_document_get_n_pages(self->document);
    data->results = g_array_new(FALSE, TRUE, sizeof(SearchPageResult));
    data->total_matches = 0;
    
    /* Use high priority idle to process quickly but still allow UI events */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, search_idle_callback, data, NULL);
    
    return G_SOURCE_REMOVE;
}

void
pdfv_document_view_search(PdfvDocumentView* self, const gchar* text)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    /* Cancel pending debounce */
    if (self->search_debounce_id) {
        g_source_remove(self->search_debounce_id);
        self->search_debounce_id = 0;
    }
    
    /* Clear current results */
    clear_search_results(self);
    g_free(self->search_text);
    self->search_text = NULL;
    
    if (!text || !*text || !self->document) {
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    
    /* Require minimum 2 characters to avoid searching entire doc for single letters */
    if (g_utf8_strlen(text, -1) < 2) {
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    
    self->search_text = g_strdup(text);
    
    /* Debounce: wait 250ms after last keystroke before searching */
    self->search_debounce_id = g_timeout_add(250, search_debounce_callback, self);
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
pdfv_document_view_search_next(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!self->search_results || self->search_results->len == 0)
        return;
    
    self->search_current_match++;
    if (self->search_current_match >= (gint)self->search_results->len)
        self->search_current_match = 0;
    
    SearchPageResult* result = &g_array_index(self->search_results, SearchPageResult, 
                                               self->search_current_match);
    pdfv_document_view_go_to_page(self, result->page);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
pdfv_document_view_search_prev(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!self->search_results || self->search_results->len == 0)
        return;
    
    self->search_current_match--;
    if (self->search_current_match < 0)
        self->search_current_match = self->search_results->len - 1;
    
    SearchPageResult* result = &g_array_index(self->search_results, SearchPageResult, 
                                               self->search_current_match);
    pdfv_document_view_go_to_page(self, result->page);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
pdfv_document_view_clear_search(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    clear_search_results(self);
    g_free(self->search_text);
    self->search_text = NULL;
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gint
pdfv_document_view_get_search_match_count(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), 0);
    return self->search_total_matches;
}

gint
pdfv_document_view_get_search_current_match(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), -1);
    return self->search_current_match;
}

gchar*
pdfv_document_view_get_selected_text(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), NULL);
    
    if (self->selection_start_page < 0 || self->selection_end_page < 0)
        return NULL;
    
    /* For now, only support selection within single page */
    if (self->selection_start_page != self->selection_end_page)
        return NULL;
    
    PhiPage* page = g_ptr_array_index(self->pages, self->selection_start_page);
    if (!page)
        return NULL;
    
    return phi_page_copy_selection(page, &self->selection_start, &self->selection_end);
}

void
pdfv_document_view_clear_selection(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    g_free(self->selection_quads);
    self->selection_quads = NULL;
    self->selection_quad_count = 0;
    self->selection_start_page = -1;
    self->selection_end_page = -1;
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

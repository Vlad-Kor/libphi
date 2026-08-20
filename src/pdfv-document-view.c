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

struct _PdfvDocumentView {
    GtkWidget parent_instance;
    
    PhiDocument* document;
    GPtrArray* pages; /* PhiPage* array, lazily populated */
    
    gdouble zoom;
    gboolean continuous;
    gboolean dual_page;
    gboolean inverted;
    
    gint current_page;
    gdouble scroll_x;
    gdouble scroll_y;
    
    /* Navigation history */
    GArray* history;
    gint history_pos;
    
    /* Cached page sizes for layout */
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
    
    /* Render cache - simple approach: cache current page ± 2 */
    GskRenderNode** render_cache;
    gint cache_first_page;
    gint cache_size;
    
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

G_DEFINE_TYPE_WITH_CODE(PdfvDocumentView, pdfv_document_view, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, pdfv_document_view_scrollable_init))

static void
clear_render_cache(PdfvDocumentView* self)
{
    if (self->render_cache) {
        for (gint i = 0; i < self->cache_size; i++) {
            if (self->render_cache[i])
                gsk_render_node_unref(self->render_cache[i]);
        }
        g_free(self->render_cache);
        self->render_cache = NULL;
    }
    self->cache_first_page = -1;
    self->cache_size = 0;
}

static void
calculate_layout(PdfvDocumentView* self)
{
    if (!self->document)
        return;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (n_pages == 0)
        return;
    
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
        g_array_index(self->page_heights, gdouble, i) = scaled_h;
        offset += scaled_h + PAGE_GAP;
        if (scaled_w > self->max_width)
            self->max_width = scaled_w;
    }
    
    self->total_height = offset > 0 ? offset - PAGE_GAP : 0;
}

static void
update_adjustments(PdfvDocumentView* self)
{
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));
    
    if (self->hadjustment) {
        /* When page is centered, scroll_x=0 means centered.
         * To see left edge: need scroll_x = -(max_width - width) / 2
         * To see right edge: need scroll_x = +(max_width - width) / 2
         * If page fits in viewport (max_width <= width), no scrolling needed.
         */
        gdouble scroll_range = MAX(0, self->max_width - width);
        gdouble lower = -scroll_range / 2.0;
        gdouble upper = scroll_range / 2.0 + width;
        
        gtk_adjustment_configure(self->hadjustment,
            CLAMP(self->scroll_x, lower, upper - width),
            lower, upper,
            width * 0.1,
            width * 0.9,
            width);
    }
    
    if (self->vadjustment) {
        gtk_adjustment_configure(self->vadjustment,
            self->scroll_y,
            0, self->total_height,
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
    gdouble page_offset =
        g_array_index(self->page_offsets, gdouble, page);
    gdouble page_height =
        g_array_index(self->page_heights, gdouble, page);
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

static GskRenderNode*
render_page_node(PdfvDocumentView* self, gint page_num)
{
    PhiPage* page = g_ptr_array_index(self->pages, page_num);
    if (!page) {
        /* Loading a newly visible page can replace its estimated height.
         * Preserve the location under the active pinch (or the viewport
         * center otherwise) while shifting all following page offsets. */
        gint viewport_height = gtk_widget_get_height(GTK_WIDGET(self));
        gdouble layout_focus_y =
            gtk_gesture_is_active(self->zoom_gesture)
                ? self->pinch_center_y
                : viewport_height / 2.0;
        VerticalAnchor layout_anchor = vertical_anchor_at(
            self, self->scroll_y + layout_focus_y);

        GError* load_error = NULL;
        page = phi_document_get_page(self->document, page_num, &load_error);
        if (!page) {
            g_warning("Failed to load page %d: %s", page_num + 1,
                      load_error ? load_error->message : "unknown error");
            g_clear_error(&load_error);
            return NULL;
        }
        g_ptr_array_index(self->pages, page_num) = page;

        /* Correct the estimated layout from this page onward. This is cheap
         * and keeps mixed-size documents accurate without eager loading. */
        gfloat width, height;
        phi_page_get_size(page, &width, &height);
        gdouble old_height =
            g_array_index(self->page_heights, gdouble, page_num);
        gdouble new_height = height * self->zoom;
        gdouble delta = new_height - old_height;
        self->max_width = MAX(self->max_width, width * self->zoom);
        if (fabs(delta) > 0.01) {
            g_array_index(self->page_heights, gdouble, page_num) = new_height;
            for (guint i = page_num + 1; i < self->page_offsets->len; i++)
                g_array_index(self->page_offsets, gdouble, i) += delta;
            self->total_height += delta;
            self->scroll_y = vertical_anchor_position(self, &layout_anchor) -
                layout_focus_y;
            self->scroll_y = CLAMP(
                self->scroll_y, 0,
                MAX(0, self->total_height - viewport_height));
            update_adjustments(self);
            gtk_widget_queue_resize(GTK_WIDGET(self));
        }
    }
    
    GError* error = NULL;
    GskRenderNode* base_node = phi_page_render_to_node(page, &error);
    
    if (error) {
        g_warning("Failed to render page %d: %s", page_num, error->message);
        g_error_free(error);
        return NULL;
    }
    
    /* Return base node without zoom - zoom applied at render time */
    return base_node;
}

static void
ensure_page_range_cached(PdfvDocumentView* self, gint first_page,
                         gint last_page)
{
    if (!self->document)
        return;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (first_page < 0 || first_page >= n_pages ||
        last_page < first_page)
        return;

    /* Render exactly what intersects the viewport. This stays lazy for long
     * documents while avoiding permanently blank pages when zoomed out. */
    gint cache_start = first_page;
    gint cache_end = MIN(last_page, n_pages - 1);
    gint new_cache_size = cache_end - cache_start + 1;
    
    /* Check if we need to rebuild cache */
    if (self->render_cache && cache_start == self->cache_first_page && 
        new_cache_size == self->cache_size) {
        return;
    }
    
    /* Build new cache */
    GskRenderNode** new_cache = g_new0(GskRenderNode*, new_cache_size);
    
    for (gint i = cache_start; i <= cache_end; i++) {
        gint old_idx = i - self->cache_first_page;
        gint new_idx = i - cache_start;
        
        /* Reuse from old cache if available */
        if (self->render_cache && old_idx >= 0 && old_idx < self->cache_size &&
            self->render_cache[old_idx]) {
            new_cache[new_idx] = self->render_cache[old_idx];
            self->render_cache[old_idx] = NULL;
        } else {
            new_cache[new_idx] = render_page_node(self, i);
        }
    }
    
    /* Free old cache */
    clear_render_cache(self);
    
    self->render_cache = new_cache;
    self->cache_first_page = cache_start;
    self->cache_size = new_cache_size;
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
    gint first_visible = get_page_at_offset(self, view_top, NULL);
    gint last_visible = get_page_at_offset(self, view_bottom, NULL);
    
    /* Update current page */
    gint center_page = get_page_at_offset(self, view_top + height / 2, NULL);
    if (center_page != self->current_page) {
        self->current_page = center_page;
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_PAGE]);
    }
    
    /* Ensure every visible page is cached. */
    ensure_page_range_cached(self, first_visible, last_visible);
    
    /* Check if system is in dark mode */
    AdwStyleManager* style_manager = adw_style_manager_get_default();
    gboolean is_dark = adw_style_manager_get_dark(style_manager);
    
    /* Background colors based on system theme */
    static const GdkRGBA bg_light = {0.878, 0.878, 0.878, 1.0};  /* #e0e0e0 */
    static const GdkRGBA bg_dark = {0.118, 0.118, 0.118, 1.0};   /* #1e1e1e */
    gtk_snapshot_append_color(snapshot, is_dark ? &bg_dark : &bg_light, 
        &GRAPHENE_RECT_INIT(0, 0, width, height));
    
    /* Render visible pages - start from first visible page */
    static const GdkRGBA shadow_color = {0, 0, 0, 0.2};
    static const GdkRGBA page_bg_light = {1.0, 1.0, 1.0, 1.0};   /* #ffffff */
    static const GdkRGBA page_bg_dark = {0.102, 0.102, 0.102, 1.0}; /* #1a1a1a */
    const GdkRGBA* page_bg = self->inverted ? &page_bg_dark : &page_bg_light;
    
    for (gint i = first_visible; i < n_pages; i++) {
        gdouble y_offset = g_array_index(self->page_offsets, gdouble, i);
        
        /* Stop if past visible area */
        if (y_offset > view_bottom)
            break;
        
        /* Get page - may not be loaded yet */
        PhiPage* page = g_ptr_array_index(self->pages, i);
        if (!page)
            continue;
        
        /* Get page dimensions */
        gfloat pw_f, ph_f;
        phi_page_get_size(page, &pw_f, &ph_f);
        gdouble pw = pw_f * self->zoom;
        gdouble ph = ph_f * self->zoom;
        
        /* Center page horizontally */
        gdouble x = (width - pw) / 2.0 - self->scroll_x;
        gdouble y = y_offset - self->scroll_y;
        
        /* Page shadow */
        gtk_snapshot_append_color(snapshot, &shadow_color,
            &GRAPHENE_RECT_INIT(x + 3, y + 3, pw, ph));
        
        /* Page background */
        gtk_snapshot_append_color(snapshot, page_bg,
            &GRAPHENE_RECT_INIT(x, y, pw, ph));
        
        /* Render page content */
        gint cache_idx = i - self->cache_first_page;
        if (self->render_cache && cache_idx >= 0 && cache_idx < self->cache_size &&
            self->render_cache[cache_idx]) {
            
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
            
            /* Apply zoom - cached nodes are at base resolution */
            if (self->zoom != 1.0)
                gtk_snapshot_scale(snapshot, self->zoom, self->zoom);
            
            /* Apply inversion with hue rotation if needed */
            if (self->inverted) {
                /* First push hue rotation 180° */
                /* Hue rotation matrix for 180°:
                 * Uses the standard formula with cos(180°)=-1, sin(180°)=0
                 * With luminance preservation weights: R=0.213, G=0.715, B=0.072
                 */
                graphene_matrix_t hue_matrix;
                graphene_vec4_t hue_offset;
                graphene_matrix_init_from_float(&hue_matrix, (float[16]){
                    -0.574f,  0.426f,  0.426f, 0,
                     0.285f, -0.715f,  0.285f, 0,
                     0.928f,  0.928f, -0.072f, 0,
                     0,       0,       0,      1
                });
                graphene_vec4_init(&hue_offset, 0, 0, 0, 0);
                gtk_snapshot_push_color_matrix(snapshot, &hue_matrix, &hue_offset);
                
                /* Then push invert */
                graphene_matrix_t invert_matrix;
                graphene_vec4_t invert_offset;
                graphene_matrix_init_from_float(&invert_matrix, (float[16]){
                    -1, 0, 0, 0,
                    0, -1, 0, 0,
                    0, 0, -1, 0,
                    0, 0, 0, 1
                });
                graphene_vec4_init(&invert_offset, 1, 1, 1, 0);
                gtk_snapshot_push_color_matrix(snapshot, &invert_matrix, &invert_offset);
            }
            
            gtk_snapshot_append_node(snapshot, self->render_cache[cache_idx]);
            
            if (self->inverted) {
                gtk_snapshot_pop(snapshot);  /* pop invert */
                gtk_snapshot_pop(snapshot);  /* pop hue rotate */
            }
            
            /* Get page bounds for coordinate offset */
            gfloat bounds_x0, bounds_y0;
            phi_page_get_bounds(page, &bounds_x0, &bounds_y0, NULL, NULL);
            
            /* Render search highlights for this page */
            /* Note: we're inside gtk_snapshot_scale context, so don't multiply by zoom */
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
                (i == self->selection_start_page || i == self->selection_end_page ||
                 (i > self->selection_start_page && i < self->selection_end_page))) {
                GdkRGBA select_color = {0.2, 0.5, 1.0, 0.4}; /* Blue */
                for (gint q = 0; q < self->selection_quad_count; q++) {
                    PhiTextQuad* quad = &self->selection_quads[q];
                    float min_x = MIN(MIN(quad->ul.x, quad->ur.x), MIN(quad->ll.x, quad->lr.x)) - bounds_x0;
                    float max_x = MAX(MAX(quad->ul.x, quad->ur.x), MAX(quad->ll.x, quad->lr.x)) - bounds_x0;
                    float min_y = MIN(MIN(quad->ul.y, quad->ur.y), MIN(quad->ll.y, quad->lr.y)) - bounds_y0;
                    float max_y = MAX(MAX(quad->ul.y, quad->ur.y), MAX(quad->ll.y, quad->lr.y)) - bounds_y0;
                    gtk_snapshot_append_color(snapshot, &select_color,
                        &GRAPHENE_RECT_INIT(min_x, min_y, max_x - min_x, max_y - min_y));
                }
            }
            
            gtk_snapshot_restore(snapshot);
        }
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
    if (value != self->scroll_x) {
        self->scroll_x = value;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static void
on_vadjustment_changed(GtkAdjustment* adj, PdfvDocumentView* self)
{
    gdouble value = gtk_adjustment_get_value(adj);
    if (value != self->scroll_y) {
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
    
    /* Double-click: select word */
    if (n_press == 2) {
        gint page_num;
        graphene_point_t page_point;
        if (screen_to_page_coords(self, x, y, &page_num, &page_point)) {
            PhiPage* page = g_ptr_array_index(self->pages, page_num);
            if (page) {
                graphene_point_t word_start, word_end;
                if (phi_page_select_word_at(page, &page_point, &word_start, &word_end)) {
                    self->selection_start_page = page_num;
                    self->selection_start = word_start;
                    self->selection_end_page = page_num;
                    self->selection_end = word_end;
                    self->double_click_selected = TRUE;  /* Prevent drag from clearing */
                    
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
        
        if (link)
            gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "pointer");
        else
            gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
    }
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
    
    clear_render_cache(self);
    g_clear_object(&self->document);
    g_clear_pointer(&self->pages, g_ptr_array_unref);
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
    self->continuous = TRUE;
    self->dual_page = FALSE;
    self->inverted = FALSE;
    self->current_page = 0;
    
    self->pages = g_ptr_array_new();  /* We don't own the pages, document does */
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
    
    clear_render_cache(self);
    g_clear_object(&self->document);
    g_ptr_array_set_size(self->pages, 0);
    g_array_set_size(self->history, 0);
    self->history_pos = -1;
    self->scroll_x = 0;
    self->scroll_y = 0;
    self->current_page = 0;
    
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
    
    self->scroll_y = get_page_offset(self, page);
    
    if (self->vadjustment)
        gtk_adjustment_set_value(self->vadjustment, self->scroll_y);
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

gint
pdfv_document_view_get_current_page(PdfvDocumentView* self)
{
    g_return_val_if_fail(PDFV_IS_DOCUMENT_VIEW(self), 0);
    return self->current_page;
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

    new_zoom = CLAMP(new_zoom, MIN_ZOOM, MAX_ZOOM);
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

void
pdfv_document_view_zoom_fit_width(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!self->document)
        return;
    
    PhiPage* page = g_ptr_array_index(self->pages, self->current_page);
    if (!page)
        return;
    
    gfloat pw, ph;
    phi_page_get_size(page, &pw, &ph);
    
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gdouble new_zoom = (width - 40) / pw; /* 20px padding on each side */
    
    pdfv_document_view_set_zoom(self, new_zoom);
}

void
pdfv_document_view_zoom_fit_page(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!self->document)
        return;
    
    PhiPage* page = g_ptr_array_index(self->pages, self->current_page);
    if (!page)
        return;
    
    gfloat pw, ph;
    phi_page_get_size(page, &pw, &ph);
    
    gint width = gtk_widget_get_width(GTK_WIDGET(self));
    gint height = gtk_widget_get_height(GTK_WIDGET(self));
    
    gdouble zoom_w = (width - 40) / pw;
    gdouble zoom_h = (height - 40) / ph;
    
    pdfv_document_view_set_zoom(self, MIN(zoom_w, zoom_h));
}

void
pdfv_document_view_set_continuous(PdfvDocumentView* self, gboolean continuous)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (self->continuous == continuous)
        return;
    
    self->continuous = continuous;
    gtk_widget_queue_draw(GTK_WIDGET(self));
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

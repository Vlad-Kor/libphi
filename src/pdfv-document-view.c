/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-document-view.h"
#include <phi/phipage.h>
#include <phi/phidocument.h>

#define MAX_HISTORY 100
#define MIN_ZOOM 0.1
#define MAX_ZOOM 10.0
#define ZOOM_STEP 1.2
#define PAGE_GAP 10

typedef struct {
    gint page;
    gdouble scroll_y;
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
    gboolean navigating; /* True when navigating via back/forward */
    
    /* Cached page sizes for layout */
    GArray* page_heights;
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
    
    /* Middle-click pan state */
    gdouble pan_start_scroll_x;
    gdouble pan_start_scroll_y;
    
    /* Link under cursor */
    gchar* hover_link;
    
    /* Render cache - simple approach: cache current page ± 2 */
    GskRenderNode** render_cache;
    gint cache_first_page;
    gint cache_size;
    
    /* Text selection */
    gboolean selecting;
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
    N_SIGNALS
};

static GParamSpec* props[N_PROPS];
static guint signals[N_SIGNALS];

static void pdfv_document_view_scrollable_init(GtkScrollableInterface* iface);

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
    self->total_height = 0;
    self->max_width = 0;
    
    for (gint i = 0; i < n_pages; i++) {
        PhiPage* page = g_ptr_array_index(self->pages, i);
        if (!page) {
            page = phi_document_get_page(self->document, i, NULL);
            g_ptr_array_index(self->pages, i) = page;
        }
        
        if (page) {
            gfloat w, h;
            phi_page_get_size(page, &w, &h);
            gdouble scaled_w = w * self->zoom;
            gdouble scaled_h = h * self->zoom;
            
            g_array_index(self->page_heights, gdouble, i) = scaled_h;
            self->total_height += scaled_h + PAGE_GAP;
            if (scaled_w > self->max_width)
                self->max_width = scaled_w;
        }
    }
    
    if (self->total_height > 0)
        self->total_height -= PAGE_GAP; /* Remove last gap */
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
    if (!self->document)
        return 0;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    gdouble offset = 0;
    
    for (gint i = 0; i < n_pages; i++) {
        gdouble h = g_array_index(self->page_heights, gdouble, i);
        if (y < offset + h) {
            if (page_offset)
                *page_offset = offset;
            return i;
        }
        offset += h + PAGE_GAP;
    }
    
    if (page_offset)
        *page_offset = offset - g_array_index(self->page_heights, gdouble, n_pages - 1) - PAGE_GAP;
    return n_pages - 1;
}

static gdouble
get_page_offset(PdfvDocumentView* self, gint page)
{
    if (!self->document || page < 0)
        return 0;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (page >= n_pages)
        page = n_pages - 1;
    
    gdouble offset = 0;
    for (gint i = 0; i < page; i++) {
        offset += g_array_index(self->page_heights, gdouble, i) + PAGE_GAP;
    }
    return offset;
}

static void
push_history(PdfvDocumentView* self)
{
    if (self->navigating)
        return;
    
    /* Remove any forward history */
    if (self->history_pos < (gint)self->history->len - 1) {
        g_array_set_size(self->history, self->history_pos + 1);
    }
    
    HistoryEntry entry = {
        .page = self->current_page,
        .scroll_y = self->scroll_y
    };
    
    g_array_append_val(self->history, entry);
    
    /* Limit history size */
    if (self->history->len > MAX_HISTORY) {
        g_array_remove_index(self->history, 0);
    }
    
    self->history_pos = self->history->len - 1;
    
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_BACK]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_FORWARD]);
}

static GskRenderNode*
render_page_node(PdfvDocumentView* self, gint page_num)
{
    PhiPage* page = g_ptr_array_index(self->pages, page_num);
    if (!page)
        return NULL;
    
    GError* error = NULL;
    GskRenderNode* base_node = phi_page_render_to_node(page, &error);
    
    if (error) {
        g_warning("Failed to render page %d: %s", page_num, error->message);
        g_error_free(error);
        return NULL;
    }
    
    /* Apply zoom transform */
    if (base_node && self->zoom != 1.0) {
        GtkSnapshot* snap = gtk_snapshot_new();
        gtk_snapshot_scale(snap, self->zoom, self->zoom);
        gtk_snapshot_append_node(snap, base_node);
        gsk_render_node_unref(base_node);
        base_node = gtk_snapshot_free_to_node(snap);
    }
    
    return base_node;
}

static void
ensure_page_cached(PdfvDocumentView* self, gint page)
{
    if (!self->document)
        return;
    
    gint n_pages = phi_document_get_n_pages(self->document);
    if (page < 0 || page >= n_pages)
        return;
    
    /* Define cache window */
    gint cache_start = MAX(0, page - 2);
    gint cache_end = MIN(n_pages - 1, page + 2);
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
    
    /* Update current page */
    gint center_page = get_page_at_offset(self, view_top + height / 2, NULL);
    if (center_page != self->current_page) {
        self->current_page = center_page;
        g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CURRENT_PAGE]);
    }
    
    /* Ensure pages are cached */
    ensure_page_cached(self, center_page);
    
    /* Background */
    GdkRGBA bg_color;
    if (self->inverted)
        gdk_rgba_parse(&bg_color, "#1e1e1e");
    else
        gdk_rgba_parse(&bg_color, "#e0e0e0");
    gtk_snapshot_append_color(snapshot, &bg_color, 
        &GRAPHENE_RECT_INIT(0, 0, width, height));
    
    /* Render visible pages */
    gdouble y_offset = 0;
    for (gint i = 0; i < n_pages; i++) {
        gdouble page_height = g_array_index(self->page_heights, gdouble, i);
        
        /* Skip if not visible */
        if (y_offset + page_height < view_top) {
            y_offset += page_height + PAGE_GAP;
            continue;
        }
        if (y_offset > view_bottom)
            break;
        
        /* Get page - may not be loaded yet */
        PhiPage* page = g_ptr_array_index(self->pages, i);
        if (!page) {
            y_offset += g_array_index(self->page_heights, gdouble, i) + PAGE_GAP;
            continue;
        }
        
        /* Get page dimensions */
        gfloat pw_f, ph_f;
        phi_page_get_size(page, &pw_f, &ph_f);
        gdouble pw = pw_f * self->zoom;
        gdouble ph = ph_f * self->zoom;
        
        /* Center page horizontally */
        gdouble x = (width - pw) / 2.0 - self->scroll_x;
        gdouble y = y_offset - self->scroll_y;
        
        /* Page shadow */
        GdkRGBA shadow_color = {0, 0, 0, 0.2};
        gtk_snapshot_append_color(snapshot, &shadow_color,
            &GRAPHENE_RECT_INIT(x + 3, y + 3, pw, ph));
        
        /* Page background (white) */
        GdkRGBA page_bg;
        if (self->inverted)
            gdk_rgba_parse(&page_bg, "#1a1a1a");
        else
            gdk_rgba_parse(&page_bg, "#ffffff");
        gtk_snapshot_append_color(snapshot, &page_bg,
            &GRAPHENE_RECT_INIT(x, y, pw, ph));
        
        /* Render page content */
        gint cache_idx = i - self->cache_first_page;
        if (self->render_cache && cache_idx >= 0 && cache_idx < self->cache_size &&
            self->render_cache[cache_idx]) {
            
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
            
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
            
            /* Render search highlights for this page */
            if (self->search_results && self->search_results->len > 0) {
                for (guint sr = 0; sr < self->search_results->len; sr++) {
                    SearchPageResult* result = &g_array_index(self->search_results, SearchPageResult, sr);
                    if (result->page == i) {
                        GdkRGBA highlight_color = {1.0, 1.0, 0.0, 0.4}; /* Yellow */
                        for (gint q = 0; q < result->quad_count; q++) {
                            PhiTextQuad* quad = &result->quads[q];
                            float min_x = MIN(MIN(quad->ul.x, quad->ur.x), MIN(quad->ll.x, quad->lr.x));
                            float max_x = MAX(MAX(quad->ul.x, quad->ur.x), MAX(quad->ll.x, quad->lr.x));
                            float min_y = MIN(MIN(quad->ul.y, quad->ur.y), MIN(quad->ll.y, quad->lr.y));
                            float max_y = MAX(MAX(quad->ul.y, quad->ur.y), MAX(quad->ll.y, quad->lr.y));
                            gtk_snapshot_append_color(snapshot, &highlight_color,
                                &GRAPHENE_RECT_INIT(min_x * self->zoom, min_y * self->zoom,
                                                    (max_x - min_x) * self->zoom,
                                                    (max_y - min_y) * self->zoom));
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
                    float min_x = MIN(MIN(quad->ul.x, quad->ur.x), MIN(quad->ll.x, quad->lr.x));
                    float max_x = MAX(MAX(quad->ul.x, quad->ur.x), MAX(quad->ll.x, quad->lr.x));
                    float min_y = MIN(MIN(quad->ul.y, quad->ur.y), MIN(quad->ll.y, quad->lr.y));
                    float max_y = MAX(MAX(quad->ul.y, quad->ur.y), MAX(quad->ll.y, quad->lr.y));
                    gtk_snapshot_append_color(snapshot, &select_color,
                        &GRAPHENE_RECT_INIT(min_x * self->zoom, min_y * self->zoom,
                                            (max_x - min_x) * self->zoom,
                                            (max_y - min_y) * self->zoom));
                }
            }
            
            gtk_snapshot_restore(snapshot);
        }
        
        y_offset += page_height + PAGE_GAP;
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
    
    if (n_press != 1)
        return;
    
    PhiLink* link = find_link_at(self, x, y);
    if (link && link->uri) {
        push_history(self);
        
        /* Try to resolve as internal link */
        PhiLinkDest dest;
        if (phi_document_resolve_link(self->document, link->uri, &dest)) {
            pdfv_document_view_go_to_page(self, dest.page);
        } else {
            /* External URI */
            g_signal_emit(self, signals[SIGNAL_LINK_ACTIVATED], 0, link->uri);
        }
    }
}

static void
on_motion(GtkEventControllerMotion* controller, gdouble x, gdouble y,
          PdfvDocumentView* self)
{
    (void)controller;
    
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
on_zoom_begin(GtkGestureZoom* gesture, GdkEventSequence* sequence,
              PdfvDocumentView* self)
{
    (void)gesture;
    (void)sequence;
    self->pinch_start_zoom = self->zoom;
}

static void
on_zoom_scale_changed(GtkGestureZoom* gesture, gdouble scale,
                      PdfvDocumentView* self)
{
    (void)gesture;
    gdouble new_zoom = self->pinch_start_zoom * scale;
    new_zoom = CLAMP(new_zoom, MIN_ZOOM, MAX_ZOOM);
    pdfv_document_view_set_zoom(self, new_zoom);
}

static gboolean
on_scroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy,
          PdfvDocumentView* self)
{
    (void)dx;
    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(controller));
    
    if (state & GDK_CONTROL_MASK) {
        /* Zoom with Ctrl+Scroll */
        if (dy < 0)
            pdfv_document_view_zoom_in(self);
        else if (dy > 0)
            pdfv_document_view_zoom_out(self);
        return TRUE;
    }
    
    return FALSE; /* Let default scrolling happen */
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
    
    /* Convert to page coordinates */
    gdouble page_x = (screen_x + self->scroll_x - (width - scaled_pw) / 2.0) / self->zoom;
    gdouble page_y = (self->scroll_y + screen_y - page_offset) / self->zoom;
    
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
    
    /* Clear previous selection */
    g_free(self->selection_quads);
    self->selection_quads = NULL;
    self->selection_quad_count = 0;
    
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
    (void)offset_x;
    (void)offset_y;
    
    self->selecting = FALSE;
    gtk_widget_set_cursor(GTK_WIDGET(self), NULL);
    
    /* Copy selection to clipboard if we have selected text */
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
    
    clear_render_cache(self);
    g_clear_object(&self->document);
    g_clear_pointer(&self->pages, g_ptr_array_unref);
    g_clear_pointer(&self->page_heights, g_array_unref);
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
    g_signal_connect(self->scroll_controller, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), self->scroll_controller);
    
    /* Motion controller for link hover */
    self->motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(self->motion_controller, "motion", G_CALLBACK(on_motion), self);
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
    
    zoom = CLAMP(zoom, MIN_ZOOM, MAX_ZOOM);
    if (zoom == self->zoom)
        return;
    
    /* Save current page position */
    gint page = self->current_page;
    
    self->zoom = zoom;
    clear_render_cache(self);
    calculate_layout(self);
    
    /* Restore page position */
    self->scroll_y = get_page_offset(self, page);
    
    update_adjustments(self);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ZOOM]);
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
    
    self->navigating = TRUE;
    self->history_pos--;
    
    HistoryEntry* entry = &g_array_index(self->history, HistoryEntry, self->history_pos);
    self->scroll_y = entry->scroll_y;
    
    if (self->vadjustment)
        gtk_adjustment_set_value(self->vadjustment, self->scroll_y);
    
    self->navigating = FALSE;
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_BACK]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_FORWARD]);
}

void
pdfv_document_view_go_forward(PdfvDocumentView* self)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    if (!pdfv_document_view_can_go_forward(self))
        return;
    
    self->navigating = TRUE;
    self->history_pos++;
    
    HistoryEntry* entry = &g_array_index(self->history, HistoryEntry, self->history_pos);
    self->scroll_y = entry->scroll_y;
    
    if (self->vadjustment)
        gtk_adjustment_set_value(self->vadjustment, self->scroll_y);
    
    self->navigating = FALSE;
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_BACK]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_GO_FORWARD]);
}

void
pdfv_document_view_activate_link(PdfvDocumentView* self, const gchar* uri)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    g_return_if_fail(uri != NULL);
    
    /* Try to resolve as internal link first */
    PhiLinkDest dest;
    if (phi_document_resolve_link(self->document, uri, &dest)) {
        if (dest.page >= 0) {
            push_history(self);
            pdfv_document_view_go_to_page(self, dest.page);
        }
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

void
pdfv_document_view_search(PdfvDocumentView* self, const gchar* text)
{
    g_return_if_fail(PDFV_IS_DOCUMENT_VIEW(self));
    
    clear_search_results(self);
    g_free(self->search_text);
    self->search_text = NULL;
    
    if (!text || !*text || !self->document) {
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    
    self->search_text = g_strdup(text);
    self->search_results = g_array_new(FALSE, TRUE, sizeof(SearchPageResult));
    
    gint n_pages = phi_document_get_n_pages(self->document);
    
    /* Search all pages */
    for (gint i = 0; i < n_pages; i++) {
        PhiPage* page = g_ptr_array_index(self->pages, i);
        if (!page) {
            page = phi_document_get_page(self->document, i, NULL);
            g_ptr_array_index(self->pages, i) = page;
        }
        
        if (!page)
            continue;
        
        PhiTextQuad quads[100];  /* Max 100 matches per page */
        gint count = phi_page_search_text(page, text, quads, 100);
        
        if (count > 0) {
            SearchPageResult result = {
                .page = i,
                .quad_count = count,
                .quads = g_memdup2(quads, count * sizeof(PhiTextQuad))
            };
            g_array_append_val(self->search_results, result);
            self->search_total_matches += count;
        }
    }
    
    /* Jump to first match */
    if (self->search_results->len > 0) {
        self->search_current_match = 0;
        SearchPageResult* first = &g_array_index(self->search_results, SearchPageResult, 0);
        pdfv_document_view_go_to_page(self, first->page);
    }
    
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

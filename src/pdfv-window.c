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
    AdwTabView* tab_view;
    AdwTabBar* tab_bar;
    AdwTabOverview* tab_overview;
    
    /* Main layout */
    AdwOverlaySplitView* split_view;
    AdwToolbarView* toolbar_view;
    AdwHeaderBar* header_bar;
    
    /* Header bar buttons */
    GtkWidget* nav_box;
    GtkButton* back_button;
    GtkButton* forward_button;
    GtkToggleButton* sidebar_button;
    GtkMenuButton* menu_button;
    
    /* Sidebar */
    GtkListBox* thumbnail_list;
    
    /* Floating zoom controls */
    GtkWidget* zoom_box;
    GtkButton* zoom_in_btn;
    GtkButton* zoom_out_btn;
    GtkLabel* zoom_label;
    
    /* Current view (active tab) */
    PdfvDocumentView* current_view;
    
    /* Outline data for current document */
    PhiOutlineItem* current_outline;
};

G_DEFINE_TYPE(PdfvWindow, pdfv_window, ADW_TYPE_APPLICATION_WINDOW)

static void
update_navigation_buttons(PdfvWindow* self)
{
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

static void
update_zoom_info(PdfvWindow* self)
{
    if (!self->current_view) {
        gtk_label_set_text(self->zoom_label, "100%");
        return;
    }
    
    gdouble zoom = pdfv_document_view_get_zoom(self->current_view);
    gchar* text = g_strdup_printf("%.0f%%", zoom * 100);
    gtk_label_set_text(self->zoom_label, text);
    g_free(text);
}

static void
on_view_notify(PdfvDocumentView* view, GParamSpec* pspec, PdfvWindow* self)
{
    (void)view;
    const gchar* name = g_param_spec_get_name(pspec);
    
    if (g_strcmp0(name, "can-go-back") == 0 || g_strcmp0(name, "can-go-forward") == 0) {
        update_navigation_buttons(self);
    } else if (g_strcmp0(name, "zoom") == 0) {
        update_zoom_info(self);
    }
}

static void
on_link_activated(PdfvDocumentView* view, const gchar* uri, PdfvWindow* self)
{
    (void)view;
    GtkUriLauncher* launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(launcher);
}

static void
setup_document_view_signals(PdfvWindow* self, PdfvDocumentView* view)
{
    g_signal_connect(view, "notify", G_CALLBACK(on_view_notify), self);
    g_signal_connect(view, "link-activated", G_CALLBACK(on_link_activated), self);
}

/* Thumbnail data for sidebar */
typedef struct {
    GtkWidget* drawing_area;
    PhiPage* page;
    gint page_num;
} ThumbnailData;

static void
thumbnail_data_free(ThumbnailData* data)
{
    /* Note: We don't unref page because phi_document_get_page returns
     * a borrowed reference - the document owns the page */
    g_free(data);
}

static void
on_thumbnail_draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, ThumbnailData* data)
{
    (void)area;
    
    if (!data->page)
        return;
    
    gfloat pw, ph;
    phi_page_get_size(data->page, &pw, &ph);
    
    gdouble scale_x = (gdouble)width / pw;
    gdouble scale_y = (gdouble)height / ph;
    gdouble scale = MIN(scale_x, scale_y) * 0.85;
    
    gdouble offset_x = (width - pw * scale) / 2.0;
    gdouble offset_y = (height - ph * scale) / 2.0;
    
    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    
    /* Shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_rectangle(cr, 3, 3, pw, ph);
    cairo_fill(cr);
    
    /* Page background */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, 0, pw, ph);
    cairo_fill(cr);
    
    /* Border */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_set_line_width(cr, 1.0 / scale);
    cairo_rectangle(cr, 0, 0, pw, ph);
    cairo_stroke(cr);
    
    cairo_restore(cr);
}

static GtkWidget*
create_thumbnail_row(PhiDocument* doc, gint page_num)
{
    ThumbnailData* data = g_new0(ThumbnailData, 1);
    data->page_num = page_num;
    data->page = phi_document_get_page(doc, page_num, NULL);
    
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "thumb-data", data, 
        (GDestroyNotify)thumbnail_data_free);
    
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    
    data->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(data->drawing_area, 100, 130);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(data->drawing_area),
        (GtkDrawingAreaDrawFunc)on_thumbnail_draw, data, NULL);
    gtk_box_append(GTK_BOX(box), data->drawing_area);
    
    gchar* label_text = g_strdup_printf("%d", page_num + 1);
    GtkWidget* label = gtk_label_new(label_text);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");
    g_free(label_text);
    gtk_box_append(GTK_BOX(box), label);
    
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    
    return row;
}

static void
populate_thumbnails(PdfvWindow* self, PhiDocument* document)
{
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->thumbnail_list)))) {
        gtk_list_box_remove(self->thumbnail_list, child);
    }
    
    if (!document)
        return;
    
    gint n_pages = phi_document_get_n_pages(document);
    for (gint i = 0; i < n_pages; i++) {
        GtkWidget* row = create_thumbnail_row(document, i);
        gtk_list_box_append(self->thumbnail_list, row);
    }
}

static void
on_thumbnail_row_activated(GtkListBox* box, GtkListBoxRow* row, PdfvWindow* self)
{
    (void)box;
    if (!self->current_view || !row)
        return;
    
    ThumbnailData* data = g_object_get_data(G_OBJECT(row), "thumb-data");
    if (data) {
        pdfv_document_view_go_to_page(self->current_view, data->page_num);
    }
}

static GtkWidget*
create_tab_content(PdfvWindow* self)
{
    PdfvDocumentView* view = pdfv_document_view_new();
    setup_document_view_signals(self, view);
    
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(view));
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    
    g_object_set_data(G_OBJECT(scrolled), "document-view", view);
    
    return scrolled;
}

static void
on_tab_selected(AdwTabView* tab_view, GParamSpec* pspec, PdfvWindow* self)
{
    (void)pspec;
    
    AdwTabPage* page = adw_tab_view_get_selected_page(tab_view);
    
    if (!page) {
        self->current_view = NULL;
        update_navigation_buttons(self);
        update_zoom_info(self);
        return;
    }
    
    GtkWidget* scrolled = adw_tab_page_get_child(page);
    self->current_view = g_object_get_data(G_OBJECT(scrolled), "document-view");
    
    update_navigation_buttons(self);
    update_zoom_info(self);
    
    if (self->current_view) {
        PhiDocument* doc = pdfv_document_view_get_document(self->current_view);
        if (doc)
            populate_thumbnails(self, doc);
    }
}

static gboolean
on_tab_close_page(AdwTabView* tab_view, AdwTabPage* page, PdfvWindow* self)
{
    /* Clear current_view if we're closing its tab */
    GtkWidget* scrolled = adw_tab_page_get_child(page);
    PdfvDocumentView* view = g_object_get_data(G_OBJECT(scrolled), "document-view");
    if (view == self->current_view) {
        /* Disconnect signals before destruction */
        g_signal_handlers_disconnect_by_data(view, self);
        self->current_view = NULL;
    }
    
    adw_tab_view_close_page_finish(tab_view, page, TRUE);
    return GDK_EVENT_STOP;
}

static void
on_file_dialog_opened(GObject* source, GAsyncResult* result, gpointer user_data)
{
    GtkFileDialog* dialog = GTK_FILE_DIALOG(source);
    PdfvWindow* self = PDFV_WINDOW(user_data);
    
    GFile* file = gtk_file_dialog_open_finish(dialog, result, NULL);
    if (file) {
        pdfv_window_open_file(self, file);
        g_object_unref(file);
    }
}

static void action_open(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open PDF");
    
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF Documents");
    gtk_file_filter_add_mime_type(filter, "application/pdf");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_object_unref(filter);
    
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, on_file_dialog_opened, self);
}

static void action_new_tab(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    pdfv_window_new_tab(PDFV_WINDOW(user_data));
}

static void action_close_tab(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    AdwTabPage* page = adw_tab_view_get_selected_page(self->tab_view);
    if (page)
        adw_tab_view_close_page(self->tab_view, page);
}

static void action_go_back(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_go_back(self->current_view);
}

static void action_go_forward(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_go_forward(self->current_view);
}

static void action_zoom_in(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_zoom_in(self->current_view);
}

static void action_zoom_out(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_zoom_out(self->current_view);
}

static void action_zoom_reset(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_set_zoom(self->current_view, 1.0);
}

static void action_zoom_fit_width(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_zoom_fit_width(self->current_view);
}

static void action_zoom_fit_page(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view)
        pdfv_document_view_zoom_fit_page(self->current_view);
}

static void action_fullscreen(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
        gtk_window_unfullscreen(GTK_WINDOW(self));
    else
        gtk_window_fullscreen(GTK_WINDOW(self));
}

static void action_toggle_sidebar(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    gboolean visible = adw_overlay_split_view_get_show_sidebar(self->split_view);
    adw_overlay_split_view_set_show_sidebar(self->split_view, !visible);
}

static void action_invert_colors(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view) {
        gboolean inverted = pdfv_document_view_get_inverted(self->current_view);
        pdfv_document_view_set_inverted(self->current_view, !inverted);
    }
}

static void action_page_next(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view) {
        gint page = pdfv_document_view_get_current_page(self->current_view);
        pdfv_document_view_go_to_page(self->current_view, page + 1);
    }
}

static void action_page_prev(GSimpleAction* action, GVariant* parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    PdfvWindow* self = PDFV_WINDOW(user_data);
    if (self->current_view) {
        gint page = pdfv_document_view_get_current_page(self->current_view);
        pdfv_document_view_go_to_page(self->current_view, page - 1);
    }
}

static GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL },
    { "new-tab", action_new_tab, NULL, NULL, NULL },
    { "close-tab", action_close_tab, NULL, NULL, NULL },
    { "go-back", action_go_back, NULL, NULL, NULL },
    { "go-forward", action_go_forward, NULL, NULL, NULL },
    { "zoom-in", action_zoom_in, NULL, NULL, NULL },
    { "zoom-out", action_zoom_out, NULL, NULL, NULL },
    { "zoom-reset", action_zoom_reset, NULL, NULL, NULL },
    { "zoom-fit-width", action_zoom_fit_width, NULL, NULL, NULL },
    { "zoom-fit-page", action_zoom_fit_page, NULL, NULL, NULL },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL },
    { "toggle-sidebar", action_toggle_sidebar, NULL, NULL, NULL },
    { "invert-colors", action_invert_colors, NULL, NULL, NULL },
    { "page-next", action_page_next, NULL, NULL, NULL },
    { "page-prev", action_page_prev, NULL, NULL, NULL },
};

static void
on_tab_overview_button_clicked(GtkButton* button, PdfvWindow* self)
{
    (void)button;
    gboolean is_open = adw_tab_overview_get_open(self->tab_overview);
    adw_tab_overview_set_open(self->tab_overview, !is_open);
}

static AdwTabPage*
on_tab_overview_create_tab(AdwTabOverview* overview, PdfvWindow* self)
{
    (void)overview;
    GtkWidget* content = create_tab_content(self);
    AdwTabPage* page = adw_tab_view_append(self->tab_view, content);
    adw_tab_page_set_title(page, "New Tab");
    return page;
}

static void
on_zoom_in_clicked(GtkButton* button, PdfvWindow* self)
{
    (void)button;
    if (self->current_view)
        pdfv_document_view_zoom_in(self->current_view);
}

static void
on_zoom_out_clicked(GtkButton* button, PdfvWindow* self)
{
    (void)button;
    if (self->current_view)
        pdfv_document_view_zoom_out(self->current_view);
}

static void
on_sidebar_show_changed(AdwOverlaySplitView* split_view, GParamSpec* pspec, PdfvWindow* self)
{
    (void)pspec;
    gboolean visible = adw_overlay_split_view_get_show_sidebar(split_view);
    gtk_toggle_button_set_active(self->sidebar_button, visible);
}

static void
pdfv_window_dispose(GObject* object)
{
    PdfvWindow* self = PDFV_WINDOW(object);
    
    if (self->current_outline) {
        phi_outline_item_free(self->current_outline);
        self->current_outline = NULL;
    }
    
    G_OBJECT_CLASS(pdfv_window_parent_class)->dispose(object);
}

static void
pdfv_window_init(PdfvWindow* self)
{
    self->current_view = NULL;
    self->current_outline = NULL;
    
    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
        G_N_ELEMENTS(win_actions), self);
    
    /* ===== WIDGET HIERARCHY =====
     * 
     * AdwApplicationWindow
     *   └─ AdwTabOverview (content)
     *        └─ AdwToolbarView (child)
     *             ├─ AdwHeaderBar (top-bar)
     *             ├─ AdwTabBar (top-bar)
     *             └─ AdwOverlaySplitView (content)
     *                  ├─ Sidebar (sidebar)
     *                  └─ GtkOverlay (content)
     *                       ├─ AdwTabView (child)
     *                       └─ ZoomControls (overlay)
     */
    
    /* Tab Overview - outermost wrapper for tab overview gesture */
    self->tab_overview = ADW_TAB_OVERVIEW(adw_tab_overview_new());
    adw_tab_overview_set_enable_new_tab(self->tab_overview, TRUE);
    g_signal_connect(self->tab_overview, "create-tab", 
        G_CALLBACK(on_tab_overview_create_tab), self);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), 
        GTK_WIDGET(self->tab_overview));
    
    /* Main toolbar view */
    self->toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_tab_overview_set_child(self->tab_overview, GTK_WIDGET(self->toolbar_view));
    
    /* Header bar */
    self->header_bar = ADW_HEADER_BAR(adw_header_bar_new());
    adw_toolbar_view_add_top_bar(self->toolbar_view, GTK_WIDGET(self->header_bar));
    
    /* Navigation buttons */
    self->nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->nav_box, "linked");
    gtk_widget_set_visible(self->nav_box, FALSE);
    
    self->back_button = GTK_BUTTON(gtk_button_new_from_icon_name("go-previous-symbolic"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->back_button), "win.go-back");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->back_button), "Go Back (Alt+Left)");
    gtk_widget_set_visible(GTK_WIDGET(self->back_button), FALSE);
    gtk_box_append(GTK_BOX(self->nav_box), GTK_WIDGET(self->back_button));
    
    self->forward_button = GTK_BUTTON(gtk_button_new_from_icon_name("go-next-symbolic"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->forward_button), "win.go-forward");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->forward_button), "Go Forward (Alt+Right)");
    gtk_widget_set_visible(GTK_WIDGET(self->forward_button), FALSE);
    gtk_box_append(GTK_BOX(self->nav_box), GTK_WIDGET(self->forward_button));
    
    adw_header_bar_pack_start(self->header_bar, self->nav_box);
    
    /* Sidebar toggle */
    self->sidebar_button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(self->sidebar_button), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->sidebar_button), "Toggle Sidebar (F9)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->sidebar_button), "win.toggle-sidebar");
    adw_header_bar_pack_start(self->header_bar, GTK_WIDGET(self->sidebar_button));
    
    /* New tab button */
    GtkWidget* new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(new_tab_btn), "win.new-tab");
    gtk_widget_set_tooltip_text(new_tab_btn, "New Tab (Ctrl+T)");
    adw_header_bar_pack_start(self->header_bar, new_tab_btn);
    
    /* Menu button */
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->menu_button), "Main Menu");
    gtk_menu_button_set_primary(self->menu_button, TRUE);
    
    GMenu* menu = g_menu_new();
    GMenu* file_section = g_menu_new();
    g_menu_append(file_section, "Open…", "win.open");
    g_menu_append(file_section, "New Tab", "win.new-tab");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(file_section));
    
    GMenu* zoom_section = g_menu_new();
    g_menu_append(zoom_section, "Zoom In", "win.zoom-in");
    g_menu_append(zoom_section, "Zoom Out", "win.zoom-out");
    g_menu_append(zoom_section, "Reset Zoom", "win.zoom-reset");
    g_menu_append(zoom_section, "Fit Width", "win.zoom-fit-width");
    g_menu_append(zoom_section, "Fit Page", "win.zoom-fit-page");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(zoom_section));
    
    GMenu* view_section = g_menu_new();
    g_menu_append(view_section, "Invert Colors", "win.invert-colors");
    g_menu_append(view_section, "Fullscreen", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view_section));
    
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    adw_header_bar_pack_end(self->header_bar, GTK_WIDGET(self->menu_button));
    
    g_object_unref(file_section);
    g_object_unref(zoom_section);
    g_object_unref(view_section);
    g_object_unref(menu);
    
    /* Tab bar - below header bar */
    self->tab_bar = ADW_TAB_BAR(adw_tab_bar_new());
    adw_tab_bar_set_autohide(self->tab_bar, TRUE);
    adw_toolbar_view_add_top_bar(self->toolbar_view, GTK_WIDGET(self->tab_bar));
    
    /* Split view */
    self->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_width_fraction(self->split_view, 0.18);
    adw_overlay_split_view_set_min_sidebar_width(self->split_view, 160);
    adw_overlay_split_view_set_max_sidebar_width(self->split_view, 260);
    adw_overlay_split_view_set_enable_hide_gesture(self->split_view, TRUE);
    adw_overlay_split_view_set_enable_show_gesture(self->split_view, TRUE);
    g_signal_connect(self->split_view, "notify::show-sidebar", 
        G_CALLBACK(on_sidebar_show_changed), self);
    adw_toolbar_view_set_content(self->toolbar_view, GTK_WIDGET(self->split_view));
    
    /* Sidebar content */
    GtkWidget* sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar_box, "view");
    
    GtkWidget* sidebar_header = gtk_label_new("Pages");
    gtk_widget_add_css_class(sidebar_header, "title-4");
    gtk_widget_set_margin_top(sidebar_header, 12);
    gtk_widget_set_margin_bottom(sidebar_header, 8);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_header);
    
    gtk_box_append(GTK_BOX(sidebar_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    
    GtkWidget* thumb_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thumb_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(thumb_scroll, TRUE);
    
    self->thumbnail_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->thumbnail_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->thumbnail_list), "navigation-sidebar");
    g_signal_connect(self->thumbnail_list, "row-activated",
        G_CALLBACK(on_thumbnail_row_activated), self);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(thumb_scroll),
        GTK_WIDGET(self->thumbnail_list));
    gtk_box_append(GTK_BOX(sidebar_box), thumb_scroll);
    
    adw_overlay_split_view_set_sidebar(self->split_view, sidebar_box);
    
    /* Tab view */
    self->tab_view = ADW_TAB_VIEW(adw_tab_view_new());
    adw_tab_bar_set_view(self->tab_bar, self->tab_view);
    adw_tab_overview_set_view(self->tab_overview, self->tab_view);
    
    /* Tab overview button - must be set after tab_view exists */
    GtkWidget* tab_overview_btn = adw_tab_button_new();
    adw_tab_button_set_view(ADW_TAB_BUTTON(tab_overview_btn), self->tab_view);
    gtk_widget_set_tooltip_text(tab_overview_btn, "View Open Tabs");
    g_signal_connect(tab_overview_btn, "clicked",
        G_CALLBACK(on_tab_overview_button_clicked), self);
    adw_header_bar_pack_end(self->header_bar, tab_overview_btn);
    
    g_signal_connect(self->tab_view, "notify::selected-page",
        G_CALLBACK(on_tab_selected), self);
    g_signal_connect(self->tab_view, "close-page",
        G_CALLBACK(on_tab_close_page), self);
    
    /* Content overlay for floating controls */
    GtkWidget* content_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(content_overlay), GTK_WIDGET(self->tab_view));
    
    /* Floating zoom controls */
    self->zoom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->zoom_box, "linked");
    gtk_widget_add_css_class(self->zoom_box, "osd");
    gtk_widget_add_css_class(self->zoom_box, "toolbar");
    gtk_widget_set_halign(self->zoom_box, GTK_ALIGN_END);
    gtk_widget_set_valign(self->zoom_box, GTK_ALIGN_END);
    gtk_widget_set_margin_end(self->zoom_box, 12);
    gtk_widget_set_margin_bottom(self->zoom_box, 12);
    
    self->zoom_out_btn = GTK_BUTTON(gtk_button_new_from_icon_name("zoom-out-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(self->zoom_out_btn), "flat");
    g_signal_connect(self->zoom_out_btn, "clicked", G_CALLBACK(on_zoom_out_clicked), self);
    gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_out_btn));
    
    self->zoom_label = GTK_LABEL(gtk_label_new("100%"));
    gtk_widget_set_margin_start(GTK_WIDGET(self->zoom_label), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(self->zoom_label), 6);
    gtk_label_set_width_chars(self->zoom_label, 5);
    gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_label));
    
    self->zoom_in_btn = GTK_BUTTON(gtk_button_new_from_icon_name("zoom-in-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(self->zoom_in_btn), "flat");
    g_signal_connect(self->zoom_in_btn, "clicked", G_CALLBACK(on_zoom_in_clicked), self);
    gtk_box_append(GTK_BOX(self->zoom_box), GTK_WIDGET(self->zoom_in_btn));
    
    gtk_overlay_add_overlay(GTK_OVERLAY(content_overlay), self->zoom_box);
    
    adw_overlay_split_view_set_content(self->split_view, content_overlay);
    
    /* Window setup */
    gtk_window_set_default_size(GTK_WINDOW(self), 900, 700);
    gtk_window_set_title(GTK_WINDOW(self), "PDF Viewer");
    
    /* Create initial tab */
    pdfv_window_new_tab(self);
    
    update_navigation_buttons(self);
}

static void
pdfv_window_class_init(PdfvWindowClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = pdfv_window_dispose;
}

PdfvWindow*
pdfv_window_new(AdwApplication* app)
{
    return g_object_new(PDFV_TYPE_WINDOW, "application", app, NULL);
}

void
pdfv_window_open_file(PdfvWindow* self, GFile* file)
{
    g_return_if_fail(PDFV_IS_WINDOW(self));
    g_return_if_fail(G_IS_FILE(file));
    
    GError* error = NULL;
    PhiDocument* doc = phi_document_new_from_file(file, &error);
    
    if (error) {
        AdwDialog* dialog = adw_alert_dialog_new("Error Opening File", error->message);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
        adw_dialog_present(dialog, GTK_WIDGET(self));
        g_error_free(error);
        return;
    }
    
    PdfvDocumentView* view = NULL;
    AdwTabPage* page = adw_tab_view_get_selected_page(self->tab_view);
    
    if (page && self->current_view && 
        !pdfv_document_view_get_document(self->current_view)) {
        view = self->current_view;
    } else {
        GtkWidget* content = create_tab_content(self);
        view = g_object_get_data(G_OBJECT(content), "document-view");
        page = adw_tab_view_append(self->tab_view, content);
        adw_tab_view_set_selected_page(self->tab_view, page);
    }
    
    pdfv_document_view_set_document(view, doc);
    
    gchar* basename = g_file_get_basename(file);
    adw_tab_page_set_title(page, basename);
    g_free(basename);
    
    populate_thumbnails(self, doc);
    
    g_object_unref(doc);
}

void
pdfv_window_new_tab(PdfvWindow* self)
{
    g_return_if_fail(PDFV_IS_WINDOW(self));
    
    GtkWidget* content = create_tab_content(self);
    AdwTabPage* page = adw_tab_view_append(self->tab_view, content);
    adw_tab_page_set_title(page, "New Tab");
    adw_tab_view_set_selected_page(self->tab_view, page);
}

PdfvDocumentView*
pdfv_window_get_current_view(PdfvWindow* self)
{
    g_return_val_if_fail(PDFV_IS_WINDOW(self), NULL);
    return self->current_view;
}

/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pdfv-application.h"
#include "pdfv-window.h"

struct _PdfvApplication {
	AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE(PdfvApplication, pdfv_application, ADW_TYPE_APPLICATION)

static void pdfv_application_activate(GApplication* app) {
	PdfvWindow* window = pdfv_window_new(ADW_APPLICATION(app));
	pdfv_window_restore_last_workspace(window);
	gtk_window_present(GTK_WINDOW(window));
}

static void pdfv_application_open(GApplication* app, GFile** files, gint n_files, const gchar* hint) {
	(void)hint;
	
	for (gint i = 0; i < n_files; i++) {
		PdfvWindow* window = pdfv_window_new(ADW_APPLICATION(app));
		pdfv_window_open_file(window, files[i]);
		gtk_window_present(GTK_WINDOW(window));
	}
}

static void pdfv_application_startup(GApplication* app) {
	G_APPLICATION_CLASS(pdfv_application_parent_class)->startup(app);
	
	/* Set up keyboard shortcuts */
	const char* open_accels[] = { "<Control>o", NULL };
	const char* new_tab_accels[] = { "<Control>t", NULL };
	const char* close_tab_accels[] = { "<Control>w", NULL };
	const char* quit_accels[] = { "<Control>q", NULL };
	const char* zoom_in_accels[] = { "<Control>plus", "<Control>equal", "plus", NULL };
	const char* zoom_out_accels[] = { "<Control>minus", "minus", NULL };
	const char* zoom_reset_accels[] = { "<Control>0", NULL };
	const char* fullscreen_accels[] = { "F11", NULL };
	const char* find_accels[] = { "<Control>f", "slash", NULL };
	const char* workspace_find_accels[] = { "<Control><Shift>f", NULL };
	const char* find_next_accels[] = { "F3", "<Control>g", NULL };
	const char* find_prev_accels[] = { "<Shift>F3", "<Control><Shift>g", NULL };
	const char* go_back_accels[] = { "<Alt>Left", NULL };
	const char* go_forward_accels[] = { "<Alt>Right", NULL };
	const char* prev_page_accels[] = { "Page_Up", "p", NULL };
	const char* next_page_accels[] = { "Page_Down", "n", NULL };
	const char* first_page_accels[] = { "Home", NULL };
	const char* last_page_accels[] = { "End", NULL };
	const char* toggle_sidebar_accels[] = { "F9", NULL };
	const char* invert_accels[] = { "<Control>i", NULL };
	
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.new-tab", new_tab_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.close-tab", close_tab_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-in", zoom_in_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-out", zoom_out_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-reset", zoom_reset_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.fullscreen", fullscreen_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find", find_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.workspace-search", workspace_find_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find-next", find_next_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find-prev", find_prev_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.go-back", go_back_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.go-forward", go_forward_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prev-page", prev_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.next-page", next_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.first-page", first_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.last-page", last_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.toggle-sidebar", toggle_sidebar_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.invert-colors", invert_accels);
}

static void pdfv_application_quit_action(GSimpleAction* action, GVariant* param, gpointer app) {
	(void)action;
	(void)param;
	g_application_quit(G_APPLICATION(app));
}

static void pdfv_application_about_action(GSimpleAction* action, GVariant* param, gpointer app) {
	(void)action;
	(void)param;
	
	GtkWindow* window = gtk_application_get_active_window(GTK_APPLICATION(app));
	
	const char* developers[] = { "sp1rit", "Vlad-Kor", NULL };
	adw_show_about_dialog(GTK_WIDGET(window),
		"application-name", "Phi PDF Viewer",
		"application-icon", "arpa.sp1rit.phi.viewer",
		"version", "0.1",
		"developers", developers,
		"license-type", GTK_LICENSE_AGPL_3_0,
		"comments", "A high-performance PDF viewer powered by MuPDF and GTK4",
		"website", "https://github.com/sp1ritCS/libphi",
		NULL);
}

static void pdfv_application_class_init(PdfvApplicationClass* klass) {
	GApplicationClass* app_class = G_APPLICATION_CLASS(klass);
	
	app_class->activate = pdfv_application_activate;
	app_class->open = pdfv_application_open;
	app_class->startup = pdfv_application_startup;
}

static void pdfv_application_init(PdfvApplication* self) {
	/* Add application actions */
	static const GActionEntry app_actions[] = {
		{ .name = "quit", .activate = pdfv_application_quit_action },
		{ .name = "about", .activate = pdfv_application_about_action },
	};
	
	g_action_map_add_action_entries(G_ACTION_MAP(self), app_actions, G_N_ELEMENTS(app_actions), self);
}

PdfvApplication* pdfv_application_new(void) {
	return g_object_new(PDFV_TYPE_APPLICATION,
		"application-id", "arpa.sp1rit.phi.viewer",
		"flags", G_APPLICATION_HANDLES_OPEN,
		NULL);
}

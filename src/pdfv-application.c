/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
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
	const char* reopen_closed_tab_accels[] = { "<Control><Shift>t", NULL };
	const char* save_accels[] = { "<Control>s", NULL };
	const char* quit_accels[] = { "<Control>q", NULL };
	const char* zoom_in_accels[] = { "<Control>plus", "<Control>equal", NULL };
	const char* zoom_out_accels[] = { "<Control>minus", NULL };
	const char* zoom_reset_accels[] = { "<Control>0", NULL };
	const char* fullscreen_accels[] = { "F11", NULL };
	const char* presentation_accels[] = { "F5", NULL };
	const char* find_accels[] = { "<Control>f", NULL };
	const char* new_window_accels[] = { "<Control>n", NULL };
	const char* workspace_find_accels[] = { "<Control><Shift>f", NULL };
	const char* find_next_accels[] = { "F3", "<Control>g", NULL };
	const char* find_prev_accels[] = { "<Shift>F3", "<Control><Shift>g", NULL };
	const char* go_back_accels[] = { "<Alt>Left", NULL };
	const char* go_forward_accels[] = { "<Alt>Right", NULL };
	const char* prev_page_accels[] = { "Page_Up", NULL };
	const char* next_page_accels[] = { "Page_Down", NULL };
	const char* toggle_sidebar_accels[] = { "F9", NULL };
	const char* invert_accels[] = { "<Control>i", NULL };
	
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.new-tab", new_tab_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.close-tab", close_tab_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reopen-closed-tab", reopen_closed_tab_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.save", save_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-in", zoom_in_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-out", zoom_out_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-reset", zoom_reset_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.fullscreen", fullscreen_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.present", presentation_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find", find_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.new-workspace-window", new_window_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.workspace-search", workspace_find_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find-next", find_next_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.find-prev", find_prev_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.go-back", go_back_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.go-forward", go_forward_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.page-prev", prev_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.page-next", next_page_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.toggle-sidebar", toggle_sidebar_accels);
	gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.invert-colors", invert_accels);
}

static void pdfv_application_quit_action(GSimpleAction* action, GVariant* param, gpointer app) {
	(void)action;
	(void)param;
	GList* windows = g_list_copy(gtk_application_get_windows(GTK_APPLICATION(app)));
	for (GList* at = windows; at; at = at->next)
		g_object_ref(at->data);
	for (GList* at = windows; at; at = at->next)
		gtk_window_close(GTK_WINDOW(at->data));
	g_list_free_full(windows, g_object_unref);
}

static void pdfv_application_about_action(GSimpleAction* action, GVariant* param, gpointer app) {
	(void)action;
	(void)param;
	
	GtkWindow* window = gtk_application_get_active_window(GTK_APPLICATION(app));
	
	const char* developers[] = {
		"Vlad Korsakov",
		"Florian \"sp1rit\"",
		NULL,
	};
	const char* acknowledgements[] = {
		"artisticat1 and the Obsidian LaTeX Suite contributors",
		"Karl Yngve Lervåg and the VimTeX contributors",
		NULL,
	};
	const char* technology_acknowledgements[] = {
		"Artifex Software and the MuPDF contributors",
		"The MathJax Consortium and contributors",
		"Knut Sveidqvist and the Mermaid contributors",
		"Marijn Haverbeke and the CodeMirror and Lezer contributors",
		"The Independent JPEG Group",
		"The authors of the bundled Markdown editor dependencies",
		NULL,
	};
	AdwAboutDialog* about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
	adw_about_dialog_set_application_name(about, "Phi Document Viewer");
	adw_about_dialog_set_application_icon(about, "ai.korsakov.Phi");
	adw_about_dialog_set_version(about, "0.1");
	adw_about_dialog_set_developers(about, developers);
	adw_about_dialog_set_copyright(
		about,
		"Copyright © 2026 Vlad Korsakov\n"
		"libphi Copyright © 2025 Florian \"sp1rit\"");
	adw_about_dialog_set_license_type(about, GTK_LICENSE_AGPL_3_0);
	adw_about_dialog_set_comments(
		about,
		"A high-performance PDF and Markdown viewer powered by MuPDF, GTK4, and WebKitGTK");
	adw_about_dialog_set_website(about, "https://github.com/Vlad-Kor/libphi");
	adw_about_dialog_add_link(about, "Other Repository", "https://github.com/sp1ritCS/libphi");
	adw_about_dialog_add_acknowledgement_section(
		about, "LaTeX snippet defaults", acknowledgements);
	adw_about_dialog_add_acknowledgement_section(
		about, "Third-party technology", technology_acknowledgements);
	adw_about_dialog_add_legal_section(
		about, "Obsidian LaTeX Suite snippet defaults",
		"Copyright © 2022 artisticat1", GTK_LICENSE_MIT_X11, NULL);
	adw_about_dialog_add_legal_section(
		about, "VimTeX-derived conceal mappings",
		"Copyright © 2025 Karl Yngve Lervåg", GTK_LICENSE_MIT_X11,
		NULL);
	adw_about_dialog_add_legal_section(
		about, "Mermaid", "Copyright © 2014–2022 Knut Sveidqvist",
		GTK_LICENSE_MIT_X11, NULL);
	adw_about_dialog_add_legal_section(
		about, "CodeMirror",
		"Copyright © 2018–2021 Marijn Haverbeke and others",
		GTK_LICENSE_MIT_X11, NULL);
	adw_about_dialog_add_legal_section(
		about, "MathJax", "The MathJax Consortium and contributors",
		GTK_LICENSE_APACHE_2_0, NULL);
	adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(window));
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
	GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
	if (g_getenv("PHI_DEVELOPMENT_NON_UNIQUE"))
		flags |= G_APPLICATION_NON_UNIQUE;
	return g_object_new(PDFV_TYPE_APPLICATION,
		"application-id", "ai.korsakov.Phi",
		"flags", flags,
		NULL);
}

/*
 * libphi - High performance document renderer for GTK
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtk/gtk.h>

#include <phi/phidocument.h>
#include <phi/phiview.h>

static void app_open(GtkApplication* app, GFile** files, gint n_files, gchar*, gpointer) {
	if (n_files != 1)
		g_error("Expected one file");

	GtkWidget *window = gtk_application_window_new(app);	
	
	GError* err = NULL;
	PhiDocument* doc = phi_document_new_from_file(files[0], &err);
	if (err)
		g_error("Failed loading document: %s", err->message);
	PhiPage* page = phi_document_get_page(doc, 0, &err);
	if (err)
		g_error("Failed loading page: %s", err->message);
	GskRenderNode* node = phi_page_render_to_node(page, &err);
	if (err)
		g_error("Failed to render to node: %s", err->message);

	GtkWidget* view = phi_view_new(node);
	gtk_widget_set_hexpand(view, TRUE);
	gtk_widget_set_vexpand(view, TRUE);
	gtk_widget_set_overflow(view, GTK_OVERFLOW_HIDDEN);

	g_object_unref (page);
	gsk_render_node_unref (node);

	gtk_window_set_child(GTK_WINDOW(window), view);
	gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char** argv) {
	g_autoptr(GtkApplication) app = gtk_application_new("arpa.sp1rit.phi.viewer", G_APPLICATION_HANDLES_OPEN);
	g_signal_connect(app, "open", G_CALLBACK(app_open), NULL);
	return g_application_run(G_APPLICATION(app), argc, argv);
}

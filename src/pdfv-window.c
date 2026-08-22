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
#include "pdfv-settings.h"
#include "pdfv-workspace.h"
#include "markdown-editor.h"
#include "markdown-resource-scheme.h"
#include <phi/phidocument.h>
#include <phi/phipage.h>

#include <string.h>

#define WORKSPACE_VISIBLE_MATCHES 4
#define WORKSPACE_PREVIEW_DELAY_MS 90

struct _PdfvWindow {
  AdwApplicationWindow parent_instance;

  /* Tab system */
  AdwTabView *tab_view;
  AdwTabBar *tab_bar;
  AdwTabOverview *tab_overview;

  /* Main layout */
  AdwOverlaySplitView *split_view;
  AdwToastOverlay *toast_overlay;
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;

  /* Header bar buttons */
  GtkWidget *nav_box;
  GtkButton *back_button;
  GtkButton *forward_button;
  GtkToggleButton *sidebar_button;
  GtkMenuButton *menu_button;
  GMenu *file_menu_section;
  GMenu *workspace_menu_section;
  GMenu *zoom_menu_section;
  GMenu *view_menu_section;

  /* Sidebar */
  GtkListView *thumbnail_list;
  GtkSingleSelection *thumbnail_selection;
  AdwViewStack *sidebar_stack;
  AdwViewStackPage *pages_sidebar_page;
  AdwViewStackPage *workspace_sidebar_page;
  GtkStack *workspace_content_stack;
  GtkWidget *workspace_loading_spinner;
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
  GHashTable *workspace_expanded_paths; /* Relative folder paths */
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
  AdwTabPage *workspace_browse_tab;
  GFile *workspace_preview_file;
  GCancellable *workspace_preview_cancellable;
  guint workspace_preview_delay_id;
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
  PdfvMarkdownEditor *current_editor;
  AdwTabPage *window_title_page;
  gboolean closing_window;

  /* One initialized editor removes WebKit/CodeMirror startup from the next
   * new Markdown tab without retaining a document or its contents. */
  PdfvMarkdownEditor *markdown_editor_spare;
  GFile *markdown_editor_spare_root;
  guint markdown_editor_prewarm_id;

  /* Global preferences, mirrored into every window/editor. */
  PdfvSettings *settings;
  guint settings_update_timeout_id;

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
static AdwTabPage *workspace_open_file(PdfvWindow *self, GFile *file,
                                       gboolean persistent);
static void open_markdown_in_tab_async(PdfvWindow *self, GFile *file,
                                       AdwTabPage *page);
static GFile *markdown_vault_root_for_file(PdfvWindow *self, GFile *file);
static void workspace_search_close(PdfvWindow *self, gboolean commit);
static void workspace_search_schedule(PdfvWindow *self, guint delay_ms);
static void workspace_preview_cancel_load(PdfvWindow *self);
static void on_markdown_error(PdfvMarkdownEditor *editor,
                              const gchar *message, PdfvWindow *self);
static void on_markdown_ready(PdfvMarkdownEditor *editor, PdfvWindow *self);
static void rebuild_main_menu(PdfvWindow *self);

static void apply_preferences_to_editor(PdfvWindow *self,
                                        PdfvMarkdownEditor *editor) {
  AdwStyleManager *style = adw_style_manager_get_default();
  pdfv_markdown_editor_set_theme(
      editor, adw_style_manager_get_dark(style),
      pdfv_settings_get_markdown_font_scale(self->settings));
  pdfv_markdown_editor_set_remote_images_allowed(
      editor, pdfv_settings_get_allow_remote_images(self->settings));
  pdfv_markdown_editor_set_readable_line_width(
      editor, pdfv_settings_get_readable_line_width(self->settings));
  pdfv_markdown_editor_set_latex_conceal(
      editor, pdfv_settings_get_latex_conceal(self->settings));
  pdfv_markdown_editor_set_snippets(
      editor, pdfv_settings_get_latex_snippets(self->settings));
  GFile *workspace_root = self->workspace
      ? pdfv_workspace_get_folder(self->workspace) : NULL;
  GFile *attachment_folder = NULL;
  if (workspace_root &&
      pdfv_settings_get_workspace_attachment_fixed(self->settings,
                                                    workspace_root)) {
    gchar *uri = pdfv_settings_dup_workspace_attachment_folder_uri(
        self->settings, workspace_root);
    if (uri && *uri)
      attachment_folder = g_file_new_for_uri(uri);
    g_free(uri);
  }
  pdfv_markdown_editor_set_attachment_folder(editor, attachment_folder);
  g_clear_object(&attachment_folder);
}

static void apply_markdown_preferences(PdfvWindow *self) {
  if (self->tab_view) {
    guint pages = adw_tab_view_get_n_pages(self->tab_view);
    for (guint i = 0; i < pages; i++) {
      AdwTabPage *page = adw_tab_view_get_nth_page(self->tab_view, i);
      GtkWidget *stack = adw_tab_page_get_child(page);
      PdfvMarkdownEditor *editor = GTK_IS_STACK(stack)
          ? g_object_get_data(G_OBJECT(stack), "markdown-editor")
          : NULL;
      if (editor)
        apply_preferences_to_editor(self, editor);
    }
  }
  if (self->markdown_editor_spare)
    apply_preferences_to_editor(self, self->markdown_editor_spare);
  GAction *remote = g_action_map_lookup_action(
      G_ACTION_MAP(self), "allow-remote-images");
  if (remote)
    g_simple_action_set_state(
        G_SIMPLE_ACTION(remote),
        g_variant_new_boolean(
            pdfv_settings_get_allow_remote_images(self->settings)));
}

static void propagate_markdown_preferences(PdfvWindow *source) {
  GtkApplication *application = gtk_window_get_application(GTK_WINDOW(source));
  for (GList *at = application ? gtk_application_get_windows(application)
                               : NULL;
       at; at = at->next) {
    if (!PDFV_IS_WINDOW(at->data))
      continue;
    PdfvWindow *window = PDFV_WINDOW(at->data);
    if (window != source)
      pdfv_settings_copy(window->settings, source->settings);
    apply_markdown_preferences(window);
  }
  GError *error = NULL;
  if (!pdfv_settings_save(source->settings, &error)) {
    g_warning("Could not save Phi settings: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }
}

static gboolean preferences_update_timeout(gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  self->settings_update_timeout_id = 0;
  propagate_markdown_preferences(self);
  return G_SOURCE_REMOVE;
}

static void schedule_preferences_update(PdfvWindow *self, guint delay_ms) {
  if (self->settings_update_timeout_id)
    g_source_remove(self->settings_update_timeout_id);
  self->settings_update_timeout_id = g_timeout_add_full(
      G_PRIORITY_DEFAULT, delay_ms, preferences_update_timeout,
      g_object_ref(self), g_object_unref);
}

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

  const gchar *pdf_action_names[] = {
      "go-back",        "go-forward",   "zoom-in",   "zoom-out",
      "zoom-reset",     "zoom-fit-width", "zoom-fit-page",
      "invert-colors",  "page-next",    "page-prev",
  };
  for (guint i = 0; i < G_N_ELEMENTS(pdf_action_names); i++) {
    GAction *pdf_action = g_action_map_lookup_action(
        G_ACTION_MAP(self), pdf_action_names[i]);
    if (pdf_action)
      g_simple_action_set_enabled(G_SIMPLE_ACTION(pdf_action), has_document);
  }

  /* Hide floating zoom controls when no document */
  gtk_widget_set_visible(self->zoom_box, has_document);
  if (self->pages_sidebar_page)
    adw_view_stack_page_set_visible(self->pages_sidebar_page, has_document);
  if (!has_document && self->workspace_sidebar_page && self->workspace &&
      adw_view_stack_page_get_visible(self->workspace_sidebar_page))
    adw_view_stack_set_visible_child_name(self->sidebar_stack, "workspace");
  if (!has_document && !self->workspace)
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
  rebuild_main_menu(self);
}

static void update_markdown_actions(PdfvWindow *self) {
  const gchar *names[] = {"save", "toggle-markdown-source"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    GAction *action =
        g_action_map_lookup_action(G_ACTION_MAP(self), names[i]);
    if (action)
      g_simple_action_set_enabled(G_SIMPLE_ACTION(action),
                                  self->current_editor != NULL);
  }
  rebuild_main_menu(self);
}

static void rebuild_main_menu(PdfvWindow *self) {
  if (!self->file_menu_section || !self->zoom_menu_section ||
      !self->view_menu_section)
    return;

  g_menu_remove_all(self->file_menu_section);
  g_menu_append(self->file_menu_section, "Open…", "win.open");
  g_menu_append(self->file_menu_section, "Open Folder…", "win.open-folder");
  g_menu_append(self->file_menu_section, "New Workspace Window",
                "win.new-workspace-window");
  g_menu_append(self->file_menu_section, "New Tab", "win.new-tab");

  gboolean has_pdf = self->current_view &&
      pdfv_document_view_get_document(self->current_view) != NULL;
  g_menu_remove_all(self->zoom_menu_section);
  if (has_pdf) {
    g_menu_append(self->zoom_menu_section, "Zoom In", "win.zoom-in");
    g_menu_append(self->zoom_menu_section, "Zoom Out", "win.zoom-out");
    g_menu_append(self->zoom_menu_section, "Reset Zoom", "win.zoom-reset");
    g_menu_append(self->zoom_menu_section, "Fit Width", "win.zoom-fit-width");
    g_menu_append(self->zoom_menu_section, "Fit Page", "win.zoom-fit-page");
  }

  g_menu_remove_all(self->view_menu_section);
  if (self->current_editor)
    g_menu_append(self->view_menu_section, "Source / Live Preview",
                  "win.toggle-markdown-source");
  if (has_pdf)
    g_menu_append(self->view_menu_section, "Invert Colors",
                  "win.invert-colors");
  g_menu_append(self->view_menu_section, "Fullscreen", "win.fullscreen");
}

static void update_empty_state_chrome(PdfvWindow *self) {
  if (!self->toolbar_view || !self->tab_view)
    return;

  gboolean empty = FALSE;
  AdwTabPage *page = adw_tab_view_get_selected_page(self->tab_view);
  if (page) {
    GtkWidget *content = adw_tab_page_get_child(page);
    if (GTK_IS_STACK(content))
      empty = g_strcmp0(
                  gtk_stack_get_visible_child_name(GTK_STACK(content)),
                  "empty") == 0;
  }

  /* A flat toolbar lets the transparent tab bar share the empty page's
   * background. Restore the normal raised chrome for loading/documents;
   * tab visibility is deliberately never changed here. */
  adw_toolbar_view_set_top_bar_style(
      self->toolbar_view, empty ? ADW_TOOLBAR_FLAT : ADW_TOOLBAR_RAISED);
}

static void on_tab_content_changed(GtkStack *stack, GParamSpec *pspec,
                                   PdfvWindow *self) {
  (void)stack;
  (void)pspec;
  update_empty_state_chrome(self);
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

static void on_markdown_open_file(PdfvMarkdownEditor *editor, GFile *file,
                                  PdfvWindow *self) {
  (void)editor;
  pdfv_window_open_file_internal(self, file, FALSE);
}

static void on_markdown_external_uri(PdfvMarkdownEditor *editor,
                                     const gchar *uri, PdfvWindow *self) {
  if (!uri || !*uri)
    return;
  gchar *scheme = g_uri_parse_scheme(uri);
  gboolean allowed = scheme &&
      (g_ascii_strcasecmp(scheme, "http") == 0 ||
       g_ascii_strcasecmp(scheme, "https") == 0 ||
       g_ascii_strcasecmp(scheme, "mailto") == 0 ||
       g_ascii_strcasecmp(scheme, "obsidian") == 0);
  g_free(scheme);
  if (!allowed) {
    on_markdown_error(editor, "The link uses a blocked URI scheme.", self);
    return;
  }
  GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
  gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
  g_object_unref(launcher);
}

static void on_markdown_error(PdfvMarkdownEditor *editor,
                              const gchar *message, PdfvWindow *self) {
  (void)editor;
  AdwDialog *dialog = adw_alert_dialog_new("Markdown Editor Error", message);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void on_markdown_dirty_changed(PdfvMarkdownEditor *editor,
                                      gboolean dirty, PdfvWindow *self) {
  AdwTabPage *page =
      g_object_get_data(G_OBJECT(editor), "markdown-tab-page");
  if (!page)
    return;
  GIcon *icon = dirty ? g_themed_icon_new("document-save-symbolic") : NULL;
  adw_tab_page_set_indicator_icon(page, icon);
  adw_tab_page_set_indicator_tooltip(page,
                                     dirty ? "Unsaved Markdown changes" : "");
  g_clear_object(&icon);
  if (dirty && page == self->workspace_browse_tab)
    self->workspace_browse_tab = NULL;
  if (dirty && page == self->workspace_preview_tab) {
    self->workspace_preview_tab = NULL;
    g_clear_object(&self->workspace_preview_file);
    workspace_preview_cancel_load(self);
  }
}

static void show_autosave_toast(PdfvWindow *self) {
  if (!self->toast_overlay)
    return;
  AdwToast *toast = adw_toast_new("Phi autosaves your documents!");
  adw_toast_set_timeout(toast, 3);
  adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

static void on_markdown_manual_save(PdfvMarkdownEditor *editor,
                                    PdfvWindow *self) {
  (void)editor;
  show_autosave_toast(self);
}

typedef struct {
  PdfvWindow *window;
  PdfvMarkdownEditor *editor;
} MarkdownConflictData;

static void markdown_conflict_data_free(MarkdownConflictData *data) {
  g_clear_object(&data->window);
  g_clear_object(&data->editor);
  g_free(data);
}

static void on_markdown_reloaded(GObject *source, GAsyncResult *result,
                                 gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GError *error = NULL;
  if (!pdfv_markdown_editor_open_file_finish(PDFV_MARKDOWN_EDITOR(source),
                                             result, &error))
    on_markdown_error(PDFV_MARKDOWN_EDITOR(source), error->message, self);
  g_clear_error(&error);
  g_object_unref(self);
}

static void on_conflict_response(GObject *source, GAsyncResult *result,
                                 gpointer user_data) {
  MarkdownConflictData *data = user_data;
  const gchar *response = adw_alert_dialog_choose_finish(
      ADW_ALERT_DIALOG(source), result);
  if (g_strcmp0(response, "reload") == 0)
    pdfv_markdown_editor_reload_async(data->editor, NULL,
                                      on_markdown_reloaded,
                                      g_object_ref(data->window));
  markdown_conflict_data_free(data);
}

static void on_markdown_conflict(PdfvMarkdownEditor *editor,
                                 PdfvWindow *self) {
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
      "Markdown Changed on Disk",
      "The file changed outside Phi while this editor has unsaved changes."));
  adw_alert_dialog_add_responses(dialog, "keep", "Keep Editor Copy",
                                 "reload", "Reload from Disk", NULL);
  adw_alert_dialog_set_response_appearance(dialog, "reload",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(dialog, "keep");
  adw_alert_dialog_set_close_response(dialog, "keep");
  MarkdownConflictData *data = g_new0(MarkdownConflictData, 1);
  data->window = g_object_ref(self);
  data->editor = g_object_ref(editor);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(self), NULL,
                          on_conflict_response, data);
}

typedef struct {
  PdfvWindow *window;
  PdfvMarkdownEditor *editor;
  GFile *file;
} MarkdownCreateNoteData;

static void markdown_create_note_data_free(MarkdownCreateNoteData *data) {
  g_clear_object(&data->window);
  g_clear_object(&data->editor);
  g_clear_object(&data->file);
  g_free(data);
}

static void on_create_note_response(GObject *source, GAsyncResult *result,
                                    gpointer user_data) {
  MarkdownCreateNoteData *data = user_data;
  const gchar *response = adw_alert_dialog_choose_finish(
      ADW_ALERT_DIALOG(source), result);
  if (g_strcmp0(response, "create") == 0) {
    GError *error = NULL;
    if (!g_file_query_exists(data->file, NULL)) {
      GFile *parent = g_file_get_parent(data->file);
      if (parent &&
          !g_file_make_directory_with_parents(parent, NULL, &error) &&
          !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
        on_markdown_error(data->editor, error->message, data->window);
        g_clear_error(&error);
        g_clear_object(&parent);
        markdown_create_note_data_free(data);
        return;
      }
      g_clear_error(&error);
      GFileOutputStream *stream =
          g_file_create(data->file, G_FILE_CREATE_NONE, NULL, &error);
      if (stream) {
        g_output_stream_close(G_OUTPUT_STREAM(stream), NULL, NULL);
        g_object_unref(stream);
      }
      g_clear_object(&parent);
    }
    if (error)
      on_markdown_error(data->editor, error->message, data->window);
    else
      pdfv_window_open_file_internal(data->window, data->file, FALSE);
    g_clear_error(&error);
  }
  markdown_create_note_data_free(data);
}

static void on_markdown_create_link(PdfvMarkdownEditor *editor,
                                    const gchar *target, PdfvWindow *self) {
  GFile *current = pdfv_markdown_editor_get_file(editor);
  GError *error = NULL;
  GFile *file = pdfv_markdown_editor_resolve_new_note(editor, target, &error);
  if (!file || !current) {
    on_markdown_error(editor, "The note link is not a safe vault path.", self);
    g_clear_error(&error);
    g_clear_object(&file);
    return;
  }

  GFile *root = markdown_vault_root_for_file(self, current);
  gchar *relative = g_file_get_relative_path(root, file);
  if (!file || !relative || g_str_has_prefix(relative, "../")) {
    on_markdown_error(editor, "The note link escapes the active vault.", self);
    g_clear_object(&file);
    g_object_unref(root);
    g_free(relative);
    return;
  }

  gchar *body = g_strdup_printf("Create “%s” and open it?", relative);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(
      adw_alert_dialog_new("Create Linked Note?", body));
  adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "create",
                                 "Create", NULL);
  adw_alert_dialog_set_response_appearance(dialog, "create",
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(dialog, "create");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  MarkdownCreateNoteData *data = g_new0(MarkdownCreateNoteData, 1);
  data->window = g_object_ref(self);
  data->editor = g_object_ref(editor);
  data->file = g_object_ref(file);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(self), NULL,
                          on_create_note_response, data);
  g_free(body);
  g_free(relative);
  g_object_unref(file);
  g_object_unref(root);
}

static void setup_markdown_editor_signals(PdfvWindow *self,
                                          PdfvMarkdownEditor *editor) {
  g_signal_connect(editor, "ready", G_CALLBACK(on_markdown_ready), self);
  g_signal_connect(editor, "open-file", G_CALLBACK(on_markdown_open_file),
                   self);
  g_signal_connect(editor, "open-external-uri",
                   G_CALLBACK(on_markdown_external_uri), self);
  g_signal_connect(editor, "editor-error", G_CALLBACK(on_markdown_error),
                   self);
  g_signal_connect(editor, "dirty-changed",
                   G_CALLBACK(on_markdown_dirty_changed), self);
  g_signal_connect(editor, "conflict", G_CALLBACK(on_markdown_conflict), self);
  g_signal_connect(editor, "create-link", G_CALLBACK(on_markdown_create_link),
                   self);
  g_signal_connect(editor, "manual-save",
                   G_CALLBACK(on_markdown_manual_save), self);
}

static void clear_markdown_editor_spare(PdfvWindow *self) {
  if (self->markdown_editor_prewarm_id) {
    g_source_remove(self->markdown_editor_prewarm_id);
    self->markdown_editor_prewarm_id = 0;
  }
  g_clear_object(&self->markdown_editor_spare);
  g_clear_object(&self->markdown_editor_spare_root);
}

static gboolean create_markdown_editor_spare(gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  self->markdown_editor_prewarm_id = 0;
  if (self->closing_window || !self->markdown_editor_spare_root ||
      self->markdown_editor_spare)
    return G_SOURCE_REMOVE;

  PdfvMarkdownEditor *editor =
      pdfv_markdown_editor_new(self->markdown_editor_spare_root);
  if (!editor)
    return G_SOURCE_REMOVE;
  self->markdown_editor_spare = g_object_ref_sink(editor);
  setup_markdown_editor_signals(self, editor);
  apply_preferences_to_editor(self, editor);
  return G_SOURCE_REMOVE;
}

static void schedule_markdown_editor_prewarm(PdfvWindow *self, GFile *root) {
  if (self->closing_window || !root)
    return;
  if (self->markdown_editor_spare && self->markdown_editor_spare_root &&
      g_file_equal(self->markdown_editor_spare_root, root))
    return;

  clear_markdown_editor_spare(self);
  self->markdown_editor_spare_root = g_object_ref(root);
  self->markdown_editor_prewarm_id = g_idle_add_full(
      G_PRIORITY_LOW, create_markdown_editor_spare, g_object_ref(self),
      g_object_unref);
}

static void on_markdown_ready(PdfvMarkdownEditor *editor, PdfvWindow *self) {
  if (editor == self->markdown_editor_spare ||
      !g_object_get_data(G_OBJECT(editor), "markdown-tab-page"))
    return;
  schedule_markdown_editor_prewarm(
      self, pdfv_markdown_editor_get_vault_root(editor));
}

static gboolean apply_style_after_update(gpointer user_data) {
  apply_markdown_preferences(PDFV_WINDOW(user_data));
  return G_SOURCE_REMOVE;
}

static void on_style_dark_changed(AdwStyleManager *manager, GParamSpec *pspec,
                                  PdfvWindow *self) {
  (void)manager;
  (void)pspec;
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, apply_style_after_update,
                  g_object_ref(self), g_object_unref);
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

static gint workspace_path_compare(gconstpointer a, gconstpointer b) {
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void save_workspace_tree_session(PdfvWindow *self) {
  if (!self->workspace || !self->workspace_expanded_paths)
    return;
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  GHashTableIter iter;
  gpointer key = NULL;
  g_hash_table_iter_init(&iter, self->workspace_expanded_paths);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(paths, g_strdup(key));
  g_ptr_array_sort(paths, workspace_path_compare);
  pdfv_settings_set_workspace_expanded_folders(
      self->settings, pdfv_workspace_get_folder(self->workspace),
      (const gchar *const *)paths->pdata, paths->len);
  GError *error = NULL;
  if (!pdfv_settings_save(self->settings, &error)) {
    g_debug("Could not save workspace tree state: %s",
            error ? error->message : "unknown error");
  }
  g_clear_error(&error);
  g_ptr_array_unref(paths);
}

static void load_workspace_tree_session(PdfvWindow *self, GFile *folder) {
  g_hash_table_remove_all(self->workspace_expanded_paths);
  gsize count = 0;
  gchar **paths = pdfv_settings_dup_workspace_expanded_folders(
      self->settings, folder, &count);
  for (gsize i = 0; paths && i < count; i++) {
    if (paths[i] && *paths[i])
      g_hash_table_add(self->workspace_expanded_paths, g_strdup(paths[i]));
  }
  g_strfreev(paths);
}

static void restore_workspace_tree_session(PdfvWindow *self) {
  if (!self->workspace_tree)
    return;
  /* Expanding an ancestor inserts its children into the flattened model, so
   * re-read the item count on every iteration to restore nested folders too. */
  for (guint position = 0;
       position < g_list_model_get_n_items(G_LIST_MODEL(self->workspace_tree));
       position++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(
        self->workspace_tree, position);
    if (!row)
      continue;
    PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
    if (pdfv_workspace_item_is_folder(item) &&
        g_hash_table_contains(
            self->workspace_expanded_paths,
            pdfv_workspace_item_get_relative_path(item)))
      gtk_tree_list_row_set_expanded(row, TRUE);
    g_object_unref(item);
    g_object_unref(row);
  }
}

static void on_workspace_row_expanded_changed(GtkTreeListRow *row,
                                               GParamSpec *pspec,
                                               PdfvWindow *self) {
  (void)pspec;
  if (!self->workspace)
    return;
  PdfvWorkspaceItem *item = gtk_tree_list_row_get_item(row);
  if (!pdfv_workspace_item_is_folder(item)) {
    g_object_unref(item);
    return;
  }
  const gchar *path = pdfv_workspace_item_get_relative_path(item);
  if (gtk_tree_list_row_get_expanded(row)) {
    g_hash_table_add(self->workspace_expanded_paths, g_strdup(path));
    /* Child rows may have been recreated after their parent was collapsed. */
    restore_workspace_tree_session(self);
  } else {
    g_hash_table_remove(self->workspace_expanded_paths, path);
  }
  g_object_unref(item);
  save_workspace_tree_session(self);
}

static void workspace_set_pdf_icon(GtkImage *image) {
  GIcon *icon = g_content_type_get_symbolic_icon("application/pdf");
  gtk_image_set_from_gicon(image, icon);
  g_object_unref(icon);
}

static void workspace_set_markdown_icon(GtkImage *image) {
  GIcon *icon = g_themed_icon_new("document-edit-symbolic");
  gtk_image_set_from_gicon(image, icon);
  g_object_unref(icon);
}

static void tab_set_document_icon(AdwTabPage *page, gboolean markdown) {
  GIcon *icon = markdown
      ? g_themed_icon_new("document-edit-symbolic")
      : g_content_type_get_symbolic_icon("application/pdf");
  adw_tab_page_set_icon(page, icon);
  g_object_unref(icon);
}

static gboolean file_is_markdown(GFile *file) {
  gchar *basename = g_file_get_basename(file);
  const gchar *dot = strrchr(basename, '.');
  gboolean markdown = dot && g_ascii_strcasecmp(dot, ".md") == 0;
  g_free(basename);
  return markdown;
}

static gboolean file_is_supported_document(GFile *file) {
  if (file_is_markdown(file))
    return TRUE;
  gchar *basename = g_file_get_basename(file);
  const gchar *dot = strrchr(basename, '.');
  gboolean pdf = dot && g_ascii_strcasecmp(dot, ".pdf") == 0;
  g_free(basename);
  return pdf;
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
  else if (file_is_markdown(pdfv_workspace_item_get_file(item)))
    workspace_set_markdown_icon(icon);
  else
    workspace_set_pdf_icon(icon);
  gtk_label_set_text(label, pdfv_workspace_item_get_name(item));
  gtk_widget_set_tooltip_text(GTK_WIDGET(label),
                              pdfv_workspace_item_get_relative_path(item));
  g_object_set_data_full(G_OBJECT(expander), "workspace-file",
                         pdfv_workspace_item_is_folder(item)
                             ? NULL
                             : g_object_ref(pdfv_workspace_item_get_file(item)),
                         g_object_unref);
  if (pdfv_workspace_item_is_folder(item)) {
    gulong handler = g_signal_connect(
        row, "notify::expanded",
        G_CALLBACK(on_workspace_row_expanded_changed), self);
    g_object_set_data(G_OBJECT(list_item), "workspace-expanded-handler",
                      GSIZE_TO_POINTER(handler));
  }
  g_object_unref(item);
}

static void workspace_factory_unbind(GtkSignalListItemFactory *factory,
                                     GtkListItem *list_item,
                                     PdfvWindow *self) {
  (void)factory;
  (void)self;
  GtkTreeExpander *expander =
      GTK_TREE_EXPANDER(gtk_list_item_get_child(list_item));
  GtkTreeListRow *row = gtk_tree_expander_get_list_row(expander);
  gulong handler = GPOINTER_TO_SIZE(g_object_get_data(
      G_OBJECT(list_item), "workspace-expanded-handler"));
  if (row && handler && g_signal_handler_is_connected(row, handler))
    g_signal_handler_disconnect(row, handler);
  g_object_set_data(G_OBJECT(list_item), "workspace-expanded-handler", NULL);
  g_object_set_data(G_OBJECT(expander), "workspace-file", NULL);
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
    workspace_open_file(self, pdfv_workspace_item_get_file(item), FALSE);
  }
  g_object_unref(item);
  g_object_unref(row);
}

static void on_workspace_middle_click(GtkGestureClick *gesture,
                                      gint n_press, gdouble x, gdouble y,
                                      PdfvWindow *self) {
  (void)n_press;
  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) !=
      GDK_BUTTON_MIDDLE)
    return;
  GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self->workspace_list), x, y,
                                      GTK_PICK_DEFAULT);
  for (GtkWidget *at = picked; at && at != GTK_WIDGET(self->workspace_list);
       at = gtk_widget_get_parent(at)) {
    GFile *file = g_object_get_data(G_OBJECT(at), "workspace-file");
    if (!file)
      continue;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    workspace_open_file(self, file, TRUE);
    return;
  }
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

static void workspace_preview_cancel_delay(PdfvWindow *self) {
  if (!self->workspace_preview_delay_id)
    return;
  g_source_remove(self->workspace_preview_delay_id);
  self->workspace_preview_delay_id = 0;
}

static void workspace_preview_cancel_load(PdfvWindow *self) {
  if (self->workspace_preview_cancellable)
    g_cancellable_cancel(self->workspace_preview_cancellable);
  g_clear_object(&self->workspace_preview_cancellable);
  self->workspace_preview_generation++;
}

static gboolean workspace_preview_show_loaded(PdfvWindow *self, GFile *file,
                                              gint page) {
  if (!self->workspace_preview_tab)
    return FALSE;
  GtkWidget *stack = adw_tab_page_get_child(self->workspace_preview_tab);
  GFile *loaded_file = g_object_get_data(G_OBJECT(stack), "document-file");
  PdfvDocumentView *view =
      g_object_get_data(G_OBJECT(stack), "document-view");
  if (!loaded_file || !g_file_equal(loaded_file, file) ||
      !pdfv_document_view_get_document(view))
    return FALSE;

  /* A request for another file may have hidden this already-loaded document.
   * Cancel it and restore the document without doing any PDF work. */
  GCancellable *opening =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (opening)
    g_cancellable_cancel(opening);
  g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "document");
  pdfv_document_view_go_to_page(view, page);
  adw_tab_view_set_selected_page(self->tab_view, self->workspace_preview_tab);
  self->current_view = view;
  update_navigation_buttons(self);
  update_zoom_info(self);
  update_sidebar_button(self);
  return TRUE;
}

static void workspace_preview_selected_now(PdfvWindow *self) {
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

  if (file_is_markdown(group->file)) {
    workspace_preview_cancel_load(self);
    g_set_object(&self->workspace_preview_file, group->file);
    open_markdown_in_tab_async(self, group->file,
                               self->workspace_preview_tab);
    adw_tab_view_set_selected_page(self->tab_view,
                                   self->workspace_preview_tab);
    return;
  }

  if (workspace_preview_show_loaded(self, group->file, match->page))
    return;

  GtkWidget *stack = adw_tab_page_get_child(self->workspace_preview_tab);
  GCancellable *opening =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (self->workspace_preview_file &&
      g_file_equal(self->workspace_preview_file, group->file) && opening &&
      !g_cancellable_is_cancelled(opening))
    return;

  g_set_object(&self->workspace_preview_file, group->file);
  workspace_preview_cancel_load(self);
  self->workspace_preview_cancellable = g_cancellable_new();

  PhiDocument *cached =
      workspace_document_cache_lookup(self, group->file);
  if (cached) {
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
    g_clear_object(&self->workspace_preview_cancellable);
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

static gboolean workspace_preview_delay_elapsed(gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  self->workspace_preview_delay_id = 0;
  if (gtk_widget_get_visible(self->workspace_search_overlay))
    workspace_preview_selected_now(self);
  return G_SOURCE_REMOVE;
}

static void workspace_preview_selected(PdfvWindow *self) {
  PdfvWorkspaceResultGroup *group = workspace_selected_group(self);
  PdfvWorkspaceMatch *match = workspace_selected_match(self);
  if (!group || !match)
    return;
  /* Markdown results open on activation. Creating editable WebViews while the
   * user only arrows through search results would be wasteful and surprising. */
  if (file_is_markdown(group->file))
    return;

  self->workspace_preview_page = match->page;
  gboolean target_changed =
      !self->workspace_preview_file ||
      !g_file_equal(self->workspace_preview_file, group->file);
  if (target_changed) {
    workspace_preview_cancel_load(self);
    g_set_object(&self->workspace_preview_file, group->file);
  }

  workspace_preview_cancel_delay(self);
  if (workspace_preview_show_loaded(self, group->file, match->page))
    return;

  if (target_changed &&
      adw_tab_view_get_selected_page(self->tab_view) ==
          self->workspace_preview_tab &&
      self->workspace_return_tab &&
      adw_tab_view_get_page_position(self->tab_view,
                                     self->workspace_return_tab) >= 0)
    adw_tab_view_set_selected_page(self->tab_view,
                                   self->workspace_return_tab);

  if (self->workspace_preview_tab) {
    GtkWidget *stack = adw_tab_page_get_child(self->workspace_preview_tab);
    GCancellable *opening =
        g_object_get_data(G_OBJECT(stack), "open-cancellable");
    if (!target_changed && opening &&
        !g_cancellable_is_cancelled(opening)) {
      g_object_set_data(G_OBJECT(stack), "open-target-page",
                        GINT_TO_POINTER(match->page + 1));
      return;
    }
  }

  self->workspace_preview_delay_id = g_timeout_add_full(
      G_PRIORITY_DEFAULT_IDLE, WORKSPACE_PREVIEW_DELAY_MS,
      workspace_preview_delay_elapsed, self, NULL);
}

static void workspace_preview_flush(PdfvWindow *self) {
  workspace_preview_cancel_delay(self);
  workspace_preview_selected_now(self);
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
  workspace_preview_cancel_delay(self);
  workspace_preview_cancel_load(self);
  g_clear_object(&self->workspace_preview_file);
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
  if (file_is_markdown(group->file))
    workspace_set_markdown_icon(GTK_IMAGE(icon));
  else
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
      gchar *title = file_is_markdown(group->file)
                         ? g_strdup("Markdown match")
                         : g_strdup_printf("Page %d", match->page + 1);
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
  guint total = pdfv_workspace_get_document_count(self->workspace);
  gchar *status = total > indexed
                      ? g_strdup_printf("Indexing %u of %u documents…",
                                        indexed, total)
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
  if (commit && workspace_selected_match(self))
    workspace_preview_flush(self);
  else
    workspace_preview_cancel_delay(self);
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
    workspace_preview_cancel_load(self);
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
                                  "Open a PDF or Markdown file");

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
  adw_status_page_set_title(ADW_STATUS_PAGE(loading), "Loading…");
  GtkWidget *loading_spinner = adw_spinner_new();
  gtk_widget_set_size_request(loading_spinner, 32, 32);
  adw_status_page_set_child(ADW_STATUS_PAGE(loading), loading_spinner);
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
  pdfv_document_view_capture_zoom_scroll(view, stack);

  /* Start with empty state */
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
  g_signal_connect(stack, "notify::visible-child-name",
                   G_CALLBACK(on_tab_content_changed), self);

  g_object_set_data(G_OBJECT(stack), "document-view", view);
  g_object_set_data(G_OBJECT(stack), "scrolled-window", scrolled);

  return stack;
}

static gboolean prepare_tab_for_open(PdfvWindow *self, AdwTabPage *page) {
  GtkWidget *stack = adw_tab_page_get_child(page);
  PdfvMarkdownEditor *editor =
      g_object_get_data(G_OBJECT(stack), "markdown-editor");
  if (editor && pdfv_markdown_editor_get_dirty(editor))
    return FALSE;

  GCancellable *opening =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (opening)
    g_cancellable_cancel(opening);
  g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
  g_object_set_data(G_OBJECT(stack), "document-file", NULL);
  if (editor) {
    g_signal_handlers_disconnect_by_data(editor, self);
    g_object_set_data(G_OBJECT(editor), "markdown-tab-page", NULL);
    if (self->current_editor == editor)
      self->current_editor = NULL;
    g_object_set_data(G_OBJECT(stack), "markdown-editor", NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
    gtk_stack_remove(GTK_STACK(stack), GTK_WIDGET(editor));
  }
  adw_tab_page_set_indicator_icon(page, NULL);
  adw_tab_page_set_indicator_tooltip(page, "");
  return TRUE;
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

typedef struct {
  OpenRequest *request;
  PhiDocument *document;
  GError *error;
} OpenCompletion;

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

static gboolean finish_document_load_idle(gpointer user_data) {
  OpenCompletion *completion = user_data;
  OpenRequest *request = completion->request;
  PdfvWindow *self = request->window;
  PhiDocument *document = completion->document;
  GError *error = completion->error;
  gboolean page_is_open =
      adw_tab_view_get_page_position(self->tab_view, request->page) >= 0;
  gboolean active_preview =
      request->preview && request->page == self->workspace_preview_tab;
  gboolean stale_preview =
      active_preview &&
      request->generation != self->workspace_preview_generation;
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
    if (request->preview &&
        self->workspace_preview_cancellable == request->cancellable)
      g_clear_object(&self->workspace_preview_cancellable);
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    PdfvDocumentView *view =
        g_object_get_data(G_OBJECT(stack), "document-view");
    pdfv_document_view_set_document(view, document);
    gpointer target_page_data =
        g_object_get_data(G_OBJECT(stack), "open-target-page");
    gint target_page = target_page_data
                           ? GPOINTER_TO_INT(target_page_data) - 1
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

    if (active_preview)
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
    if (request->preview &&
        self->workspace_preview_cancellable == request->cancellable)
      g_clear_object(&self->workspace_preview_cancellable);
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    g_object_set_data(G_OBJECT(stack), "document-file", NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    AdwDialog *dialog = adw_alert_dialog_new("Error Opening File",
                                             error->message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(self));
  }

  g_clear_error(&error);
  g_clear_object(&document);
  open_request_free(request);
  g_free(completion);
  return G_SOURCE_REMOVE;
}

static void on_document_loaded(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  (void)source;
  OpenCompletion *completion = g_new0(OpenCompletion, 1);
  completion->request = user_data;
  completion->document =
      pdfv_workspace_load_document_finish(result, &completion->error);

  /* Installing a long document still performs layout bookkeeping on the main
   * thread. Do it only when input is idle so arrow-key selection always wins
   * over a preview completion that became ready at the same moment. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, finish_document_load_idle,
                  completion, NULL);
}

static void open_file_in_tab_async(PdfvWindow *self, GFile *file,
                                   AdwTabPage *page, gint target_page,
                                   gboolean preview, gboolean fit_width,
                                   guint generation) {
  GtkWidget *stack = adw_tab_page_get_child(page);
  if (!prepare_tab_for_open(self, page))
    return;
  g_object_set_data_full(G_OBJECT(stack), "document-file",
                         g_object_ref(file), g_object_unref);
  gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
  tab_set_document_icon(page, FALSE);
  gchar *basename = g_file_get_basename(file);
  adw_tab_page_set_title(page, basename);
  g_free(basename);
  g_object_set_data(G_OBJECT(stack), "open-target-page",
                    GINT_TO_POINTER(target_page + 1));

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

typedef struct {
  PdfvWindow *window;
  AdwTabPage *page;
  GFile *file;
  PdfvMarkdownEditor *editor;
  GCancellable *cancellable;
  gchar *fragment;
} MarkdownOpenRequest;

static void markdown_open_request_free(MarkdownOpenRequest *request) {
  g_clear_object(&request->window);
  g_clear_object(&request->page);
  g_clear_object(&request->file);
  g_clear_object(&request->editor);
  g_clear_object(&request->cancellable);
  g_free(request->fragment);
  g_free(request);
}

static void on_markdown_opened(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  MarkdownOpenRequest *request = user_data;
  PdfvWindow *self = request->window;
  GError *error = NULL;
  gboolean opened = pdfv_markdown_editor_open_file_finish(
      PDFV_MARKDOWN_EDITOR(source), result, &error);
  gboolean page_is_open =
      adw_tab_view_get_page_position(self->tab_view, request->page) >= 0;
  GtkWidget *stack = page_is_open
      ? adw_tab_page_get_child(request->page) : NULL;
  gboolean current_request = stack &&
      g_object_get_data(G_OBJECT(stack), "open-cancellable") ==
          request->cancellable &&
      g_object_get_data(G_OBJECT(stack), "markdown-editor") ==
          request->editor;
  if (opened && current_request) {
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "markdown");
    g_object_set_data_full(G_OBJECT(stack), "document-file",
                           g_object_ref(request->file), g_object_unref);
    gchar *basename = g_file_get_basename(request->file);
    adw_tab_page_set_title(request->page, basename);
    g_free(basename);
    if (adw_tab_view_get_selected_page(self->tab_view) == request->page) {
      self->current_editor = request->editor;
      self->current_view = NULL;
      populate_thumbnails(self, NULL);
      update_navigation_buttons(self);
      update_zoom_info(self);
      update_sidebar_button(self);
      update_markdown_actions(self);
      pdfv_markdown_editor_focus(request->editor);
      if (request->fragment)
        pdfv_markdown_editor_reveal_fragment(request->editor,
                                             request->fragment);
    }
    if (pdfv_markdown_editor_get_ready(request->editor))
      schedule_markdown_editor_prewarm(
          self, pdfv_markdown_editor_get_vault_root(request->editor));
  } else if (error && current_request &&
             !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    g_object_set_data(G_OBJECT(stack), "document-file", NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "empty");
    on_markdown_error(request->editor, error->message, self);
  }
  g_clear_error(&error);
  markdown_open_request_free(request);
}

static GFile *markdown_vault_root_for_file(PdfvWindow *self, GFile *file) {
  GFile *cursor = g_file_get_parent(file);
  while (cursor) {
    GFile *metadata = g_file_get_child(cursor, ".obsidian");
    gboolean is_vault =
        g_file_query_file_type(metadata, G_FILE_QUERY_INFO_NONE, NULL) ==
        G_FILE_TYPE_DIRECTORY;
    g_object_unref(metadata);
    if (is_vault)
      return cursor;
    GFile *parent = g_file_get_parent(cursor);
    g_object_unref(cursor);
    cursor = parent;
  }
  if (self->workspace) {
    GFile *root = pdfv_workspace_get_folder(self->workspace);
    gchar *relative = g_file_get_relative_path(root, file);
    if (relative) {
      g_free(relative);
      return g_object_ref(root);
    }
  }
  GFile *parent = g_file_get_parent(file);
  return parent ? parent : g_object_ref(file);
}

static void open_markdown_in_tab_async(PdfvWindow *self, GFile *file,
                                       AdwTabPage *page) {
  GtkWidget *stack = adw_tab_page_get_child(page);
  GFile *root = markdown_vault_root_for_file(self, file);
  PdfvMarkdownEditor *editor =
      g_object_get_data(G_OBJECT(stack), "markdown-editor");
  gboolean reuse = editor && !pdfv_markdown_editor_get_dirty(editor) &&
      g_file_equal(pdfv_markdown_editor_get_vault_root(editor), root);
  gboolean use_spare = !reuse && self->markdown_editor_spare &&
      self->markdown_editor_spare_root &&
      g_file_equal(self->markdown_editor_spare_root, root);
  if (reuse) {
    GCancellable *opening =
        g_object_get_data(G_OBJECT(stack), "open-cancellable");
    if (opening)
      g_cancellable_cancel(opening);
    g_object_set_data(G_OBJECT(stack), "open-cancellable", NULL);
    g_object_set_data(G_OBJECT(stack), "document-file", NULL);
  } else if (!prepare_tab_for_open(self, page)) {
    g_object_unref(root);
    return;
  }
  g_object_set_data_full(G_OBJECT(stack), "document-file",
                         g_object_ref(file), g_object_unref);
  if (!reuse)
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "loading");
  tab_set_document_icon(page, TRUE);
  gchar *basename = g_file_get_basename(file);
  adw_tab_page_set_title(page, basename);
  g_free(basename);

  if (!reuse) {
    if (use_spare) {
      editor = g_steal_pointer(&self->markdown_editor_spare);
      g_clear_object(&self->markdown_editor_spare_root);
    } else {
      editor = pdfv_markdown_editor_new(root);
      setup_markdown_editor_signals(self, editor);
    }
    gtk_stack_add_named(GTK_STACK(stack), GTK_WIDGET(editor), "markdown");
    g_object_set_data(G_OBJECT(stack), "markdown-editor", editor);
    g_object_set_data(G_OBJECT(editor), "markdown-tab-page", page);
    if (use_spare)
      g_object_unref(editor);
  }
  g_object_unref(root);
  apply_preferences_to_editor(self, editor);

  MarkdownOpenRequest *request = g_new0(MarkdownOpenRequest, 1);
  request->window = g_object_ref(self);
  request->page = g_object_ref(page);
  request->file = g_object_ref(file);
  request->editor = g_object_ref(editor);
  request->cancellable = g_cancellable_new();
  request->fragment = g_strdup(g_object_get_data(
      G_OBJECT(file), "markdown-link-target"));
  g_object_set_data_full(G_OBJECT(stack), "open-cancellable",
                         g_object_ref(request->cancellable), g_object_unref);
  pdfv_markdown_editor_open_file_async(editor, file, request->cancellable,
                                       on_markdown_opened, request);
}

static void update_window_title(PdfvWindow *self) {
  const gchar *title = self->window_title_page
      ? adw_tab_page_get_title(self->window_title_page) : NULL;
  gtk_window_set_title(GTK_WINDOW(self), title && *title
      ? title : "Phi Document Viewer");
}

static void on_selected_tab_title_changed(AdwTabPage *page,
                                          GParamSpec *pspec,
                                          PdfvWindow *self) {
  (void)page;
  (void)pspec;
  update_window_title(self);
}

static void bind_window_title(PdfvWindow *self, AdwTabPage *page) {
  if (self->window_title_page == page) {
    update_window_title(self);
    return;
  }
  if (self->window_title_page)
    g_signal_handlers_disconnect_by_func(
        self->window_title_page, on_selected_tab_title_changed, self);
  g_set_object(&self->window_title_page, page);
  if (page)
    g_signal_connect(page, "notify::title",
                     G_CALLBACK(on_selected_tab_title_changed), self);
  update_window_title(self);
}

static void on_tab_selected(AdwTabView *tab_view, GParamSpec *pspec,
                            PdfvWindow *self) {
  (void)pspec;

  AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
  bind_window_title(self, page);
  update_empty_state_chrome(self);

  if (!page) {
    self->current_view = NULL;
    self->current_editor = NULL;
    update_navigation_buttons(self);
    update_zoom_info(self);
    update_sidebar_button(self);
    update_markdown_actions(self);
    if (!self->workspace)
      adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
    return;
  }

  GtkWidget *stack = adw_tab_page_get_child(page);
  self->current_view = g_object_get_data(G_OBJECT(stack), "document-view");
  self->current_editor =
      g_object_get_data(G_OBJECT(stack), "markdown-editor");
  if (self->current_editor)
    self->current_view = NULL;

  update_navigation_buttons(self);
  update_zoom_info(self);
  update_sidebar_button(self);
  update_markdown_actions(self);

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
  if (self->current_editor) {
    gtk_search_bar_set_search_mode(self->search_bar, FALSE);
    gtk_label_set_text(self->search_status, "");
    apply_preferences_to_editor(self, self->current_editor);
  }
}

typedef struct {
  PdfvWindow *window;
  AdwTabView *tab_view;
  AdwTabPage *page;
  PdfvMarkdownEditor *editor;
} MarkdownCloseRequest;

static gboolean continue_window_close_idle(gpointer user_data) {
  gtk_window_close(GTK_WINDOW(user_data));
  return G_SOURCE_REMOVE;
}

static void markdown_close_request_free(MarkdownCloseRequest *request) {
  g_clear_object(&request->window);
  g_clear_object(&request->tab_view);
  g_clear_object(&request->page);
  g_clear_object(&request->editor);
  g_free(request);
}

static void finish_markdown_close(MarkdownCloseRequest *request,
                                  gboolean close_page) {
  PdfvWindow *self = request->window;
  g_object_set_data(G_OBJECT(request->page), "markdown-close-pending", NULL);
  if (!close_page) {
    self->closing_window = FALSE;
  } else {
    g_signal_handlers_disconnect_by_data(request->editor, self);
    if (self->current_editor == request->editor)
      self->current_editor = NULL;
  }
  adw_tab_view_close_page_finish(request->tab_view, request->page, close_page);
  gboolean continue_window_close = close_page && self->closing_window;
  gboolean last_page = close_page &&
                       adw_tab_view_get_n_pages(request->tab_view) == 0;
  if (continue_window_close || last_page)
    g_object_ref(self);
  markdown_close_request_free(request);
  if (continue_window_close || last_page) {
    gtk_window_close(GTK_WINDOW(self));
    g_object_unref(self);
  }
}

static void on_close_flush_done(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
  MarkdownCloseRequest *request = user_data;
  GError *error = NULL;
  gboolean flushed = pdfv_markdown_editor_flush_finish(
      PDFV_MARKDOWN_EDITOR(source), result, &error);
  if (!flushed && error &&
      !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED)) {
    on_markdown_error(request->editor, error->message, request->window);
    finish_markdown_close(request, FALSE);
  } else {
    finish_markdown_close(request, TRUE);
  }
  g_clear_error(&error);
}

static void on_close_save_done(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  MarkdownCloseRequest *request = user_data;
  GError *error = NULL;
  if (!pdfv_markdown_editor_save_finish(PDFV_MARKDOWN_EDITOR(source), result,
                                        &error)) {
    on_markdown_error(request->editor, error->message, request->window);
    finish_markdown_close(request, FALSE);
  } else {
    finish_markdown_close(request, TRUE);
  }
  g_clear_error(&error);
}

static gboolean on_tab_close_page(AdwTabView *tab_view, AdwTabPage *page,
                                  PdfvWindow *self) {
  if (page == self->workspace_preview_tab) {
    workspace_preview_cancel_delay(self);
    workspace_preview_cancel_load(self);
    self->workspace_preview_tab = NULL;
    g_clear_object(&self->workspace_preview_file);
  }
  if (page == self->workspace_return_tab)
    g_clear_object(&self->workspace_return_tab);
  if (page == self->workspace_browse_tab)
    self->workspace_browse_tab = NULL;

  /* Clear current_view if we're closing its tab */
  GtkWidget *stack = adw_tab_page_get_child(page);
  GCancellable *open_cancellable =
      g_object_get_data(G_OBJECT(stack), "open-cancellable");
  if (open_cancellable)
    g_cancellable_cancel(open_cancellable);
  PdfvDocumentView *view = g_object_get_data(G_OBJECT(stack), "document-view");
  PdfvMarkdownEditor *editor =
      g_object_get_data(G_OBJECT(stack), "markdown-editor");
  if (editor) {
    if (g_object_get_data(G_OBJECT(page), "markdown-close-pending"))
      return GDK_EVENT_STOP;
    g_object_set_data(G_OBJECT(page), "markdown-close-pending",
                      GINT_TO_POINTER(1));
    MarkdownCloseRequest *request = g_new0(MarkdownCloseRequest, 1);
    request->window = g_object_ref(self);
    request->tab_view = g_object_ref(tab_view);
    request->page = g_object_ref(page);
    request->editor = g_object_ref(editor);
    if (pdfv_markdown_editor_get_dirty(editor)) {
      pdfv_markdown_editor_save_async(editor, NULL, on_close_save_done,
                                      request);
    } else {
      pdfv_markdown_editor_flush_async(editor, NULL, on_close_flush_done,
                                       request);
    }
    return GDK_EVENT_STOP;
  }
  if (view == self->current_view) {
    /* Disconnect signals before destruction */
    g_signal_handlers_disconnect_by_data(view, self);
    self->current_view = NULL;
  }

  adw_tab_view_close_page_finish(tab_view, page, TRUE);
  if (self->closing_window || adw_tab_view_get_n_pages(tab_view) == 0)
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, continue_window_close_idle,
                    g_object_ref(self), g_object_unref);
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
  gtk_file_dialog_set_title(dialog, "Open Document");

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "PDF and Markdown Documents");
  gtk_file_filter_add_mime_type(filter, "application/pdf");
  gtk_file_filter_add_mime_type(filter, "text/markdown");
  gtk_file_filter_add_pattern(filter, "*.pdf");
  gtk_file_filter_add_pattern(filter, "*.md");

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

static gchar *workspace_page_relative_path(PdfvWindow *self,
                                           AdwTabPage *page,
                                           GFile *folder) {
  if (!page || page == self->workspace_preview_tab)
    return NULL;
  GtkWidget *stack = adw_tab_page_get_child(page);
  if (!GTK_IS_STACK(stack))
    return NULL;
  GFile *file = g_object_get_data(G_OBJECT(stack), "document-file");
  if (!file || !file_is_supported_document(file))
    return NULL;
  gchar *relative = g_file_get_relative_path(folder, file);
  if (!relative || !*relative) {
    g_free(relative);
    return NULL;
  }
  return relative;
}

static void save_workspace_tab_session(PdfvWindow *self) {
  if (!self->workspace)
    return;
  GFile *folder = pdfv_workspace_get_folder(self->workspace);
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  AdwTabPage *selected = adw_tab_view_get_selected_page(self->tab_view);
  gchar *active = NULL;
  guint pages = adw_tab_view_get_n_pages(self->tab_view);
  for (guint i = 0; i < pages; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(self->tab_view, i);
    gchar *relative = workspace_page_relative_path(self, page, folder);
    if (!relative)
      continue;
    if (page == selected)
      active = g_strdup(relative);
    g_ptr_array_add(paths, relative);
  }
  pdfv_settings_set_workspace_tabs(
      self->settings, folder, (const gchar *const *)paths->pdata,
      paths->len, active);
  GError *error = NULL;
  if (!pdfv_settings_save(self->settings, &error)) {
    g_debug("Could not save workspace tabs: %s",
            error ? error->message : "unknown error");
  }
  g_clear_error(&error);
  g_free(active);
  g_ptr_array_unref(paths);
}

static AdwTabPage *find_open_document_page(PdfvWindow *self, GFile *file) {
  guint pages = adw_tab_view_get_n_pages(self->tab_view);
  for (guint i = 0; i < pages; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page(self->tab_view, i);
    GtkWidget *stack = adw_tab_page_get_child(page);
    GFile *open_file = GTK_IS_STACK(stack)
        ? g_object_get_data(G_OBJECT(stack), "document-file") : NULL;
    if (open_file && g_file_equal(open_file, file))
      return page;
  }
  return NULL;
}

static void restore_workspace_tab_session(PdfvWindow *self, GFile *folder) {
  gsize count = 0;
  gchar **paths = pdfv_settings_dup_workspace_open_tabs(
      self->settings, folder, &count);
  if (!paths || count == 0) {
    g_strfreev(paths);
    return;
  }
  gchar *active = pdfv_settings_dup_workspace_active_tab(
      self->settings, folder);
  GHashTable *seen = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, NULL);
  AdwTabPage *active_page = NULL;
  for (gsize i = 0; i < count; i++) {
    if (!paths[i] || !*paths[i] || g_hash_table_contains(seen, paths[i]))
      continue;
    GFile *file = g_file_resolve_relative_path(folder, paths[i]);
    gchar *relative = g_file_get_relative_path(folder, file);
    gboolean valid = relative && *relative &&
        g_strcmp0(relative, paths[i]) == 0 &&
        file_is_supported_document(file) &&
        g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) ==
            G_FILE_TYPE_REGULAR;
    if (valid) {
      g_hash_table_add(seen, g_strdup(relative));
      AdwTabPage *page = find_open_document_page(self, file);
      if (!page)
        page = workspace_open_file(self, file, TRUE);
      if (page && g_strcmp0(relative, active) == 0)
        active_page = page;
    }
    g_free(relative);
    g_object_unref(file);
  }
  /* Restored pages are pinned. The next primary workspace click gets a new,
   * replaceable browse tab instead of replacing a restored document. */
  self->workspace_browse_tab = NULL;
  if (active_page)
    adw_tab_view_set_selected_page(self->tab_view, active_page);
  g_hash_table_unref(seen);
  g_free(active);
  g_strfreev(paths);
}

static void close_workspace(PdfvWindow *self, gboolean forget) {
  save_workspace_tab_session(self);
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
  g_hash_table_remove_all(self->workspace_expanded_paths);
  g_clear_object(&self->workspace_scan_cancellable);
  g_clear_object(&self->workspace_search_cancellable);
  g_clear_object(&self->workspace_preview_cancellable);
  g_clear_object(&self->workspace_preview_file);
  self->workspace_browse_tab = NULL;
  workspace_document_cache_clear(self);
  self->workspace_search_running = FALSE;
  self->workspace_index_dirty = FALSE;
  self->workspace_suppress_preview = FALSE;
  gtk_widget_set_visible(self->workspace_loading_spinner, FALSE);

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
  apply_markdown_preferences(self);

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
    gtk_widget_set_visible(self->workspace_loading_spinner, FALSE);
    gtk_stack_set_visible_child_name(self->workspace_content_stack, "files");
    load_workspace_tree_session(self,
                                pdfv_workspace_get_folder(workspace));
    g_clear_object(&self->workspace_tree);
    self->workspace_tree = gtk_tree_list_model_new(
        g_object_ref(pdfv_workspace_get_items(workspace)), FALSE, FALSE,
        workspace_create_children, self, NULL);
    restore_workspace_tree_session(self);
    gtk_single_selection_set_model(
        self->workspace_selection, G_LIST_MODEL(self->workspace_tree));
    adw_view_stack_page_set_visible(self->workspace_sidebar_page, TRUE);
    adw_view_stack_set_visible_child_name(self->sidebar_stack, "workspace");
    adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
    remember_workspace(pdfv_workspace_get_folder(workspace));
    apply_markdown_preferences(self);
    update_sidebar_button(self);
  } else if (workspace == self->workspace && error &&
             !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    gtk_widget_set_visible(self->workspace_loading_spinner, FALSE);
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
                                  "Scanning for PDF and Markdown files");
  gtk_widget_set_visible(self->workspace_loading_spinner, TRUE);
  gtk_stack_set_visible_child_name(self->workspace_content_stack, "loading");
  adw_view_stack_page_set_visible(self->workspace_sidebar_page, TRUE);
  adw_view_stack_set_visible_child_name(self->sidebar_stack, "workspace");
  adw_overlay_split_view_set_show_sidebar(self->split_view, TRUE);
  pdfv_workspace_load_async(self->workspace, self->workspace_scan_cancellable,
                            on_workspace_loaded, g_object_ref(self));
  restore_workspace_tab_session(self, folder);
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

static void action_new_workspace_window(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GtkApplication *application = gtk_window_get_application(GTK_WINDOW(self));
  if (!application)
    return;
  PdfvWindow *window = pdfv_window_new(ADW_APPLICATION(application));
  pdfv_window_restore_last_workspace(window);
  gtk_window_present(GTK_WINDOW(window));
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

static void on_window_markdown_saved(GObject *source, GAsyncResult *result,
                                     gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  GError *error = NULL;
  if (!pdfv_markdown_editor_save_finish(PDFV_MARKDOWN_EDITOR(source), result,
                                        &error))
    on_markdown_error(PDFV_MARKDOWN_EDITOR(source), error->message, self);
  else
    show_autosave_toast(self);
  g_clear_error(&error);
  g_object_unref(self);
}

static void action_save(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_editor)
    pdfv_markdown_editor_save_async(self->current_editor, NULL,
                                    on_window_markdown_saved,
                                    g_object_ref(self));
}

static void action_toggle_markdown_source(GSimpleAction *action,
                                          GVariant *parameter,
                                          gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_editor)
    pdfv_markdown_editor_run_command(self->current_editor,
                                     "editor.toggleSourceMode");
}

static void action_allow_remote_images(GSimpleAction *action,
                                       GVariant *value,
                                       gpointer user_data) {
  PdfvWindow *self = PDFV_WINDOW(user_data);
  gboolean allowed = g_variant_get_boolean(value);
  g_simple_action_set_state(action, value);
  pdfv_settings_set_allow_remote_images(self->settings, allowed);
  propagate_markdown_preferences(self);
}

static void on_preferences_font_changed(AdwSpinRow *row,
                                        GParamSpec *pspec,
                                        PdfvWindow *self) {
  (void)pspec;
  pdfv_settings_set_markdown_font_scale(
      self->settings, adw_spin_row_get_value(row) / 16.0);
  schedule_preferences_update(self, 80);
}

static void on_preferences_line_width_changed(AdwSwitchRow *row,
                                              GParamSpec *pspec,
                                              PdfvWindow *self) {
  (void)pspec;
  pdfv_settings_set_readable_line_width(
      self->settings, adw_switch_row_get_active(row));
  schedule_preferences_update(self, 80);
}

static void on_preferences_remote_changed(AdwSwitchRow *row,
                                          GParamSpec *pspec,
                                          PdfvWindow *self) {
  (void)pspec;
  pdfv_settings_set_allow_remote_images(
      self->settings, adw_switch_row_get_active(row));
  schedule_preferences_update(self, 80);
}

static void on_preferences_latex_conceal_changed(AdwSwitchRow *row,
                                                 GParamSpec *pspec,
                                                 PdfvWindow *self) {
  (void)pspec;
  pdfv_settings_set_latex_conceal(
      self->settings, adw_switch_row_get_active(row));
  schedule_preferences_update(self, 40);
}

static void on_preferences_snippets_changed(GtkTextBuffer *buffer,
                                            PdfvWindow *self) {
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  pdfv_settings_set_latex_snippets(self->settings, text);
  g_free(text);
  schedule_preferences_update(self, 450);
}

typedef struct {
  PdfvWindow *window;
  GtkTextBuffer *buffer;
} SnippetResetData;

static void snippet_reset_data_free(SnippetResetData *data) {
  g_clear_object(&data->window);
  g_clear_object(&data->buffer);
  g_free(data);
}

static void on_reset_snippets_confirmed(GObject *source,
                                        GAsyncResult *result,
                                        gpointer user_data) {
  SnippetResetData *data = user_data;
  const gchar *response = adw_alert_dialog_choose_finish(
      ADW_ALERT_DIALOG(source), result);
  if (g_strcmp0(response, "reset") == 0) {
    GError *error = NULL;
    gchar *defaults =
        pdfv_markdown_resource_scheme_load_default_snippets(&error);
    if (!defaults) {
      on_markdown_error(NULL, error ? error->message
                                    : "Could not load the default snippets",
                        data->window);
    } else {
      g_signal_handlers_block_by_func(
          data->buffer, on_preferences_snippets_changed, data->window);
      gtk_text_buffer_set_text(data->buffer, defaults, -1);
      g_signal_handlers_unblock_by_func(
          data->buffer, on_preferences_snippets_changed, data->window);
      pdfv_settings_set_latex_snippets(data->window->settings, "");
      propagate_markdown_preferences(data->window);
    }
    g_free(defaults);
    g_clear_error(&error);
  }
  snippet_reset_data_free(data);
}

static void on_reset_snippets_clicked(GtkButton *button,
                                      PdfvWindow *self) {
  GtkTextBuffer *buffer =
      g_object_get_data(G_OBJECT(button), "snippets-buffer");
  if (!buffer)
    return;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
      "Reset LaTeX Snippets?",
      "Your custom snippet set will be replaced by Phi’s built-in defaults."));
  adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "reset",
                                 "Reset", NULL);
  adw_alert_dialog_set_response_appearance(dialog, "reset",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(dialog, "cancel");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  SnippetResetData *data = g_new0(SnippetResetData, 1);
  data->window = g_object_ref(self);
  data->buffer = g_object_ref(buffer);
  adw_alert_dialog_choose(dialog, GTK_WIDGET(self), NULL,
                          on_reset_snippets_confirmed, data);
}

static gchar *attachment_folder_label(GFile *workspace, GFile *folder) {
  if (!workspace || !folder)
    return g_strdup("Same folder as the Markdown file");
  if (g_file_equal(workspace, folder))
    return g_strdup("Workspace root");
  gchar *relative = g_file_get_relative_path(workspace, folder);
  if (relative)
    return relative;
  return g_file_get_parse_name(folder);
}

static void on_attachment_mode_changed(AdwSwitchRow *row,
                                       GParamSpec *pspec,
                                       PdfvWindow *self) {
  (void)pspec;
  if (!self->workspace)
    return;
  GFile *workspace = pdfv_workspace_get_folder(self->workspace);
  gboolean fixed = adw_switch_row_get_active(row);
  gchar *uri = pdfv_settings_dup_workspace_attachment_folder_uri(
      self->settings, workspace);
  if (fixed && (!uri || !*uri)) {
    g_free(uri);
    uri = g_file_get_uri(workspace);
  }
  pdfv_settings_set_workspace_attachment_policy(self->settings, workspace,
                                                 fixed, uri);
  g_free(uri);
  schedule_preferences_update(self, 40);
}

typedef struct {
  PdfvWindow *window;
  AdwSwitchRow *mode;
  AdwActionRow *folder_row;
} AttachmentFolderRequest;

static void attachment_folder_request_free(AttachmentFolderRequest *data) {
  g_clear_object(&data->window);
  g_clear_object(&data->mode);
  g_clear_object(&data->folder_row);
  g_free(data);
}

static void on_attachment_folder_selected(GObject *source,
                                          GAsyncResult *result,
                                          gpointer user_data) {
  AttachmentFolderRequest *data = user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish(
      GTK_FILE_DIALOG(source), result, NULL);
  if (!folder || !data->window->workspace) {
    g_clear_object(&folder);
    attachment_folder_request_free(data);
    return;
  }
  GFile *workspace = pdfv_workspace_get_folder(data->window->workspace);
  gchar *relative = g_file_equal(workspace, folder)
      ? g_strdup("") : g_file_get_relative_path(workspace, folder);
  if (!relative) {
    adw_toast_overlay_add_toast(
        data->window->toast_overlay,
        adw_toast_new("Choose a folder inside the current workspace"));
  } else {
    gchar *uri = g_file_get_uri(folder);
    pdfv_settings_set_workspace_attachment_policy(
        data->window->settings, workspace, TRUE, uri);
    gchar *label = attachment_folder_label(workspace, folder);
    adw_action_row_set_subtitle(data->folder_row, label);
    g_signal_handlers_block_by_func(
        data->mode, on_attachment_mode_changed, data->window);
    adw_switch_row_set_active(data->mode, TRUE);
    g_signal_handlers_unblock_by_func(
        data->mode, on_attachment_mode_changed, data->window);
    propagate_markdown_preferences(data->window);
    g_free(label);
    g_free(uri);
  }
  g_free(relative);
  g_object_unref(folder);
  attachment_folder_request_free(data);
}

static void on_choose_attachment_folder(GtkButton *button,
                                        PdfvWindow *self) {
  if (!self->workspace)
    return;
  AdwSwitchRow *mode = g_object_get_data(
      G_OBJECT(button), "attachment-mode-row");
  AdwActionRow *folder_row = g_object_get_data(
      G_OBJECT(button), "attachment-folder-row");
  if (!mode || !folder_row)
    return;
  GFile *workspace = pdfv_workspace_get_folder(self->workspace);
  GtkFileDialog *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Choose Image Folder");
  gchar *uri = pdfv_settings_dup_workspace_attachment_folder_uri(
      self->settings, workspace);
  GFile *initial = uri && *uri ? g_file_new_for_uri(uri)
                              : g_object_ref(workspace);
  gtk_file_dialog_set_initial_folder(dialog, initial);
  AttachmentFolderRequest *data = g_new0(AttachmentFolderRequest, 1);
  data->window = g_object_ref(self);
  data->mode = g_object_ref(mode);
  data->folder_row = g_object_ref(folder_row);
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL,
                                on_attachment_folder_selected, data);
  g_object_unref(initial);
  g_free(uri);
}

static void action_preferences(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  AdwDialog *dialog = adw_preferences_dialog_new();
  adw_dialog_set_title(dialog, "Settings");

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(
      adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Editor");
  adw_preferences_page_set_icon_name(page, "document-edit-symbolic");
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);

  AdwPreferencesGroup *appearance = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(appearance, "Appearance");
  adw_preferences_page_add(page, appearance);

  AdwSpinRow *font = ADW_SPIN_ROW(
      adw_spin_row_new_with_range(11.0, 32.0, 1.0));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(font),
                                "Markdown font size");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(font),
                              "Base editor size in pixels");
  adw_spin_row_set_value(
      font, pdfv_settings_get_markdown_font_scale(self->settings) * 16.0);
  adw_preferences_group_add(appearance, GTK_WIDGET(font));
  g_signal_connect_object(font, "notify::value",
                          G_CALLBACK(on_preferences_font_changed), self, 0);

  AdwSwitchRow *line_width = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(line_width),
                                "Readable line width");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(line_width),
      "Center notes at a comfortable maximum width");
  adw_switch_row_set_active(
      line_width, pdfv_settings_get_readable_line_width(self->settings));
  adw_preferences_group_add(appearance, GTK_WIDGET(line_width));
  g_signal_connect_object(line_width, "notify::active",
                          G_CALLBACK(on_preferences_line_width_changed),
                          self, 0);

  AdwSwitchRow *remote = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remote),
                                "Allow remote images");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(remote),
      "Local and vault images are always available");
  adw_switch_row_set_active(
      remote, pdfv_settings_get_allow_remote_images(self->settings));
  adw_preferences_group_add(appearance, GTK_WIDGET(remote));
  g_signal_connect_object(remote, "notify::active",
                          G_CALLBACK(on_preferences_remote_changed), self, 0);

  AdwPreferencesGroup *attachments = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(attachments, "Pasted images");
  adw_preferences_group_set_description(
      attachments, self->workspace
          ? "The destination is stored by Phi for this workspace."
          : "Open a workspace to use a fixed image folder.");
  adw_preferences_page_add(page, attachments);

  GFile *workspace = self->workspace
      ? pdfv_workspace_get_folder(self->workspace) : NULL;
  gboolean fixed = workspace &&
      pdfv_settings_get_workspace_attachment_fixed(self->settings,
                                                    workspace);
  AdwSwitchRow *fixed_row = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fixed_row),
                                "Use a fixed folder");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(fixed_row),
      "Otherwise images are saved beside the Markdown file");
  adw_switch_row_set_active(fixed_row, fixed);
  gtk_widget_set_sensitive(GTK_WIDGET(fixed_row), workspace != NULL);
  adw_preferences_group_add(attachments, GTK_WIDGET(fixed_row));
  g_signal_connect_object(fixed_row, "notify::active",
                          G_CALLBACK(on_attachment_mode_changed), self, 0);

  gchar *folder_uri = workspace
      ? pdfv_settings_dup_workspace_attachment_folder_uri(self->settings,
                                                          workspace)
      : NULL;
  GFile *folder = folder_uri && *folder_uri
      ? g_file_new_for_uri(folder_uri) : NULL;
  gchar *folder_label = attachment_folder_label(workspace, folder);
  AdwActionRow *folder_row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row),
                                "Fixed folder");
  adw_action_row_set_subtitle(folder_row, folder_label);
  GtkWidget *choose_folder = gtk_button_new_with_label("Choose…");
  gtk_widget_set_valign(choose_folder, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(folder_row, choose_folder);
  gtk_widget_set_sensitive(GTK_WIDGET(folder_row), workspace != NULL);
  adw_preferences_group_add(attachments, GTK_WIDGET(folder_row));
  g_object_set_data(G_OBJECT(choose_folder), "attachment-mode-row",
                    fixed_row);
  g_object_set_data(G_OBJECT(choose_folder), "attachment-folder-row",
                    folder_row);
  g_signal_connect_object(choose_folder, "clicked",
                          G_CALLBACK(on_choose_attachment_folder), self, 0);
  g_clear_object(&folder);
  g_free(folder_uri);
  g_free(folder_label);

  AdwPreferencesGroup *latex = ADW_PREFERENCES_GROUP(
      adw_preferences_group_new());
  adw_preferences_group_set_title(latex, "LaTeX Suite");
  adw_preferences_group_set_description(
      latex,
      "Editor enhancements and snippets are stored globally.");
  adw_preferences_page_add(page, latex);

  AdwSwitchRow *conceal = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(conceal),
                                "Conceal LaTeX syntax");
  adw_action_row_set_subtitle(
      ADW_ACTION_ROW(conceal),
      "Show readable symbols outside the cursor; disabled by default");
  adw_switch_row_set_active(
      conceal, pdfv_settings_get_latex_conceal(self->settings));
  adw_preferences_group_add(latex, GTK_WIDGET(conceal));
  g_signal_connect_object(conceal, "notify::active",
                          G_CALLBACK(on_preferences_latex_conceal_changed),
                          self, 0);

  GtkWidget *snippet_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(snippet_box, 6);
  gtk_widget_set_margin_bottom(snippet_box, 6);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroll, -1, 300);
  gtk_widget_add_css_class(scroll, "card");
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 10);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  const gchar *custom = pdfv_settings_get_latex_snippets(self->settings);
  GError *default_error = NULL;
  gchar *defaults = !custom || !*custom
      ? pdfv_markdown_resource_scheme_load_default_snippets(&default_error)
      : NULL;
  gtk_text_buffer_set_text(buffer, custom && *custom ? custom
                                                     : defaults ? defaults : "",
                           -1);
  if (default_error) {
    g_warning("Could not load default snippets: %s", default_error->message);
    g_clear_error(&default_error);
  }
  g_free(defaults);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_box_append(GTK_BOX(snippet_box), scroll);

  GtkWidget *reset = gtk_button_new_with_label("Reset to Defaults…");
  gtk_widget_set_halign(reset, GTK_ALIGN_END);
  gtk_widget_add_css_class(reset, "flat");
  gtk_box_append(GTK_BOX(snippet_box), reset);
  adw_preferences_group_add(latex, snippet_box);
  g_signal_connect_object(buffer, "changed",
                          G_CALLBACK(on_preferences_snippets_changed),
                          self, 0);
  g_object_set_data(G_OBJECT(reset), "snippets-buffer", buffer);
  g_signal_connect(reset, "clicked",
                   G_CALLBACK(on_reset_snippets_clicked), self);

  adw_dialog_present(dialog, GTK_WIDGET(self));
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

  if (self->current_editor) {
    pdfv_markdown_editor_run_command(self->current_editor, "editor.find");
    return;
  }

  gtk_search_bar_set_search_mode(self->search_bar, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
}

static void action_find_next(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_editor) {
    pdfv_markdown_editor_run_command(self->current_editor,
                                     "editor.findNext");
    return;
  }
  if (self->current_view)
    pdfv_document_view_search_next(self->current_view);
}

static void action_find_prev(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  PdfvWindow *self = PDFV_WINDOW(user_data);
  if (self->current_editor) {
    pdfv_markdown_editor_run_command(self->current_editor,
                                     "editor.findPrevious");
    return;
  }
  if (self->current_view)
    pdfv_document_view_search_prev(self->current_view);
}

static GActionEntry win_actions[] = {
    {.name = "open", .activate = action_open},
    {.name = "open-folder", .activate = action_open_folder},
    {.name = "workspace-search", .activate = action_workspace_search},
    {.name = "close-workspace", .activate = action_close_workspace},
    {.name = "new-tab", .activate = action_new_tab},
    {.name = "new-workspace-window",
     .activate = action_new_workspace_window},
    {.name = "close-tab", .activate = action_close_tab},
    {.name = "save", .activate = action_save},
    {.name = "toggle-markdown-source",
     .activate = action_toggle_markdown_source},
    {.name = "allow-remote-images", .state = "false",
     .change_state = action_allow_remote_images},
    {.name = "preferences", .activate = action_preferences},
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
  if (!self->closing_window)
    save_workspace_tab_session(self);
  workspace_preview_cancel_delay(self);
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
  if (n_pages == 0)
    return GDK_EVENT_PROPAGATE;

  self->closing_window = TRUE;
  AdwTabPage *page = adw_tab_view_get_nth_page(self->tab_view, 0);
  if (!g_object_get_data(G_OBJECT(page), "markdown-close-pending"))
    adw_tab_view_close_page(self->tab_view, page);
  return GDK_EVENT_STOP;
}

static void pdfv_window_dispose(GObject *object) {
  PdfvWindow *self = PDFV_WINDOW(object);

  g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), self);

  if (self->window_title_page)
    g_signal_handlers_disconnect_by_func(
        self->window_title_page, on_selected_tab_title_changed, self);
  g_clear_object(&self->window_title_page);
  clear_markdown_editor_spare(self);

  workspace_preview_cancel_delay(self);
  if (self->settings_update_timeout_id) {
    g_source_remove(self->settings_update_timeout_id);
    self->settings_update_timeout_id = 0;
  }
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
  g_clear_pointer(&self->workspace_expanded_paths, g_hash_table_unref);
  g_clear_object(&self->file_menu_section);
  g_clear_object(&self->workspace_menu_section);
  g_clear_object(&self->zoom_menu_section);
  g_clear_object(&self->view_menu_section);
  g_clear_pointer(&self->settings, pdfv_settings_free);

  if (self->current_outline) {
    phi_outline_item_free(self->current_outline);
    self->current_outline = NULL;
  }

  G_OBJECT_CLASS(pdfv_window_parent_class)->dispose(object);
}

static void pdfv_window_init(PdfvWindow *self) {
  self->current_view = NULL;
  self->current_editor = NULL;
  self->current_outline = NULL;
  self->workspace_pending_group = -1;
  self->settings = pdfv_settings_new();
  self->workspace_expanded_paths =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->workspace_document_cache =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                  G_N_ELEMENTS(win_actions), self);
  GAction *remote_images_action = g_action_map_lookup_action(
      G_ACTION_MAP(self), "allow-remote-images");
  g_simple_action_set_state(
      G_SIMPLE_ACTION(remote_images_action),
      g_variant_new_boolean(
          pdfv_settings_get_allow_remote_images(self->settings)));
  GAction *workspace_search_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "workspace-search");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(workspace_search_action), FALSE);
  GAction *close_workspace_action =
      g_action_map_lookup_action(G_ACTION_MAP(self), "close-workspace");
  g_simple_action_set_enabled(G_SIMPLE_ACTION(close_workspace_action), FALSE);
  update_markdown_actions(self);
  g_signal_connect(self, "close-request", G_CALLBACK(on_window_close_request),
                   self);
  g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                   G_CALLBACK(on_style_dark_changed), self);

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
  gtk_widget_add_css_class(GTK_WIDGET(self->split_view), "pdfv-split-view");
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

  /* Set up the hierarchy: window -> toast overlay -> split view -> tabs. */
  adw_overlay_split_view_set_content(self->split_view,
                                     GTK_WIDGET(self->tab_overview));
  self->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_toast_overlay_set_child(self->toast_overlay,
                              GTK_WIDGET(self->split_view));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                     GTK_WIDGET(self->toast_overlay));

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
  self->file_menu_section = g_menu_new();
  g_menu_append_section(menu, NULL, G_MENU_MODEL(self->file_menu_section));

  self->workspace_menu_section = g_menu_new();
  g_menu_append_section(menu, NULL,
                        G_MENU_MODEL(self->workspace_menu_section));

  self->zoom_menu_section = g_menu_new();
  g_menu_append_section(menu, NULL, G_MENU_MODEL(self->zoom_menu_section));

  self->view_menu_section = g_menu_new();
  g_menu_append_section(menu, NULL, G_MENU_MODEL(self->view_menu_section));

  GMenu *about_section = g_menu_new();
  g_menu_append(about_section, "Settings", "win.preferences");
  g_menu_append(about_section, "About Phi Document Viewer", "app.about");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(about_section));

  gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
  adw_header_bar_pack_end(self->header_bar, GTK_WIDGET(self->menu_button));

  g_object_unref(about_section);
  g_object_unref(menu);
  rebuild_main_menu(self);

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

  self->pages_sidebar_page = adw_view_stack_add_titled_with_icon(
      self->sidebar_stack, thumb_scroll, "pages", "Pages",
      "view-paged-symbolic");
  adw_view_stack_page_set_visible(self->pages_sidebar_page, FALSE);

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
  self->workspace_loading_spinner = adw_spinner_new();
  gtk_widget_set_size_request(self->workspace_loading_spinner, 24, 24);
  adw_status_page_set_child(
      self->workspace_loading_page, self->workspace_loading_spinner);
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
  GtkGesture *workspace_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(workspace_click), 0);
  g_signal_connect(workspace_click, "pressed",
                   G_CALLBACK(on_workspace_middle_click), self);
  gtk_widget_add_controller(GTK_WIDGET(self->workspace_list),
                            GTK_EVENT_CONTROLLER(workspace_click));
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
      ".pdfv-split-view > .sidebar-pane {"
      "  box-shadow: none;"
      "}"
      ".workspace-search-results {"
      "  background-color: alpha(@window_fg_color, 0.025);"
      "  border-radius: 12px;"
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
      self->workspace_search_entry, "Search PDFs and Markdown in this workspace");
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
  gtk_window_set_title(GTK_WINDOW(self), "Phi Document Viewer");

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

static AdwTabPage *workspace_open_file(PdfvWindow *self, GFile *file,
                                       gboolean persistent) {
  AdwTabPage *page = NULL;
  if (!persistent && self->workspace_browse_tab &&
      adw_tab_view_get_page_position(self->tab_view,
                                     self->workspace_browse_tab) >= 0)
    page = self->workspace_browse_tab;

  if (!page) {
    AdwTabPage *selected = adw_tab_view_get_selected_page(self->tab_view);
    if (selected) {
      GtkWidget *child = adw_tab_page_get_child(selected);
      if (GTK_IS_STACK(child) &&
          g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(child)),
                    "empty") == 0)
        page = selected;
    }
  }
  if (!page) {
    GtkWidget *content = create_tab_content(self);
    page = adw_tab_view_append(self->tab_view, content);
  }
  if (!persistent)
    self->workspace_browse_tab = page;
  adw_tab_view_set_selected_page(self->tab_view, page);
  if (file_is_markdown(file))
    open_markdown_in_tab_async(self, file, page);
  else
    open_file_in_tab_async(self, file, page, 0, FALSE, TRUE, 0);
  return page;
}

static void pdfv_window_open_file_internal(PdfvWindow *self, GFile *file,
                                           gboolean fit_width) {
  g_return_if_fail(PDFV_IS_WINDOW(self));
  g_return_if_fail(G_IS_FILE(file));

  AdwTabPage *page = adw_tab_view_get_selected_page(self->tab_view);
  GtkWidget *stack;
  gboolean selected_is_empty = FALSE;
  if (page) {
    GtkWidget *selected = adw_tab_page_get_child(page);
    selected_is_empty = GTK_IS_STACK(selected) &&
                        g_strcmp0(gtk_stack_get_visible_child_name(
                                      GTK_STACK(selected)),
                                  "empty") == 0;
  }
  if (page && selected_is_empty) {
    stack = adw_tab_page_get_child(page);
  } else {
    stack = create_tab_content(self);
    page = adw_tab_view_append(self->tab_view, stack);
    adw_tab_view_set_selected_page(self->tab_view, page);
  }
  if (file_is_markdown(file))
    open_markdown_in_tab_async(self, file, page);
  else
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

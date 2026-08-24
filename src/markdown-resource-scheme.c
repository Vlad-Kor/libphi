/*
 * Phi Markdown editor - WebKit app:// and vault:// resources
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "markdown-resource-scheme.h"

#include <string.h>

#ifndef PDFV_EDITOR_DIR
#define PDFV_EDITOR_DIR "/usr/share/phi/editor"
#endif

#ifndef PDFV_EDITOR_SOURCE_DIR
#define PDFV_EDITOR_SOURCE_DIR PDFV_EDITOR_DIR
#endif

struct _PdfvMarkdownResourceScheme {
  GObject parent_instance;
  PdfvMarkdownVaultAdapter *vault;
  WebKitWebContext *context;
};

#define MARKDOWN_VAULT_DATA "phi-markdown-vault-adapter"

/* All Markdown tabs share a context so WebKit can reuse its web process,
 * compiled JavaScript and resource cache. Vault access remains scoped to the
 * requesting WebView through MARKDOWN_VAULT_DATA. */
static WebKitWebContext *shared_context;

G_DEFINE_FINAL_TYPE(PdfvMarkdownResourceScheme, pdfv_markdown_resource_scheme,
                    G_TYPE_OBJECT)

static void finish_error(WebKitURISchemeRequest *request, GQuark domain,
                         gint code, const gchar *message) {
  GError *error = g_error_new_literal(domain, code, message);
  webkit_uri_scheme_request_finish_error(request, error);
  g_error_free(error);
}

static gboolean asset_path_is_safe(const gchar *path) {
  if (!path || !*path || g_path_is_absolute(path) || strchr(path, '\\'))
    return FALSE;
  gchar **parts = g_strsplit(path, "/", -1);
  gboolean safe = TRUE;
  for (guint i = 0; parts[i]; i++) {
    if (!*parts[i] || g_str_equal(parts[i], ".") ||
        g_str_equal(parts[i], "..")) {
      safe = FALSE;
      break;
    }
  }
  g_strfreev(parts);
  return safe;
}

static gchar *mime_for_path(const gchar *path, const gchar *contents,
                            gsize length) {
  if (g_str_has_suffix(path, ".js"))
    return g_strdup("text/javascript");
  if (g_str_has_suffix(path, ".css"))
    return g_strdup("text/css");
  if (g_str_has_suffix(path, ".html"))
    return g_strdup("text/html");
  if (g_str_has_suffix(path, ".woff2"))
    return g_strdup("font/woff2");
  gboolean uncertain = FALSE;
  gchar *type = g_content_type_guess(path, (const guchar *)contents,
                                     MIN(length, 512), &uncertain);
  gchar *mime = g_content_type_get_mime_type(type);
  g_free(type);
  return mime;
}

static gchar *editor_asset_filename(const gchar *relative) {
  /* Prefer the source bundle while running from a checkout. Otherwise an older
   * system installation can silently shadow the assets that were just built. */
  gchar *source = g_build_filename(PDFV_EDITOR_SOURCE_DIR, relative, NULL);
  if (g_file_test(source, G_FILE_TEST_IS_REGULAR))
    return source;
  g_free(source);
  return g_build_filename(PDFV_EDITOR_DIR, relative, NULL);
}

static void app_scheme_request(WebKitURISchemeRequest *request,
                               gpointer user_data) {
  (void)user_data;
  const gchar *uri = webkit_uri_scheme_request_get_uri(request);
  GError *error = NULL;
  GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
  if (!parsed || g_strcmp0(g_uri_get_host(parsed), "editor") != 0) {
    g_clear_pointer(&parsed, g_uri_unref);
    g_clear_error(&error);
    finish_error(request, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "Invalid app resource origin");
    return;
  }
  const gchar *uri_path = g_uri_get_path(parsed);
  while (uri_path && *uri_path == '/')
    uri_path++;
  gchar *relative = g_uri_unescape_string(
      uri_path && *uri_path ? uri_path : "index.html", NULL);
  if (!asset_path_is_safe(relative)) {
    g_uri_unref(parsed);
    g_free(relative);
    finish_error(request, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "Unsafe app resource path");
    return;
  }
  gchar *filename = editor_asset_filename(relative);
  gchar *contents = NULL;
  gsize length = 0;
  if (!g_file_get_contents(filename, &contents, &length, &error)) {
    g_debug("Could not serve editor asset '%s': %s", relative,
            error ? error->message : "unknown error");
    webkit_uri_scheme_request_finish_error(request, error);
    g_clear_error(&error);
  } else {
    gchar *mime = mime_for_path(relative, contents, length);
    GBytes *bytes = g_bytes_new_take(contents, length);
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, (gint64)length, mime);
    g_object_unref(stream);
    g_bytes_unref(bytes);
    g_free(mime);
  }
  g_free(filename);
  g_free(relative);
  g_uri_unref(parsed);
}

static gchar *load_editor_text_asset(const gchar *name, GError **error) {
  gchar *filename = editor_asset_filename(name);
  gchar *contents = NULL;
  if (!g_file_get_contents(filename, &contents, NULL, error))
    g_clear_pointer(&contents, g_free);
  g_free(filename);
  return contents;
}

gchar *pdfv_markdown_resource_scheme_load_default_snippets(GError **error) {
  return load_editor_text_asset("default-snippets.txt", error);
}

gchar *pdfv_markdown_resource_scheme_load_default_snippet_variables(
    GError **error) {
  return load_editor_text_asset("default-snippet-variables.txt", error);
}

static void vault_scheme_request(WebKitURISchemeRequest *request,
                                 gpointer user_data) {
  (void)user_data;
  WebKitWebView *web_view = webkit_uri_scheme_request_get_web_view(request);
  PdfvMarkdownVaultAdapter *vault = web_view
      ? g_object_get_data(G_OBJECT(web_view), MARKDOWN_VAULT_DATA)
      : NULL;
  if (!vault) {
    finish_error(request, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "Markdown vault is unavailable for this view");
    return;
  }
  const gchar *uri = webkit_uri_scheme_request_get_uri(request);
  GError *error = NULL;
  GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
  if (!parsed || (g_uri_get_host(parsed) && *g_uri_get_host(parsed))) {
    g_clear_pointer(&parsed, g_uri_unref);
    g_clear_error(&error);
    finish_error(request, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                 "Invalid vault resource origin");
    return;
  }
  const gchar *uri_path = g_uri_get_path(parsed);
  while (uri_path && *uri_path == '/')
    uri_path++;
  gchar *content_type = NULL;
  GBytes *bytes = pdfv_markdown_vault_adapter_read_bytes(
      vault, uri_path ? uri_path : "", &content_type, &error);
  if (!bytes) {
    webkit_uri_scheme_request_finish_error(request, error);
    g_clear_error(&error);
  } else {
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream,
                                     (gint64)g_bytes_get_size(bytes),
                                     content_type);
    g_object_unref(stream);
    g_bytes_unref(bytes);
  }
  g_free(content_type);
  g_uri_unref(parsed);
}

static void pdfv_markdown_resource_scheme_finalize(GObject *object) {
  PdfvMarkdownResourceScheme *self = PDFV_MARKDOWN_RESOURCE_SCHEME(object);
  g_clear_object(&self->context);
  g_clear_object(&self->vault);
  G_OBJECT_CLASS(pdfv_markdown_resource_scheme_parent_class)->finalize(object);
}

static void pdfv_markdown_resource_scheme_class_init(
    PdfvMarkdownResourceSchemeClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = pdfv_markdown_resource_scheme_finalize;
}

static void pdfv_markdown_resource_scheme_init(
    PdfvMarkdownResourceScheme *self) {
  (void)self;
}

PdfvMarkdownResourceScheme *pdfv_markdown_resource_scheme_new(
    PdfvMarkdownVaultAdapter *vault) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_VAULT_ADAPTER(vault), NULL);
  PdfvMarkdownResourceScheme *self =
      g_object_new(PDFV_TYPE_MARKDOWN_RESOURCE_SCHEME, NULL);
  self->vault = g_object_ref(vault);
  if (!shared_context) {
    shared_context = webkit_web_context_new();
    webkit_web_context_set_cache_model(shared_context,
                                       WEBKIT_CACHE_MODEL_WEB_BROWSER);
    webkit_web_context_register_uri_scheme(shared_context, "app",
                                           app_scheme_request, NULL, NULL);
    webkit_web_context_register_uri_scheme(shared_context, "vault",
                                           vault_scheme_request, NULL, NULL);
    WebKitSecurityManager *security =
        webkit_web_context_get_security_manager(shared_context);
    webkit_security_manager_register_uri_scheme_as_local(security, "app");
    webkit_security_manager_register_uri_scheme_as_secure(security, "app");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                                 "app");
    webkit_security_manager_register_uri_scheme_as_local(security, "vault");
    webkit_security_manager_register_uri_scheme_as_secure(security, "vault");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                                 "vault");
  }
  self->context = g_object_ref(shared_context);
  return self;
}

WebKitWebContext *pdfv_markdown_resource_scheme_get_context(
    PdfvMarkdownResourceScheme *self) {
  g_return_val_if_fail(PDFV_IS_MARKDOWN_RESOURCE_SCHEME(self), NULL);
  return self->context;
}

void pdfv_markdown_resource_scheme_bind_web_view(
    PdfvMarkdownResourceScheme *self, WebKitWebView *web_view) {
  g_return_if_fail(PDFV_IS_MARKDOWN_RESOURCE_SCHEME(self));
  g_return_if_fail(WEBKIT_IS_WEB_VIEW(web_view));
  g_object_set_data_full(G_OBJECT(web_view), MARKDOWN_VAULT_DATA,
                         g_object_ref(self->vault), g_object_unref);
}

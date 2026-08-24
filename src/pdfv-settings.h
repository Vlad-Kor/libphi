/* Global application preferences for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_SETTINGS_H
#define PDFV_SETTINGS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PdfvSettings PdfvSettings;

PdfvSettings *pdfv_settings_new(void);
void pdfv_settings_free(PdfvSettings *self);
gboolean pdfv_settings_save(PdfvSettings *self, GError **error);

gdouble pdfv_settings_get_markdown_font_scale(PdfvSettings *self);
void pdfv_settings_set_markdown_font_scale(PdfvSettings *self,
                                           gdouble scale);
gboolean pdfv_settings_get_readable_line_width(PdfvSettings *self);
void pdfv_settings_set_readable_line_width(PdfvSettings *self,
                                           gboolean enabled);
gboolean pdfv_settings_get_allow_remote_images(PdfvSettings *self);
void pdfv_settings_set_allow_remote_images(PdfvSettings *self,
                                           gboolean allowed);
gboolean pdfv_settings_get_latex_conceal(PdfvSettings *self);
void pdfv_settings_set_latex_conceal(PdfvSettings *self,
                                     gboolean enabled);
gboolean pdfv_settings_get_pdf_inverted(PdfvSettings *self);
void pdfv_settings_set_pdf_inverted(PdfvSettings *self,
                                    gboolean inverted);
const gchar *pdfv_settings_get_latex_snippets(PdfvSettings *self);
void pdfv_settings_set_latex_snippets(PdfvSettings *self,
                                      const gchar *snippets);
gboolean pdfv_settings_get_workspace_attachment_fixed(
    PdfvSettings *self, GFile *workspace);
gchar *pdfv_settings_dup_workspace_attachment_folder_uri(
    PdfvSettings *self, GFile *workspace);
void pdfv_settings_set_workspace_attachment_policy(
    PdfvSettings *self, GFile *workspace, gboolean fixed,
    const gchar *folder_uri);
gchar **pdfv_settings_dup_workspace_open_tabs(
    PdfvSettings *self, GFile *workspace, gsize *length);
gchar *pdfv_settings_dup_workspace_active_tab(
    PdfvSettings *self, GFile *workspace);
void pdfv_settings_set_workspace_tabs(
    PdfvSettings *self, GFile *workspace,
    const gchar *const *relative_paths, gsize length,
    const gchar *active_relative_path);
gchar **pdfv_settings_dup_workspace_expanded_folders(
    PdfvSettings *self, GFile *workspace, gsize *length);
void pdfv_settings_set_workspace_expanded_folders(
    PdfvSettings *self, GFile *workspace,
    const gchar *const *relative_paths, gsize length);
void pdfv_settings_copy(PdfvSettings *destination,
                        PdfvSettings *source);

G_END_DECLS

#endif

/* Global application preferences for Phi.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_SETTINGS_H
#define PDFV_SETTINGS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _PdfvSettings PdfvSettings;

PdfvSettings *pdfv_settings_new(void);
void pdfv_settings_free(PdfvSettings *self);
gboolean pdfv_settings_save(PdfvSettings *self, GError **error);

gdouble pdfv_settings_get_markdown_font_scale(PdfvSettings *self);
void pdfv_settings_set_markdown_font_scale(PdfvSettings *self,
                                           gdouble scale);
gboolean pdfv_settings_get_allow_remote_images(PdfvSettings *self);
void pdfv_settings_set_allow_remote_images(PdfvSettings *self,
                                           gboolean allowed);
const gchar *pdfv_settings_get_latex_snippets(PdfvSettings *self);
void pdfv_settings_set_latex_snippets(PdfvSettings *self,
                                      const gchar *snippets);
void pdfv_settings_copy(PdfvSettings *destination,
                        PdfvSettings *source);

G_END_DECLS

#endif

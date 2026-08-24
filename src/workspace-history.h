/*
 * Phi PDF Viewer - Persistent recent workspace history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PDFV_WORKSPACE_HISTORY_H
#define PDFV_WORKSPACE_HISTORY_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define PDFV_WORKSPACE_HISTORY_LIMIT 5

void pdfv_workspace_history_remember(GFile *folder);
void pdfv_workspace_history_clear_current(void);
GFile *pdfv_workspace_history_dup_current(void);
GPtrArray *pdfv_workspace_history_list(void);
void pdfv_workspace_history_forget(GFile *folder);

G_END_DECLS

#endif /* PDFV_WORKSPACE_HISTORY_H */

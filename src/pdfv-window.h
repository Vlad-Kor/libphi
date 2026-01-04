/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2025  Florian "sp1rit" <sp1rit@disoot.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PDFV_WINDOW_H__
#define __PDFV_WINDOW_H__

#include <adwaita.h>

G_BEGIN_DECLS

#define PDFV_TYPE_WINDOW (pdfv_window_get_type())
G_DECLARE_FINAL_TYPE(PdfvWindow, pdfv_window, PDFV, WINDOW, AdwApplicationWindow)

PdfvWindow* pdfv_window_new(AdwApplication* app);
void pdfv_window_open_file(PdfvWindow* self, GFile* file);
void pdfv_window_new_tab(PdfvWindow* self);

G_END_DECLS

#endif // __PDFV_WINDOW_H__

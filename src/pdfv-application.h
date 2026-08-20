/*
 * Phi PDF Viewer - High performance PDF viewer using libphi
 * Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PDFV_APPLICATION_H__
#define __PDFV_APPLICATION_H__

#include <adwaita.h>

G_BEGIN_DECLS

#define PDFV_TYPE_APPLICATION (pdfv_application_get_type())
G_DECLARE_FINAL_TYPE(PdfvApplication, pdfv_application, PDFV, APPLICATION, AdwApplication)

PdfvApplication* pdfv_application_new(void);

G_END_DECLS

#endif // __PDFV_APPLICATION_H__

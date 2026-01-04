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

int
main(int argc, char* argv[])
{
    g_autoptr(PdfvApplication) app = pdfv_application_new();
    return g_application_run(G_APPLICATION(app), argc, argv);
}

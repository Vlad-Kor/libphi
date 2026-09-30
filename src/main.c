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

#ifdef G_OS_WIN32
#include <stdio.h>
#include <windows.h>

/* pdfv is a GUI program on Windows, so it opens no console of its own. When
 * started from a terminal, write --help and messages to that terminal. */
static void reopen_on_console(DWORD handle, FILE *stream) {
    /* Keep streams that were redirected to a file or pipe. */
    HANDLE current = GetStdHandle(handle);
    if (current && current != INVALID_HANDLE_VALUE &&
        GetFileType(current) != FILE_TYPE_UNKNOWN)
        return;
    if (freopen("CONOUT$", "w", stream))
        setvbuf(stream, NULL, _IONBF, 0);
}

static void attach_parent_console(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    reopen_on_console(STD_OUTPUT_HANDLE, stdout);
    reopen_on_console(STD_ERROR_HANDLE, stderr);
}
#endif

int
main(int argc, char* argv[])
{
#ifdef G_OS_WIN32
    attach_parent_console();
#endif
    g_autoptr(PdfvApplication) app = pdfv_application_new();
    return g_application_run(G_APPLICATION(app), argc, argv);
}

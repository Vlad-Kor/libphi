#!/usr/bin/env python3
# libphi - GObject Introspection smoke test
# Copyright (C) 2026 Vlad Korsakov <ulqba@student.kit.edu>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Uses libphi exactly as a PyGObject application would: through the Phi-1.0
# typelib, with a PDF that only ever exists in memory. Exits with 77 (skip)
# when PyGObject or a display is unavailable.

import sys

SKIP = 77

try:
    import gi

    gi.require_version("GLib", "2.0")
    gi.require_version("Gtk", "4.0")
    gi.require_version("Phi", "1.0")
    from gi.repository import GLib, Gtk, Phi
except (ImportError, ValueError) as error:
    print(f"skipping: {error}")
    sys.exit(SKIP)


def build_pdf(text: str) -> bytes:
    """Return a minimal, valid one-page 300x400 pt PDF showing @text."""
    content = f"BT /F1 24 Tf 40 300 Td ({text}) Tj ET 50 50 100 100 re f"
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        f"<< /Length {len(content)} >>\nstream\n{content}\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n{body}\nendobj\n".encode("latin-1")
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += (
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref}\n%%EOF\n"
    ).encode()
    return bytes(out)


def main() -> int:
    data = GLib.Bytes.new(build_pdf("Hello from Typst"))
    document = Phi.Document.new_from_bytes(data, None)
    # The document must retain its own reference to the data.
    del data

    assert document.get_n_pages() == 1
    assert document.get_n_items() == 1
    page = document.get_page(0)
    size = page.get_size()
    assert (size.width, size.height) == (300.0, 400.0), size
    assert "Hello from Typst" in page.get_text()

    # The background raster renderer works for memory-backed documents.
    texture = document.render_page_texture(0, 2.0, 0, 0, 0, 0, None)
    assert (texture.get_width(), texture.get_height()) == (600, 800)
    tile = document.render_page_texture(0, 2.0, 64, 64, 128, 128, None)
    assert (tile.get_width(), tile.get_height()) == (128, 128)

    try:
        Phi.Document.new_from_bytes(GLib.Bytes.new(b"not a pdf"), None)
    except GLib.Error as error:
        assert error.domain == "PhiMuError", error.domain
    else:
        raise AssertionError("invalid data was accepted")

    if not Gtk.init_check():
        print("skipping view checks: no display")
        return SKIP

    view = Phi.DocumentView()
    view.set_document(document)
    assert view.get_document() == document
    assert view.props.document == document
    view.set_zoom(1.5)
    assert abs(view.get_zoom() - 1.5) < 1e-9
    page_index, fraction, center = view.get_scroll_state()
    assert page_index == 0

    scrolled = Gtk.ScrolledWindow(child=view)
    window = Gtk.Window(child=scrolled)
    view.capture_zoom_scroll(window)
    view.set_document(None)
    assert view.get_document() is None
    window.destroy()
    return 0


if __name__ == "__main__":
    sys.exit(main())

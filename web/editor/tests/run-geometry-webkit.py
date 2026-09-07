#!/usr/bin/python3
"""Real WebKitGTK layout checks. Run after `npm run build`, with a desktop display.

Example: ./tests/run-geometry-webkit.py --width 600 --scale 1.25 --output /tmp/geometry.json
Requires PyGObject, GTK 4, WebKit 6, and the installed editor npm dependencies.
The tiny video fixture was generated with ffmpeg's blue 320x180 color source.
"""
import argparse
import json
import mimetypes
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("WebKit", "6.0")
from gi.repository import Gio, GLib, Gtk, WebKit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--width", type=int, default=1000)
parser.add_argument("--scale", type=float, default=1)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
editor = Path(__file__).resolve().parents[1]
workspace = tempfile.TemporaryDirectory(prefix="phi-geometry-")
fixtures = Path(workspace.name)
subprocess.run([
    str(editor / "node_modules/.bin/esbuild"), "tests/geometry-webkit.ts",
    "--bundle", "--format=iife", "--target=safari17", "--loader:.txt=text",
    f"--outfile={fixtures / 'geometry-test.js'}",
], cwd=editor, check=True)
(fixtures / "geometry-test.html").write_text('''<!doctype html><html><head>
<link rel="stylesheet" href="app://editor/editor.css">
<script src="app://editor/mathjax-config.js"></script></head><body>
<script>
window.onerror=(message,source,line)=>window.webkit.messageHandlers.test.postMessage("FAIL "+message+" "+source+":"+line);
console.error=(...args)=>window.webkit.messageHandlers.test.postMessage("FAIL console "+args.map(a=>a?.stack||String(a)).join(" "));
</script><div id="editor"></div><script src="app://editor/geometry-test.js"></script>
</body></html>''')
(fixtures / "image.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg" width="300" height="150"><rect width="300" height="150" fill="red"/></svg>')
(fixtures / "video.webm").write_bytes((editor / "tests/fixtures/geometry.webm").read_bytes())
with wave.open(str(fixtures / "audio.wav"), "wb") as audio:
    audio.setparams((1, 2, 8000, 800, "NONE", "not compressed"))
    audio.writeframes(bytes(1600))

app = Gtk.Application(application_id="org.phi.GeometryTest", flags=Gio.ApplicationFlags.NON_UNIQUE)
status = 1


def activate(application):
    context = WebKit.WebContext()

    def request(req):
        name = req.get_path().lstrip("/")
        file = fixtures / name if (fixtures / name).is_file() else editor / "dist" / name
        try:
            data = file.read_bytes()
        except OSError as error:
            print(error, file=sys.stderr)
            data = b""
        req.finish(Gio.MemoryInputStream.new_from_bytes(GLib.Bytes.new(data)), len(data),
                   mimetypes.guess_type(str(file))[0] or "application/octet-stream")

    context.register_uri_scheme("app", request)
    context.register_uri_scheme("vault", request)
    manager = WebKit.UserContentManager()
    manager.register_script_message_handler("test", None)

    def message(_manager, value):
        global status
        result = value.to_string()
        status = 0 if result.startswith("PASS ") else 1
        if status:
            print(result, flush=True)
        else:
            report = json.loads(result[5:])
            print(f"PASS: {report['cached']} exact heights at {report['width']}px, "
                  f"font {report['scale']}; open {report['openMs']:.0f}ms, "
                  f"first frame {report['firstFrameMs']:.0f}ms, "
                  f"background pass {report['preflightMs']:.0f}ms", flush=True)
        if args.output:
            args.output.write_text(result.split(" ", 1)[1] + "\n")
        application.quit()

    manager.connect("script-message-received::test", message)
    web = WebKit.WebView(web_context=context, user_content_manager=manager)
    window = Gtk.ApplicationWindow(application=application)
    window.set_default_size(args.width, 700)
    window.set_child(web)
    window.present()
    web.load_uri(f"app://editor/geometry-test.html?scale={args.scale}")

    def timeout():
        print("FAIL: WebKit geometry check timed out", flush=True)
        application.quit()
        return False

    GLib.timeout_add_seconds(180, timeout)


app.connect("activate", activate)
app.run([])
workspace.cleanup()
sys.exit(status)

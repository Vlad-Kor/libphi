#!/usr/bin/python3
"""Real WebKitGTK typing and scrolling benchmark. Run after `npm run build`.

Example: ./tests/run-perf-webkit.py --bytes 60000 --conceal
Reports main-thread work per keystroke (dispatch plus CodeMirror's measure,
which includes forced style/layout) and per scrolled frame, and how many
preview widgets were re-rendered per keystroke. Requires a desktop display,
PyGObject, GTK 4, WebKit 6, and the installed editor npm dependencies.
"""
import argparse
import json
import mimetypes
from pathlib import Path
import subprocess
import sys
import tempfile

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("WebKit", "6.0")
from gi.repository import Gio, GLib, Gtk, WebKit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--width", type=int, default=1000)
parser.add_argument("--bytes", type=int, default=40000)
parser.add_argument("--conceal", action="store_true")
parser.add_argument("--engine", action="store_true", help="only run engine sanity checks")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
editor = Path(__file__).resolve().parents[1]
workspace = tempfile.TemporaryDirectory(prefix="phi-perf-")
fixtures = Path(workspace.name)
subprocess.run([
    str(editor / "node_modules/.bin/esbuild"), "tests/perf-webkit.ts",
    "--bundle", "--format=iife", "--target=safari17", "--minify",
    "--log-level=warning", f"--outfile={fixtures / 'perf-test.js'}",
], cwd=editor, check=True)
(fixtures / "perf-test.html").write_text('''<!doctype html><html><head>
<link rel="stylesheet" href="app://editor/editor.css">
<script src="app://editor/mathjax-config.js"></script></head><body>
<script>
window.onerror=(message,source,line)=>window.webkit.messageHandlers.test.postMessage("FAIL "+message+" "+source+":"+line);
</script><div id="editor"></div><script src="app://editor/perf-test.js"></script>
</body></html>''')

app = Gtk.Application(application_id="org.phi.PerfTest", flags=Gio.ApplicationFlags.NON_UNIQUE)
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
        if result.startswith("LOG "):
            print(result, file=sys.stderr, flush=True)
            return
        status = 0 if result.startswith("PASS ") else 1
        if status:
            print(result, flush=True)
        else:
            print(json.dumps(json.loads(result[5:]), indent=1), flush=True)
        if args.output:
            args.output.write_text(result.split(" ", 1)[1] + "\n")
        application.quit()

    manager.connect("script-message-received::test", message)
    web = WebKit.WebView(web_context=context, user_content_manager=manager)
    window = Gtk.ApplicationWindow(application=application)
    window.set_default_size(args.width, 700)
    window.set_child(web)
    window.present()
    web.load_uri(f"app://editor/perf-test.html?bytes={args.bytes}&conceal={int(args.conceal)}&engine={int(args.engine)}")

    def timeout():
        print("FAIL: WebKit benchmark timed out", flush=True)
        application.quit()
        return False

    GLib.timeout_add_seconds(900, timeout)


app.connect("activate", activate)
app.run([])
workspace.cleanup()
sys.exit(status)

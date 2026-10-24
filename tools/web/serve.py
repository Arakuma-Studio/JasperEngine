#!/usr/bin/env python3
"""Static file server for web (Emscripten) builds.

    python3 tools/web/serve.py --root build/web --port 8080
    then open http://127.0.0.1:8080/jasperengine-project.html

Sends the cross-origin-isolation headers a threaded (SharedArrayBuffer) build needs
(Cross-Origin-Opener-Policy: same-origin, Cross-Origin-Embedder-Policy: require-corp), the
WebAssembly and preload-data MIME types, and Cache-Control: no-store so a rebuild is always what
the browser runs. tools/web/browser_gate.py serves through the same handler. Standard library only.

Adapted from OpenEngine's Tools/Web/serve.py (MIT):
Copyright (c) 2026 Gamecrafters Guild Limited and contributors
"""

from __future__ import annotations

import argparse
import functools
import http.server
from pathlib import Path


class WebBuildHandler(http.server.SimpleHTTPRequestHandler):
    """Serves a directory with the headers above. cross_origin_isolation=False leaves out
    COOP/COEP (browser_gate.py --no-isolation: what a threaded build does without them)."""

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".data": "application/octet-stream",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
    }

    def __init__(self, *args, cross_origin_isolation: bool = True, quiet: bool = False, **kwargs):
        self.cross_origin_isolation = cross_origin_isolation
        self.quiet = quiet
        super().__init__(*args, **kwargs)

    def end_headers(self):
        if self.cross_origin_isolation:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        # browsers ask for one; a 404 would be an error-level console message in the gate
        if self.path == "/favicon.ico":
            self.send_response(204)
            self.end_headers()
            return
        super().do_GET()

    def log_message(self, format, *args):
        if not self.quiet:
            super().log_message(format, *args)


def make_server(root: Path, port: int = 0, cross_origin_isolation: bool = True, quiet: bool = False):
    """A ThreadingHTTPServer on 127.0.0.1 serving root (port 0: a free one, server.server_port)."""
    handler = functools.partial(WebBuildHandler, directory=str(root), cross_origin_isolation=cross_origin_isolation,
                                quiet=quiet)
    return http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, required=True, help="the build directory (e.g. build/web)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--no-isolation", action="store_true", help="leave out COOP/COEP")
    args = parser.parse_args()
    server = make_server(args.root, args.port, not args.no_isolation)
    print(f"serving {args.root} at http://127.0.0.1:{server.server_port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

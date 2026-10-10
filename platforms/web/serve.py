#!/usr/bin/env python3
"""Serves a Hyoshi web build locally, with the headers it needs.

Threads (SharedArrayBuffer) need cross-origin isolation, so every response carries COOP and COEP,
and .wasm is served as application/wasm. Usage: python3 serve.py [folder] [port]
"""

import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".data": "application/octet-stream"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Handler, directory=folder))
    print(f"Serving {folder} at http://localhost:{port}/")
    server.serve_forever()

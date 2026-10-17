#!/usr/bin/env python3
"""Serves a web build and a disc image with HTTP range requests.

usage: serve.py <build dir> <disc image> [port] [--host address] [--record <dir for unrecorded VU1 programs>]
"""

import http.server
import os
import re
import shutil
import sys

RECORD = None
if '--record' in sys.argv:
    at = sys.argv.index('--record')
    RECORD = os.path.abspath(sys.argv[at + 1])
    del sys.argv[at:at + 2]
HOST = '127.0.0.1'
if '--host' in sys.argv:
    at = sys.argv.index('--host')
    HOST = sys.argv[at + 1]
    del sys.argv[at:at + 2]
BUILD = os.path.abspath(sys.argv[1])
DISC = os.path.abspath(sys.argv[2])
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8620
TYPES = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream'}


class Handler(http.server.BaseHTTPRequestHandler):
    def target(self):
        path = self.path.split('?', 1)[0]
        if path in ('/', '/index.html'):
            path = '/ps2web.html'
        if path == '/disc.iso':
            return DISC
        if path.endswith('/'):
            path += 'index.html'
        name = os.path.normpath(path.lstrip('/'))
        if name.startswith('..') or name == '.':
            return None
        full = os.path.join(BUILD, name)
        return full if os.path.isfile(full) else None

    def headers_for(self, path, length, encoding=None):
        self.send_header('Content-Type', TYPES.get(os.path.splitext(path)[1], 'application/octet-stream'))
        if encoding:
            self.send_header('Content-Encoding', encoding)
            self.send_header('Vary', 'Accept-Encoding')
        self.send_header('Content-Length', str(length))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.end_headers()

    def do_HEAD(self):
        path = self.target()
        if not path:
            self.send_error(404)
            return
        self.send_response(200)
        self.headers_for(path, os.path.getsize(path))

    def do_GET(self):
        path = self.target()
        if not path:
            self.send_error(404)
            return
        packed = path + '.gz'
        if ('Range' not in self.headers and 'gzip' in self.headers.get('Accept-Encoding', '') and os.path.isfile(packed)
                and os.path.getmtime(packed) >= os.path.getmtime(path)):
            self.send_response(200)
            self.headers_for(path, os.path.getsize(packed), 'gzip')
            with open(packed, 'rb') as f:
                shutil.copyfileobj(f, self.wfile, 1 << 20)
            return
        size = os.path.getsize(path)
        match = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        start, end = 0, size - 1
        if match:
            start = int(match.group(1))
            end = min(int(match.group(2)) if match.group(2) else size - 1, size - 1)
            self.send_response(206)
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        else:
            self.send_response(200)
        self.headers_for(path, end - start + 1)
        with open(path, 'rb') as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def do_PUT(self):
        match = re.match(r'^/vu1/([0-9A-F]{16}_[0-9A-F]{4}\.bin)$', self.path.split('?', 1)[0])
        length = int(self.headers.get('Content-Length', '0'))
        if not RECORD or not match or length != 16384:
            self.send_error(501 if not RECORD else 400)
            return
        data = self.rfile.read(length)
        target = os.path.join(RECORD, match.group(1))
        if not os.path.exists(target):
            os.makedirs(RECORD, exist_ok=True)
            with open(target, 'wb') as f:
                f.write(data)
            print('recorded', match.group(1), flush=True)
        self.send_response(201)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


print('serving %s and %s on http://%s:%d/' % (BUILD, DISC, HOST, PORT))
http.server.ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()

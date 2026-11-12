#!/usr/bin/env python3
"""Serves a web build and a disc image with HTTP range requests.

usage: serve.py <build dir> <disc image> [port] [--host address] [--game <games/name>] [--record <dir for unrecorded VU1 programs>]

With --game the page's findings are kept for the next port.sh run: VU1 programs
the build lacks go to <games/name>/vu1 and entry points its interpreter met to
<games/name>/entries.txt.

It also relays the messages that connect a host page to the guests it invites
(/room), so a friend can play along from another browser.
"""

import http.server
import json
import os
import re
import secrets
import shutil
import sys
import threading
import time
import urllib.parse

RECORD = None
if '--record' in sys.argv:
    at = sys.argv.index('--record')
    RECORD = os.path.abspath(sys.argv[at + 1])
    del sys.argv[at:at + 2]
ENTRIES = None
if '--game' in sys.argv:
    at = sys.argv.index('--game')
    game = os.path.abspath(sys.argv[at + 1])
    del sys.argv[at:at + 2]
    RECORD = RECORD or os.path.join(game, 'vu1')
    ENTRIES = os.path.join(game, 'entries.txt')
ENTRY_LOCK = threading.Lock()
HOST = '127.0.0.1'
if '--host' in sys.argv:
    at = sys.argv.index('--host')
    HOST = sys.argv[at + 1]
    del sys.argv[at:at + 2]
BUILD = os.path.abspath(sys.argv[1])
DISC = os.path.abspath(sys.argv[2])
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8620
ROOMS = {}
ROOMS_CHANGED = threading.Condition()
ROOM_PATH = re.compile(r'^/room(?:/([A-Z2-9]{6})/([a-z0-9]{1,16}))?$')
ROOM_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
ROOM_IDLE = 3600
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

    def send_json(self, value, status=200):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def room_get(self, code, me):
        match = re.search(r'after=(\d+)', self.path)
        after = int(match.group(1)) if match else 0
        deadline = time.time() + 20
        with ROOMS_CHANGED:
            while True:
                room = ROOMS.get(code)
                if room is None:
                    self.send_json({'error': 'no such room'}, 404)
                    return
                room['seen'] = time.time()
                inbox = room['inboxes'].setdefault(me, [])
                left = deadline - time.time()
                if len(inbox) > after or left <= 0:
                    messages = inbox[after:]
                    break
                ROOMS_CHANGED.wait(left)
        self.send_json({'messages': messages, 'next': after + len(messages)})

    def do_POST(self):
        match = ROOM_PATH.match(self.path.split('?', 1)[0])
        length = int(self.headers.get('Content-Length', '0'))
        if not match or length > 65536:
            self.send_error(404 if not match else 413)
            return
        body = self.rfile.read(length)
        code, to = match.groups()
        with ROOMS_CHANGED:
            now = time.time()
            for old in [c for c, r in ROOMS.items() if now - r['seen'] > ROOM_IDLE]:
                del ROOMS[old]
            if not code:
                code = ''.join(secrets.choice(ROOM_LETTERS) for _ in range(6))
                while code in ROOMS:
                    code = ''.join(secrets.choice(ROOM_LETTERS) for _ in range(6))
                ROOMS[code] = {'inboxes': {}, 'seen': now}
                result = {'room': code}
            elif code in ROOMS:
                try:
                    message = json.loads(body)
                except ValueError:
                    message = None
                if not isinstance(message, dict):
                    self.send_json({'error': 'bad message'}, 400)
                    return
                ROOMS[code]['inboxes'].setdefault(to, []).append(message)
                ROOMS[code]['seen'] = now
                ROOMS_CHANGED.notify_all()
                result = {}
            else:
                self.send_json({'error': 'no such room'}, 404)
                return
        self.send_json(result)

    def do_HEAD(self):
        path = self.target()
        if not path:
            self.send_error(404)
            return
        self.send_response(200)
        self.headers_for(path, os.path.getsize(path))

    def do_GET(self):
        match = ROOM_PATH.match(self.path.split('?', 1)[0])
        if match and match.group(1):
            self.room_get(*match.groups())
            return
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
        path = urllib.parse.unquote(self.path.split('?', 1)[0])
        entry = re.match(r'^/entry/((?:[A-Za-z0-9_.-]{1,64}:)?[0-9a-f]{1,8})$', path)
        if entry:
            if not ENTRIES:
                self.send_error(501)
                return
            with ENTRY_LOCK:
                known = set(open(ENTRIES).read().split()) if os.path.exists(ENTRIES) else set()
                if entry.group(1) not in known:
                    with open(ENTRIES, 'a') as f:
                        f.write(entry.group(1) + '\n')
                    print('recorded entry point', entry.group(1), flush=True)
            self.send_response(201)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        match = re.match(r'^/vu1/([0-9A-F]{16}_[0-9A-F]{4}\.bin)$', path)
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

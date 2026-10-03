#!/usr/bin/env python3
"""Local Streamable HTTP adapter for the native MCP server (Python standard library).

The solver remains in C. Default backend connects to the app/server, so simulations
survive an HTTP client's disconnect. JSON responses; GET/SSE is intentionally unsupported.
This is a single-user development adapter, not an authenticated public deployment.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import select
import signal
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
MAX_BODY = 1 << 20
MAX_REPLY = 64 << 20
VERSIONS = {'2025-11-25', '2025-06-18', '2025-03-26', '2024-11-05'}


class Backend:
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, bufsize=0)
        self.lock = threading.Lock()
        self.pending = b''
        self.touched = time.monotonic()
        self.version = None

    def call(self, message):
        with self.lock:
            self.touched = time.monotonic()
            data = json.dumps(message, separators=(',', ':'), allow_nan=False).encode() + b'\n'
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
            if 'id' not in message:
                return None
            deadline = time.monotonic() + 90
            while b'\n' not in self.pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('native backend response exceeded 90 seconds')
                ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
                if not ready:
                    continue
                block = os.read(self.proc.stdout.fileno(), 65536)
                if not block:
                    raise BrokenPipeError('native backend exited')
                self.pending += block
                if len(self.pending) > MAX_REPLY:
                    raise ValueError('native response exceeded limit')
            line, self.pending = self.pending.split(b'\n', 1)
            reply = json.loads(line)
            if reply.get('id') != message['id']:
                raise ValueError('native response identity mismatch')
            return reply

    def close(self):
        # Never wait on an in-flight call's lock during shutdown.
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        finally:
            self.proc.stdin.close()
            self.proc.stdout.close()


class Adapter(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, command, max_sessions=8):
        self.command = command
        self.sessions = {}
        self.guard = threading.Lock()
        self.max_sessions = max_sessions
        super().__init__(address, Handler)

    def new_session(self):
        with self.guard:
            if len(self.sessions) >= self.max_sessions:
                return None, None
            sid = secrets.token_urlsafe(32)
            backend = Backend(self.command)
            self.sessions[sid] = backend
            return sid, backend

    def drop(self, sid):
        with self.guard:
            backend = self.sessions.pop(sid, None)
        if backend:
            backend.close()

    def server_close(self):
        with self.guard:
            sessions = list(self.sessions.values())
            self.sessions.clear()
        for backend in sessions:
            backend.close()
        super().server_close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, fmt, *args):
        # Log transport status only; never request payloads or session tokens.
        pass

    def send(self, status, body=None, sid=None):
        data = json.dumps(body, separators=(',', ':'), allow_nan=False).encode() if body is not None else b''
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        if body is not None:
            self.send_header('Content-Type', 'application/json')
        if sid:
            self.send_header('Mcp-Session-Id', sid)
        self.end_headers()
        if data:
            self.wfile.write(data)

    def boundary(self):
        self.connection.settimeout(15)
        if self.path != '/mcp':
            self.send(404, {'error': 'endpoint is /mcp'})
            return False
        host = self.headers.get('Host', '')
        try:
            parsed = urlsplit('http://' + host)
            valid = parsed.hostname in ('127.0.0.1', 'localhost') and parsed.port == self.server.server_port
        except ValueError:
            valid = False
        if not valid:
            self.send(403, {'error': 'untrusted host'})
            return False
        origin = self.headers.get('Origin')
        # Same-origin browser access only. Non-browser MCP clients omit Origin.
        if origin and origin != 'http://' + host:
            self.send(403, {'error': 'untrusted origin'})
            return False
        return True

    def do_GET(self):
        if self.boundary():
            self.send(405, {'error': 'SSE is not offered; use POST with JSON responses'})

    def do_DELETE(self):
        if not self.boundary():
            return
        sid = self.headers.get('Mcp-Session-Id')
        with self.server.guard:
            known = sid in self.server.sessions
        if not known:
            self.send(404, {'error': 'unknown session'})
            return
        self.server.drop(sid)
        self.send(204)

    def do_POST(self):
        if not self.boundary():
            self.close_connection = True
            return
        if self.headers.get('Content-Type', '').split(';')[0].strip() != 'application/json':
            self.send(415, {'error': 'Content-Type must be application/json'})
            self.close_connection = True
            return
        if self.headers.get('Transfer-Encoding'):
            self.send(400, {'error': 'use Content-Length'})
            self.close_connection = True
            return
        try:
            size = int(self.headers.get('Content-Length', '-1'))
        except ValueError:
            size = -1
        if not 0 <= size <= MAX_BODY:
            self.send(413 if size > MAX_BODY else 400, {'error': 'invalid message length'})
            self.close_connection = True
            return
        accept = self.headers.get('Accept', '')
        if 'application/json' not in accept or 'text/event-stream' not in accept:
            self.send(406, {'error': 'Accept must include application/json and text/event-stream'})
            self.close_connection = True
            return
        try:
            message = json.loads(self.rfile.read(size), parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite')))
        except (ValueError, UnicodeError):
            self.send(400, {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'invalid JSON'}})
            return
        if not isinstance(message, dict) or message.get('jsonrpc') != '2.0':
            self.send(400, {'error': 'one JSON-RPC object required'})
            return
        if 'id' in message and (isinstance(message['id'], bool) or not isinstance(message['id'], (int, str))):
            self.send(400, {'error': 'request id must be an integer or string'})
            return
        sid = self.headers.get('Mcp-Session-Id')
        if message.get('method') == 'initialize' and sid is None:
            if 'id' not in message:
                self.send(400, {'error': 'initialize requires a request id'})
                return
            sid, backend = self.server.new_session()
            if backend is None:
                self.send(429, {'error': 'session limit reached; DELETE idle sessions'})
                return
        else:
            with self.server.guard:
                backend = self.server.sessions.get(sid)
            if backend is None:
                self.send(404 if sid else 400, {'error': 'initialize a session first'})
                return
            version = self.headers.get('MCP-Protocol-Version')
            if version not in VERSIONS or version != backend.version:
                self.send(400, {'error': 'MCP-Protocol-Version must match the initialized session'})
                return
        try:
            reply = backend.call(message)
        except (BrokenPipeError, TimeoutError, ValueError, OSError):
            self.server.drop(sid)
            self.send(502, {'error': 'native backend unavailable; reconnect; mutation outcome may be unknown'})
            return
        if message.get('method') == 'initialize':
            if reply and 'result' in reply:
                backend.version = reply['result']['protocolVersion']
            else:
                self.server.drop(sid)
                self.send(400, reply)
                return
        self.send(200 if reply is not None else 202, reply, sid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--max-sessions', type=int, default=8)
    parser.add_argument('backend_args', nargs=argparse.REMAINDER,
                        help='after --: navier-mcp options; default --connect')
    args = parser.parse_args()
    if not 0 <= args.port <= 65535 or not 1 <= args.max_sessions <= 32:
        parser.error('port or max-sessions outside allowed bounds')
    opts = args.backend_args
    if opts[:1] == ['--']:
        opts = opts[1:]
    command = [str(ROOT / 'navier-mcp')] + (opts or ['--connect'])
    if not Path(command[0]).is_file():
        parser.error('build navier-mcp with make am first')
    server = Adapter(('127.0.0.1', args.port), command, args.max_sessions)
    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    print(f'OpenPhysicsAI local MCP: http://127.0.0.1:{server.server_port}/mcp', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()

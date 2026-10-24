#!/usr/bin/env python3
"""live_standin.py - a stand-in for OpenAI's live voice sessions, for testing the headset's provider voice without a
key, a network or a cost (tools/xr/live.py, tools/xr/talk_test.py).

    python3 tools/xr/live_standin.py PORT LOG

POST /v1/live/sessions answers a WebRTC session creation with a session id and an SDP answer; GET
/v1/live/sessions/{id}/attach is the sideband WebSocket. Everything received (the creation's headers and body, each
event the sideband sends) is a JSON line in LOG; POST /drive with an event in its body sends that event down the
sideband, as GPT-Live would (what the person said, a delegation, ...). It plays no voice and recognises nothing.
"""
import base64, hashlib, http.server, json, struct, sys, threading, time

PORT, LOG = int(sys.argv[1]), sys.argv[2]
SID = "sess_standin"
side = {"sock": None, "lock": threading.Lock()}


def mirror(offer):
    """an answer a browser accepts for its offer (the offer's own lines, the other side's role): no media flows, as no
    peer is there, but the page goes live and the rest can be tested"""
    out = []
    for line in offer.replace("\r\n", "\n").split("\n"):
        if line.startswith("a=setup:"):
            line = "a=setup:active"
        elif line.startswith("a=ice-ufrag:"):
            line = "a=ice-ufrag:stnd"
        elif line.startswith("a=ice-pwd:"):
            line = "a=ice-pwd:standinstandinstandin12"
        out.append(line)
    return "\r\n".join(out)


def log(**kv):
    with open(LOG, "a") as f:
        f.write(json.dumps(dict(kv, t=time.time())) + "\n")


def frame(text):
    data = text.encode()
    n = len(data)
    head = bytes([0x81]) + (bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n))
    return head + data


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))
        if self.path == "/v1/live/sessions":
            j = json.loads(body)
            log(event="create", authorization=self.headers.get("Authorization"), body=j)
            offer = (j.get("transport") or {}).get("sdp") or ""
            answer = mirror(offer) if "m=audio" in offer else "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=standin answer\r\n"
            return self.reply(200, {"id": SID, "object": "live.session", "transport": {"type": "webrtc", "sdp": answer}})
        if self.path == "/drive":
            with side["lock"]:
                sock = side["sock"]
                if sock:
                    sock.sendall(frame(body.decode()))
            return self.reply(200 if sock else 409, {"sent": bool(sock)})
        self.reply(404, {"error": "not found"})

    def do_GET(self):
        if self.path == "/v1/live/sessions/%s/attach" % SID and self.headers.get("Upgrade", "").lower() == "websocket":
            log(event="attach", authorization=self.headers.get("Authorization"))
            accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
            self.wfile.flush()
            with side["lock"]:
                side["sock"] = self.connection
            self.connection.sendall(frame(json.dumps({"type": "session.started", "session": {"id": SID, "model": "gpt-live-1"}})))
            rf = self.rfile
            while True:  # the client's frames: masked, logged
                try:
                    b0, b1 = rf.read(2)
                    n = b1 & 0x7F
                    if n == 126:
                        n = struct.unpack(">H", rf.read(2))[0]
                    elif n == 127:
                        n = struct.unpack(">Q", rf.read(8))[0]
                    mask = rf.read(4)
                    data = bytes(c ^ mask[i % 4] for i, c in enumerate(rf.read(n)))
                except (ValueError, OSError):
                    break
                if b0 & 0x0F == 8:
                    break
                if b0 & 0x0F == 1:
                    try:
                        log(event="client", data=json.loads(data))
                    except ValueError:
                        log(event="client", raw=data.decode("utf-8", "replace"))
            with side["lock"]:
                side["sock"] = None
            log(event="detach")
            self.close_connection = True
            return
        self.reply(404, {"error": "not found"})


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()

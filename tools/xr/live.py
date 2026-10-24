"""live.py - the provider's own voice in the headset (docs/xr.md, "The provider's voice").

OpenAI's GPT-Live (gpt-live-1, the full-duplex voice of ChatGPT Voice: it listens while it speaks, and the person can
talk over it) holds the conversation; every request about the simulation is delegated to the agent the app already
runs (Claude Code or Codex), and the agent's answer goes back for the voice to say. Anthropic offers no voice to
developers (Claude's apps speak through ElevenLabs, and Claude Code's /voice is dictation only), so for Claude this is
OpenAI's voice carrying Claude's answers; for Codex it is the provider's own.

The headset holds the audio over WebRTC (its microphone out, the voice in). This bridge alone holds the key: it trades
the headset's SDP offer for OpenAI's answer (POST /v1/live/sessions) and attaches a sideband WebSocket to the same
session (/v1/live/sessions/{id}/attach), where it reads what the person and the voice say and answers the voice's
delegations: the person's words since the last delegation become one question to the app's agent (the ask files),
quiet progress goes back as session.thinking.append, the finished answer as session.commentary.append.

The key: $OPENAI_API_KEY, else the macOS Keychain item "OpenPhysicsAI OpenAI" (security add-generic-password -a "$USER"
-s "OpenPhysicsAI OpenAI" -w). Tests point OPENPHYSICS_OPENAI_BASE at tools/xr/live_standin.py. Python's standard library
only: a small WebSocket client (RFC 6455) below; TLS is checked against the system's certificates (/etc/ssl/cert.pem),
as this Python's own store is empty.
"""
import base64, hashlib, json, os, socket, ssl, struct, subprocess, threading, time, urllib.error, urllib.parse, urllib.request

BASE = (os.environ.get("OPENPHYSICS_OPENAI_BASE") or os.environ.get("NAVIER_OPENAI_BASE") or "https://api.openai.com").rstrip("/")
MODEL = "gpt-live-1"
VOICE = os.environ.get("OPENPHYSICS_LIVE_VOICE") or os.environ.get("NAVIER_LIVE_VOICE") or "marin"
KEYCHAIN = "OpenPhysicsAI OpenAI"
ANSWER_CHARS = 1800  # session.commentary.append takes up to 500 tokens

INSTRUCTIONS = (
    "You are the voice of OpenPhysicsAI, a physics laboratory. The person wears a headset and sees a simulation result "
    "in front of them. A backend agent (Claude Code or Codex) can see what they see, read the values, change the view, "
    "mark places, and run or change simulations. Delegate to it every request about the result, its numbers, the "
    "physics, the app, or a simulation to run or change; never invent a value. While it works say a few words at most; "
    "when its result arrives, say it in one to three short sentences, with units in words. Plain view commands (play, "
    "pause, next frame, previous frame, show a field such as the temperature, cut along x, y or z, remove the cut, true "
    "size, bigger, smaller, show or hide the controls) are done by the headset itself: acknowledge them in two or three "
    "words and do not delegate them. If the person says stop, stop talking at once.")


def norm(words):
    """words compared as the app keeps them: case, punctuation and spacing aside"""
    return " ".join("".join(c.lower() if c.isalnum() else " " for c in words).split())


class LiveError(Exception):
    def __init__(self, message, status=502):
        super().__init__(message)
        self.status = status


_key_cache = {"at": 0.0, "key": None}


def api_key():
    """the OpenAI key, from the environment or the Keychain (looked up at most every 10 s); None without one"""
    k = os.environ.get("OPENAI_API_KEY", "").strip()
    if k:
        return k
    if (os.environ.get("OPENPHYSICS_LIVE_NO_KEYCHAIN") or os.environ.get("NAVIER_LIVE_NO_KEYCHAIN")) == "1":
        return None
    now = time.time()
    if now - _key_cache["at"] > 10:
        _key_cache["at"] = now
        try:
            r = subprocess.run(["security", "find-generic-password", "-s", KEYCHAIN, "-w"], capture_output=True, text=True, timeout=5)
            _key_cache["key"] = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None
        except (OSError, subprocess.TimeoutExpired):
            _key_cache["key"] = None
    return _key_cache["key"]


def tls():
    return ssl.create_default_context(cafile="/etc/ssl/cert.pem") if os.path.exists("/etc/ssl/cert.pem") else ssl.create_default_context()


def session_config():
    return {"model": MODEL, "instructions": INSTRUCTIONS, "audio": {"output": {"voice": VOICE}}, "delegation": {"type": "client"}}


def create_session(offer_sdp, key):
    """(answer SDP, session id): the headset's offer traded for OpenAI's answer (POST /v1/live/sessions)"""
    body = json.dumps({"session": session_config(), "transport": {"type": "webrtc", "sdp": offer_sdp}}).encode()
    req = urllib.request.Request(BASE + "/v1/live/sessions", data=body, method="POST",
                                 headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20, context=tls() if BASE.startswith("https") else None) as r:
            raw, ctype, location = r.read(), r.headers.get("Content-Type", ""), r.headers.get("Location", "")
    except urllib.error.HTTPError as e:
        detail = e.read()[:2000].decode("utf-8", "replace")
        try:
            detail = json.loads(detail).get("error", {}).get("message", detail)
        except ValueError:
            pass
        raise LiveError("OpenAI refused the voice session (%d): %s" % (e.code, detail[:300]), 502 if e.code >= 500 else 400)
    except (urllib.error.URLError, OSError) as e:
        raise LiveError("OpenAI could not be reached: %s" % e, 502)
    if "json" in ctype:
        j = json.loads(raw)
        answer = j.get("sdp") or (j.get("transport") or {}).get("sdp") or (j.get("answer") or {}).get("sdp")
        sid = j.get("id") or (j.get("session") or {}).get("id")
    else:  # an SDP body, the session named by its Location
        answer, sid = raw.decode("utf-8", "replace"), location.rstrip("/").split("/")[-1] or None
    if not answer or not sid:
        raise LiveError("OpenAI's answer has no SDP or no session id", 502)
    return answer, sid


class WebSocket:
    """a small client (RFC 6455): text messages in and out, pings answered, safe to send from several threads"""
    def __init__(self, url, headers, timeout=20):
        u = urllib.parse.urlsplit(url)
        secure = u.scheme == "wss"
        sock = socket.create_connection((u.hostname, u.port or (443 if secure else 80)), timeout=timeout)
        if secure:
            sock = tls().wrap_socket(sock, server_hostname=u.hostname)
        nonce = base64.b64encode(os.urandom(16)).decode()
        lines = ["GET %s HTTP/1.1" % (u.path + ("?" + u.query if u.query else "")), "Host: " + u.netloc, "Upgrade: websocket",
                 "Connection: Upgrade", "Sec-WebSocket-Key: " + nonce, "Sec-WebSocket-Version: 13"]
        lines += ["%s: %s" % kv for kv in headers.items()]
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = sock.recv(4096)
            if not chunk:
                raise LiveError("the voice session's sideband closed during its handshake")
            resp += chunk
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0].decode("latin-1")
        want = base64.b64encode(hashlib.sha1((nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        if " 101 " not in status + " " or want not in head:
            raise LiveError("the voice session's sideband was refused: " + status)
        sock.settimeout(None)
        self.sock, self.lock = sock, threading.Lock()

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        """the next text message, or None once the connection is closed"""
        parts = []
        while True:
            try:
                b0, b1 = self._read(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._read(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._read(8))[0]
                mask = self._read(4) if b1 & 0x80 else None
                data = self._read(n)
            except (OSError, ConnectionError, ValueError):
                return None
            if mask:
                data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
            op = b0 & 0x0F
            if op == 8:
                return None
            if op == 9:
                self._send(10, data)
                continue
            if op in (0, 1, 2):
                parts.append(data)
                if b0 & 0x80:
                    return b"".join(parts).decode("utf-8", "replace")

    def _send(self, op, data):
        n = len(data)
        head = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536
                                     else bytes([0x80 | 127]) + struct.pack(">Q", n))
        mask = os.urandom(4)
        with self.lock:
            self.sock.sendall(head + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

    def send(self, obj):
        self._send(1, json.dumps(obj).encode())

    def close(self):
        try:
            self._send(8, b"")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Live:
    """one voice session at a time: the sideband, what was said on both sides, and the delegations in flight"""
    def __init__(self, ask, bump):
        self.ask, self.bump = ask, bump  # ask(words, context): a question for the app's agent; bump(): the state changed
        self.lock = threading.Lock()
        self.ws, self.sid, self.error, self.started = None, None, None, 0.0
        self.you, self.ai, self.you_at, self.ai_at = [], [], 0.0, 0.0
        self.taken = 0          # input fragments already given to a delegation
        self.pending = {}       # delegation id -> {"words", "turn", "asked", "thought", "said"}
        self.handled = []       # (when, words): view commands the headset did itself
        self.context = ""       # what the headset shows and the person points at, from the page
        self.shown = ""         # the last description of the result given to the voice
        self.told = set()       # the runs that stopped without their record that the voice was told of (name@ended)
        self.seq = 0

    @property
    def active(self):
        return self.ws is not None

    def start(self, offer_sdp):
        key = api_key()
        if not key:
            raise LiveError("no OpenAI key: set OPENAI_API_KEY for the bridge, or store it in the Keychain as \"%s\"" % KEYCHAIN, 503)
        self.stop()
        answer, sid = create_session(offer_sdp, key)
        base = BASE.replace("https://", "wss://", 1).replace("http://", "ws://", 1)
        ws = WebSocket("%s/v1/live/sessions/%s/attach" % (base, urllib.parse.quote(sid)), {"Authorization": "Bearer " + key})
        with self.lock:
            self.ws, self.sid, self.error, self.started = ws, sid, None, time.time()
            self.you, self.ai, self.taken, self.pending, self.handled, self.shown = [], [], 0, {}, [], ""
        threading.Thread(target=self.read, args=(ws,), daemon=True).start()
        self.bump()
        return {"sdp": answer, "session": sid, "voice": VOICE}

    def stop(self):
        with self.lock:
            ws, self.ws = self.ws, None
        if ws:
            try:
                ws.send({"type": "session.close", "event_id": self.event_id("close")})
            except OSError:
                pass
            threading.Timer(1.0, ws.close).start()  # the service drains, then closes
            self.bump()

    def event_id(self, what):
        self.seq += 1
        return "%s_%d" % (what, self.seq)

    def send(self, kind, content, delegation_id=None):
        ws = self.ws
        if ws:
            try:
                ws.send({"type": "session.%s.append" % kind, "event_id": self.event_id(kind), "delegation_id": delegation_id,
                         "content": content[:ANSWER_CHARS]})
            except OSError as e:
                self.error = "the voice session's sideband failed: %s" % e

    def read(self, ws):
        while True:
            msg = ws.recv()
            if msg is None:
                with self.lock:
                    if self.ws is ws:
                        self.ws, self.error = None, self.error or "the voice session ended"
                self.bump()
                return
            try:
                ev = json.loads(msg)
            except ValueError:
                continue
            t = ev.get("type", "")
            with self.lock:
                if t == "session.input_transcript.delta":
                    self.you.append(ev.get("delta") or "")
                    self.you_at = time.time()
                elif t == "session.output_transcript.delta":
                    self.ai.append(ev.get("delta") or "")
                    self.ai_at = time.time()
                elif t == "session.closed":
                    self.error = "the voice session ended (%s)" % ev.get("reason", "closed")
                elif t == "error":
                    self.error = (ev.get("error") or {}).get("message") or "the voice reported an error"
            if t == "session.delegation.created":
                self.delegate((ev.get("delegation") or {}).get("id"))
            self.bump()

    def delegate(self, did):
        """the voice wants the agent: the person's words since the last delegation become one question"""
        if not did:
            return
        with self.lock:
            words = "".join(self.you[self.taken:]).strip()
            self.taken = len(self.you)
            done = [w for t, w in self.handled if time.time() - t < 3]
            for p in self.pending.values():  # a newer request supersedes an older one (the app interrupts that answer)
                p["said"] = True
        if done:  # a view command the headset has already done
            return self.send("commentary", "Done: %s." % done[-1], did)
        if not words:
            return self.send("commentary", "Sorry, I did not catch that. Could you say it again?", did)
        if len(words.split()) <= 6:  # perhaps a view command the headset is about to do itself: a moment for it to say so
            return threading.Thread(target=self.delegate_short, args=(did, words), daemon=True).start()
        self.hand_over(did, words)

    def delegate_short(self, did, words):
        end = time.time() + 1.2
        while time.time() < end:
            with self.lock:
                done = [w for t, w in self.handled if time.time() - t < 3]
            if done:
                return self.send("commentary", "Done: %s." % done[-1], did)
            time.sleep(0.05)
        self.hand_over(did, words)

    def hand_over(self, did, words):
        with self.lock:
            self.pending[did] = {"words": words, "key": norm(words), "asked": time.time(), "thought": 0.0, "said": False}
        self.ask(words, self.context)

    def cancel(self):
        """"stop": the answers still awaited are not spoken when they come"""
        with self.lock:
            for p in self.pending.values():
                p["said"] = True

    def handled_command(self, words):
        """the headset did a view command itself: the voice is told, so that it does not hand it to the agent"""
        with self.lock:
            self.handled.append((time.time(), words))
            self.handled = self.handled[-8:]
        self.send("thinking", "The headset itself just did what the person said: \"%s\". It is done; do not delegate it." % words)

    def on_state(self, st):
        """each new app state: a finished answer goes back to the voice (the turn asked with the delegation's words);
        progress while the agent works; what is shown"""
        ag = st.get("agent") or {}
        say, busy = ag.get("say") or {}, bool(ag.get("busy"))
        asked, now = norm(say.get("asked") or ""), time.time()
        with self.lock:
            if not self.ws:
                return
            items = list(self.pending.items())
        for did, p in items:
            if p["said"]:
                continue
            if asked == p["key"] and say.get("done") and not busy:
                p["said"] = True
                answer = (say.get("text") or "").strip() or "The agent finished without words."
                self.send("commentary", answer, did)
            elif now - p["asked"] > 3 and now - p["thought"] > 4:
                p["thought"] = now
                comp = ag.get("computing") or []
                lines = [l.get("text", "") for l in (ag.get("lines") or []) if l.get("kind") == 1]
                what = ("The simulation %s is computing%s." % (comp[0].get("name"), ", %d %% done" % round(100 * comp[0]["done"])
                                                                if comp[0].get("done") is not None else "") if comp
                        else "The agent is working: %s." % lines[-1] if lines else "The agent is still working on it.")
                self.send("thinking", what, did)
        with self.lock:
            self.pending = {k: v for k, v in self.pending.items() if not v["said"] or now - v["asked"] < 60}
        for r in ag.get("stopped") or []:  # a run that failed after the agent answered: the voice knows when asked
            key = "%s@%s" % (r.get("name"), r.get("ended"))
            if key not in self.told:
                self.told.add(key)
                self.send("thinking", "The simulation %s stopped before its end: %s." % (r.get("name"), r.get("why")))
        shown = "%s|%s" % (st.get("title", ""), ((st.get("lab") or {}).get("view") or {}).get("field") or (st.get("fem") or {}).get("field_name", ""))
        if shown != self.shown and st.get("has_scene"):
            self.shown = shown
            lab = st.get("lab") or {}
            field = (lab.get("view") or {}).get("field") or (st.get("fem") or {}).get("field_label") or ""
            self.send("thinking", "The headset now shows %s%s." % (st.get("title", "a result"), ", coloured by %s" % field if field else ""))

    def public(self):
        """for the page: is the voice live, and the last words on each side"""
        with self.lock:
            now = time.time()
            return {"active": self.ws is not None, "voice": VOICE, "error": self.error,
                    "you": "".join(self.you)[-300:], "ai": "".join(self.ai)[-500:], "you_n": sum(len(x) for x in self.you),
                    "you_s": round(now - self.you_at, 2) if self.you_at else None,
                    "ai_s": round(now - self.ai_at, 2) if self.ai_at else None,
                    "delegating": any(not p["said"] for p in self.pending.values())}

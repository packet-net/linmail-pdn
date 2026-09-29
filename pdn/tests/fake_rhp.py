"""A fake pdn RHPv2 server for testing linmail-pdn.

Speaks the same wire as pdn (two byte big endian length, then JSON; data as
Latin-1 strings) and answers the requests linmail-pdn makes. Every message in
either direction is recorded, and the test drives the "far end" by pushing
accept, status, recv and close messages.
"""

from __future__ import annotations

import json
import socket
import struct
import threading
import time


class FakeRhp:
    def __init__(self, auth: tuple[str, str] | None = None):
        self.auth = auth
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.port = self.listener.getsockname()[1]
        self.lock = threading.Condition()
        self.log: list[tuple[str, dict]] = []     # ("in" | "out", message)
        self.conn: socket.socket | None = None
        self.connections = 0
        self.authed = False
        self.next_handle = 100
        self.seqno = 0
        self.sent: dict[int, bytearray] = {}      # data linmail-pdn sent, per handle
        self.handles: dict[int, dict] = {}
        # Called for a stream open: return (errCode, errText). Default: accept.
        self.on_open = lambda msg: (0, "Ok")
        self.running = True
        threading.Thread(target=self._accept_loop, daemon=True).start()

    # -- plumbing ---------------------------------------------------------

    def close(self):
        self.running = False
        try:
            self.listener.close()
        except OSError:
            pass
        self.drop()

    def drop(self):
        """Close the current client connection, as a pdn restart would."""
        with self.lock:
            conn, self.conn = self.conn, None
        if conn:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with self.lock:
                self.conn = conn
                self.connections += 1
                for v in self.handles.values():
                    v["closed"] = True      # handles die with the connection
                self.authed = False
                self.seqno = 0
                self.lock.notify_all()
            threading.Thread(target=self._read_loop, args=(conn,), daemon=True).start()

    def _read_loop(self, conn):
        buf = b""
        while True:
            try:
                data = conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            buf += data
            while len(buf) >= 2:
                n = struct.unpack(">H", buf[:2])[0]
                if len(buf) < n + 2:
                    break
                msg = json.loads(buf[2:n + 2].decode("utf-8"))
                buf = buf[n + 2:]
                with self.lock:
                    self.log.append(("in", msg))
                    self.lock.notify_all()
                self._handle(msg)

    def _write(self, msg: dict):
        body = json.dumps(msg).encode("utf-8")
        with self.lock:
            conn = self.conn
            self.log.append(("out", msg))
            self.lock.notify_all()
        if conn:
            try:
                conn.sendall(struct.pack(">H", len(body)) + body)
            except OSError:
                pass            # linmail-pdn has gone; the test will notice

    def _new_handle(self) -> int:
        with self.lock:
            self.next_handle += 1
            return self.next_handle

    def _next_seqno(self) -> int:
        with self.lock:
            s = self.seqno
            self.seqno += 1
            return s

    # -- the server side of the protocol -----------------------------------

    def _reply(self, msg, **fields):
        reply = {"type": msg["type"] + "Reply"}
        if "id" in msg:
            reply["id"] = msg["id"]
        reply.update(fields)
        reply.setdefault("errCode", 0)
        reply.setdefault("errText", "Ok")
        self._write(reply)

    def _handle(self, msg):
        t = msg.get("type")

        if t == "auth":
            ok = self.auth is not None and (msg.get("user"), msg.get("pass")) == self.auth
            self.authed = ok
            self._reply(msg, errCode=0 if ok else 14, errText="Ok" if ok else "Unauthorised")
            return

        if self.auth is not None and not self.authed:
            self._reply(msg, errCode=14, errText="Unauthorised")
            return

        if t == "socket":
            h = self._new_handle()
            self.handles[h] = {"kind": "socket"}
            self._reply(msg, handle=h)
        elif t == "bind":
            self.handles[msg["handle"]]["local"] = msg.get("local")
            self._reply(msg, handle=msg["handle"])
        elif t == "listen":
            self.handles[msg["handle"]]["listening"] = True
            self._reply(msg, handle=msg["handle"])
        elif t == "open" and msg.get("mode") == "dgram":
            h = self._new_handle()
            self.handles[h] = {"kind": "dgram", "local": msg.get("local")}
            self._reply(msg, handle=h)
        elif t == "open":
            err, text = self.on_open(msg)
            if err:
                self._reply(msg, errCode=err, errText=text)
            else:
                h = self._new_handle()
                self.handles[h] = {"kind": "stream", "remote": msg.get("remote")}
                self.sent[h] = bytearray()
                self._reply(msg, handle=h)
                self.push({"type": "status", "handle": h, "flags": 3})
        elif t == "send":
            h = msg["handle"]
            self.sent.setdefault(h, bytearray()).extend(msg["data"].encode("latin-1"))
            with self.lock:
                self.lock.notify_all()
            self._reply(msg, handle=h, status=2)
        elif t == "sendto":
            self._reply(msg, handle=msg["handle"])
        elif t == "close":
            self.handles.setdefault(msg["handle"], {})["closed"] = True
            self._reply(msg, handle=msg["handle"])
        else:
            self._reply(msg, errCode=2, errText="Unknown type")

    # -- what tests use -----------------------------------------------------

    def push(self, msg: dict):
        msg = dict(msg)
        msg["seqno"] = self._next_seqno()
        self._write(msg)

    def accept(self, remote: str, local: str = "N0LMB", port: str = "bpq") -> int:
        """A station connects to a callsign linmail-pdn listens on."""
        listener = self.wait(lambda: next((h for h, v in list(self.handles.items())
                                           if v.get("listening") and not v.get("closed")
                                           and v.get("local") == local), None),
                             what=f"a listener on {local}")
        child = self._new_handle()
        self.handles[child] = {"kind": "stream", "remote": remote}
        self.sent[child] = bytearray()
        self.push({"type": "accept", "handle": listener, "child": child,
                   "remote": remote, "local": local, "port": port})
        self.push({"type": "status", "handle": child, "flags": 3})
        return child

    def recv(self, handle: int, data: bytes | str):
        """The far end sends data."""
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.push({"type": "recv", "handle": handle, "data": data.decode("latin-1")})

    def hangup(self, handle: int):
        """The far end disconnects."""
        self.push({"type": "close", "handle": handle})

    def wait(self, pred, timeout: float = 20.0, what: str = "condition"):
        end = time.time() + timeout
        with self.lock:
            while True:
                result = pred()
                if result:
                    return result
                left = end - time.time()
                if left <= 0:
                    raise AssertionError(f"timed out waiting for {what}")
                self.lock.wait(min(left, 0.2))

    def wait_msg(self, type_: str, timeout: float = 20.0, direction: str = "in", **match) -> dict:
        def find():
            for d, m in self.log:
                if d == direction and m.get("type") == type_ and all(m.get(k) == v for k, v in match.items()):
                    return m
            return None
        return self.wait(find, timeout, f"{direction} {type_} {match}")

    def messages(self, type_: str, direction: str = "in") -> list[dict]:
        with self.lock:
            return [m for d, m in self.log if d == direction and m.get("type") == type_]

    def wait_text(self, handle: int, text: str | bytes, timeout: float = 20.0, start: int = 0) -> int:
        """Wait until linmail-pdn has sent text on handle; returns the offset after it."""
        needle = text.encode("latin-1") if isinstance(text, str) else text

        def find():
            i = bytes(self.sent.get(handle, b"")).find(needle, start)
            return i + len(needle) if i >= 0 else None
        return self.wait(find, timeout, f"{text!r} on handle {handle}")

    def closed(self, handle: int) -> bool:
        return bool(self.handles.get(handle, {}).get("closed"))

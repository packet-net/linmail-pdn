"""linmail-pdn against a real pdn node and a real LinBPQ, all on loopback.

Needs PDN_BIN (the packetnet executable, or packetnet.dll to run with dotnet)
and LINBPQ_BIN (a linbpq built from this tree). Skipped without them.

pdn (N0PDN) has one AXUDP port, "bpq", to the LinBPQ node (N0BPQ, BBS
N0BPQ-1). linmail-pdn (BBS N0LMB) reaches pdn over RHPv2. A user logs in to
LinBPQ, connects to N0LMB through pdn and posts, lists and reads a message;
then one message is forwarded each way between the two BBSes with FBB B2
compression.
"""

from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from conftest import BIN, linmail_cfg, partner

PDN_BIN = os.environ.get("PDN_BIN")
LINBPQ_BIN = os.environ.get("LINBPQ_BIN")

pytestmark = pytest.mark.skipif(not (PDN_BIN and LINBPQ_BIN and BIN.exists()),
                                reason="needs PDN_BIN, LINBPQ_BIN and a built linmail-pdn")


def free_port(kind=socket.SOCK_STREAM) -> int:
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port(port: int, timeout: float = 60):
    end = time.time() + timeout
    while time.time() < end:
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.5)
    raise AssertionError(f"nothing listening on {port}")


def wait_file(path: Path, pattern: str, timeout: float = 60) -> re.Match:
    end = time.time() + timeout
    while time.time() < end:
        for p in (path.parent.glob(path.name) if "*" in path.name else [path]):
            if p.exists():
                m = re.search(pattern, p.read_bytes().decode("latin-1"))
                if m:
                    return m
        time.sleep(0.5)
    raise AssertionError(f"timed out waiting for {pattern!r} in {path}")


class TelnetUser:
    """A user on the LinBPQ telnet port."""

    def __init__(self, port: int, transcript: Path):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.sock.settimeout(0.5)
        self.buf = b""
        self.log = open(transcript, "a")

    def expect(self, pattern: str, timeout: float = 60) -> str:
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern.encode(), self.buf, re.I)
            if m:
                out, self.buf = self.buf[:m.end()], self.buf[m.end():]
                text = out.decode("latin-1").replace("\r\n", "\n").replace("\r", "\n")
                self.log.write(text)
                self.log.flush()
                return text
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                self.buf += data
            except socket.timeout:
                pass
        raise AssertionError(f"timed out waiting for {pattern!r}; have {self.buf[-500:]!r}")

    def send(self, line: str):
        self.log.write(f"[SEND] {line}\n")
        self.sock.sendall(line.encode() + b"\r\n")

    def close(self):
        self.sock.close()
        self.log.close()


def session(port, transcript, connect, to, title, read_back=True):
    u = TelnetUser(port, transcript)
    try:
        u.expect("user:")
        u.send("test")
        u.expect("password:")
        u.send("test")
        u.expect(r"\n")
        time.sleep(1)
        u.send(connect)
        banner = u.expect(r">\s*$", 90)
        if re.search(r"enter your name", banner, re.I):
            # A BBS with a default linmail.cfg asks a new user's name first
            u.send("Tester")
            u.expect(r">\s*$", 30)
        u.send(f"SP {to}")
        u.expect("Title", 30)
        u.send(title)
        u.expect("Message", 30)
        u.send("Hello from the real pdn test.")
        u.send("Second line.")
        u.send("/EX")
        saved = u.expect(r">\s*$", 30)
        num = re.search(r"Message: (\d+)", saved).group(1)
        if read_back:
            u.send("L")
            listing = u.expect(r">\s*$", 30)
            assert title in listing
            u.send(f"R {num}")
            text = u.expect(r">\s*$", 30)
            assert "Second line." in text
        u.send("B")
        time.sleep(2)
        return num
    finally:
        u.close()


PDN_YAML = """\
schemaVersion: 2
identity:
  callsign: N0PDN
  alias: PDNLAB
ports:
  - id: bpq
    enabled: true
    transport:
      kind: axudp
      host: 127.0.0.1
      port: {bpq_udp}
      localPort: {pdn_udp}
    link:
      dial: v20
management:
  telnet:
    enabled: true
    bind: 127.0.0.1
    port: {pdn_telnet}
  http:
    bind: 127.0.0.1
    port: {pdn_http}
  auth:
    enabled: false
rhp:
  enabled: true
  bind: 127.0.0.1
  port: {rhp}
  requireAuth: {require_auth}
"""

BPQ32_CFG = """\
SIMPLE=1
NODECALL=N0BPQ
NODEALIAS=BPQLAB
LOCATOR=NONE
APPLICATIONS=BBS
BBSCALL=N0BPQ-1
BBSALIAS=BBS
APPL1CALL=N0BPQ-1
APPL1ALIAS=BBS

PORT
 ID=Telnet
 DRIVER=Telnet
 CONFIG
 TCPPORT={bpq_telnet}
 HTTPPORT={bpq_http}
 MAXSESSIONS=10
 USER=test,test,N0USR,,SYSOP
ENDPORT

PORT
 ID=AXIP to pdn
 DRIVER=BPQAXIP
 QUALITY=0
 MINQUAL=0
 CONFIG
 UDP {bpq_udp}
 MAP N0PDN 127.0.0.1 UDP {pdn_udp}
 MAP N0LMB 127.0.0.1 UDP {pdn_udp}
ENDPORT
"""


def test_real_pdn(tmp_path):
    ports = {k: free_port() for k in ("pdn_telnet", "pdn_http", "rhp", "bpq_telnet", "bpq_http")}
    ports.update(bpq_udp=free_port(socket.SOCK_DGRAM), pdn_udp=free_port(socket.SOCK_DGRAM))

    pdn_dir, bpq_dir, lm_dir = tmp_path / "pdn", tmp_path / "linbpq", tmp_path / "linmail-pdn"
    for d in (pdn_dir, bpq_dir, lm_dir):
        d.mkdir()

    (pdn_dir / "packetnet.yaml").write_text(PDN_YAML.format(require_auth="false", **ports))
    (bpq_dir / "bpq32.cfg").write_text(BPQ32_CFG.format(**ports))
    (bpq_dir / "linmail.cfg").write_text(
        linmail_cfg(bbs="N0BPQ", partners=[partner(call="N0LMB", script=("C 2 N0LMB",))]))
    ui = "UIPort1:\n{\n  Enabled = 1;\n  SendMF = 1;\n  SendHDDR = 1;\n  SendNull = 0;\n};\n"
    (lm_dir / "linmail.cfg").write_text(linmail_cfg(partners=[partner()], main_extra={"EnableUI": 1}, groups=ui))

    pdn_cmd = ["dotnet", PDN_BIN] if PDN_BIN.endswith(".dll") else [PDN_BIN]
    procs = []
    try:
        procs.append(subprocess.Popen(
            pdn_cmd + ["--config", str(pdn_dir / "packetnet.yaml"), "--db", str(pdn_dir / "pdn.db")],
            cwd=pdn_dir, stdin=subprocess.DEVNULL, stdout=open(pdn_dir / "pdn.log", "wb"),
            stderr=subprocess.STDOUT))
        procs.append(subprocess.Popen(
            [LINBPQ_BIN, "mail"], cwd=bpq_dir, stdin=subprocess.DEVNULL,
            stdout=open(bpq_dir / "linbpq.log", "wb"), stderr=subprocess.STDOUT))
        wait_port(ports["rhp"], 120)
        wait_port(ports["bpq_telnet"], 60)

        procs.append(subprocess.Popen(
            [str(BIN), "-d", str(lm_dir), "-r", f"127.0.0.1:{ports['rhp']}", "-c", "N0LMB",
             "-n", "N0PDN", "-m", "1=bpq", "-t"],
            cwd=lm_dir, stdin=subprocess.DEVNULL, stdout=open(lm_dir / "stdout.log", "wb"),
            stderr=subprocess.STDOUT))
        wait_file(lm_dir / "stdout.log", r"Listening for connects to N0LMB")

        # 0. UI header broadcasts go out as RHP datagrams and pdn takes them
        wait_file(lm_dir / "stdout.log", r'"type":"sendtoReply"[^}]*"errCode":0', 30)

        # 1. A user session over AX.25 through pdn
        session(ports["bpq_telnet"], tmp_path / "user.txt", "C 2 N0LMB", "N0ABC", "Real pdn user test")
        wait_file(lm_dir / "logs" / "log_*_BBS.txt", r"Incoming Connect from N0USR")

        # 2. linmail-pdn forwards to LinBPQ
        session(ports["bpq_telnet"], tmp_path / "post-out.txt", "C 2 N0LMB", "N0ABC @ N0BPQ",
                "Real pdn forward out", read_back=False)
        crc = wait_file(lm_dir / "logs" / "log_*_BBS.txt",
                        r"Compressed Message Comp Len \d+ Msg Len \d+ CRC (\w+)", 90).group(1)
        wait_file(bpq_dir / "logs" / "log_*_BBS.txt", rf"Uncompressing Message .* CRC {crc}", 90)

        # 3. LinBPQ forwards to linmail-pdn
        session(ports["bpq_telnet"], tmp_path / "post-in.txt", "BBS", "N0XYZ @ N0LMB",
                "Real pdn forward in", read_back=False)
        crc = wait_file(bpq_dir / "logs" / "log_*_BBS.txt",
                        r"Compressed Message Comp Len \d+ Msg Len \d+ CRC (\w+)", 90).group(1)
        wait_file(lm_dir / "logs" / "log_*_BBS.txt", rf"Uncompressing Message .* CRC {crc}", 90)
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()
        for log in [tmp_path / "user.txt", lm_dir / "stdout.log"] + \
                sorted(lm_dir.glob("logs/log_*_BBS.txt")) + sorted(bpq_dir.glob("logs/log_*_BBS.txt")):
            if log.exists():
                print(f"===== {log.relative_to(tmp_path)}")
                print(log.read_bytes().decode("latin-1")[-6000:])


def test_real_pdn_rhp_auth(tmp_path):
    """With rhp.requireAuth on, linmail-pdn logs in with a pdn user."""
    ports = {k: free_port() for k in ("pdn_telnet", "pdn_http", "rhp")}
    ports.update(bpq_udp=free_port(socket.SOCK_DGRAM), pdn_udp=free_port(socket.SOCK_DGRAM))
    pdn_dir = tmp_path / "pdn"
    pdn_dir.mkdir()
    (pdn_dir / "packetnet.yaml").write_text(PDN_YAML.format(require_auth="true", **ports))

    pdn_cmd = ["dotnet", PDN_BIN] if PDN_BIN.endswith(".dll") else [PDN_BIN]
    procs = []
    try:
        procs.append(subprocess.Popen(
            pdn_cmd + ["--config", str(pdn_dir / "packetnet.yaml"), "--db", str(pdn_dir / "pdn.db")],
            cwd=pdn_dir, stdin=subprocess.DEVNULL, stdout=open(pdn_dir / "pdn.log", "wb"),
            stderr=subprocess.STDOUT))
        wait_port(ports["rhp"], 120)
        wait_port(ports["pdn_http"], 60)

        # Claim the node: the first admin user is also an RHP login
        body = json.dumps({"identity": {"callsign": "N0PDN"},
                           "admin": {"username": "linmail", "password": "linmail-pdn-test-pass"}}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{ports['pdn_http']}/api/v1/setup", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as resp:
            assert resp.status in (200, 201)

        for name, password, expect in [("good", "linmail-pdn-test-pass", r"Listening for connects to N0LMB"),
                                       ("bad", "wrong-password-here", r"RHP auth failed: 14")]:
            lm_dir = tmp_path / name
            lm_dir.mkdir()
            (lm_dir / "linmail.cfg").write_text(linmail_cfg())
            (lm_dir / "linmail-pdn.conf").write_text(
                f"rhp = 127.0.0.1:{ports['rhp']}\nrhp_user = linmail\nrhp_pass = {password}\n")
            p = subprocess.Popen([str(BIN), "-d", str(lm_dir), "-t"], cwd=lm_dir, stdin=subprocess.DEVNULL,
                                 stdout=open(lm_dir / "stdout.log", "wb"), stderr=subprocess.STDOUT)
            procs.append(p)
            try:
                wait_file(lm_dir / "stdout.log", expect, 30)
            finally:
                p.send_signal(signal.SIGTERM)
                p.wait(15)
                print(f"===== {name}")
                print((lm_dir / "stdout.log").read_bytes().decode("latin-1")[-3000:])
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()


def api(port, method, path, body=None, token=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=headers,
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            text = r.read().decode("latin-1")
            return r.status, text
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("latin-1")


PACKAGE_PDN_YAML = """\
schemaVersion: 2
identity:
  callsign: N0PDN
ports: []
management:
  telnet:
    enabled: false
  http:
    bind: 127.0.0.1
    port: {pdn_http}
  auth:
    enabled: true
rhp:
  enabled: true
  bind: 127.0.0.1
  port: {rhp}
appPackageRoots:
  - {apps}
apps:
  - id: linmail
    enabled: true
    callsign: N0LMB
"""


def test_real_pdn_app_package_and_gateway(tmp_path):
    """pdn discovers the app package, starts linmail-pdn itself, and serves its
    web pages through the app gateway with the viewer's identity."""
    ports = {k: free_port() for k in ("pdn_http", "rhp", "web")}

    # The package: manifest (web port changed for the test), binary and templates.
    # Kept under a short path: the mail code's log file names must fit in 100 bytes.
    import tempfile
    tmp_path = Path(tempfile.mkdtemp(prefix="lmpkg", dir="/tmp"))
    pkg = tmp_path / "apps" / "linmail"
    pkg.mkdir(parents=True)
    repo = BIN.parent.parent
    manifest = (BIN.parent / "packaging" / "pdn-app.yaml").read_text()
    manifest = manifest.replace("@VERSION@", "test").replace("127.0.0.1:18095", f"127.0.0.1:{ports['web']}")
    manifest = manifest.replace("command: ./linmail-pdn", f"command: ./linmail-pdn\n  args: [-W, \"{ports['web']}\", -t]")
    (pkg / "pdn-app.yaml").write_text(manifest)
    (pkg / "linmail-pdn").symlink_to(BIN)
    (pkg / "HTML").symlink_to(repo / "HTML")

    # Its state (overridden package roots put it in <package>/state): one user
    state = pkg / "state"
    state.mkdir()
    (state / "linmail.cfg").write_text(linmail_cfg())
    subprocess.run([str(BIN), "-d", str(state), "--adduser", "N0USR", "x", "FALSE"], check=True,
                   stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL)

    pdn_dir = tmp_path / "pdn"
    pdn_dir.mkdir()
    (pdn_dir / "packetnet.yaml").write_text(PACKAGE_PDN_YAML.format(apps=tmp_path / "apps", **ports))
    pdn_cmd = ["dotnet", PDN_BIN] if PDN_BIN.endswith(".dll") else [PDN_BIN]
    pdn = subprocess.Popen(
        pdn_cmd + ["--config", str(pdn_dir / "packetnet.yaml"), "--db", str(pdn_dir / "pdn.db")],
        cwd=pdn_dir, stdin=subprocess.DEVNULL, stdout=open(pdn_dir / "pdn.log", "wb"), stderr=subprocess.STDOUT)
    try:
        wait_port(ports["pdn_http"], 120)

        # Claim the node, then a second, read-only user named after a callsign
        status, _ = api(ports["pdn_http"], "POST", "/api/v1/setup",
                        {"identity": {"callsign": "N0PDN"}, "admin": {"username": "sysop", "password": "linmail-pdn-test-pass"}})
        assert status in (200, 201)
        status, text = api(ports["pdn_http"], "POST", "/api/v1/auth/login", {"username": "sysop", "password": "linmail-pdn-test-pass"})
        assert status == 200, text
        admin = json.loads(text)["token"]
        status, text = api(ports["pdn_http"], "POST", "/api/v1/users",
                           {"username": "N0USR", "password": "n0usr-test-password", "scope": "read"}, admin)
        assert status in (200, 201), text
        status, text = api(ports["pdn_http"], "POST", "/api/v1/auth/login", {"username": "N0USR", "password": "n0usr-test-password"})
        user = json.loads(text)["token"]

        # pdn started linmail-pdn, which bound the callsign pdn gave it
        wait_file(pdn_dir / "pdn.log", r"listening on N0LMB", 60)
        wait_port(ports["web"], 30)

        # Straight to the upstream, without the gateway: refused
        status, _ = api(ports["web"], "GET", "/WebMail")
        assert status == 403

        # Through the gateway as the admin: the sysop, with the management pages
        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/", token=admin)
        assert status == 200 and "BBS user N0LMB" in body and "Mail management" in body, body
        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/Mail/Header", token=admin)
        assert status == 200
        key = re.search(r"/apps/linmail/Mail/Status\?(M[0-9A-F]+)", body).group(1)
        status, body = api(ports["pdn_http"], "GET", f"/apps/linmail/Mail/Users?{key}", token=admin)
        assert status == 200 and "UserList.txt" in body
        status, body = api(ports["pdn_http"], "POST", f"/apps/linmail/Mail/UserList.txt?{key}", body={}, token=admin)
        assert status == 200 and "N0USR" in body, body

        # As N0USR: their own webmail, and no management pages
        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/WebMail", token=user)
        assert status == 200 and "User N0USR" in body and 'src="/apps/linmail/WebMail/webscript.js"' in body
        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/Mail/Header", token=user)
        assert status == 403 and "admin rights" in body
    finally:
        pdn.send_signal(signal.SIGTERM)
        try:
            pdn.wait(20)
        except subprocess.TimeoutExpired:
            pdn.kill()
        print((pdn_dir / "pdn.log").read_bytes().decode("latin-1")[-8000:])
        import shutil
        shutil.rmtree(tmp_path, ignore_errors=True)

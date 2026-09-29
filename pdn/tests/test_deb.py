"""The pdn-linmail .deb, installed for real and run by a real pdn node.

Needs LINMAIL_PDN_DEB (the .deb to install), PDN_BIN and LINBPQ_BIN, and
passwordless sudo. Installs the package, lets pdn discover it in
/usr/share/packetnet/apps/linmail and start it with state in
/var/lib/packetnet/apps/linmail, checks a user session over AX.25 from a
LinBPQ node and webmail through pdn's app gateway, then removes the package
and anything it created. Skipped without the variables.
"""

from __future__ import annotations

import json
import os
import re
import signal
import socket
import subprocess
import urllib.request
from pathlib import Path

import pytest

from test_real_pdn import (BPQ32_CFG, LINBPQ_BIN, PDN_BIN, api, free_port, session, wait_file,
                           wait_port)

DEB = os.environ.get("LINMAIL_PDN_DEB")

pytestmark = pytest.mark.skipif(not (DEB and PDN_BIN and LINBPQ_BIN),
                                reason="needs LINMAIL_PDN_DEB, PDN_BIN and LINBPQ_BIN")

APP_DIR = Path("/usr/share/packetnet/apps/linmail")
STATE_DIR = Path("/var/lib/packetnet/apps/linmail")

DEB_PDN_YAML = """\
schemaVersion: 2
identity:
  callsign: N0PDN
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
apps:
  - id: linmail
    enabled: true
    callsign: N0LMB-1
"""


def sudo(*args, check=True):
    return subprocess.run(["sudo", "-n", *args], check=check, capture_output=True, text=True)


def test_deb_install_and_run(tmp_path):
    for port in (18095,):
        with socket.socket() as s:
            assert s.connect_ex(("127.0.0.1", port)) != 0, f"port {port} (the packaged web port) is in use"

    made_var = not Path("/var/lib/packetnet").exists()
    made_share = not Path("/usr/share/packetnet").exists()
    ports = {k: free_port() for k in ("pdn_http", "rhp", "bpq_telnet", "bpq_http")}
    ports.update(bpq_udp=free_port(socket.SOCK_DGRAM), pdn_udp=free_port(socket.SOCK_DGRAM))

    procs = []
    try:
        out = sudo("dpkg", "-i", DEB)
        print(out.stdout)
        assert (APP_DIR / "linmail-pdn").exists() and (APP_DIR / "pdn-app.yaml").exists()
        assert (APP_DIR / "HTML" / "WebMailPage.txt").exists()

        # pdn runs as this user here, not as packetnet: give it the state dir
        sudo("install", "-d", "-o", str(os.getuid()), "-g", str(os.getgid()), "-m", "0750", str(STATE_DIR))

        pdn_dir, bpq_dir = tmp_path / "pdn", tmp_path / "linbpq"
        pdn_dir.mkdir()
        bpq_dir.mkdir()
        (pdn_dir / "packetnet.yaml").write_text(DEB_PDN_YAML.format(**ports))
        (bpq_dir / "bpq32.cfg").write_text(BPQ32_CFG.format(**ports).replace("MAP N0LMB ", "MAP N0LMB-1 "))

        pdn_cmd = ["dotnet", PDN_BIN] if PDN_BIN.endswith(".dll") else [PDN_BIN]
        procs.append(subprocess.Popen(
            pdn_cmd + ["--config", str(pdn_dir / "packetnet.yaml"), "--db", str(pdn_dir / "pdn.db")],
            cwd=pdn_dir, stdin=subprocess.DEVNULL, stdout=open(pdn_dir / "pdn.log", "wb"), stderr=subprocess.STDOUT))
        procs.append(subprocess.Popen(
            [LINBPQ_BIN], cwd=bpq_dir, stdin=subprocess.DEVNULL,
            stdout=open(bpq_dir / "linbpq.log", "wb"), stderr=subprocess.STDOUT))

        # pdn found the installed package, started it, and it bound the callsign pdn gave it
        wait_file(pdn_dir / "pdn.log", r"listening on N0LMB-1", 120)
        wait_port(18095, 30)
        assert (STATE_DIR / "linmail.cfg").exists(), "first start writes a default linmail.cfg"
        assert 'BBSName = "N0LMB"' in (STATE_DIR / "linmail.cfg").read_text()

        # A user connects over AX.25 from the LinBPQ node, through pdn
        wait_port(ports["bpq_telnet"], 60)
        # (The default linmail.cfg holds a new user's mail, so the user can't list it yet)
        session(ports["bpq_telnet"], tmp_path / "user.txt", "C 2 N0LMB-1", "N0ABC", "Deb install test",
                read_back=False)
        wait_file(STATE_DIR / "logs" / "log_*_BBS.txt", r"Incoming Connect from N0USR")
        wait_file(STATE_DIR / "logs" / "log_*_BBS.txt", r"Routing Trace To N0ABC")

        # Webmail through the app gateway, as a pdn user named after that callsign
        status, _ = api(ports["pdn_http"], "POST", "/api/v1/setup",
                        {"identity": {"callsign": "N0PDN"}, "admin": {"username": "sysop", "password": "linmail-pdn-test-pass"}})
        assert status in (200, 201)
        _, text = api(ports["pdn_http"], "POST", "/api/v1/auth/login", {"username": "sysop", "password": "linmail-pdn-test-pass"})
        admin = json.loads(text)["token"]
        status, text = api(ports["pdn_http"], "POST", "/api/v1/users",
                           {"username": "N0USR", "password": "n0usr-test-password", "scope": "read"}, admin)
        assert status in (200, 201), text
        _, text = api(ports["pdn_http"], "POST", "/api/v1/auth/login", {"username": "N0USR", "password": "n0usr-test-password"})
        user = json.loads(text)["token"]

        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/WebMail", token=user)
        assert status == 200 and "User N0USR" in body
        key = re.search(r"WMB\?(W[0-9A-F]+)", body).group(1)

        # Compose in webmail: a multipart form post through the gateway
        boundary = "linmailpdntest"
        fields = {"To": "N0ABC", "Subj": "Webmail through pdn", "Type": "P", "BID": "",
                  "Msg": "Sent from webmail behind the pdn app gateway", "Send": "Send"}
        form = "".join(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n"
                       for k, v in fields.items()) + f"--{boundary}--\r\n"
        req = urllib.request.Request(f"http://127.0.0.1:{ports['pdn_http']}/apps/linmail/WebMail/EMSave?{key}",
                                     data=form.encode(), method="POST",
                                     headers={"Authorization": f"Bearer {user}",
                                              "Content-Type": f"multipart/form-data; boundary={boundary}"})
        with urllib.request.urlopen(req, timeout=30) as r:
            assert r.status == 200
        wait_file(STATE_DIR / "logs" / "log_*_BBS.txt", r"Webmail Connect from N0USR through pdn")
        assert len(re.findall(r"Routing Trace To N0ABC",
                              "".join(p.read_text(errors="replace") for p in (STATE_DIR / "logs").glob("log_*_BBS.txt")))) == 2

        status, body = api(ports["pdn_http"], "GET", "/apps/linmail/", token=admin)
        assert status == 200 and "Mail management" in body

        status, text = api(ports["pdn_http"], "GET", "/api/v1/apps/packages", token=admin)
        pkg = next(p for p in json.loads(text) if p["id"] == "linmail")
        print("pdn sees:", {k: pkg.get(k) for k in ("id", "name", "version", "source", "service", "state", "callsign", "command")})
        assert pkg["state"] == "Running" and pkg["callsign"] == "N0LMB-1"
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(20)
            except subprocess.TimeoutExpired:
                p.kill()
        for log in [tmp_path / "user.txt", tmp_path / "pdn" / "pdn.log"]:
            if log.exists():
                print(f"===== {log.name}")
                print(log.read_bytes().decode("latin-1")[-6000:])
        sudo("dpkg", "-r", "pdn-linmail", check=False)
        sudo("rm", "-rf", str(STATE_DIR), check=False)
        if made_var:
            sudo("rm", "-rf", "/var/lib/packetnet", check=False)
        if made_share:
            sudo("rm", "-rf", "/usr/share/packetnet", check=False)

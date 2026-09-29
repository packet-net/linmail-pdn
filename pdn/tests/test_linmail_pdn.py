"""linmail-pdn against a fake pdn RHPv2 server.

Each test starts linmail-pdn with a fresh data directory and plays pdn (and
the station at the far end) from Python.
"""

from __future__ import annotations

import json
import re
import socket
import time
from pathlib import Path

from conftest import Linmail, linmail_cfg, partner
from fake_rhp import FakeRhp

DATA = Path(__file__).resolve().parent / "data"
SID_BPQ = "[BPQ-6.0.25.28-B12FWIHJM$]\r"


def login(fake: FakeRhp, call="N0USR", local="N0LMB") -> int:
    """A user connects and gets the BBS prompt."""
    h = fake.accept(call, local=local)
    fake.wait_text(h, ">\r")
    return h


def post(fake: FakeRhp, h: int, to: str, title: str, body: list[str]) -> str:
    start = len(fake.sent[h])
    fake.recv(h, f"SP {to}\r")
    fake.wait_text(h, "Title", start=start)
    fake.recv(h, title + "\r")
    fake.wait_text(h, "Message", start=start)
    fake.recv(h, "".join(line + "\r" for line in body) + "/EX\r")
    end = fake.wait_text(h, "de N0LMB>", start=start + 1)
    text = fake.sent[h][start:end].decode("latin-1")
    return re.search(r"Message: (\d+)", text).group(1)


def test_listens_on_call_and_alias_from_settings_file(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg())
    linmail.write("linmail-pdn.conf", f"# test\nrhp = 127.0.0.1:{fake.port}\nalias = BBS\nportmap = 1=bpq\n")
    linmail.start(rhp=False)
    fake.wait_msg("listenReply", direction="out")
    fake.wait(lambda: len([m for m in fake.messages("listen")]) == 2, what="two listens")
    binds = sorted(m["local"] for m in fake.messages("bind"))
    assert binds == ["BBS", "N0LMB"]
    assert all(m["port"] is None for m in fake.messages("bind"))

    # A user can come in on the alias too
    h = login(fake, local="BBS")
    assert b"[BPQ-" in fake.sent[h]


def test_settings_precedence(tmp_path):
    """Settings file, then the pdn supervisor's environment, then options."""
    for env, args, expected in [
        ({}, [], "N0FILE"),
        ({"PDN_APP_CALLSIGN": "N0ENV"}, [], "N0ENV"),
        ({"PDN_APP_CALLSIGN": "N0ENV"}, ["-c", "N0ARG"], "N0ARG"),
    ]:
        fake = FakeRhp()
        work = tmp_path / expected
        work.mkdir()
        lm = Linmail(work, fake)
        lm.write("linmail.cfg", linmail_cfg())
        lm.write("linmail-pdn.conf", f"call = N0FILE\nrhp = 127.0.0.1:1\n")
        env["PDN_RHP_PORT"] = str(fake.port)
        try:
            lm.start(*args, env=env, rhp=False)
            bind = fake.wait_msg("bind")
            assert bind["local"] == expected
        finally:
            lm.stop()
            fake.close()


def test_rhp_auth(tmp_path):
    fake = FakeRhp(auth=("linmail", "s3cret"))
    lm = Linmail(tmp_path, fake)
    lm.write("linmail.cfg", linmail_cfg())
    try:
        lm.start("-u", "linmail", "-w", "s3cret")
        fake.wait_msg("listenReply", direction="out", errCode=0)
        first = [m for d, m in fake.log if d == "in"][0]
        assert first["type"] == "auth" and first["user"] == "linmail"
    finally:
        lm.stop()
        fake.close()


def test_user_session(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg())
    linmail.start("-m", "1=bpq")
    fake.wait_msg("listenReply", direction="out")

    h = login(fake)
    num = post(fake, h, "N0ABC", "Test over fake RHP", ["First line", "Second line"])

    start = len(fake.sent[h])
    fake.recv(h, "L\r")
    fake.wait_text(h, "Test over fake RHP", start=start)
    fake.recv(h, f"R {num}\r")
    fake.wait_text(h, "Second line", start=start)

    fake.recv(h, "B\r")
    fake.wait(lambda: fake.closed(h), what="linmail-pdn to close the session")
    linmail.wait_bbslog(r"N0USR\s+N0USR Disconnected")


def test_refuses_peer_that_is_not_a_callsign(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg())
    linmail.start()
    fake.wait_msg("listenReply", direction="out")
    h = fake.accept("127.0.0.1:51398")
    fake.wait_text(h, "needs a callsign")
    fake.wait(lambda: fake.closed(h), what="close")
    assert linmail.proc.poll() is None


def fbb_blocks(data: bytes):
    """Parse one FBB compressed message: SOH header, STX blocks, EOT checksum."""
    assert data[0] == 1, "no SOH"
    i = 2 + data[1]
    body = b""
    while data[i] == 2:
        n = data[i + 1] or 256
        body += data[i + 2:i + 2 + n]
        i += 2 + n
    assert data[i] == 4, "no EOT"
    return body, data[i + 1], i + 2


def test_forward_out_b2(linmail, fake):
    """linmail-pdn dials a partner through its connect script and forwards
    one message with FBB B2 compression."""
    linmail.write("linmail.cfg", linmail_cfg(partners=[partner()]))
    linmail.start("-m", "1=bpq", "-n", "N0PDN")
    fake.wait_msg("listenReply", direction="out")

    h = login(fake)
    post(fake, h, "N0ABC @ N0BPQ", "Outbound B2", ["Binary must survive"])

    op = fake.wait_msg("open", timeout=30, remote="N0BPQ-1")
    assert op["port"] == "bpq" and op["local"] == "N0LMB" and op["mode"] == "stream"
    linmail.wait_bbslog(r"N0PDN\} Connected to N0BPQ-1")
    fwd = next(k for k, v in fake.handles.items() if v.get("remote") == "N0BPQ-1")

    fake.recv(fwd, SID_BPQ + "de N0BPQ>\r")
    end = fake.wait_text(fwd, "F> ")
    proposal = fake.sent[fwd][:end].decode("latin-1")
    fc = re.search(r"FC EM (\S+) (\d+) (\d+) N0USR N0BPQ N0ABC P", proposal)
    assert fc, proposal
    comp_len = int(fc.group(3))

    start = fake.wait_text(fwd, "\r", start=end)
    fake.recv(fwd, "FS Y\r")

    def whole_message():
        data = bytes(fake.sent[fwd][start:])
        try:
            return fbb_blocks(data)
        except (IndexError, AssertionError):
            return None
    body, checksum, _ = fake.wait(whole_message, what="the compressed message")
    assert len(body) == comp_len
    assert (sum(body) + checksum) % 256 == 0
    assert any(b >= 0x80 for b in body) and 0 in fake.sent[fwd][start:], "expected binary data"

    fake.recv(fwd, "FF\r")
    fake.wait_text(fwd, "FQ")
    fake.hangup(fwd)
    linmail.wait_stdout(r"N0BPQ-1 disconnected")
    linmail.wait_bbslog(r"N0BPQ\s+N0BPQ Disconnected")


def test_forward_in_b2_recorded(linmail, fake):
    """Replay a B2 transfer recorded from a real LinBPQ into linmail-pdn."""
    rec = json.loads((DATA / "b2-from-linbpq.json").read_text())
    linmail.write("linmail.cfg", linmail_cfg(partners=[partner(script=())]))
    linmail.start("-m", "1=bpq")
    fake.wait_msg("listenReply", direction="out")

    h = fake.accept("N0BPQ-1")
    fake.wait_text(h, "de N0LMB>")
    fake.recv(h, rec["proposal"])
    fake.wait_text(h, "FS Y")
    fake.recv(h, rec["binary"].encode("latin-1"))
    fake.wait_text(h, "FF")
    fake.recv(h, "FQ\r")
    linmail.wait_bbslog(r"Uncompressing Message Comp Len 280 Msg Len 355 CRC 9d04")
    linmail.wait_bbslog(r"B2 Msg To: N0XYZ@N0LMB")
    fake.wait(lambda: fake.closed(h), what="linmail-pdn to close after FQ")


def test_connect_script_failures_and_unsupported(linmail, fake):
    """An unsupported command and a failed connect both fall through to ELSE."""
    script = ("NC N0BPQ", "ELSE", "C 1 N0ZZZ", "ELSE", "C 1 N0BPQ-1")
    linmail.write("linmail.cfg", linmail_cfg(partners=[partner(script=script)]))
    fake.on_open = lambda m: (15, "Connect to N0ZZZ failed: timed out") if m["remote"] == "N0ZZZ" else (0, "Ok")
    linmail.start("-m", "1=bpq", "-n", "N0PDN")
    fake.wait_msg("listenReply", direction="out")

    h = login(fake)
    post(fake, h, "N0ABC @ N0BPQ", "Script test", ["x"])

    linmail.wait_bbslog(r"N0PDN\} Sorry, command not supported", timeout=30)
    linmail.wait_bbslog(r"N0PDN\} Failure with N0ZZZ", timeout=30)
    linmail.wait_bbslog(r"N0PDN\} Connected to N0BPQ-1", timeout=30)
    remotes = [m["remote"] for m in fake.messages("open")]
    assert remotes == ["N0ZZZ", "N0BPQ-1"]
    assert "node command not mapped to pdn: \"NC N0BPQ\"" in linmail.stdout()


def test_multi_hop_script(linmail, fake):
    """After the first hop the stream is transparent: later lines go to the far node."""
    linmail.write("linmail.cfg", linmail_cfg(partners=[partner(script=("C 1 N0BPQ", "BBS"))]))
    linmail.start("-m", "1=bpq", "-n", "N0PDN")
    fake.wait_msg("listenReply", direction="out")
    h = login(fake)
    post(fake, h, "N0ABC @ N0BPQ", "Hop test", ["x"])

    fake.wait_msg("open", timeout=30, remote="N0BPQ")
    fwd = next(k for k, v in fake.handles.items() if v.get("remote") == "N0BPQ")
    fake.wait_text(fwd, "BBS\r")
    fake.recv(fwd, "BPQLAB:N0BPQ} Connected to BBS\r" + SID_BPQ + "de N0BPQ>\r")
    fake.wait_text(fwd, "FC EM")


def test_idle_timeout(linmail, fake):
    """IDLETIME in a connect script sets the session idle time; the shim enforces it."""
    linmail.write("linmail.cfg", linmail_cfg(partners=[partner(script=("IDLETIME 3", "C 1 N0BPQ-1"))]))
    linmail.start("-m", "1=bpq")
    fake.wait_msg("listenReply", direction="out")
    h = login(fake)
    post(fake, h, "N0ABC @ N0BPQ", "Idle test", ["x"])

    fake.wait_msg("open", timeout=30, remote="N0BPQ-1")
    fwd = next(k for k, v in fake.handles.items() if v.get("remote") == "N0BPQ-1")
    opened = time.time()
    # The far end says nothing at all
    fake.wait(lambda: fake.closed(fwd), timeout=15, what="idle close")
    assert time.time() - opened >= 3
    linmail.wait_stdout(r"N0BPQ-1 idle for 3 seconds - disconnecting")


def test_ui_beacons(linmail, fake):
    """With EnableUI on, header broadcasts go out as RHP datagrams."""
    groups = "UIPort1:\n{\n  Enabled = 1;\n  SendMF = 1;\n  SendHDDR = 1;\n  SendNull = 0;\n};\n"
    linmail.write("linmail.cfg", linmail_cfg(main_extra={"EnableUI": 1}, groups=groups))
    linmail.start("-m", "1=bpq")
    op = fake.wait_msg("open", mode="dgram", timeout=15)
    assert op["local"] == "N0LMB"
    st = fake.wait_msg("sendto", timeout=15)
    assert st["port"] == "bpq" and st["local"] == "N0LMB" and st["remote"] == "FBB"
    assert "!!" in st["data"]

    h = login(fake)
    post(fake, h, "N0ABC", "Beacon me", ["x"])
    fake.wait(lambda: any("Beacon me" in m["data"] for m in fake.messages("sendto")),
              timeout=15, what="a header broadcast for the new message")


def test_reconnects_when_pdn_restarts(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg())
    linmail.start()
    fake.wait_msg("listenReply", direction="out")
    h = login(fake)
    fake.drop()
    linmail.wait_stdout(r"Lost RHP connection")
    fake.wait(lambda: fake.connections == 2, timeout=20, what="reconnect")
    fake.wait(lambda: len(fake.messages("listen")) == 2, timeout=20, what="listen again")
    linmail.wait_bbslog(r"N0USR\s+N0USR Disconnected")
    login(fake)


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_smtp_pop3_nntp_never_start(linmail, fake):
    ports = {"SMTPPort": free_port(), "POP3Port": free_port(), "NNTPPort": free_port()}
    linmail.write("linmail.cfg", linmail_cfg(main_extra=ports))
    linmail.start()
    linmail.wait_stdout(r"SMTP, POP3 and NNTP servers are not supported")
    fake.wait_msg("listenReply", direction="out")
    for port in ports.values():
        s = socket.socket()
        try:
            assert s.connect_ex(("127.0.0.1", port)) != 0, f"something listens on {port}"
        finally:
            s.close()
    linmail.stop()
    saved = (linmail.dir / "linmail.cfg").read_text()
    for key, port in ports.items():
        assert f"{key} = {port};" in saved

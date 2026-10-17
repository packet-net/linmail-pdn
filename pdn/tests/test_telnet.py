"""linmail-pdn's telnet and FBB logins (pdntelnet.c).

They stand in for LinBPQ's Telnet driver (TCPPORT and FBBPORT), so the
programs that log in to a LinBPQ BBS keep working. The exchanges below are the
ones those programs use: the pdn-mailcast head end on the FBB port (Q0HEAD
forwarding, Q0CAST listing reports), and GB7RDG's sysop scripts on the telnet
port (bbs-trim-digests, fwd-on-heard).
"""

from __future__ import annotations

import json
import os
import re
import socket
import time
from pathlib import Path

from conftest import free_port, linmail_cfg, partner

DATA = Path(__file__).resolve().parent / "data"

SYSOP_PW = "Sys#op1pw"          # a # in it: part of the password in a user line
USER_PW = "userpass1"
HEAD_PW = "headpass1"
CAST_PW = "castpass1"
SECRETS = (SYSOP_PW, USER_PW, HEAD_PW, CAST_PW, "wrongpass9")

IAC, WILL, DO, DONT, ECHO, SGA = 255, 251, 253, 254, 1, 3


class Client:
    """One TCP connection to linmail-pdn, keeping everything it sends."""

    def __init__(self, port: int):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.got = bytearray()
        self.eof = False

    def send(self, data: bytes | str):
        self.s.sendall(data.encode("latin-1") if isinstance(data, str) else data)

    def line(self, text: str):
        self.send(text + "\r")

    def _read(self, timeout: float) -> bool:
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return False
        except ConnectionResetError:
            d = b""
        if not d:
            self.eof = True
            return False
        self.got += d
        return True

    def until(self, marker: str | bytes, start: int = 0, timeout: float = 15.0) -> int:
        """Wait for marker at or after start; return the offset just past it."""
        m = marker.encode("latin-1") if isinstance(marker, str) else marker
        end = time.time() + timeout
        while True:
            at = self.got.find(m, start)
            if at >= 0:
                return at + len(m)
            if self.eof or time.time() > end:
                raise AssertionError(f"no {marker!r} (eof={self.eof}): {bytes(self.got[start:])!r}")
            self._read(min(0.5, max(0.05, end - time.time())))

    def quiet(self, t: float = 0.5) -> bytes:
        """Everything that arrives until it has been quiet for t seconds."""
        start = len(self.got)
        while self._read(t):
            pass
        return bytes(self.got[start:])

    def closed(self, timeout: float = 15.0) -> bool:
        end = time.time() + timeout
        while not self.eof and time.time() < end:
            self._read(0.5)
        return self.eof

    def text(self, start: int = 0) -> str:
        return self.got[start:].decode("latin-1")

    def close(self):
        self.s.close()


def setup(linmail, *, users=None, extra="", partners=(), cfg_extra=None, sysops=("N0SYS",)) -> tuple[int, int]:
    """linmail.cfg and linmail-pdn.conf with both login ports. Returns them."""
    tport, fport = free_port(), free_port()
    users = users if users is not None else [
        f"user = tf,{SYSOP_PW},N0SYS,,SYSOP",
        f"USER=g5rkt,{USER_PW},N0USR",
        f"    USER=Q0HEAD,{HEAD_PW},Q0HEAD        ; pasted from bpq32.cfg",
        f"USER=Q0CAST,{CAST_PW},Q0CAST",
    ]
    linmail.write("linmail.cfg", linmail_cfg(partners=partners, sysops=sysops, main_extra=cfg_extra))
    linmail.write("linmail-pdn.conf",
                  f"telnet_port = {tport}\nfbb_port = {fport}\n" + "\n".join(users) + "\n" + extra)
    return tport, fport


def start(linmail, fake, *args):
    linmail.start(*args)
    linmail.wait_stdout(r"telnet: FBB logins on|telnet: telnet logins on")
    fake.wait_msg("listenReply", direction="out")


def read_quiet_login(port: int, user: str, password: str) -> Client:
    """bbs-trim-digests and fwd-on-heard: read what comes, send the user, read,
    send the password, read, then BBS and wait for the BBS prompt."""
    c = Client(port)
    c.quiet()
    c.line(user)
    c.quiet()
    c.line(password)
    c.quiet()
    start = len(c.got)
    c.line("BBS")
    c.until("de N0LMB>", start)
    return c


def cmd(c: Client, text: str) -> str:
    start = len(c.got)
    c.line(text)
    end = c.until("de N0LMB>", start)
    return c.text(start)[:end - start]


def no_secrets(linmail, *clients: Client):
    """No password in anything sent back, in stdout or in any log file."""
    places = {"stdout": linmail.stdout()}
    for p in (linmail.dir / "logs").glob("*"):
        places[p.name] = p.read_bytes().decode("latin-1")
    for c in clients:
        places["client"] = c.text()
    for where, text in places.items():
        for secret in SECRETS:
            assert secret not in text, f"a password reached {where}"


def listening_ports(pid: int) -> set[int]:
    """The TCP ports a process listens on, from /proc."""
    inodes = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        m = re.fullmatch(r"socket:\[(\d+)\]", target)
        if m:
            inodes.add(m.group(1))
    ports = set()
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            f = line.split()
            if f[3] == "0A" and f[9] in inodes:
                ports.add(int(f[1].rsplit(":", 1)[1], 16))
    return ports


def test_nothing_listens_unless_configured(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg())
    linmail.write("linmail-pdn.conf", f"user = tf,{SYSOP_PW},N0SYS,,SYSOP\n")
    linmail.start()
    fake.wait_msg("listenReply", direction="out")
    assert listening_ports(linmail.proc.pid) == {linmail.web_port}
    assert "telnet logins on" not in linmail.stdout() and "FBB logins on" not in linmail.stdout()


def test_sysop_scripts_exchange(linmail, fake):
    """bbs-trim-digests, step for step: log in on the telnet port as a sysop,
    BBS, list a sender's messages with L<, kill some with K, FWD QUEUE, B."""
    tport, _ = setup(linmail, partners=[partner(script=(), at="N0BPQ")])
    start(linmail, fake)

    c = Client(tport)
    first = c.quiet()
    assert first.startswith(bytes([IAC, WILL, SGA, IAC, WILL, ECHO])) and first.endswith(b"user:")
    c.line("tf")
    assert c.quiet() == b"password:"
    c.line(SYSOP_PW)
    after = c.quiet()
    assert after.startswith(b"\r\n") and b"Type BBS" in after and b">" not in after
    s0 = len(c.got)
    c.line("BBS")
    c.until("de N0LMB>", s0)

    nums = []
    for title in ("NEWS 01-OCT", "NEWS 02-OCT", "NEWS 03-OCT"):
        s0 = len(c.got)
        c.line("SB ALL @ N0BPQ")
        c.until("Title", s0)
        c.line(title)
        c.until("Message", s0)
        c.send("Digest\r/EX\r")
        c.until("de N0LMB>", s0)
        nums.append(re.search(r"Message: (\d+)", c.text(s0)).group(1))

    listing = cmd(c, "L< N0SYS")
    for n in nums:
        assert re.search(rf"^{n}\s+\d\d-\w{{3}}\s+B\$\s+\d+\s+ALL\s+@N0BPQ\s+N0SYS\s+NEWS", listing, re.M), listing
    assert "\r\n" in listing and "\r\r" not in listing

    queue = cmd(c, "FWD QUEUE")
    assert "needs SYSOP" not in queue
    assert re.search(r"N0BPQ 3 Msgs", queue), queue

    killed = cmd(c, f"K {nums[0]} {nums[1]}")
    assert re.findall(r"Message #(\d+) Killed", killed) == nums[:2]

    c.line("B")
    assert c.closed()
    linmail.wait_bbslog(r"N0SYS\s+N0SYS Disconnected")
    no_secrets(linmail, c)


def test_fwd_on_heard_exchange(linmail, fake):
    """fwd-on-heard: read_quiet login, BBS, FWD QUEUE, FWD <bbs> NOW."""
    tport, _ = setup(linmail, partners=[partner(script=("C 1 N0BPQ-1",), at="N0BPQ")])
    start(linmail, fake, "-m", "1=bpq")
    c = read_quiet_login(tport, "tf", SYSOP_PW)
    assert "N0BPQ" not in cmd(c, "FWD QUEUE")
    reply = cmd(c, "FWD N0BPQ NOW")
    assert "needs SYSOP" not in reply and "not found" not in reply
    fake.wait_msg("open", timeout=15, remote="N0BPQ-1")
    c.line("B")
    assert c.closed()


def test_sysop_needs_both_flags(linmail, fake):
    """As LinBPQ: sysop status needs SYSOP on the USER line (a secure session)
    and the sysop flag on the BBS user record."""
    users = [
        f"user = tf,{SYSOP_PW},N0SYS,,SYSOP",       # both: sysop
        f"user = g5rkt,{USER_PW},N0USR",            # neither
        f"user = q0cast,{CAST_PW},N0SYS",           # BBS record only
        f"user = q0head,{HEAD_PW},N0OTH,,SYSOP",    # USER line only
    ]
    tport, _ = setup(linmail, users=users)
    start(linmail, fake)
    for name, pw, sysop in (("tf", SYSOP_PW, True), ("g5rkt", USER_PW, False),
                            ("q0cast", CAST_PW, False), ("q0head", HEAD_PW, False)):
        c = read_quiet_login(tport, name, pw)
        reply = cmd(c, "FWD QUEUE")
        assert ("FWD command needs SYSOP status" not in reply) == sysop, (name, reply)
        c.line("B")
        assert c.closed()
    assert "telnet login from 127.0.0.1 as N0SYS (secure session)" in linmail.stdout()
    assert "telnet login from 127.0.0.1 as N0USR\n" in linmail.stdout()


def test_head_end_forwarding_on_fbb_port(linmail, fake):
    """The pdn-mailcast head end as Q0HEAD on the FBB port: user, password and
    BBS sent at once with no prompts awaited, then a B1F session as caller.
    The BBS's first bytes are its SID: no prompts, no telnet negotiation."""
    head = partner(call="Q0HEAD", script=(), at="Q0HEAD")
    head.to_calls = ["MCAST"]
    head.allow_b2 = False
    head.allow_compressed = True
    tport, fport = setup(linmail, partners=[head])
    start(linmail, fake)

    # A bulletin for the head end to collect
    c = read_quiet_login(tport, "tf", SYSOP_PW)
    s0 = len(c.got)
    c.line("SB MCAST")
    c.until("Title", s0)
    c.line("For the mailcast")
    c.until("Message", s0)
    c.send("Bulletin body\r/EX\r")
    c.until("de N0LMB>", s0)
    c.line("B")
    c.closed()

    h = Client(fport)
    h.send(f"Q0HEAD\r{HEAD_PW}\rBBS\r")
    sid_end = h.until("$]\r")
    assert h.got.startswith(b"[BPQ-"), bytes(h.got[:40])
    assert IAC not in h.got and b"user:" not in h.got and b"password:" not in h.got
    prompt_end = h.until(">\r", sid_end)
    assert b"\n" not in h.got[:prompt_end], "the FBB port must not add LF"

    h.send("[MAILCAST-1.0-B1FHM$]\rFF\r")
    fa_end = h.until("F> ", prompt_end)
    proposal = h.text(prompt_end)[:fa_end - prompt_end]
    assert re.search(r"FA B N0SYS \S+ MCAST \S+_N0LMB \d+", proposal), proposal
    h.until("\r", fa_end)
    h.send("FS -\r")           # refused, so the BBS counts it handed over
    h.send("FF\r")             # and nothing to send back
    h.until("FQ", fa_end)
    assert h.closed()
    linmail.wait_bbslog(r"Q0HEAD\s+Q0HEAD Disconnected")
    no_secrets(linmail, c, h)


def test_report_reader_on_fbb_port(linmail, fake):
    """The head end as Q0CAST: user, password, BBS, wait for a prompt ending
    in >, L> MCAST, wait for the next prompt, B."""
    _, fport = setup(linmail, sysops=("N0SYS", "Q0CAST"))
    start(linmail, fake)
    r = Client(fport)
    r.send(f"Q0CAST\r{CAST_PW}\rBBS\r")
    end = r.until("de N0LMB>\r")
    welcome = r.text()[:end]
    assert "user:" not in welcome and "password:" not in welcome and "\n" not in welcome
    s0 = len(r.got)
    r.line("L> MCAST")
    r.until("de N0LMB>", s0)
    r.line("B")
    assert r.closed()
    no_secrets(linmail, r)


def test_b2_forward_in_on_fbb_port(linmail, fake):
    """A recorded LinBPQ B2 transfer (binary, with 0xFF bytes in it) through
    the FBB port arrives intact: nothing is taken for telnet commands."""
    rec = json.loads((DATA / "b2-from-linbpq.json").read_text())
    assert b"\xff" in rec["binary"].encode("latin-1")
    _, fport = setup(linmail, partners=[partner(script=())],
                     users=[f"USER=n0bpq,{USER_PW},N0BPQ-1"])
    start(linmail, fake)
    b = Client(fport)
    b.send(f"n0bpq\r{USER_PW}\r")
    b.send("BBS\r")
    b.until("de N0LMB>")
    b.send(rec["proposal"])
    b.until("FS Y")
    b.send(rec["binary"].encode("latin-1"))
    b.until("FF")
    b.send("FQ\r")
    linmail.wait_bbslog(r"Uncompressing Message Comp Len 280 Msg Len 355 CRC 9d04")
    linmail.wait_bbslog(r"B2 Msg To: N0XYZ@N0LMB")
    assert b.closed()


def test_login_failures(linmail, fake):
    tport, fport = setup(linmail)
    start(linmail, fake)

    # A wrong password gets the password prompt again; the right one still works
    c = Client(tport)
    c.quiet()
    c.line("tf")
    c.quiet()
    c.line("wrongpass9")
    assert c.quiet() == b"\r\npassword:"
    c.line(SYSOP_PW)
    c.quiet()
    s0 = len(c.got)
    c.line("BBS")
    c.until("de N0LMB>", s0)
    c.close()

    # Names are case-sensitive, as in LinBPQ; five failures and it hangs up
    c = Client(tport)
    c.quiet()
    for i in range(4):
        c.line("TF")
        assert c.quiet() == b"user:"
    c.line("TF")
    c.until("Too many attempts - Disconnected\r\n")
    assert c.closed()

    # The same on the FBB port, where nothing is prompted for
    f = Client(fport)
    f.send("".join(f"Q0HEAD\rwrongpass9\r" for _ in range(5)))
    f.until("Too many attempts - Disconnected\r")
    assert f.closed()
    assert f.text().count("password:") == 4 and "user:" not in f.text()

    out = linmail.stdout()
    assert out.count("login failed from 127.0.0.1: unknown user") == 5
    assert out.count("login failed from 127.0.0.1: wrong password for tf") == 1
    assert out.count("login failed from 127.0.0.1: wrong password for Q0HEAD") == 5
    no_secrets(linmail, c, f)


def test_echo_and_negotiation(linmail, fake):
    """A real telnet client asks for echo: names and commands come back, the
    password never does."""
    tport, _ = setup(linmail)
    start(linmail, fake)
    c = Client(tport)
    c.until("user:")
    c.send(bytes([IAC, DO, SGA, IAC, DO, ECHO, IAC, WILL, 24, IAC, 250, 24, 1, IAC, 240]))
    c.send("tf\r\n")
    c.until("tf\r\npassword:")
    s0 = len(c.got)
    c.send(SYSOP_PW + "\r\n")
    c.until("Type BBS", s0)
    c.send("BBX\x08S\r\n")
    c.until("de N0LMB>", s0)
    assert bytes([IAC, DONT, 24]) in c.got, "a WILL we don't want gets DONT"
    assert c.got.count(bytes([IAC, WILL, ECHO])) == 1, "DO ECHO is not answered again"
    c.close()
    no_secrets(linmail, c)


def test_straight_in_and_pasted_user_line(linmail, fake):
    """A USER line whose appl field is BBS goes straight in, as in LinBPQ.
    The line is read as LinBPQ reads it: a # is part of the password, a ;
    starts a comment, and each field ends at a space."""
    users = [f"USER=n0abc,{SYSOP_PW} ignored,N0ABC,BBS   ; a comment"]
    tport, _ = setup(linmail, users=users)
    start(linmail, fake)
    c = Client(tport)
    c.until("user:")
    c.line("n0abc")
    c.until("password:")
    c.line(SYSOP_PW)
    c.until("de N0LMB>")
    assert "Type BBS" in c.text()
    c.close()


def test_disconnects_and_concurrent_sessions(linmail, fake):
    tport, fport = setup(linmail)
    start(linmail, fake)

    # Two at once, on both ports
    a = read_quiet_login(tport, "tf", SYSOP_PW)
    b = Client(fport)
    b.send(f"Q0CAST\r{CAST_PW}\rBBS\r")
    b.until("de N0LMB>")
    cmd(a, "L")
    s0 = len(b.got)
    b.line("L")
    b.until("de N0LMB>", s0)

    # The caller hangs up mid session: the BBS hears, and the stream is freed
    a.close()
    linmail.wait_bbslog(r"N0SYS\s+N0SYS Disconnected")
    linmail.wait_stdout(r"N0SYS disconnected")

    # The BBS hangs up: the caller is disconnected after what it was sent
    b.line("B")
    assert b.closed()
    linmail.wait_bbslog(r"Q0CAST\s+Q0CAST Disconnected")

    # A typed message sent just before a hangup is not lost
    c = read_quiet_login(tport, "g5rkt", USER_PW)
    s0 = len(c.got)
    c.line("SP N0ABC")
    c.until("Title", s0)
    c.line("Hangup test")
    c.until("Message", s0)
    c.send("Last words\r/EX\r")
    c.s.shutdown(socket.SHUT_WR)
    c.close()
    linmail.wait_bbslog(r"Routing Trace To N0ABC")
    linmail.wait_bbslog(r"N0USR\s+N0USR Disconnected")

    # A B at the prompt, before the BBS
    d = Client(tport)
    d.quiet()
    d.line("g5rkt")
    d.quiet()
    d.line(USER_PW)
    d.quiet()
    d.line("NODES")
    assert b"type BBS" in d.quiet()
    d.line("B")
    assert d.closed()


def test_session_limits(linmail, fake):
    """telnet_max caps the sessions; with every BBS stream busy a login waits
    a few seconds for one, then is told."""
    tport, fport = setup(linmail, extra="telnet_max = 3\n", cfg_extra={"Streams": 1})
    start(linmail, fake)
    a = read_quiet_login(tport, "tf", SYSOP_PW)
    b = Client(fport)
    b.send(f"Q0CAST\r{CAST_PW}\rBBS\r")
    b.until("Sorry, no free BBS streams - try again later\r", timeout=15)
    assert b.closed()
    c1, c2 = Client(tport), Client(tport)
    c1.until("user:")
    c2.until("user:")
    d = Client(fport)
    d.until("No Free Sessions\r\n")
    assert d.closed()
    a.close()


def test_works_while_pdn_is_down(linmail, fake):
    """Telnet logins don't need the RHP connection."""
    tport, _ = setup(linmail)
    linmail.start("-r", "127.0.0.1:1")
    linmail.wait_stdout(r"telnet logins on")
    c = read_quiet_login(tport, "tf", SYSOP_PW)
    assert "needs SYSOP" not in cmd(c, "FWD QUEUE")
    c.line("B")
    assert c.closed()


def test_port_in_use(linmail, fake):
    """A port something else has (pdn's console on 8011, say) is logged, and
    the BBS carries on without telnet logins."""
    blocker = socket.socket()
    blocker.bind(("127.0.0.1", 0))
    blocker.listen(1)
    taken = blocker.getsockname()[1]
    try:
        linmail.write("linmail.cfg", linmail_cfg())
        linmail.write("linmail-pdn.conf", f"fbb_port = {taken}\nuser = tf,{SYSOP_PW},N0SYS,,SYSOP\n")
        linmail.start()
        linmail.wait_stdout(rf"cannot listen on 127.0.0.1 port {taken} \(Address already in use\) - FBB logins off")
        fake.wait_msg("listenReply", direction="out")
        assert linmail.proc.poll() is None
    finally:
        blocker.close()

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

import pytest
import re
import socket
import time
from pathlib import Path

from conftest import BIN, free_port, linmail_cfg, partner

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


def telnet_setup(linmail, *, users=None, extra="", partners=(), cfg_extra=None, sysops=("N0SYS",)) -> tuple[int, int]:
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
    tport, _ = telnet_setup(linmail, partners=[partner(script=(), at="N0BPQ")])
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
    tport, _ = telnet_setup(linmail, partners=[partner(script=("C 1 N0BPQ-1",), at="N0BPQ")])
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
    tport, _ = telnet_setup(linmail, users=users)
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


@pytest.mark.parametrize("protocol", ["B1", "B2"])
def test_head_end_collects_a_bulletin_on_fbb_port(linmail, fake, protocol):
    """The pdn-mailcast head end's real job, as Q0HEAD on the FBB port: user,
    password and BBS sent at once with no prompts awaited, then a B1F session
    as caller with its own CR LF line ends, taking a bulletin. The compressed
    message must arrive byte for byte as the BBS sends it over the air to an
    identical partner, and it holds 0xFF, CR and LF bytes, which a telnet
    port would have mangled. The head end speaks B1F; B2F is checked too."""
    def b1_partner(call):
        p = partner(call=call, script=(), at=call)
        p.to_calls = ["MCAST"]
        p.allow_b2 = protocol == "B2"
        p.allow_compressed = True
        p.send_new_immediately = False
        p.fwd_interval = 3600
        return p
    tport, fport = telnet_setup(linmail, partners=[b1_partner("Q0HEAD"), b1_partner("N0BPQ")])
    start(linmail, fake)

    # The bulletin, from a sysop at the telnet port
    import random
    rnd = random.Random(7)
    words = ["mailcast", "GB7RDG", "bulletin", "solar", "flux", "K", "index", "QSL", "73", "net"]
    body = ["".join(rnd.choice(words) + rnd.choice(" ,.-/") for _ in range(12)) for _ in range(60)]
    c = read_quiet_login(tport, "tf", SYSOP_PW)
    s0 = len(c.got)
    c.line("SB MCAST")
    c.until("Title", s0)
    c.line("For the mailcast")
    c.until("Message", s0)
    c.send("".join(line + "\r" for line in body) + "/EX\r")
    c.until("de N0LMB>", s0)
    c.line("B")
    c.closed()

    def collect(send, recv_until, got):
        """One B1F collection as caller. Returns the compressed message."""
        send(f"[MAILCAST-1.0-{protocol}FHM$]\r\nFF\r\n")
        fa = recv_until("F> ")
        recv_until("\r", fa)
        proposal = bytes(got()[:fa]).decode("latin-1")
        pattern = r"FA B N0SYS \S+ MCAST \S+_N0LMB \d+" if protocol == "B1" else r"FC EM \S+_N0LMB \d+ \d+ \d+"
        assert re.search(pattern, proposal), proposal
        send("FS +\r\n")

        def whole():
            data = bytes(got()[fa:])
            start = data.find(b"\x01")
            if start < 0:
                return None
            try:
                _, _, end = fbb_blocks(data[start:])
            except (IndexError, AssertionError):
                return None
            return data[start:start + end]
        end = time.time() + 15
        while (block := whole()) is None:
            assert time.time() < end, "no compressed message: %r" % bytes(got()[fa:fa + 120])
            time.sleep(0.1)
        send("FF\r\n")
        recv_until("FQ", fa)
        return block

    from test_linmail_pdn import fbb_blocks

    h = Client(fport)
    h.send(f"Q0HEAD\r{HEAD_PW}\rBBS\r")
    sid_end = h.until("$]\r")
    assert h.got.startswith(b"[BPQ-"), bytes(h.got[:40])
    assert IAC not in h.got and b"user:" not in h.got and b"password:" not in h.got
    prompt_end = h.until(">\r", sid_end)
    assert b"\n" not in h.got[:prompt_end], "the FBB port must not add LF"
    fbb = collect(h.send, lambda m, start=prompt_end: h.until(m, start), lambda: (h._read(0.1), h.got)[1])
    assert h.closed()
    linmail.wait_bbslog(r"Q0HEAD\s+Q0HEAD Disconnected")

    r = fake.accept("N0BPQ-1")
    fake.wait_text(r, "de N0LMB>")
    base = len(fake.sent[r])
    air = collect(lambda d: fake.recv(r, d),
                  lambda m, start=0: fake.wait_text(r, m, start=base + start) - base,
                  lambda: fake.sent[r][base:])

    assert fbb == air, "the FBB port changed the message"
    body_bytes, _, _ = fbb_blocks(fbb)
    for b in (0xFF, 0x0D, 0x0A):
        assert bytes([b]) in body_bytes, f"pick a body whose compressed form holds {b:#x}"
    no_secrets(linmail, c, h)


def test_report_reader_on_fbb_port(linmail, fake):
    """The head end as Q0CAST: user, password, BBS, wait for a prompt ending
    in >, L> MCAST, wait for the next prompt, B."""
    _, fport = telnet_setup(linmail, sysops=("N0SYS", "Q0CAST"))
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
    _, fport = telnet_setup(linmail, partners=[partner(script=())],
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


def test_failed_login_is_delayed_and_says_nothing(linmail, fake):
    """A wrong password and an unknown name get the same answer, a second
    later: a new line, then the user prompt again. The right login still works."""
    tport, _ = telnet_setup(linmail)
    start(linmail, fake)
    answers = []
    c = Client(tport)
    c.until("user:")
    for name in ("tf", "nosuchuser"):
        c.line(name)
        s0 = c.until("password:", len(c.got))
        t0 = time.time()
        c.line("wrongpass9")
        end = c.until("user:", s0)
        assert time.time() - t0 >= 0.9, "a failed login must be answered only after a delay"
        answers.append(bytes(c.got[s0:end]))
    assert answers[0] == answers[1] == b"\r\nuser:"
    c.line("tf")
    c.until("password:", len(c.got))
    c.line(SYSOP_PW)
    s0 = len(c.got)
    c.until("Type BBS", s0)
    c.line("BBS")
    c.until("de N0LMB>", s0)
    c.close()
    out = linmail.stdout()
    assert out.count("login failed from 127.0.0.1: wrong password for tf") == 1
    assert out.count("login failed from 127.0.0.1: unknown user") == 1
    assert "nosuchuser" not in out
    no_secrets(linmail, c)


def test_five_failures_hang_up_and_delays_grow(linmail, fake):
    """Names are case-sensitive, as in LinBPQ. Five failures and the session
    is dropped; after five failures from an address, the delay doubles."""
    tport, _ = telnet_setup(linmail)
    start(linmail, fake)
    c = Client(tport)
    c.until("user:")
    for i in range(5):
        c.line("TF")
        c.until("password:", len(c.got))
        c.line(SYSOP_PW)
    c.until("Too many attempts - Disconnected\r\n", timeout=20)
    assert c.closed()
    assert c.text().count("user:") == 5

    d = Client(tport)
    d.until("user:")
    d.line("tf")
    s0 = d.until("password:", len(d.got))
    t0 = time.time()
    d.line("wrongpass9")
    d.until("user:", s0, timeout=10)
    assert time.time() - t0 >= 1.9
    d.close()
    no_secrets(linmail, c, d)


def test_five_failures_on_fbb_port(linmail, fake):
    """The head end sends its login blind: five wrong ones in one go are
    answered one by one, with the delay between, and nothing tells it the
    name was right."""
    _, fport = telnet_setup(linmail)
    start(linmail, fake)
    f = Client(fport)
    t0 = time.time()
    f.send("".join(f"Q0HEAD\rwrongpass9\r" for _ in range(5)))
    f.until("Too many attempts - Disconnected\r", timeout=20)
    assert time.time() - t0 >= 4.5
    assert f.closed()
    assert f.text().count("user:") == 4 and "password:" not in f.text()
    assert linmail.stdout().count("login failed from 127.0.0.1: wrong password for Q0HEAD") == 5
    no_secrets(linmail, f)


def test_echo_and_negotiation(linmail, fake):
    """A real telnet client asks for echo: names and commands come back, the
    password never does."""
    tport, _ = telnet_setup(linmail)
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
    tport, _ = telnet_setup(linmail, users=users)
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
    tport, fport = telnet_setup(linmail)
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
    """telnet_max caps the sessions; when every one is logged in, a new
    connect is told. With every BBS stream busy, a login waits a few seconds
    for one, then is told."""
    tport, fport = telnet_setup(linmail, extra="telnet_max = 2\n", cfg_extra={"Streams": 1})
    start(linmail, fake)
    a = read_quiet_login(tport, "tf", SYSOP_PW)
    b = Client(fport)
    b.send(f"Q0CAST\r{CAST_PW}\rBBS\r")
    b.until("Sorry, no free BBS streams - try again later\r", timeout=15)
    assert b.closed()
    p = Client(tport)
    p.quiet()
    p.line("g5rkt")
    p.quiet()
    p.line(USER_PW)
    p.until("Type BBS")
    d = Client(fport)
    d.until("No Free Sessions\r\n")
    assert d.closed()
    a.close()
    p.close()


def test_logins_in_progress_give_way(linmail, fake):
    """Connections that never log in can't lock out the head end: at most
    three logins in progress from one address, and when the slots run out
    the oldest login in progress is dropped to make room."""
    tport, fport = telnet_setup(linmail, extra="telnet_max = 3\n")
    start(linmail, fake)
    idle = []
    for _ in range(5):
        c = Client(tport)
        c.until("user:")
        idle.append(c)
    assert idle[0].closed(5) and idle[1].closed(5), "only three logins in progress from one address"
    a = read_quiet_login(tport, "tf", SYSOP_PW)        # drops idle[2]
    h = Client(fport)                                  # every slot full: drops the oldest login in progress
    h.send(f"Q0HEAD\r{HEAD_PW}\rBBS\r")
    h.until("$]\r")
    states = [c.closed(1) for c in idle]
    assert states == [True, True, True, True, False], states
    a.close()
    h.close()


def churn(port: int, stop, base: int):
    """Connect from rotating 127.<base>.x.y addresses as fast as possible,
    never logging in, keeping the last 200 open (the reviewer's R1 probe)."""
    n = 0
    socks = []
    while not stop.is_set():
        n += 1
        try:
            s = socket.socket()
            s.bind((f"127.{base}.{(n >> 8) & 255}.{(n & 255) or 1}", 0))
            s.setblocking(False)
            try:
                s.connect(("127.0.0.1", port))
            except BlockingIOError:
                pass
            socks.append(s)
            if len(socks) > 200:
                socks.pop(0).close()
        except OSError:
            time.sleep(0.001)
    for s in socks:
        s.close()


@pytest.mark.parametrize("gap", [0, 0.002, 0.04])
def test_head_end_gets_in_through_connect_churn(linmail, fake, gap):
    """Connects that never log in, as fast as they come, from thousands of
    loopback addresses on both ports, can't drop the head end's login (review
    R1). A new connection is read as soon as it is accepted, the login
    dropped for a newcomer is the oldest by connect order (not by the second
    it came in), and it is read once more first, so a login that has just
    arrived logs in instead of being dropped. A login that has sent
    anything in the last two seconds is never dropped, so a head end whose
    user, password and BBS lines come a round trip apart gets in too."""
    import threading
    tport, fport = telnet_setup(linmail)
    start(linmail, fake)
    stop = threading.Event()
    threads = [threading.Thread(target=churn, args=(port, stop, base), daemon=True)
               for base, port in ((10, tport), (11, fport))]
    for t in threads:
        t.start()
    got_in, dropped, unreachable = 0, 0, 0
    try:
        time.sleep(1.0)
        for _ in range(10):
            try:
                h = Client(fport)
            except OSError:
                # The churn can swamp the kernel's own connect queue, so a
                # connect may time out before linmail-pdn ever sees it
                unreachable += 1
                continue
            try:
                # As the head end: user, password and BBS as separate writes,
                # a network round trip apart on a LAN (review R2)
                for line in ("Q0HEAD", HEAD_PW, "BBS"):
                    h.line(line)
                    time.sleep(gap)
                h.until("$]\r", timeout=10)
                got_in += 1
            except OSError:
                dropped += 1
            except AssertionError:
                dropped += 1
            h.close()
    finally:
        stop.set()
        for t in threads:
            t.join(10)
    assert dropped == 0, f"the head end was dropped {dropped} times ({got_in} in, {unreachable} never connected)"
    assert got_in >= 2, f"only {got_in} of 10 got in ({unreachable} never connected)"


def test_works_while_pdn_is_down(linmail, fake):
    """Telnet logins don't need the RHP connection."""
    tport, _ = telnet_setup(linmail)
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


def rss_kb(pid: int) -> int:
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return -1


def flood(port: int, stop, login: bytes = b"", reconnect: bool = True):
    """Send as fast as possible until told to stop, reconnecting if cut off."""
    chunk = b"A" * 65536
    while not stop.is_set():
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            if login:
                s.sendall(login)
                time.sleep(1.0)
            while not stop.is_set():
                s.sendall(chunk)
        except OSError:
            pass
        finally:
            s.close()
        if not reconnect:
            return
        time.sleep(0.05)


def test_flood_before_login_stalls_nothing(linmail, fake):
    """A client that streams without logging in is cut off after a few KB,
    and while it keeps coming back, other logins are served and memory stays
    put."""
    import threading
    tport, fport = telnet_setup(linmail)
    start(linmail, fake)
    pid = linmail.proc.pid
    before = rss_kb(pid)
    stop = threading.Event()
    t = threading.Thread(target=flood, args=(tport, stop), daemon=True)
    t.start()
    try:
        time.sleep(1.0)
        for _ in range(3):
            t0 = time.time()
            r = Client(fport)
            r.send(f"Q0CAST\r{CAST_PW}\rBBS\r")
            r.until("de N0LMB>", timeout=10)
            assert time.time() - t0 < 5
            r.line("B")
            r.closed()
        assert rss_kb(pid) - before < 20_000
    finally:
        stop.set()
        t.join(10)
    linmail.wait_stdout(r"sent too much without logging in - closed")


def test_flood_into_the_bbs_is_held_back(linmail, fake):
    """A logged in client that sends faster than the BBS reads is pushed back
    by TCP, not buffered: memory stays bounded and other sessions are served."""
    import threading
    tport, fport = telnet_setup(linmail)
    start(linmail, fake)
    pid = linmail.proc.pid
    before = rss_kb(pid)
    stop = threading.Event()
    t = threading.Thread(target=flood, args=(fport, stop),
                         kwargs={"login": f"Q0CAST\r{CAST_PW}\rBBS\r".encode(), "reconnect": False}, daemon=True)
    t.start()
    peak = before
    try:
        for _ in range(3):
            time.sleep(1.0)
            t0 = time.time()
            c = read_quiet_login(tport, "tf", SYSOP_PW)
            cmd(c, "L")
            assert time.time() - t0 < 8
            c.line("B")
            c.closed()
            peak = max(peak, rss_kb(pid))
        for _ in range(3):
            time.sleep(1.0)
            peak = max(peak, rss_kb(pid))
    finally:
        stop.set()
        t.join(10)
    # AddressSanitizer holds freed memory back for a while (its quarantine,
    # 256 MB by default), so the bound means nothing under it
    if "asan" not in BIN.name:
        assert peak - before < 50_000, f"memory grew from {before} kB to {peak} kB"
    assert linmail.proc.poll() is None


def test_junk_input(linmail, fake):
    """Random bytes, NULs, long lines, split and unfinished telnet commands,
    before and after logging in, on both ports: nothing breaks (CI runs this
    under AddressSanitizer too), and logins still work afterwards."""
    import random
    tport, fport = telnet_setup(linmail)
    start(linmail, fake)
    rnd = random.Random(1)
    for port in (tport, fport):
        for _ in range(8):
            c = Client(port)
            for _ in range(4):
                try:
                    c.send(bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 3000))))
                except OSError:
                    break
            c.close()
        c = Client(port)
        c.send(f"tf\r{SYSOP_PW}\rBBS\r")
        time.sleep(0.5)
        for _ in range(10):
            try:
                c.send(bytes([IAC, 250, 24]) + b"\x00" * 600 + bytes([IAC]) + b"\r\n" * 3 + b"x" * 1000)
            except OSError:
                break
        c.close()
    c = read_quiet_login(tport, "tf", SYSOP_PW)
    cmd(c, "L")
    c.line("B")
    assert c.closed()
    assert linmail.proc.poll() is None
    no_secrets(linmail)


def test_bad_user_lines(linmail, fake):
    """A USER line in the wrong order, or with a field too long, is refused at
    start-up, and nothing that might be its password is shown."""
    tport, _ = telnet_setup(linmail, users=[
        "USER=swapped,N0ABC,Secret#Pw1",            # callsign and password swapped
        "USER=long," + "p" * 300 + ",N0LNG",        # LinBPQ's limit is 255
        f"user = tf,{SYSOP_PW},N0SYS,,SYSOP",
    ])
    start(linmail, fake)
    out = linmail.stdout()
    assert "user swapped: the callsign field is not a callsign the BBS can use - user ignored" in out
    assert "a user line has a field longer than 255 characters - user ignored" in out
    assert "Secret#Pw1" not in out and "p" * 50 not in out
    assert "telnet logins on 127.0.0.1 port %d, 1 user" % tport in out

"""Fixtures for the linmail-pdn tests.

LINMAIL_PDN_BIN points at the binary (default: pdn/linmail-pdn, built by
make -C pdn). Each test gets a fresh data directory and a fake RHP server.
"""

from __future__ import annotations

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from bpqmail_cfg import FwdPartner, _user_record_string, render_bpqmail_cfg  # noqa: E402
from fake_rhp import FakeRhp  # noqa: E402

BIN = Path(os.environ.get("LINMAIL_PDN_BIN", HERE.parent / "linmail-pdn"))
UPSTREAM = HERE.parents[1]


def upstream_log_names() -> tuple[int, int, list[str]]:
    """From BBSUtilities.c: how many FilesNames entries there are, how long
    each is, and the names in Logs. The mail code builds each log file name
    as "<log dir>/logs/log_YYMMDD_<name>.txt" and strcpy's it into one of
    those buffers."""
    src = (UPSTREAM / "BBSUtilities.c").read_text(errors="replace")
    count, size = map(int, re.search(r"^char FilesNames\[(\d+)\]\[(\d+)\]", src, re.M).groups())
    names = re.findall(r'"([^"]*)"', re.search(r"^char \* Logs\[\d+\] = \{([^}]*)\}", src, re.M).group(1))
    assert '"%s/logs/log_%02d%02d%02d_%s.txt", GetLogDirectory()' in src, \
        "OpenLogfile's log file name format changed: check LogDirLimit in linmail-pdn.c"
    return count, size, names


def log_dir_limit() -> int:
    """The longest log directory path for which every log file name fits."""
    _, size, names = upstream_log_names()
    return size - 1 - max(len(f"/logs/log_YYMMDD_{name}.txt") for name in names)


@pytest.fixture
def short_tmp():
    """A fresh directory with a short path under /tmp. The mail code's log
    file names must fit in 100 bytes (see log_dir_limit), and pytest's
    tmp_path, which grows with the user and test names, can run past that on
    a CI runner. Use this wherever linmail-pdn or LinBPQ's own mail runs."""
    path = Path(tempfile.mkdtemp(prefix="lm", dir="/tmp"))
    yield path
    shutil.rmtree(path, ignore_errors=True)


def linmail_cfg(bbs="N0LMB", partners=(), main_extra: dict | None = None, groups: str = "",
                sysops=()) -> str:
    """A linmail.cfg, with extra keys in the main group, extra groups, and
    extra users flagged sysop."""
    users = [(call, _user_record_string(name=call, flags=0x08, bbs_number=0)) for call in sysops]
    text = render_bpqmail_cfg(bbs_call=bbs, sysop_call=bbs, partners=list(partners), extra_users=users)
    for key, value in (main_extra or {}).items():
        line = f"  {key} = {value};"
        text, n = re.subn(rf"^  {re.escape(key)} = .*;$", line, text, count=1, flags=re.M)
        if n == 0:
            text = text.replace("main:\n{\n", "main:\n{\n" + line + "\n", 1)
    return text + groups


def partner(call="N0BPQ", script=("C 1 N0BPQ-1",), at=None) -> FwdPartner:
    return FwdPartner(call=call, connect_script=list(script), at_calls=[at or call],
                      fwd_interval=5, allow_blocked=True, allow_compressed=True,
                      allow_b1=True, allow_b2=True, send_new_immediately=True,
                      con_timeout=60)


def free_port() -> int:
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Linmail:
    """One linmail-pdn process with its own data directory."""

    def __init__(self, workdir: Path, fake: FakeRhp):
        self.dir = workdir
        self.fake = fake
        self.proc: subprocess.Popen | None = None
        self.out = workdir / "stdout.log"
        self.web_port = free_port()

    def write(self, name: str, text: str):
        (self.dir / name).write_text(text)

    def adduser(self, call: str, password: str = "secret", bbs: bool = False):
        """linmail-pdn --adduser, as LinBPQ's setup scripts use it."""
        subprocess.run([str(BIN), "-d", str(self.dir), "--adduser", call, password, "TRUE" if bbs else "FALSE"],
                       cwd=self.dir, check=True, stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                       env={k: v for k, v in os.environ.items() if not k.startswith("PDN_")})

    def start(self, *args: str, env: dict | None = None, rhp: bool = True):
        argv = [str(BIN), "-d", str(self.dir), "-t", "-W", str(self.web_port)]
        if rhp:
            argv += ["-r", f"127.0.0.1:{self.fake.port}"]
        argv += list(args)
        full_env = {k: v for k, v in os.environ.items() if not k.startswith("PDN_")}
        full_env["PDN_APP_DIR"] = str(HERE.parent)  # so pdn/HTML is found wherever the binary is
        full_env.update(env or {})
        self.proc = subprocess.Popen(argv, cwd=self.dir, env=full_env, stdin=subprocess.DEVNULL,
                                     stdout=open(self.out, "wb"), stderr=subprocess.STDOUT)
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def stdout(self) -> str:
        return self.out.read_bytes().decode("latin-1") if self.out.exists() else ""

    def bbslog(self) -> str:
        logs = sorted((self.dir / "logs").glob("log_*_BBS.txt"))
        return "".join(p.read_bytes().decode("latin-1") for p in logs)

    def wait_stdout(self, pattern: str, timeout: float = 20.0) -> re.Match:
        return self._wait(self.stdout, pattern, timeout)

    def wait_bbslog(self, pattern: str, timeout: float = 20.0) -> re.Match:
        return self._wait(self.bbslog, pattern, timeout)

    def _wait(self, source, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, source())
            if m:
                return m
            if self.proc and self.proc.poll() is not None:
                raise AssertionError(f"linmail-pdn exited ({self.proc.returncode}):\n{self.stdout()[-3000:]}")
            time.sleep(0.1)
        raise AssertionError(f"timed out waiting for {pattern!r}\n{self.stdout()[-3000:]}")


@pytest.fixture
def fake():
    server = FakeRhp()
    yield server
    server.close()


@pytest.fixture
def linmail(tmp_path, fake):
    if not BIN.exists():
        pytest.skip(f"{BIN} not built (make -C pdn)")
    lm = Linmail(tmp_path, fake)
    yield lm
    lm.stop()
    if lm.proc:
        # Keep the evidence in the pytest output when something fails
        print(lm.stdout()[-5000:])
        # Under an AddressSanitizer build (as CI runs one), any memory error fails the test
        assert "ERROR: AddressSanitizer" not in lm.stdout(), "AddressSanitizer reported an error"
        assert lm.proc.returncode in (0, -15), f"linmail-pdn exited with {lm.proc.returncode}"

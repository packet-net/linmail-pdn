"""The log directory length limit.

The mail code keeps each log file name in a fixed 100 byte buffer, and a
name that doesn't fit aborts the program (UPSTREAM-BUGS.md). linmail-pdn
works out the longest log directory that fits from the same log names the
mail code uses, and refuses a longer one at start-up. These tests pin that
boundary from both sides, for the data directory (where the logs go by
default) and for a separate log directory given with -l.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

from conftest import HERE, BIN, Linmail, linmail_cfg, log_dir_limit, upstream_log_names
from test_linmail_pdn import login

pytestmark = pytest.mark.skipif(not BIN.exists(), reason=f"{BIN} not built (make -C pdn)")


def path_of_length(base: Path, length: int) -> Path:
    """A directory under base whose path is exactly length characters."""
    pad = length - len(str(base)) - 1
    assert 1 <= pad <= 255, f"{base} is too long to build a {length} character path under"
    path = base / ("d" * pad)
    assert len(str(path)) == length
    return path


def dirs(short_tmp: Path, where: str, length: int) -> tuple[Path, Path, list[str]]:
    """The data directory, the log directory of the given length, and the
    extra arguments to use them."""
    if where == "datadir":
        data = path_of_length(short_tmp, length)
        return data, data, []
    data = short_tmp / "data"
    logs = path_of_length(short_tmp, length)
    logs.mkdir()
    return data, logs, ["-l", str(logs)]


def test_log_dir_limit_matches_upstream():
    """linmail-pdn.c declares FilesNames and Logs to work the limit out; the
    declarations must match the mail code's, or the limit is wrong."""
    count, size, names = upstream_log_names()
    shim = (HERE.parent / "linmail-pdn.c").read_text()
    assert f"extern char FilesNames[{count}][{size}];" in shim
    assert f"extern char * Logs[{len(names)}];" in shim
    assert count == len(names)


@pytest.mark.parametrize("where", ["datadir", "logdir"])
def test_log_dir_at_the_limit_starts_and_logs(short_tmp, fake, where):
    """At exactly the limit linmail-pdn starts, takes a session, and writes
    every log it keeps. The DEBUG log has the longest name, so its full path
    is exactly 99 characters, the most the 100 byte buffer holds."""
    limit = log_dir_limit()
    data, logs, args = dirs(short_tmp, where, limit)
    data.mkdir(exist_ok=True)
    lm = Linmail(data, fake)
    lm.write("linmail.cfg", linmail_cfg())
    try:
        lm.start(*args)
        lm.wait_stdout(re.escape(f"logs in {logs}/logs"))
        fake.wait_msg("listenReply", direction="out")
        h = login(fake)
        fake.recv(h, "B\r")
        fake.wait(lambda: fake.closed(h), what="linmail-pdn to close the session")

        # The mail code writes the BBS log for sessions and the DEBUG log from
        # start-up. CHAT and TCP belong to the chat server and SMTP/POP3,
        # which linmail-pdn never starts.
        def written(name: str) -> Path | None:
            found = [p for p in (logs / "logs").glob(f"log_*_{name}.txt") if p.stat().st_size]
            return found[0] if found else None

        fake.wait(lambda: written("DEBUG") and written("BBS")
                  and re.search(r"N0USR\s+N0USR Disconnected", written("BBS").read_text(errors="replace")),
                  timeout=20, what="the DEBUG log, and the session in the BBS log")
        debug = written("DEBUG")
        assert len(str(debug)) == 99, debug
        for name in ("BBS", "DEBUG"):
            link = data / f"logLatest_{name}.txt"
            assert os.readlink(link) == str(written(name)), link
        assert lm.proc.poll() is None, "linmail-pdn stopped"
    finally:
        lm.stop()
        out = lm.stdout()
        print(out[-5000:])
    assert "ERROR: AddressSanitizer" not in out, "AddressSanitizer reported an error"
    assert "buffer overflow" not in out
    assert lm.proc.returncode in (0, -15), f"linmail-pdn exited with {lm.proc.returncode}"


@pytest.mark.parametrize("where", ["datadir", "logdir"])
def test_log_dir_one_over_the_limit_is_refused(short_tmp, where):
    """One character over, linmail-pdn refuses to start, says why and what
    the limit is, and exits cleanly instead of crashing in the mail code."""
    limit = log_dir_limit()
    data, logs, args = dirs(short_tmp, where, limit + 1)
    data.mkdir(exist_ok=True)
    (data / "linmail.cfg").write_text(linmail_cfg())
    env = {k: v for k, v in os.environ.items() if not k.startswith("PDN_")}
    r = subprocess.run([str(BIN), "-d", str(data), "-r", "127.0.0.1:1", "-W", "0", *args],
                       cwd=data, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       errors="replace", timeout=30)
    out = r.stdout + r.stderr
    print(out)
    assert r.returncode == 1, f"exit {r.returncode}"
    assert (f"linmail-pdn: the log directory {logs} is {limit + 1} characters long, "
            f"and the mail code allows at most {limit}.") in out
    assert "buffer overflow" not in out and "AddressSanitizer" not in out
    assert not (logs / "logs").exists()

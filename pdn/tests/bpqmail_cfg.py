"""Build a BPQMail.cfg with a forwarding-partner entry pre-populated.

Copied from tests/integration/helpers in the linbpq fork where linmail-pdn
started; G8BPQ's tree has no test suite.

BPQMail uses libconfig syntax, which looks like JSON with `;` line
terminators.  Important groups:

- ``main``: global params (BBSName, SYSOPCall, MaxStreams, etc.)
- ``BBSUsers``: known users / BBSes; each entry's value is a single
  ``^``-delimited string carrying Name / flags / BBSNumber / etc.
  Flags bit 0x10 (``F_BBS`` = 16) marks the entry as a partner BBS.
- ``BBSForwarding``: per-partner forwarding config: TOCalls,
  ATCalls, HRoutes, FWDTimes, ConnectScript, plus the boolean
  / numeric options visible in the web UI's FwdDetail screen.
- ``Housekeeping``: global housekeeping config.

This helper builds a minimal-but-functional cfg with one or more
forwarding partners configured.  All forwarding-partner options
exposed by the FwdDetail web form are spelled out as keyword
parameters with sensible defaults.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable


@dataclass
class FwdPartner:
    """Settings exposed by the BPQMail FwdDetail web-config screen.

    Defaults match the screen's defaults except where empty multi-strings
    would prevent the BBS from treating us as a forwarding peer at all.
    """

    call: str

    # Multi-line textarea inputs.  Empty list = empty textarea.
    to_calls: list[str] = field(default_factory=list)
    at_calls: list[str] = field(default_factory=list)
    hroutes_bulls: list[str] = field(default_factory=list)
    hroutes_personal: list[str] = field(default_factory=list)
    fwd_times: list[str] = field(default_factory=list)
    connect_script: list[str] = field(default_factory=list)

    # Single-string fields.
    bbs_ha: str = ""

    # Boolean toggles (FwdDetail checkboxes).
    enabled: bool = True
    request_reverse: bool = False
    send_new_immediately: bool = False
    allow_blocked: bool = True  # F flag in SID, required for FBB mode
    allow_compressed: bool = False  # B flag in SID
    allow_b1: bool = False
    allow_b2: bool = False
    send_ctrlz: bool = True  # ASCII msg terminator (\x1A vs /EX)
    personal_only: bool = False

    # Numeric tunables.
    fwd_interval: int = 600  # seconds
    rev_fwd_interval: int = 0
    max_fbb_block: int = 256
    con_timeout: int = 60


def _libconfig_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _multistring(lines: Iterable[str]) -> str:
    """BPQMail multi-string is a single ``|``-delimited string.

    Format read by ``GetMultiStringValue`` (BBSUtilities.c:7540) and
    written by ``SaveMultiStringValue``: the setting is a libconfig
    string whose value is the entries joined by ``|``.  An empty
    multi-string is the empty string ``""``.
    """
    return _libconfig_string("|".join(lines))


def _user_record_string(
    *, name: str = "BBS", flags: int = 0x10, bbs_number: int = 1
) -> str:
    """The ``BBSUsers.<call>`` value is a single ``^``-delimited string.

    Field layout (read in ``GetUserDatabase``):
    ``Name^Address^HomeBBS^QRA^pass^ZIP^CMSPass^lastmsg^flags^PageLen^
    BBSNumber^RMSSSIDBits^WebSeqNo^TimeLastConnected^Stats^LastStats``

    ``Stats`` and ``LastStats`` are big-endian ints stuffed into the
    string by ``GetNetInt``; for an "empty" partner we pass enough
    zero bytes via repeated separators that the parser pulls 0 for
    every counter.  The total stat counters are 26 ints * 4 bytes
    each = 104 bytes per stats group, but ``GetNetInt`` is happy
    with shorter input and just yields 0.
    """
    parts = [
        name,                       # Name
        "",                         # Address
        "",                         # HomeBBS
        "",                         # QRA
        "",                         # pass
        "",                         # ZIP
        "",                         # CMSPass
        "0",                        # lastmsg
        str(flags),                 # flags
        "0",                        # PageLen
        str(bbs_number),            # BBSNumber
        "0",                        # RMSSSIDBits
        "0",                        # WebSeqNo
        "0",                        # TimeLastConnected
        "",                         # Stats (empty -> all 0)
        "",                         # LastStats (empty -> all 0)
    ]
    return "^".join(parts)


def _bool(b: bool) -> str:
    return "1" if b else "0"


def render_bpqmail_cfg(
    *,
    bbs_call: str,
    sysop_call: str = "",
    hroute: str = "",
    partners: Iterable[FwdPartner] = (),
    extra_users: Iterable[tuple[str, str]] = (),
    bbs_appl_num: int = 1,
    max_streams: int = 10,
) -> str:
    """Render a complete BPQMail.cfg.

    ``extra_users`` lets tests add non-BBS user records (e.g. local
    senders); each tuple is ``(call, raw_field_string)``.
    """
    if not sysop_call:
        sysop_call = bbs_call

    out: list[str] = []
    out.append("main:\n{")
    out.append(f"  Streams = {max_streams};")
    out.append(f"  BBSApplNum = {bbs_appl_num};")
    out.append(f"  BBSName = {_libconfig_string(bbs_call)};")
    out.append(f"  SYSOPCall = {_libconfig_string(sysop_call)};")
    out.append(f'  H-Route = {_libconfig_string(hroute)};')
    out.append("  EnableUI = 0;")
    out.append("  RefuseBulls = 0;")
    out.append("  SendBBStoSYSOPCall = 0;")
    out.append("  DontHoldNewUsers = 1;")
    out.append("  DontCheckFromCall = 1;")
    out.append("  DontNeedHomeBBS = 1;")
    out.append("  DontNeedName = 1;")
    out.append("  AllowAnon = 1;")
    out.append("  MailForInterval = 0;")
    out.append("  MaxTXSize = 99999;")
    out.append("  MaxRXSize = 99999;")
    out.append("  Log_BBS = 1;")
    out.append("  Log_TCP = 1;")
    out.append('  Version = "6,0,25,23";')
    out.append("};")
    out.append("")

    # Partners
    out.append("BBSForwarding:\n{")
    for i, p in enumerate(partners, 1):
        # libconfig names: must start with letter or underscore;
        # subsequent chars can include hyphens (so SSID-bearing
        # calls like ``N0BBB-1`` are fine as keys).  BPQMail's own
        # writer prefixes ``*`` for digit-first calls
        # (BBSUtilities.c:10059-10072); we follow the same rule.
        key = f"*{p.call}" if not p.call[0].isalpha() else p.call
        out.append(f"  {key}:")
        out.append("  {")
        out.append(f"    TOCalls = {_multistring(p.to_calls)};")
        out.append(f"    ATCalls = {_multistring(p.at_calls)};")
        out.append(f"    HRoutes = {_multistring(p.hroutes_bulls)};")
        out.append(f"    HRoutesP = {_multistring(p.hroutes_personal)};")
        out.append(f"    FWDTimes = {_multistring(p.fwd_times)};")
        out.append(f"    ConnectScript = {_multistring(p.connect_script)};")
        out.append(f"    Enabled = {_bool(p.enabled)};")
        out.append(f"    RequestReverse = {_bool(p.request_reverse)};")
        out.append(f"    AllowBlocked = {_bool(p.allow_blocked)};")
        out.append(f"    AllowCompressed = {_bool(p.allow_compressed)};")
        out.append(f"    UseB1Protocol = {_bool(p.allow_b1)};")
        out.append(f"    UseB2Protocol = {_bool(p.allow_b2)};")
        out.append(f"    SendCTRLZ = {_bool(p.send_ctrlz)};")
        out.append(f"    FWDPersonalsOnly = {_bool(p.personal_only)};")
        out.append(f"    FWDNewImmediately = {_bool(p.send_new_immediately)};")
        out.append(f"    FwdInterval = {p.fwd_interval};")
        out.append(f"    RevFWDInterval = {p.rev_fwd_interval};")
        out.append(f"    MaxFBBBlock = {p.max_fbb_block};")
        out.append(f"    ConTimeout = {p.con_timeout};")
        out.append(f"    BBSHA = {_libconfig_string(p.bbs_ha)};")
        out.append("  };")
    out.append("};")
    out.append("")

    # Users: partner BBS records carry F_BBS=0x10 plus a BBSNumber.
    # Same key-naming rule as BBSForwarding above.
    out.append("BBSUsers:\n{")
    for i, p in enumerate(partners, 1):
        key = f"*{p.call}" if not p.call[0].isalpha() else p.call
        record = _user_record_string(
            name="BBS", flags=0x10, bbs_number=i
        )
        out.append(f"  {key} = {_libconfig_string(record)};")
    for call, record in extra_users:
        key = f"*{call}" if not call[0].isalpha() else call
        out.append(f"  {key} = {_libconfig_string(record)};")
    out.append("};")
    out.append("")

    # Empty Housekeeping group keeps BPQMail happy.
    out.append("Housekeeping:\n{};")
    out.append("")

    return "\n".join(out)

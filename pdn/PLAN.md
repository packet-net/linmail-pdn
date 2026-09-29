# linmail-pdn: the LinBPQ mail BBS as a packet.net app

linmail-pdn is the LinBPQ mail server ("linmail") built on its own, without the node, and connected to a packet.net (pdn) node over pdn's RHPv2 server. It uses the unchanged upstream mail sources, so John's mail changes keep flowing in with a normal merge. It sits alongside pdn-bbs (the from-scratch .NET BBS for pdn); both are wanted.

It lives in [packet-net/linmail-pdn](https://github.com/packet-net/linmail-pdn), a fork of G8BPQ's LinBPQ: `upstream` mirrors John's master, `main` is that plus `pdn/` and the workflows. `README.md` in this directory is the landing page and describes how John's releases flow in. Until 2026-09-29 the work lived on the `pdn-linmail` branch of a personal linbpq fork, on an older LinBPQ base.

## How it works

On Windows, BPQMail was always a separate program talking to the node through the bpq32 host API. LinBPQ compiles the same mail code into the node. linmail-pdn goes back to the separate-program model, with pdn in place of bpq32:

- `pdnhost.c` implements the host API calls the mail code makes (`SessionControl`, `SessionState`, `GetMsg`, `SendMsg`, `GetConnectionInfo`, `TXCount` and the rest) on one RHPv2 connection to pdn.
- Inbound: `socket`/`bind`/`listen` on the BBS callsign and any aliases (such as `BBS`). Each accepted child handle is attached to a free BBS stream, so the mail code sees an ordinary connect, data and disconnect. A connect that arrives while every stream is still reporting its last disconnect waits up to 5 seconds for one.
- Closing: when the mail code disconnects a session, the shim closes its RHP handle straight away. pdn keeps the link up until the far end has everything already sent, then disconnects (packet.net#850, fixed in pdn after 0.57.0), so nothing the BBS sent last is lost. If pdn has already closed the link from the far end, there is nothing to close.
- Idle sessions are dropped after the idle time the mail code sets (`ChangeSessionIdletime`, 15 minutes by default, or a connect script's `IDLETIME`), as BPQ does.
- UI frames the mail code sends (FBB message header broadcasts and mail-for beacons) go out through one RHP `dgram` socket, on the pdn port the port map gives for the `UIPortN` number.
- Outbound forwarding keeps the BPQ model. `ConnectUsingAppl` gives the stream a session with an emulated node command handler. The connect script's `C port CALL` becomes an RHP `open`, answered with BPQ-style node text (`N0PDN} Connected to CALL`, `Failure with CALL`, `Busy from CALL`). After that the stream is transparent, so later script lines go to the far node as data and multi-hop scripts work unchanged.
- `pdnweb.c` serves webmail and the management pages on a loopback port (18095) for pdn's app gateway, calling the same upstream handler LinBPQ's `HTTPcode.c` does (`ProcessMailHTTPMessage`). It trusts the gateway's identity instead of BPQ's logins: requests need `X-Pdn-Gateway: 1` and a loopback `Host` (anything else, including direct access and DNS rebinding, is refused); `X-Pdn-User` picks the BBS user (a callsign username is that BBS user; an admin with a plain username is the BBS sysop); the management pages, and any BBS account flagged sysop, need `X-Pdn-Scope: admin`. An admin always reaches the management pages, acting as the sysop, even without a BBS account; webmail needs one, and says so. Requests that would crash the upstream webmail code are refused first (see `UPSTREAM-BUGS.md`). The pages' root-relative links are rewritten under `X-Forwarded-Prefix` (`/apps/linmail`). The page templates are LinBPQ's built-in ones (upstream `HTMLCommonCode.c` and `templatedefs.c`, linked as they are), so they change when John changes them. The two pictures the pages use (`background.jpg`, `favicon.ico`) come from the data directory's `HTML/`, as in LinBPQ, then from `pdn/HTML` as installed with the package.
- `linmail-pdn.c` is the mail half of `LinBPQ.c`: it loads `linmail.cfg` and the mail files from a directory and runs the same timer calls at the same rates (one tick every 100 ms, forwarding every 2 s, slow timers every 10 s).
- The node structures the mail code reads directly (`BPQHOSTVECTOR`, the session's `L4USER`, `Secure_Session`, the `L4CROSSLINK` walk in `Connected()`) are satisfied by a small fake vector in `pdnhost.c`, so no upstream source file needed changing.

## Build and run

```
make -C pdn
pdn/linmail-pdn -d /var/lib/linmail
```

The data directory holds `linmail.cfg`, the mail files and `linmail-pdn.conf`, the shim's own settings (RHP address and login, callsign and aliases, node call, port map, web port; see `linmail-pdn.conf.example`). Settings are read from that file, then from the pdn app supervisor's environment (`PDN_APP_STATE`, `PDN_RHP_HOST`, `PDN_RHP_PORT`, `PDN_APP_CALLSIGN`, `PDN_NODE_CALLSIGN`, `PDN_NODE_ALIAS`, plus `PDN_LINMAIL_PORTMAP` and `PDN_LINMAIL_DEFAULTPORT`), then from the command line (`linmail-pdn -h`). The port map turns the port numbers in existing BPQ connect scripts into pdn port ids. Needs libjansson and libconfig.

## Tests and CI

`pdn/tests` has these suites (pytest), plus `bpqmail_cfg.py`, the linmail.cfg builder they share:

- `test_linmail_pdn.py` drives linmail-pdn against a fake RHP server (`fake_rhp.py`): listening on the call and an alias, the settings file order, RHP auth, a user session, B2 forwarding out (checking the FBB block framing and checksum) and in (replaying a transfer recorded from a real LinBPQ), connect script failures and unsupported commands, a multi-hop script, the idle timeout, closing as soon as the mail code hangs up, UI frames, reconnecting when pdn restarts, a non-callsign caller, and the SMTP/POP3/NNTP servers staying off.
- `test_real_pdn.py` runs a real pdn node (`PDN_BIN`, the `packetnet` binary from a release) and a LinBPQ built from this tree (`LINBPQ_BIN`): UI frames accepted by pdn, a user session over AX.25, B2 forwarding both ways, and RHP auth against a pdn user.
- `test_real_pdn.py` also checks that nothing is lost when the BBS closes a session: over a slowed-down link (a relay that holds every packet for 0.4 seconds, with pdn sending one frame at a time), a user reads a 4 KB message and says bye as soon as it starts to arrive. The BBS disconnects a second later, and the end of the message must still reach LinBPQ; it gets there about half a minute after the close. pdn 0.57.0 and earlier throw the rest of the message away at the close, so the test is skipped for them (the version comes from pdn's status API) and runs once a pdn release includes packet.net#850. `PDN_GRACEFUL_CLOSE=1` runs it anyway (it fails on 0.57.0), `PDN_GRACEFUL_CLOSE=0` always skips it.

- `test_web.py` covers the web front end against the running binary with the gateway headers faked: direct and anonymous access refused, webmail for a callsign user with every link under the prefix, composing in webmail, sessions not shared between users, unknown callsigns and plain usernames, the management pages only for admin scope, and sysop BBS accounts only for admins.
- `test_real_pdn.py` also has pdn discover the app package, start linmail-pdn itself with the callsign it assigns, and serve the pages through its real gateway to an admin and a read-only user.
- `test_deb.py` installs the built .deb (needs sudo), lets pdn find it in `/usr/share/packetnet/apps/linmail` and run it with state in `/var/lib/packetnet/apps/linmail`, checks an AX.25 user session and webmail, then removes everything.

`pdn/tests/ci.sh` runs the lot the way CI does, in stages (dependencies, builds, fetching the latest pdn node release, the suites, the ASan run, the .deb build and install); `pdn/tests/ci.sh build fake asan` runs just the builds and those two. `.github/workflows/linmail-pdn.yml` runs every stage, one per step, on every push to `main` and every pull request into it. LinBPQ 6.0.25.41 and later need libbacktrace to link, which comes with gcc (`-lbacktrace`).

`.github/workflows/upstream-sync.yml` (`pdn/upstream-sync.sh`) runs daily and on demand. It fetches John's master from his git server (`git://vps1.g8bpq.net/linbpq`, or `https://github.com/g8bpq/linbpq` if that is unreachable), fast-forwards `upstream`, and if `main` doesn't have it yet and nothing has been raised for that commit before, merges it into `sync/upstream-<version>` off `main`, runs `pdn/tests/ci.sh` and opens a pull request with the result (marked as failing, with the log tail, on a test failure; showing the conflicts, untested, on a merge conflict). The packet-net organisation doesn't currently let workflows open pull requests, so until that changes it opens an issue with the same text and a one-click link to open the pull request. Merge these with a merge commit, not squash or rebase.

## Packaging

`packaging/` holds the app manifest (`pdn-app.yaml`: id `linmail`, name "LinBPQ Mail", capabilities `packet` and `web`, a pdn-supervised `service`, the `ui` upstream on 18095 shown `embedded` in the panel, node verb `MAIL` so it does not collide with pdn-bbs's `BBS`), the Debian control file and postinst, `build-deb.sh`, and `build-in-docker.sh`, which builds in a Debian bookworm container (under QEMU for arm) so the binary needs only libc 2.36 and runs on bookworm and trixie. jansson and libconfig are linked in (`make -C pdn static`) because libconfig's soname differs between Debian releases. The package installs code to `/usr/share/packetnet/apps/linmail`; state stays in `/var/lib/packetnet/apps/linmail` and is never shipped.

Versions are `<LinBPQ version>-pdn<n>`: John's version from `Versions.h`, then our own release number for it, from 1. The tag is `v6.0.25.41-pdn1`, the package version `6.0.25.41-pdn1`, and `linmail-pdn --version` prints both (`linmail-pdn 6.0.25.41-pdn1`, `LinBPQ 6.0.25.41`). The Makefile takes it as `PDN_VERSION` (default `<LinBPQ version>-dev`); `build-deb.sh` refuses a binary built with a different one. A new LinBPQ version starts again at `-pdn1`. CI builds are `<LinBPQ version>-pdn0~ci<run>`, which sort before `-pdn1`.

`.github/workflows/linmail-pdn-release.yml` builds the amd64, arm64 and armhf .debs. A `v<LinBPQ version>-pdn<n>` tag whose LinBPQ part matches `Versions.h` attaches them to a GitHub Release; a push to `main` that touches the packaging builds them as artifacts only. Nothing here publishes to the packet-net apt repo or catalog.

linmail-pdn needs a pdn node newer than 0.57.0 (packet.net#850). Neither the app manifest nor pdn's app catalog has a field for a minimum node version, so this is only written down, in `README.md`, `MIGRATING.md` and a comment in `pdn-app.yaml`.

Before the first release:

- Once a pdn node release with packet.net#852 is out, replace "newer than 0.57.0" with its version number in `README.md`, `MIGRATING.md` and `pdn-app.yaml`, and don't publish a linmail-pdn release before it. CI then runs the close test in `test_real_pdn.py` instead of skipping it; check it passes.

`MIGRATING.md` is the sysop's guide to moving a LinBPQ mailbox across; it ships in the package as `/usr/share/doc/pdn-linmail/MIGRATING.md`.

## Lab

`lab/` holds the loopback lab used for the proof: a pdn node (N0PDN, one AXUDP port `bpq`, RHP on 19000), a LinBPQ node with mail (N0BPQ, BBS N0BPQ-1) built from this tree, and linmail-pdn (BBS N0LMB). `lab/lab.sh start WORKDIR packetnet linbpq` starts all three; `lab/session.py` logs in to the LinBPQ telnet port as N0USR, connects to a BBS, sends a personal message, lists it and reads it back. Posting to `N0ABC @ N0BPQ` on N0LMB, or to `N0XYZ @ N0LMB` on N0BPQ (`BBS` = `LOCAL`), forwards it with FBB B2 compression within a few seconds.

## Upstream edits

None, and there must never be any: John's files stay exactly as he publishes them, so his releases merge cleanly. The build links `BBSUtilities.c BBSHTMLConfig.c FBBRoutines.c MailCommands.c MailDataDefs.c MailRouting.c MailTCP.c MBLRoutines.c WPRoutines.c WebMail.c NNTPRoutines.c lzhuf32.c` plus the node-free helpers `Housekeeping.c UIRoutines.c utf8Routines.c md5.c compatbits.c CMSAuth.c HTMLCommonCode.c` (`make -C pdn -s upstream-sources` lists them). Anything an upstream change breaks is fixed in `pdn/`.

## Upstream drift

What changed under the shim when it moved from the old base (LinBPQ 6.0.25.28 plus the personal fork's own changes) to John's 6.0.25.41, and what was done about it:

- `Debugprintf` moved out of `BBSUtilities.c` into the node's `CommonCode.c`, and the mail code's `LOG_DEBUG_X` lines now go through it. `linmail-pdn.c` provides it, writing to the mail debug log (`logs/log_YYMMDD_DEBUG.txt`) where those lines went before.
- `WriteLogLine` now keeps each log file open for up to 30 seconds instead of closing it after every line, so lines could sit unwritten. The main loop flushes the log files every tick (under the log semaphore).
- The old base had the HTML page templates as files in `HTML/`; John's tree builds them in (`templatedefs.c`, which LinBPQ prefers to any file anyway) and has no `HTML/`. The shim now links upstream's `HTMLCommonCode.c` instead of its own template loader, and ships only the two pictures, in `pdn/HTML`.
- The tests used a helper from the old fork's own test suite (`tests/integration/helpers/bpqmail_cfg.py`), which John's tree doesn't have. It is now `pdn/tests/bpqmail_cfg.py`.
- LinBPQ itself (built for the real pdn tests) now links libbacktrace.
- No change needed: the node structures the shim fakes (`asmstrucs.h` changes are all in routing and NET/ROM fields the mail code doesn't read), `struct HTTPConnectionInfo` (rebuilt from the header), `struct HtmlFormDir` (unchanged), `ProcessMailHTTPMessage` and the other host API calls, and the timer rates in `LinBPQ.c`. John's other mail changes (a new user's `L` list starts at the last 20 messages, paging in the PG server, case-insensitive SMTP and POP3 commands, a webmail websocket URL fix) come along as they are. The upstream bugs in `UPSTREAM-BUGS.md` are all still there.

## Out of scope

Decided (Tom, 2026-09-29), not planned:

- The SMTP, POP3 and NNTP servers. linmail-pdn never starts them, even if `linmail.cfg` sets their ports; it logs one line saying so and leaves the settings in the file untouched.
- Connect script commands that only a BPQ node understands. These answer `Sorry, ...`, which the script engine treats as a failed connect (so an `ELSE` block still runs), and are logged:
  - `C CALL V DIGI ...` (digipeated connects): RHP `open` has no digipeater path.
  - `C ALIAS` where the target is a NET/ROM alias rather than a plain callsign.
  - `NC`, `ATTACH`/`A`, `MCAST` (the `MCASTRX` script keyword), `RADIO ...` (including `RADIO AUTH`), and any other node command.

Other connect script notes: a bare `C CALL` with no port only works if the call is another app on the same pdn node, or if `default_port` is set; pdn refuses an RHP open with no port otherwise. `C port CALL S` (stay) is accepted and the `S` ignored. `PACLEN` and `IDLETIME` sent to the node get `Ok`. Script keywords the mail code handles itself (`TIMES`, `ELSE`, `PAUSE`, `MSGTYPE`, `INTERLOCK`, `SKIPCON`, `SKIPPROMPT`, `TEXTFORWARDING`, `FILE`, `IMPORT` and so on) are unaffected.

## Stubbed or not wired up yet

In `pdnstubs.c` and `pdnhost.c`:

- The mail JSON API (`/Mail/API/v1/`, mailapi.c) and the webmail websocket index refresh (`RefreshWebMailIndex`) are not wired up; `/Mail/API/` answers 404.
- BPQ's own web logins (the Mail signon page, the WebMail password form) are not used: identity comes from the pdn gateway, and direct access is refused.
- UI frames received: `GetRaw` returns nothing, so the BBS does not answer FBB header resync requests (`? 00000...`) heard on air. UI frames sent lose any digipeater path (`UIPortN` `Digis`), as RHP datagrams have none.
- Multicast mail (`MCAST`), event programs, packet map reporting, the node trace copy of the BBS log (`BPQTRACE`), APRS messages and position: empty.
- MQTT: built with `NOMQTT`. Winlink session reporting: the frequency/mode lookup in `Connected()` finds no node session to read, so it reports 0.
- The ISP mail gateway (an SMTP/POP3 client, not a server) is compiled as in LinBPQ and untested.
- `GetConnectionInfo` never reports a secure (console) session, so sysop rights come only from the user record.

## Things found upstream

`UPSTREAM-BUGS.md` lists the bugs found in LinBPQ's own mail code (unchecked webmail form fields and template numbers, a bare `ELSE` read past its end, `UndoTransparency` on a trailing `%`, the 100 byte log file name buffer), how linmail-pdn works round each without editing upstream files, and a suggested fix for each.

## Things found in pdn

- The RHP server resolves `port` by port id only (`src/Packet.Node/Rhp/SupervisorRhpGateway.cs` `ResolvePortId`, line 241), but `docs/rhp2-server.md` line 18 says `port` is the 1-indexed port number and that a missing port means the first port. An open with `"port":"1"` gets `errCode 10 No such port '1'`, and an open with no port is refused unless the target is a local app. The shim works round it with the port map.
- A failed open cannot tell "busy" (DM) from "no answer": both come back as errCode 15 with "torn down before DL-CONNECT-confirm arrived (peer refused or link reset)" (`src/Packet.Ax25/Session/Ax25Listener.cs` line 573). Tracked as packet.net#849. The shim reports `Failure with` for both.
- Closing an RHP handle used to disconnect the link at once, throwing away anything still queued, so the shim held each close for 10 seconds after its last send. pdn now holds the disconnect until the far end has everything already sent, as BPQ does (packet.net#850, fixed by packet.net#852 after pdn 0.57.0), and the shim closes straight away. linmail-pdn therefore needs a pdn newer than 0.57.0. pdn still has no queue-depth or Busy signal on `sendReply`, so the mail code hands everything to pdn at once.
- A pdn telnet console user who types `C <app call>` reaches the app with `accept.remote` set to the telnet peer address (`127.0.0.1:51398`), not a callsign. The mail code keeps callsigns in 10 byte fields, and this crashed the first build of the shim. The shim now refuses non-callsign peers with a message.

## Order of work

- [x] 1. Proof of concept (2026-09-28): standalone build with no node code and no upstream edits; RHP host API shim; inbound user sessions; outbound connect scripts including failure, `ELSE` and multi-hop; FBB B2 compressed forwarding both ways with a lab LinBPQ through pdn; reconnect when pdn restarts.
- [x] 2. Hardening (2026-09-29): idle timeout enforced; alias callsigns (`alias = BBS`); RHP auth, tested against a real pdn user; `linmail-pdn.conf` for the shim's own settings; UI frames sent through an RHP `dgram` socket; SMTP/POP3/NNTP servers never started; early inbound connects wait for a free stream. Still open: receiving UI frames.
- [x] 3. Tests (2026-09-29): `pdn/tests`, a fake RHP server suite and a run against a real pdn node and LinBPQ.
- [x] 4. CI (2026-09-29): `.github/workflows/linmail-pdn.yml`.
- [x] 5. Web (2026-09-29): `pdnweb.c`, webmail and the management pages through pdn's app gateway with pdn identity; tested faked and through a real pdn gateway.
- [x] 6. Packaging (2026-09-29): `pdn-app.yaml`, amd64/arm64/armhf .debs built by `linmail-pdn-release.yml` on a release tag (now `v<LinBPQ version>-pdn<n>`); the amd64 .deb installed and run by a real pdn in CI. Still to do, outside this repo: publishing a release, the packet-net apt repo and the pdn app catalog entry.
- [x] 7. Migration notes (2026-09-29): `MIGRATING.md`.
- [x] Move (2026-09-29): to packet-net/linmail-pdn on John's 6.0.25.41 (see Upstream drift), with a daily upstream sync workflow and `<LinBPQ version>-pdn<n>` release versions.
- [x] Close without waiting (2026-09-29): the 10 second wait before closing a session is gone, now that pdn delivers everything sent before a close (packet.net#850). Needs a pdn newer than 0.57.0.
- [ ] 8. Later, if wanted: MQTT, Winlink reporting, packet map, answering FBB header resync requests.

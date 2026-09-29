# linmail-pdn: the LinBPQ mail BBS as a packet.net app

linmail-pdn is the LinBPQ mail server ("linmail") built on its own, without the node, and connected to a packet.net (pdn) node over pdn's RHPv2 server. It uses the unchanged upstream mail sources, so John's mail changes keep flowing in with a normal merge. It sits alongside pdn-bbs (the from-scratch .NET BBS for pdn); both are wanted.

## How it works

On Windows, BPQMail was always a separate program talking to the node through the bpq32 host API. LinBPQ compiles the same mail code into the node. linmail-pdn goes back to the separate-program model, with pdn in place of bpq32:

- `pdnhost.c` implements the host API calls the mail code makes (`SessionControl`, `SessionState`, `GetMsg`, `SendMsg`, `GetConnectionInfo`, `TXCount` and the rest) on one RHPv2 connection to pdn.
- Inbound: `socket`/`bind`/`listen` on the BBS callsign and any aliases (such as `BBS`). Each accepted child handle is attached to a free BBS stream, so the mail code sees an ordinary connect, data and disconnect. A connect that arrives while every stream is still reporting its last disconnect waits up to 5 seconds for one.
- Idle sessions are dropped after the idle time the mail code sets (`ChangeSessionIdletime`, 15 minutes by default, or a connect script's `IDLETIME`), as BPQ does.
- UI frames the mail code sends (FBB message header broadcasts and mail-for beacons) go out through one RHP `dgram` socket, on the pdn port the port map gives for the `UIPortN` number.
- Outbound forwarding keeps the BPQ model. `ConnectUsingAppl` gives the stream a session with an emulated node command handler. The connect script's `C port CALL` becomes an RHP `open`, answered with BPQ-style node text (`N0PDN} Connected to CALL`, `Failure with CALL`, `Busy from CALL`). After that the stream is transparent, so later script lines go to the far node as data and multi-hop scripts work unchanged.
- `linmail-pdn.c` is the mail half of `LinBPQ.c`: it loads `linmail.cfg` and the mail files from a directory and runs the same timer calls at the same rates (one tick every 100 ms, forwarding every 2 s, slow timers every 10 s).
- The node structures the mail code reads directly (`BPQHOSTVECTOR`, the session's `L4USER`, `Secure_Session`, the `L4CROSSLINK` walk in `Connected()`) are satisfied by a small fake vector in `pdnhost.c`, so no upstream source file needed changing.

## Build and run

```
make -C pdn
pdn/linmail-pdn -d /var/lib/linmail
```

The data directory holds `linmail.cfg`, the mail files and `linmail-pdn.conf`, the shim's own settings (RHP address and login, callsign and aliases, node call, port map; see `linmail-pdn.conf.example`). Settings are read from that file, then from the pdn app supervisor's environment (`PDN_APP_STATE`, `PDN_RHP_HOST`, `PDN_RHP_PORT`, `PDN_APP_CALLSIGN`, `PDN_NODE_CALLSIGN`, `PDN_NODE_ALIAS`, plus `PDN_LINMAIL_PORTMAP` and `PDN_LINMAIL_DEFAULTPORT`), then from the command line (`linmail-pdn -h`). The port map turns the port numbers in existing BPQ connect scripts into pdn port ids. Needs libjansson and libconfig.

## Tests and CI

`pdn/tests` has two suites (pytest):

- `test_linmail_pdn.py` drives linmail-pdn against a fake RHP server (`fake_rhp.py`): listening on the call and an alias, the settings file order, RHP auth, a user session, B2 forwarding out (checking the FBB block framing and checksum) and in (replaying a transfer recorded from a real LinBPQ), connect script failures and unsupported commands, a multi-hop script, the idle timeout, UI frames, reconnecting when pdn restarts, a non-callsign caller, and the SMTP/POP3/NNTP servers staying off.
- `test_real_pdn.py` runs a real pdn node (`PDN_BIN`, the `packetnet` binary from a release) and a LinBPQ built from this tree (`LINBPQ_BIN`): UI frames accepted by pdn, a user session over AX.25, B2 forwarding both ways, and RHP auth against a pdn user.

`.github/workflows/linmail-pdn.yml` builds linmail-pdn and LinBPQ, fetches the latest pdn node release, and runs both suites on every push to `pdn-linmail` or `master` that touches the mail code or `pdn/`.

## Lab

`lab/` holds the loopback lab used for the proof: a pdn node (N0PDN, one AXUDP port `bpq`, RHP on 19000), a LinBPQ node with mail (N0BPQ, BBS N0BPQ-1) built from this fork, and linmail-pdn (BBS N0LMB). `lab/lab.sh start WORKDIR packetnet linbpq` starts all three; `lab/session.py` logs in to the LinBPQ telnet port as N0USR, connects to a BBS, sends a personal message, lists it and reads it back. Posting to `N0ABC @ N0BPQ` on N0LMB, or to `N0XYZ @ N0LMB` on N0BPQ (`BBS` = `LOCAL`), forwards it with FBB B2 compression within a few seconds.

## Upstream edits

None. Every mail source file builds as it is. The build links `BBSUtilities.c BBSHTMLConfig.c FBBRoutines.c MailCommands.c MailDataDefs.c MailRouting.c MailTCP.c MBLRoutines.c WPRoutines.c WebMail.c NNTPRoutines.c lzhuf32.c` plus the node-free helpers `Housekeeping.c UIRoutines.c utf8Routines.c md5.c compatbits.c CMSAuth.c`. If a later upstream change needs a pdn-specific branch, put it behind `#ifdef PDN_LINMAIL` and list it here.

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

- Webmail and the BBS config web pages: compiled, never reached (no HTTP server yet, step 5). `GetTemplateFromFile`, `UndoTransparency`, `RefreshWebMailIndex`, `MailAPIProcessHTTPMessage` are empty.
- UI frames received: `GetRaw` returns nothing, so the BBS does not answer FBB header resync requests (`? 00000...`) heard on air. UI frames sent lose any digipeater path (`UIPortN` `Digis`), as RHP datagrams have none.
- Multicast mail (`MCAST`), event programs, packet map reporting, the node trace copy of the BBS log (`BPQTRACE`), APRS messages and position: empty.
- MQTT: built with `NOMQTT`. Winlink session reporting: the frequency/mode lookup in `Connected()` finds no node session to read, so it reports 0.
- The ISP mail gateway (an SMTP/POP3 client, not a server) is compiled as in LinBPQ and untested.
- `GetConnectionInfo` never reports a secure (console) session, so sysop rights come only from the user record.

## Things found in pdn

- The RHP server resolves `port` by port id only (`src/Packet.Node/Rhp/SupervisorRhpGateway.cs` `ResolvePortId`, line 241), but `docs/rhp2-server.md` line 18 says `port` is the 1-indexed port number and that a missing port means the first port. An open with `"port":"1"` gets `errCode 10 No such port '1'`, and an open with no port is refused unless the target is a local app. The shim works round it with the port map.
- A failed open cannot tell "busy" (DM) from "no answer": both come back as errCode 15 with "torn down before DL-CONNECT-confirm arrived (peer refused or link reset)" (`src/Packet.Ax25/Session/Ax25Listener.cs` line 573). Tracked as packet.net#849. The shim reports `Failure with` for both.
- Closing an RHP handle disconnects the link at once (`src/Packet.Node.Core/Console/Ax25NodeConnection.cs` `DisposeAsync`, line 152), and pdn has no queue-depth or Busy signal on `sendReply`. On a slow radio link the last data sent before a close could be lost. For now the shim holds each close for 10 seconds after its last send (`linger`). Whether pdn should hold the DISC until queued data is acknowledged, as BPQ does, is under discussion; the shim's close handling stays as it is until that is settled. Not seen in the lab, where AXUDP on loopback acknowledges at once.
- A pdn telnet console user who types `C <app call>` reaches the app with `accept.remote` set to the telnet peer address (`127.0.0.1:51398`), not a callsign. The mail code keeps callsigns in 10 byte fields, and this crashed the first build of the shim. The shim now refuses non-callsign peers with a message.

## Order of work

- [x] 1. Proof of concept (2026-09-28): standalone build with no node code and no upstream edits; RHP host API shim; inbound user sessions; outbound connect scripts including failure, `ELSE` and multi-hop; FBB B2 compressed forwarding both ways with a lab LinBPQ through pdn; reconnect when pdn restarts.
- [x] 2. Hardening (2026-09-29): idle timeout enforced; alias callsigns (`alias = BBS`); RHP auth, tested against a real pdn user; `linmail-pdn.conf` for the shim's own settings; UI frames sent through an RHP `dgram` socket; SMTP/POP3/NNTP servers never started; early inbound connects wait for a free stream. Still open: close handling (waiting on the pdn DISC discussion) and receiving UI frames.
- [x] 3. Tests (2026-09-29): `pdn/tests`, a fake RHP server suite and a run against a real pdn node and LinBPQ.
- [x] 4. CI (2026-09-29): `.github/workflows/linmail-pdn.yml`.
- [ ] 5. Web: an HTTP listener on loopback serving webmail and the BBS config pages through pdn's app gateway, taking the user from `X-Pdn-User`/`X-Pdn-Scope` instead of BPQ's own login.
- [ ] 6. Packaging: a `pdn-app.yaml` (capabilities `packet` and `web`, `service.command: ./linmail-pdn`), a release tarball and a .deb for the packet-net apt repo, and a catalog entry.
- [ ] 7. Migration notes: moving an existing LinBPQ mail directory across, and turning bpq32.cfg port numbers into a port map.
- [ ] 8. Later, if wanted: MQTT, Winlink reporting, packet map, answering FBB header resync requests.

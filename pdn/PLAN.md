# linmail-pdn: the LinBPQ mail BBS as a packet.net app

linmail-pdn is the LinBPQ mail server ("linmail") built on its own, without the node, and connected to a packet.net (pdn) node over pdn's RHPv2 server. It uses the unchanged upstream mail sources, so John's mail changes keep flowing in with a normal merge. It sits alongside pdn-bbs (the from-scratch .NET BBS for pdn); both are wanted.

## How it works

On Windows, BPQMail was always a separate program talking to the node through the bpq32 host API. LinBPQ compiles the same mail code into the node. linmail-pdn goes back to the separate-program model, with pdn in place of bpq32:

- `pdnhost.c` implements the host API calls the mail code makes (`SessionControl`, `SessionState`, `GetMsg`, `SendMsg`, `GetConnectionInfo`, `TXCount` and the rest) on one RHPv2 connection to pdn.
- Inbound: `socket`/`bind`/`listen` on the BBS callsign. Each accepted child handle is attached to a free BBS stream, so the mail code sees an ordinary connect, data and disconnect.
- Outbound forwarding keeps the BPQ model. `ConnectUsingAppl` gives the stream a session with an emulated node command handler. The connect script's `C port CALL` becomes an RHP `open`, answered with BPQ-style node text (`N0PDN} Connected to CALL`, `Failure with CALL`, `Busy from CALL`). After that the stream is transparent, so later script lines go to the far node as data and multi-hop scripts work unchanged.
- `linmail-pdn.c` is the mail half of `LinBPQ.c`: it loads `linmail.cfg` and the mail files from a directory and runs the same timer calls at the same rates (one tick every 100 ms, forwarding every 2 s, slow timers every 10 s).
- The node structures the mail code reads directly (`BPQHOSTVECTOR`, the session's `L4USER`, `Secure_Session`, the `L4CROSSLINK` walk in `Connected()`) are satisfied by a small fake vector in `pdnhost.c`, so no upstream source file needed changing.

## Build and run

```
make -C pdn
pdn/linmail-pdn -d /var/lib/linmail -r 127.0.0.1:9000 -c GB7XYZ -n GB7XYZ-5 -m 1=vhf,2=hf
```

Settings come from the pdn app supervisor's environment (`PDN_APP_STATE`, `PDN_RHP_HOST`, `PDN_RHP_PORT`, `PDN_APP_CALLSIGN`, `PDN_NODE_CALLSIGN`, `PDN_NODE_ALIAS`), then `PDN_LINMAIL_PORTMAP` and `PDN_LINMAIL_DEFAULTPORT`, then the command line (`linmail-pdn -h`). The port map turns the port numbers in existing BPQ connect scripts into pdn port ids. Needs libjansson and libconfig.

## Lab

`lab/` holds the loopback lab used for the proof: a pdn node (N0PDN, one AXUDP port `bpq`, RHP on 19000), a LinBPQ node with mail (N0BPQ, BBS N0BPQ-1) built from this fork, and linmail-pdn (BBS N0LMB). `lab/lab.sh start WORKDIR packetnet.dll linbpq` starts all three; `lab/session.py` logs in to the LinBPQ telnet port as N0USR, connects to a BBS, sends a personal message, lists it and reads it back. Posting to `N0ABC @ N0BPQ` on N0LMB, or to `N0XYZ @ N0LMB` on N0BPQ (`BBS` = `LOCAL`), forwards it with FBB B2 compression within a few seconds.

## Upstream edits

None. Every mail source file builds as it is. The build links `BBSUtilities.c BBSHTMLConfig.c FBBRoutines.c MailCommands.c MailDataDefs.c MailRouting.c MailTCP.c MBLRoutines.c WPRoutines.c WebMail.c NNTPRoutines.c lzhuf32.c` plus the node-free helpers `Housekeeping.c UIRoutines.c utf8Routines.c md5.c compatbits.c CMSAuth.c`. If a later upstream change needs a pdn-specific branch, put it behind `#ifdef PDN_LINMAIL` and list it here.

## Stubbed or not wired up (proof scope)

In `pdnstubs.c` and `pdnhost.c`:

- Webmail and the BBS config web pages: compiled, never reached (no HTTP server). `GetTemplateFromFile`, `UndoTransparency`, `RefreshWebMailIndex`, `MailAPIProcessHTTPMessage` are empty.
- UI frames: `SendRaw` drops them and `GetRaw` returns nothing, so mail-for beacons and FBB UI headers do nothing.
- Multicast mail (`MCAST`), event programs, packet map reporting, the node trace copy of the BBS log (`BPQTRACE`), APRS messages and position: empty.
- MQTT: built with `NOMQTT`. Winlink session reporting: the frequency/mode lookup in `Connected()` finds no node session to read, so it reports 0.
- SMTP, POP3 and NNTP servers are compiled and run exactly as in LinBPQ, so they stay off unless `linmail.cfg` turns them on. Not tested.
- `ChangeSessionIdletime` is recorded but not enforced; the mail code's own 15 minute watchdog still applies.
- `GetConnectionInfo` never reports a secure (console) session, so sysop rights come only from the user record.

## Connect script lines the shim cannot map

These answer `Sorry, ...`, which the script engine treats as a failed connect (so an `ELSE` block still runs), and are logged:

- `C CALL V DIGI ...` (digipeated connects): RHP `open` has no digipeater path.
- `C ALIAS` or `C NODE` where the target is not a plain callsign: pdn's RHP is AX.25 only, no NET/ROM connects or node aliases.
- `NC`, `ATTACH`/`A`, `MCAST` (the `MCASTRX` script keyword), `RADIO ...` (including `RADIO AUTH`), and any other node command.
- A bare `C CALL` with no port only works if the call is another app on the same pdn node, or if a default port is set (`-p`); pdn refuses an RHP open with no port otherwise.
- `C port CALL S` (stay) is accepted and the `S` ignored.

`PACLEN` and `IDLETIME` sent to the node get `Ok`. Script keywords the mail code handles itself (`TIMES`, `ELSE`, `PAUSE`, `MSGTYPE`, `INTERLOCK`, `SKIPCON`, `SKIPPROMPT`, `TEXTFORWARDING`, `FILE`, `IMPORT` and so on) are unaffected.

## Things found in pdn

- The RHP server resolves `port` by port id only (`src/Packet.Node/Rhp/SupervisorRhpGateway.cs` `ResolvePortId`, line 241), but `docs/rhp2-server.md` line 18 says `port` is the 1-indexed port number and that a missing port means the first port. An open with `"port":"1"` gets `errCode 10 No such port '1'`, and an open with no port is refused unless the target is a local app. The shim works round it with the port map.
- A failed open cannot tell "busy" (DM) from "no answer": both come back as errCode 15 with "torn down before DL-CONNECT-confirm arrived (peer refused or link reset)" (`src/Packet.Ax25/Session/Ax25Listener.cs` line 573). The shim reports `Failure with` for both.
- Closing an RHP handle disconnects the link at once (`src/Packet.Node.Core/Console/Ax25NodeConnection.cs` `DisposeAsync`, line 152), and pdn has no queue-depth or Busy signal on `sendReply`. On a slow radio link the last data sent before a close could be lost. The shim holds each close for 10 seconds after its last send (`-L`); drain-before-disconnect or a Busy flag in pdn would be the real fix. Not seen in the lab, where AXUDP on loopback acknowledges at once.
- A pdn telnet console user who types `C <app call>` reaches the app with `accept.remote` set to the telnet peer address (`127.0.0.1:51398`), not a callsign. The mail code keeps callsigns in 10 byte fields, and this crashed the first build of the shim. The shim now refuses non-callsign peers with a message.

## Order of work

- [x] 1. Proof of concept (2026-09-28): standalone build with no node code and no upstream edits; RHP host API shim; inbound user sessions; outbound connect scripts including failure, `ELSE` and multi-hop; FBB B2 compressed forwarding both ways with a lab LinBPQ through pdn; reconnect when pdn restarts.
- [ ] 2. Hardening: close handling once pdn can drain or report queue depth; idle timeout; a service alias (`BBS`) as a second listen callsign; RHP auth tested; a small `linmail-pdn.conf` instead of flags; UI frames through an RHP `dgram` socket so mail-for beacons work.
- [ ] 3. Tests in this fork: a Python fake RHP server driving linmail-pdn (connect, forward, failure, reconnect) in `tests/integration`, plus one end-to-end run against a real pdn container.
- [ ] 4. CI: build `make -C pdn` on every push and run the tests from step 3.
- [ ] 5. Web: an HTTP listener on loopback serving webmail and the BBS config pages through pdn's app gateway, taking the user from `X-Pdn-User`/`X-Pdn-Scope` instead of BPQ's own login.
- [ ] 6. Packaging: a `pdn-app.yaml` (capabilities `packet` and `web`, `service.command: ./linmail-pdn`), a release tarball and a .deb for the packet-net apt repo, and a catalog entry.
- [ ] 7. Migration notes: moving an existing LinBPQ mail directory across, and turning bpq32.cfg port numbers into a port map.
- [ ] 8. Later, if wanted: SMTP/POP3/NNTP through pdn's tailnet port forwards, MQTT, Winlink reporting, packet map.

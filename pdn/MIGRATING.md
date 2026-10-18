# Moving your LinBPQ mailbox to pdn

LinBPQ Mail on pdn (linmail-pdn) is the same mail server you run in LinBPQ today, built on its own and connected to a pdn node. Your messages, users, forwarding partners and connect scripts carry straight across. You copy your files over, tell it which pdn ports your connect scripts mean, and you're done.

## Before you start

- You need pdn node 0.58.0 or newer. Older ones can cut off the end of what the BBS sends just before it hangs up (packet.net#850).
- Install LinBPQ Mail on the machine running pdn: from Available apps in the control panel (pdn 0.60.0 or newer), or `sudo apt install pdn-linmail` from the packet-net apt repo. It lands in `/usr/share/packetnet/apps/linmail`.
- In the pdn control panel, open **Apps**, find **LinBPQ Mail** and set its **callsign**. Use your BBS's callsign from bpq32.cfg (the call on your `APPLICATION` line, or `BBSCALL`). Don't enable it yet.
- Stop LinBPQ, or at least its mail, so nothing is writing to the files while you copy them.

## Copy your files

Copy these from your LinBPQ directory into `/var/lib/packetnet/apps/linmail/`:

- `linmail.cfg` (settings, users and forwarding partners)
- `DIRMES.SYS` (the message list) and the `Mail/` folder (the messages themselves)
- `WFBID.SYS` (BIDs already seen) and `WP.cfg` (white pages; current LinBPQ keeps them there, so copy `WP.SYS` too only if you have one)
- `BADWORDS.SYS` and `INTRCPT.APS`, if you have them
- `BPQBBSUsers.dat`, if your LinBPQ is old enough to still have one

Then make them belong to pdn:

```
sudo chown -R packetnet:packetnet /var/lib/packetnet/apps/linmail
```

Nothing in these files needs editing.

## Tell it about your ports

Your connect scripts say things like `C 2 GB7XYZ`, where `2` is a LinBPQ port number. pdn names its ports instead (`vhf`, `hf` and so on). Create `/var/lib/packetnet/apps/linmail/linmail-pdn.conf` with a port map:

```
portmap = 1=vhf, 2=hf
```

The same numbers pick the ports for mail-for beacons (the `UIPort` settings in `linmail.cfg`). There's a commented example with every setting in `/usr/share/packetnet/apps/linmail/linmail-pdn.conf.example`.

Some connect script lines only a BPQ node understands, and pdn has no equivalent: `NC`, `ATTACH`, `PASSWORD`, `FRACK`, `MAXFRAME`, and a `!` in front of a callsign. Rewrite those as a plain `C <port> <call>`. The port numbers can stay, through the port map, and FRACK and MAXFRAME values belong in pdn's port settings instead. For example `MSGTYPES PTB8000R|PASSWORD|FRACK 3 21|MAXFRAME 3 3|NC 3 !GB7IOW-1` becomes `MSGTYPES PTB8000R|C 3 GB7IOW-1`.

## Telnet and FBB logins

If programs log in to your BBS over LinBPQ's telnet ports (a forwarding client such as the pdn-mailcast head end, or your own sysop scripts), or you do, LinBPQ Mail can take those logins the same way. It's off until you give it a port. Add to `linmail-pdn.conf`:

```
fbb_port = 8011            # like LinBPQ's FBBPORT, for programs: no prompts
telnet_port = 8010         # like LinBPQ's TCPPORT: user: and password: prompts
USER=tf,password,M0LTE,,SYSOP
USER=Q0HEAD,password,Q0HEAD
```

- Copy the `USER=` lines from the Telnet port in `bpq32.cfg` as they are: name, password, callsign, application, `SYSOP`. Names and passwords are case-sensitive, as in LinBPQ.
- After logging in, type `BBS`, as at LinBPQ's node prompt. Programs that send `BBS` after the password carry on working. A `USER=` line with `BBS` as its application goes straight in.
- Sysop commands (`FWD`, `K` on others' mail and so on) work for a login with `SYSOP` on its line whose BBS user also has the sysop flag, exactly as in LinBPQ.
- Programs need only the new host and port: the login and the BBS session look the same to them.
- Two things look different by hand. The banner after the password is linmail-pdn's own, and `B` in the BBS closes the connection, where LinBPQ with `DisconnectOnClose=0` (as on GB7RDG) left you at the node with the telnet session still open.
- A wrong login is answered after a short pause, and always with `user:` again, whether the name or the password was wrong. You have 30 seconds to log in.
- It listens on this machine only (`127.0.0.1`) unless you set `telnet_bind`, say to `0.0.0.0` for every address.
- **Only on networks you trust.** Like LinBPQ's telnet ports, these are plain telnet: passwords cross the network in plain text, and anyone who can reach the port can connect before logging in. linmail-pdn limits what an unknown client can do, but a determined attacker on the same network can still keep the login slots busy so that nobody else gets in. Keep the ports on loopback or a LAN you trust, never open to the internet.
- **Port 8011:** pdn's own telnet console listens on `127.0.0.1:8011`, so linmail-pdn can't have 8011 on that address too. Either move pdn's console (`management.telnet.port` in the node config), or pick another `fbb_port` and change the clients' port, or bind linmail-pdn to the machine's LAN address (`telnet_bind = 192.168.1.10`), where 8011 is free and remote clients change only the host.
- LinBPQ's telnet log records every password typed; linmail-pdn never logs, echoes or keeps them. If you're moving from LinBPQ, it's worth changing the passwords as you go.

## Start it

Enable **LinBPQ Mail** in the control panel. pdn starts it, and it answers on the callsign you set. Stations can connect to that call directly, or type `MAIL` at the node prompt. Webmail and the mail management pages open from the app's entry in the panel.

Webmail uses your pdn login instead of the BBS password. If your pdn username is a callsign with an account on the BBS, you get that mailbox. A pdn username that's a callsign with no BBS account yet gets a message saying so: connect to the BBS over the air once to create it.

Node admins always get the management pages, acting as the BBS sysop, even with no BBS account of their own. An admin whose username isn't a callsign also gets the sysop's mailbox in webmail.

## What's different from LinBPQ

- **Connect scripts:** plain `C port CALL` lines work, including multi-hop scripts where later lines are typed at the far node. So do `C CALL` and `C ALIAS` with no port (`C GB7MNK`, `c gb7wod|c gb7brk|bbs`): pdn routes them over NET/ROM, as BPQ's node does, which needs a pdn with NET/ROM connect routing on and packet.net#932. pdn can't do digipeated connects (`C CALL V DIGI`) or BPQ-only node commands (`NC`, `ATTACH`, `RADIO`, `MCAST`). Those lines fail, so an `ELSE` block in the script still runs.
- **Not included:** the SMTP, POP3 and NNTP servers. They stay off even if `linmail.cfg` turns them on, and linmail-pdn logs a line saying so. MQTT, the packet map, Winlink session reporting and multicast mail aren't included either.
- **UI beacons** go out without a digipeater path, and the BBS doesn't answer header resync requests heard on air.
- **Logins:** BPQ's own web login isn't used. The pages only open through the pdn control panel. Telnet and FBB logins work as described above, but there is no node behind them: only `BBS` (or `MAIL`) and `B` work at the prompt.

## Going back

Your files aren't converted, so you can copy them back into your LinBPQ directory at any time and carry on there.
